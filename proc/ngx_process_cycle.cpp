//和开启子进程相关
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>   //信号相关头文件 
#include <errno.h>    //errno
#include <unistd.h>

#include "ngx_func.h"
#include "ngx_macro.h"
#include "ngx_c_conf.h"
#include "ngx_global.h"

//函数声明(static表示适用范围仅限本文件)
static void ngx_start_worker_processes(int threadnums);
static int  ngx_spawn_process(int threadnums,const char *pprocname);
static void ngx_worker_process_cycle(int inum,const char *pprocname);
static void ngx_worker_process_init(int inum);

//变量声明(static表示适用范围仅限本文件)
static u_char  master_process[] = "master process";

//描述：创建worker子进程
void ngx_master_process_cycle()
{    
    sigset_t set;   //系统定义的信号集

    sigemptyset(&set);  //清空信号集

    //下列这些信号在执行本函数期间不希望收到(官方nginx中有这些信号，保护不希望由
    //信号中断的代码临界区)，建议fork()子进程时学习这种写法，防止信号的干扰；
    sigaddset(&set, SIGCHLD);     //子进程状态改变
    sigaddset(&set, SIGALRM);     //定时器超时
    sigaddset(&set, SIGIO);       //异步I/O
    sigaddset(&set, SIGINT);      //终端中断符
    sigaddset(&set, SIGHUP);      //连接断开
    sigaddset(&set, SIGUSR1);     //用户定义信号1
    sigaddset(&set, SIGUSR2);     //用户定义信号2
    sigaddset(&set, SIGWINCH);    //终端窗口大小改变
    sigaddset(&set, SIGTERM);     //终止
    sigaddset(&set, SIGQUIT);     //终端退出符
    //可以根据开发的实际需要往其中添加其他要屏蔽的信号
    
    //此时无法接受的信号；阻塞期间，你发过来的上述信号，多个会被合并为一个，暂存着，
    //等你放开信号屏蔽后才能收到这些信号。第一个参数用了SIG_BLOCK表明设置进程新的
    //信号屏蔽字为 当前信号屏蔽字和第二个参数指向的信号集的并集
    if (sigprocmask(SIG_BLOCK, &set, NULL) == -1)      
    {        
        ngx_log_error_core(NGX_LOG_ALERT, errno, 
            "In func ngx_master_process_cycle, func sigprocmask failed!");
    }
    //即便sigprocmask失败，程序流程也继续往下走

    //设置主进程标题---------begin
    size_t size;
    int    i;
    size = sizeof(master_process);  //sizeof，字符串末尾的\0是被计算进来了的
    size += g_argvneedmem;          //argv参数长度加进来    
    if (size < 1000) //长度小于这个，才设置标题
    {
        char title[1600] = {0}; // char title[1600];
                                // title[0] = 0;
                                // memset(title + 1, 0, 1599);
        strcpy(title, (const char*)master_process); //"master process\0"

        //char* strcat(char* strDestination, const char* strSource);
        //把strSource所指向的字符串追加到 strDestination 所指向的字符串的结尾，所以
        //必须保证strDestination有足够的内存空间来容纳两个字符串，否则会导致溢出错误，
        //strDestination末尾的\0会被覆盖，strSource末尾的\0会一起被复制过去，最终的
        //字符串只有一个\0
        //strcat(title, " "); //"master process \0"

        for (i = 0; g_os_argv[i]; i++)
        {
            strcat(title, g_os_argv[i]);
            strcat(title, " ");
        }

        ngx_setproctitle(title); //设置标题
        ngx_log_error_core(NGX_LOG_NOTICE, 0,  //记录下来进程名，进程id信息到日志
            "%s is up and running ......!", title); 
    }
    //设置主进程标题---------end
        
    //从配置文件中读取要创建的worker进程数量
    CConfig *p_config = CConfig::GetInstance(); //单例类
    //从配置文件中得到要创建的worker进程数量，缺省值为1
    int worker_proc_num = p_config->GetIntDefault("WorkerProcesses", 1); 

    ngx_start_worker_processes(worker_proc_num); //这里要创建worker子进程

    //创建子进程后，父进程的执行流程会返回到这里，子进程不会走下来   
    sigemptyset(&set); //信号屏蔽字为空，表示不屏蔽任何信号
    
    for (;;) 
    {
        //a)根据给定的参数设置新的mask 并阻塞当前进程(因为是个空集，所以不阻塞任何信号)
        //b)此时，一旦收到信号，便恢复原先的信号屏蔽(我们原来的mask在上边设置的，阻塞
        //  了多达10个信号)，从而保证下面的执行流程不会再次被其他信号打断
        //c)调用该信号对应的信号处理函数
        //d)信号处理函数返回后，sigsuspend返回，使程序流程继续往下走
        sigsuspend(&set); //阻塞在这里，等待一个信号，此时进程挂起，不占用cpu时间，只
                       //有收到信号才会被唤醒(返回)；此时master进程完全靠信号驱动干活    
        ngx_log_stderr(0, "The program flow after "
            "func sigsuspend has been executed.");   
        sleep(1);

        //以后扩充.......
    }

    return;
}

//描述：根据给定的参数创建指定数量的子进程，因为以后可能要扩展功能，增加参数，
//所以单独写成一个函数
//threadnums:要创建的子进程数量
static void ngx_start_worker_processes(int threadnums) 
{
    int i;
    for (i = 0; i < threadnums; i++) //master进程在执行这个循环来创建若干个子进程
    {
        ngx_spawn_process(i, "worker process");
    } 

    return;
}

//描述：产生一个子进程
//inum：进程编号【0开始】
//pprocname：子进程名字"worker process"
static int ngx_spawn_process(int inum, const char* pprocname)
{
    pid_t  pid;

    pid = fork(); //fork()系统调用产生子进程
    switch (pid)  //pid判断父子进程，分支处理
    {
    case -1: //产生子进程失败
        ngx_log_error_core(NGX_LOG_ALERT, errno,
            "In func ngx_spawn_process, "
            "func fork failed to generate subprocess: num=%d, procname=\"%s\"", 
            inum, pprocname);
        return -1;

    case 0: //子进程分支
        ngx_parent = ngx_pid;       //因为是子进程，所有原来的pid变成了父pid
        ngx_pid = getpid();         //重新获取pid，即本子进程的pid
        ngx_worker_process_cycle(inum, pprocname); //所有worker子进程，在这个函数
                                                   //里不断循环着不出来，也就是说
                                                   //子进程流程不往下边走;
        break;

    default: //父进程分支，直接break，流程往switch之后走        
        break;
    }

    //父进程分支会走到这里，子进程流程不往下边走
    //若有需要，以后再扩展增加其他代码......
    return pid;
}

//描述：worker子进程的功能函数，每个woker子进程，就在这里无限循环，处理网络事件和定时
//     器事件以对外提供服务(子进程分叉才会走到这里)
//inum：进程编号(0开始)
static void ngx_worker_process_cycle(int inum, const char* pprocname)
{
    //设置一下变量
    ngx_process = NGX_PROCESS_WORKER; //设置进程的类型，是worker进程

    //重新为子进程设置进程名，不要与父进程重复；并且给信号集初始化
    ngx_worker_process_init(inum);
    ngx_setproctitle(pprocname); //设置标题
    ngx_log_error_core(NGX_LOG_NOTICE, 0, //记录下进程名，进程id等信息到日志
        "%s is up and running ......!", pprocname); 

    //暂时先放个死循环，我们在这个循环里一直不出来
    //setvbuf(stdout,NULL,_IONBF,0); //这个函数将printf缓冲区禁止，
    //当printf末尾不带\n时也可以直接输出到屏幕
    for (;;)
    {
        //printf("worker进程休息1秒\n");       
        //fflush(stdout);      //刷新标准输出缓冲区，把输出缓冲区里的东西打印到标准
                               //输出设备上，则printf里的东西会立即输出；
        //sleep(1); //休息1秒       
        //usleep(100000);
        /*ngx_log_error_core(0, 0, "good--这是子进程，编号为%d,pid为%P！", 
            inum, ngx_pid);*/

        //if(inum == 1)
        //{
            //ngx_log_stderr(0,"good--这是子进程，编号为%d,pid为%P",inum,ngx_pid); 
            //printf("good--这是子进程，编号为%d,pid为%d\r\n",inum,ngx_pid);
            //ngx_log_error_core(0,0,"good--这是子进程，编号为%d",inum,ngx_pid);
            //printf("我的测试哈inum=%d",inum++);
            //fflush(stdout);
        //}

        //ngx_log_stderr(0,"good--这是子进程，pid为%P", ngx_pid); 
        /*ngx_log_error_core(0, 0, "good--这是子进程，编号为%d,pid为%P", 
            inum, ngx_pid);*/

        ngx_process_events_and_timers(); //处理网络事件和定时器事件
    }

    //如果从这个循环跳出来，考虑在这里停止线程池；
    /*g_threadpool.StopAll();
    g_socket.Shutdown_subproc(); */
    return;
}

//描述：子进程创建时调用本函数进行一些初始化工作
static void ngx_worker_process_init(int inum)
{
    sigset_t  set;      //信号集

    sigemptyset(&set);  //清空信号集

    if (sigprocmask(SIG_SETMASK, &set, NULL) == -1) //原来屏蔽那10个信号是为了防止
                                                    //fork()期间收到信号导致混乱，
                                                    //现不再屏蔽任何信号
    {
        ngx_log_error_core(NGX_LOG_ALERT, errno,
            "In func ngx_worker_process_init, func sigprocmask failed!");
    }

#if (VER == 1)
    //pMalloc->TzCreatePool();
    if (!memPool.CreatPool()) exit(-2);
#endif

    //配合setsockopt(isock, SOL_SOCKET, SO_REUSEPORT, (const void*)&reuseport, sizeof(reuseport))
    //函数，实现linux系统级别的多进程惊群解决方案
    if (g_socket.Initialize() == false) //仅初始化监听socket，还未初始化epoll函数
    {
        //内存没释放，简单粗暴退出；
        exit(-2);
    }

    //线程池代码，率先创建，至少要比和socket相关的内容优先
    CConfig* p_config = CConfig::GetInstance();
    //处理接收到的消息的线程池中线程数量
    int tmpthreadnums = p_config->GetIntDefault("ProcMsgRecvWorkThreadCount", 3);
    //定时器初始化
    if (timeWheel.CreateTimeWheel() == false)
    {
        //内存没释放，简单粗暴退出；
        exit(-2);
    }

    //线程池里每个 ThreadItem 对象的 _pThis 变量值都为  &g_threadpool
    if (g_threadpool.Create(tmpthreadnums) == false)  //创建线程池中线程
    {
        //内存没释放，简单粗暴退出，让系统来回收未释放资源
        exit(-2);
    }
    sleep(1); //再休息1秒；

    if (g_socket.Initialize_subproc() == false) //初始化子进程辅助线程
    {
        //内存没释放，简单粗暴退出；
        exit(-2);
    }

    //先创建当前worker进程的线程池(每个线程从消息队列取出消息并处理)，再初始化epoll
    //函数从而开始监听事件和接受消息，这样是为了在创建线程池的时候保证消息队列为空，
    //才能让所有线程进入pthread_cond_wait(待命中)；然后worker进程的主线程负责读取、
    //存放客户端发送来的消息以及可能检测和驱动向客户端写事件
    g_socket.ngx_epoll_init(); //初始化epoll相关内容，同时往监听socket上增加监听事
                              //件，从而开始让监听端口履行其职责，如果初始化失败，
                              //直接在此函数内退出该worker进程
    //将来再扩充代码... ...

    return;
}
