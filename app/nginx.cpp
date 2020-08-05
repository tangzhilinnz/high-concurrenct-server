// valgrind --tool=memcheck --leak-check=full --show-reachable=yes ./nginx
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h> 
#include <errno.h>
#include <arpa/inet.h>

#include "ngx_macro.h"         //各种宏定义
#include "ngx_func.h"          //各种函数声明
#include "ngx_c_conf.h"        //和配置文件处理相关的类,名字带c_表示和类有关
#include "ngx_c_socket.h"      //和socket通讯相关
/*#include "ngx_c_memory.h"*/  //和内存分配释放等相关
#include "ngx_c_threadpool.h"  //和多线程有关
#include "ngx_c_crc32.h"       //和crc32校验算法有关 
#include "ngx_c_slogic.h"      //和socket通讯相关

//本文件用的函数声明
static void freeresource();

//和设置标题有关的全局量
char**  g_os_argv = NULL;     //原始命令行参数数组,在main中会被赋值
char*   gp_envmem = NULL;     //指向自己分配的env环境变量的内存
size_t  g_envneedmem = 0;     //环境变量所占内存大小
char*   gp_argv = NULL;       //指向自己分配的argv(argv[1]开始)内存
size_t  g_argvlen = 0;        //主程序命令行参数所占内存大小(除argv[0]外)
size_t  g_argvneedmem = 0;    //主程序命令行参数所占内存大小(包含argv[0]外)

int     g_daemonized = 0;     //守护进程标记，标记是否启用了守护进程模式
                              //0：未启用，1：启用了

//全局对象的构造在main函数之前，在main函数之后，程序结束之前析构
CLogicSocket   g_socket;      //socket全局对象
CThreadPool    g_threadpool;  //线程池全局对象

//和进程本身有关的全局量
pid_t ngx_pid;          //当前进程的pid
pid_t ngx_parent;       //父进程的pid
int   ngx_process;      //进程类型，比如master,worker进程等
int   g_stopEvent;      //标志程序退出：0不退出，1退出

sig_atomic_t  ngx_reap; //标记子进程状态变化(一般是子进程发来SIGCHLD信号表示退出)，
                       //sig_atomic_t：系统定义的类型，访问或改变这些变量需要在计算
                      //机的一条指令内完成，一般等价于int，通常情况下，int类型的变量
                      //通常是原子访问的，也可以认为sig_atomic_t就是int类型的数据

//和数据包校验有关的全局变量
CCRC32* pCRC32;
//和内存分配有关的全局变量
//TzMemPool<_SPN64K, _LARGE>* pMalloc;
MemPool memPool;
//和定时器有关的全局变量
TimeWheel timeWheel;

//和处理错误包头有关的全局变量
int g_wrongPKGAdmit = 0;

// argv is a pointer to a const pointer to a char
// const修鉓是表示argv指向的指针是个常量，不能对其进行增减操作
int main(int argc, char* const* argv)
{
    int exitcode = 0;           //退出代码，先给0表示正常退出
    //CMemory* p_memory;

    //(0)先初始化的变量
    g_stopEvent = 0;            //标记程序是否退出，0不退出   

    //(1)无伤大雅也不需要释放的放最上边    
    ngx_pid = getpid();          //取得进程pid
    ngx_parent = getppid();      //取得父进程的id 
    g_os_argv = (char**)argv;    //保存参数指针 

    //全局量有必要初始化的
    ngx_process = NGX_PROCESS_MASTER; //先标记本进程是master进程
    ngx_reap = 0;                     //标记子进程没有发生变化

    //(2)初始化失败，就要直接退出的
    //配置文件必须最先要，后边初始化啥的都用，所以先把配置读出来，供后续使用 
    CConfig* p_config = CConfig::GetInstance(); //单例类
    if (p_config->Load("nginx.conf") == false) //把配置文件内容载入到内存        
    {
        //配置文件读取失败，程序即将退出

        ngx_log_init(); //初始化日志，目的是把错误信息输入到日志，因为这个程序若被
                        //设置为daemon模式并且在开机时就启动，或通过键盘启动但与此
                        //挂钩的终端被关闭了，导致ngx_log_stderr无法将错误信息显示
                        //到标准错误上(屏幕)，因此先执行ngx_log_init，可以打开一个
                        //有效的日志文件，从而让ngx_log_stderr打印的信息备份到日志
                        //文件中，方便查看排错

        ngx_log_stderr(0, "Configuration file[%s] loading failed, exit!",
            "nginx.conf");

        //exit终止进程，在main中出现和return效果一样：
        //exit(0)表示程序正常退出，
        //exit(1)/exit(-1)表示程序异常退出，
        //exit(2)表示表示系统找不到指定的文件
        exitcode = 2; //标记找不到文件

        ngx_log_stderr(0, "Program exit!\n");
        freeresource();  //一系列的main返回前的释放动作函数
        return exitcode;
    }

    //(2.1)内存单例类可以在这里初始化，返回值不用保存
    //CMemory::GetInstance();
    //(2.2)crc32校验算法单例类可以在这里初始化，全局变量pCRC32赋值
    pCRC32 = CCRC32::GetInstance();
    //pMalloc = TzMemPool<_SPN64K, _LARGE>::GetInstance();

    //(3)一些初始化函数，准备放这里
    ngx_log_init();    //日志初始化(创建/打开日志文件)

    if (ngx_init_signals() != 0) //信号初始化
    {
        exitcode = 1;

        ngx_log_stderr(0, "Program exit!\n");
        freeresource();  //一系列的main返回前的释放动作函数
        return exitcode;
    }

    if (g_socket.Initialize() == false) //仅初始化监听socket，还未初始化epoll函数
    {
        exitcode = 1;
        
        ngx_log_stderr(0, "Program exit!\n");
        freeresource();  //一系列的main返回前的释放动作函数
        return exitcode;
    }

    //(4)一些不好归类的其他类别的代码，准备放这里
    ngx_init_setproctitle();    //把环境变量和命令行参数搬家，让它们在修改完进程名
                                //后可以继续被使用
    //打印argv和env内存
    for (int i = 0; argv[i]; i++)
    {
        printf("argv[%d]地址=%p\n", i, &argv[i]);
        printf("argv[%d]存储的地址=%p\n", i, argv[i]);
        printf("argv[%d]指向的字符串内容=%s\n", i, argv[i]);
    }
    printf("--------------------------------------------------------\n");
    for (int i = 0; environ[i]; i++)
    {
        printf("evriron[%d]地址=%p\n", i, &environ[i]);
        printf("evriron[%d]存储的地址=%p\n", i, environ[i]);
        printf("evriron[%d]指向的字符串内容=%s\n", i, environ[i]);
    }
    printf("--------------------------------------------------------\n");

    //打印所有配置文件信息
    std::vector<LPCConfItem>::iterator pos = p_config->m_ConfigItemList.begin();
    for (; pos != p_config->m_ConfigItemList.end(); ++pos)
    {
        printf("conf: %s = %s\n", (*pos)->ItemName, (*pos)->ItemContent);
    }
    printf("--------------------------------------------------------\n");

    //测试ngx_vslprintf函数
    /*
    ngx_log_error_core(NGX_LOG_EMERG, 131, "invalid option: %.6f", 12.999);
    ngx_log_error_core(NGX_LOG_ALERT, 95, "invalid option: %10d", 21);
    ngx_log_error_core(NGX_LOG_STDERR, 56, "invalid option: %.6f", 21.378);
    ngx_log_error_core(NGX_LOG_DEBUG, 80, "invalid option: %.2f", 12.999);
    ngx_log_error_core(NGX_LOG_ERR, 21, "invalid option: %xd", 1678);
    ngx_log_error_core(NGX_LOG_DEBUG, 34, "invalid option: %Xd", 1678);
    ngx_log_error_core(NGX_LOG_NOTICE, 19, "invalid option: %s , %d", "testInfo", 326);
    ngx_log_error_core(NGX_LOG_NOTICE, 19, "invalid option: "
        "%c%   5.1f%s", 12, -32., "tangzhilin");
    ngx_log_stderr(0, "invalid option: %    00 00 00 00 00 00 0013uxxX.0000f", -799.51);
    ngx_log_stderr(0, "\n\n\n\tdtangzhilin", -799.51);
    int test = 10;
    ngx_log_stderr(0, "%pasdf%%n", &test);*/
    printf("--------------------------------------------------------\n");


    //允许校验错误包次数初始化
    const char* isOn = p_config->GetString("WrongPKGCheck");
    if (isOn == NULL)
    {
        g_wrongPKGAdmit = 0;
    }
    else
    {
        if (strcasecmp(isOn, "ON") == 0)
        {
            g_wrongPKGAdmit = p_config->GetIntDefault(
                "WrongPKGCheck_Admit", 5);
            g_wrongPKGAdmit = ngx_min(g_wrongPKGAdmit, 30000);
            g_wrongPKGAdmit = ngx_max(g_wrongPKGAdmit, 5);
        }
        else
        {
            g_wrongPKGAdmit = 0;
        }
    }

    //(5)创建守护进程
    if (p_config->GetIntDefault("Daemon", 0) == 1) //读配置文件，拿到配置文件中是
                                                   //否按守护进程方式启动的选项
    {
        //按守护进程方式运行
        int c_daemon_result = ngx_daemon();
        if (c_daemon_result == -1) //fork()失败
        {
            exitcode = 1;    //标记失败
            ngx_log_stderr(0, "Program exit!\n");
            freeresource();  //一系列的main返回前的释放动作函数
            return exitcode;
        }
        if (c_daemon_result == 1)
        {
            //这是原始父进程，正常fork()守护进程后的进程正常退出
            freeresource();   
            exitcode = 0;
            return exitcode; 
        }

        //走到这里，成功创建了守护进程并且这里已经是fork()出来的新进程，
        //现在这个新进程变成了程序的master进程，原始父进程已在上边退出
       
        g_daemonized = 1; //守护进程标记，标记是否启用了守护进程模式，
                          //0：未启用，1：启用了
    }

    //(6)开始正式的主工作流程，主流程一致在下边这个函数里循环，暂时不会走下来，
    //资源释放的问题日后再慢慢完善和考虑

    ngx_master_process_cycle(); //父进程和子进程，正常工作期间都在这个函数里循环
                              //worker子进程中首先生成处理消息队列的线程池，再初始化
                           //epoll函数来监听请求连入的客户端，然后进入一个死循环来
                          //epoll_wait所有该worker进程生成socket读写事件

    //(7)该释放的资源要释放掉
    ngx_log_stderr(0, "Program exit!\n");
    freeresource();  //一系列的main返回前的释放动作函数
    return exitcode;
}

// 专门在程序执行末尾释放资源的函数【一系列的main返回前的释放动作函数】
void freeresource()
{
    //(1)对于因为设置可执行程序标题导致的环境变量分配的内存，我们应该释放
    if (gp_envmem)
    {
        delete[]gp_envmem;
        gp_envmem = NULL;
    }
    if (gp_argv)
    {
        delete[] gp_argv;
        gp_argv = NULL;
    }
    g_os_argv = NULL;

    //(2)关闭日志文件，ngx_log结构体在ngx_global.h中声明，在ngx_log.cpp定义
    if (ngx_log.fd != STDERR_FILENO && ngx_log.fd != -1)
    {
        close(ngx_log.fd); //不用判断结果了
        ngx_log.fd = -1; //标记下，防止被再次close吧        
    }
}
