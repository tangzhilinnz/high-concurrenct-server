//1 当程序正在执行一个信号函数时，在函数执行期间，再次触发多个同样的信号,系统会保留一个
//2 当程序正在执行一个信号函数时，在函数执行期间，再次触发一个不一样的信号, 系统会暂停
//  当前函数执行，跳转到新的信号对应的函数去执行，执行完成后，才回到跳回之前所执行的函
//  数中继续执行。

//和信号有关的函数放这里
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>    //信号相关头文件 
#include <errno.h>     //errno
#include <sys/wait.h>  //waitpid

#include "ngx_global.h"
#include "ngx_macro.h"
#include "ngx_func.h"

//一个信号有关的结构 ngx_signal_t
typedef struct _ngx_signal_t
{
    int         signo;      //信号对应的数字编号，每个信号都有对应的#define
    const char* signame;    //信号对应的中文名字，比如SIGHUP 

    //信号处理函数，这个函数由我们自己来提供，但是它的参数和返回值是固定的
    //(操作系统就这样要求)，siginfo_t:系统定义的结构
    void (*handler)(int signo, siginfo_t* siginfo, void* ucontext);
} ngx_signal_t;

//声明一个信号处理函数，static表示该函数只在当前文件内可见
static void ngx_signal_handler(int signo, siginfo_t* siginfo, void* ucontext); 
//获取子进程的结束状态，防止单独kill子进程时子进程变成僵尸进程
static void ngx_process_get_status(void);                                      

//数组，定义本系统处理的各种信号，我们取一小部分nginx中的信号，并没有全部搬移到这里，
//日后若有需要根据具体情况再增加，在实际商业代码中，你能想到的要处理的信号，都弄进来
ngx_signal_t signals[] = 
{
    //signo     signame     handler
    { SIGHUP,   "SIGHUP",   ngx_signal_handler }, //终端断开信号，对于守护进程常用
                                                  //于reload重载配置文件通知(标识1)
    { SIGINT,   "SIGINT",   ngx_signal_handler }, //标识2

    { SIGTERM,  "SIGTERM",  ngx_signal_handler }, //标识15

    { SIGCHLD,  "SIGCHLD",  ngx_signal_handler }, //子进程退出时，父进程会收到这个
                                                  //信号(标识17)
    { SIGQUIT,  "SIGQUIT",  ngx_signal_handler }, //标识3

    { SIGIO,    "SIGIO",    ngx_signal_handler }, //指示一个异步I/O事件，通用异步
                                                  //I/O信号
    { SIGSYS,   "SIGSYS, SIG_IGN",  NULL}, //忽略这个信号，SIGSYS表示收到了一个无效
                                           //系统调用，如果不忽略，进程会被操作系统
                                           //杀死(标识31)，所以我们把handler设置为
                                           //NULL，代表忽略这个信号，请求操作系统不
                                           //要执行缺省的该信号处理动作(杀掉）
    //根据需要再继续增加
    { 0,    NULL,   NULL}   //信号对应的数字至少是1，所以可以用0作为一个特殊标记
};

//初始化信号的函数，用于注册信号处理程序，返回值：0成功，-1失败
int ngx_init_signals()
{
    ngx_signal_t* sig;      //指向自定义结构数组的指针 
    struct sigaction sa;    //sigaction：系统定义的跟信号有关的一个结构，后续调用
                            //系统的sigaction()函数时要用到这个同名的结构

    for (sig = signals; sig->signo != 0; sig++)  //将signo == 0作为一个结束标记，
                                                 //因为信号的编号都不为0；
    {
        //注意，现在要把一堆信息往变量sa对应的结构里放
        memset(&sa, 0, sizeof(struct sigaction));

        if (sig->handler) //如果信号处理函数不为空，表示我要定义自己的信号处理函数
        {
            sa.sa_sigaction = sig->handler; //sa_sigaction：指定信号处理程序(函数)，
                                            //注意sa_sigaction也是函数指针，是系统
                                            //定义结构sigaction中的一个函数指针成员
            sa.sa_flags = SA_SIGINFO;   //sa_flags：int型，指定信号的一些选项，设置
                                        //了该标记(SA_SIGINFO)，就表示信号附带的参
                                        //数可以被传递到信号处理函数中，即让
                                        //sa.sa_sigaction指定的信号处理程序(函数)
                                        //生效，就把sa_flags设定为SA_SIGINFO
        }
        else
        {
            sa.sa_handler = SIG_IGN; //sa_handler：这个标记SIG_IGN给到sa_handler
                                     //成员，表示忽略信号的处理程序，否则操作系统的
                                     //缺省信号处理程序很可能把这个进程杀掉；                                                                        
        } 
        //其实sa_handler和sa_sigaction都是一个函数指针用来表示信号处理程序。只不过这
        //两个函数指针所代表的函数的参数不一样，sa_sigaction带的参数多，信息量大，而
        //sa_handler带的参数少，信息量少；
        //如果你想用sa_sigaction，那么就需要把sa_flags设置为SA_SIGINFO。

        sigemptyset(&sa.sa_mask); //当处理某个信号比如SIGUSR1信号时不希望收到SIGUSR2
                                  //信号，可以用诸如sigaddset(&sa.sa_mask,SIGUSR2)，
                                  //这样的语句只针对信号为SIGUSR1时做处理；
                                  //这里.sa_mask是个信号集，用于表示要阻塞的信号，
                                  //sigemptyset把信号集中的所有信号清0，本意就是不
                                  //准备阻塞任何信号。

        //设置信号处理动作(信号处理函数)，这里就是让这个信号来了后调用自己的处理程序，
        //有个老的同类函数叫signal，不过这个函数被认为是不可靠信号语义，不建议使用
        //参数1：要操作的信号；参数2：主要就是那个信号处理函数以及执行信号处理函数时候
        //要屏蔽的信号等等内容；参数3：返回以往对信号的处理方式(类似于sigprocmask()函
        //数的第三个参数)，跟参数2同一个类型，这里不需要这个东西，所以直接设置为NULL；
        if (sigaction(sig->signo, &sa, NULL) == -1) 
        {
            ngx_log_error_core(NGX_LOG_EMERG, errno,
                "In func ngx_init_signals, func sigaction for signal %s failed!", 
                sig->signame);

            return -1;       
        }
        else
        {
            /*ngx_log_error_core(NGX_LOG_EMERG, errno, "sigaction(%s) "
                "in ngx_init_signals succeed!", sig->signame);*/ 
            ngx_log_stderr(0, "In func ngx_init_signals, "
                "func sigaction for signal %s succeeded!", sig->signame); 
        }
    } 

    return 0; //成功    
}

//信号处理函数
//siginfo_t：这个系统定义的结构中包含了信号产生原因的有关信息
static void ngx_signal_handler(int signo, siginfo_t* siginfo, void* ucontext)
{
    //printf("来信号了\n");    
    ngx_signal_t* sig;  //自定义结构指针变量，用于遍历数组signals
    char* action;       //一个字符串，用于记录一个动作字符串以往日志文件中写

    for (sig = signals; sig->signo != 0; sig++) //遍历信号数组    
    {
        //找到对应信号，即可处理
        if (sig->signo == signo)
        {
            break;
        }
    }

    action = (char*)"";  //目前还没有什么动作；

    //-----------------------------------------------------------------------------------
    if (ngx_process == NGX_PROCESS_MASTER) //master管理进程，处理的信号一般会比较多 
    {
        //master进程的往这里走
        switch (signo)
        {
        case SIGCHLD:      //一般子进程退出会收到该信号
            ngx_reap = 1;  //标记子进程状态变化，日后master主进程的for(;;)循环中可
                           //能会用到这个变量(比如重新产生一个子进程)
            break;

            //.....其他信号处理以后待增加
        default:
            break;
        } 
    }
    else if (ngx_process == NGX_PROCESS_WORKER) //worker进程，处理的信号相对比较少
    {
        //worker进程的往这里走
        //......以后再增加
        //....
    }
    else
    {
        //非master非worker进程，先啥也不干
        //do nothing
    }
    //-----------------------------------------------------------------------------------

    //这里记录一些日志信息
    //si_pid = sending process ID(发送该信号的进程id)
    if (siginfo && siginfo->si_pid) 
    {
        ngx_log_error_core(NGX_LOG_NOTICE, 0, 
            "signal %d (%s) sended from %P%s",
            signo, sig->signame, siginfo->si_pid, action);
    }
    else //没有发送该信号的进程id，所以打印收到的信号信息
    {
        ngx_log_error_core(NGX_LOG_NOTICE, 0, 
            "signal %d (%s) sended %s",
            signo, sig->signame, action);
    }
    //-----------------------------------------------------------------------------------

    //.......其他需要扩展的将来再处理；

    //子进程状态有变化，通常是意外退出(防止单独kill子进程时，子进程变成僵尸进程)
    if (signo == SIGCHLD)
    {
        ngx_process_get_status(); //获取子进程的结束状态
    } 

    return;
}

//获取子进程的结束状态，防止单独kill子进程时，子进程变成僵尸进程
static void ngx_process_get_status(void)
{
    pid_t       pid;
    int         status;
    int         err;
    int         one = 0; //抄自官方nginx，应该是标记信号正常处理过一次

    //当你杀死一个子进程时，父进程会收到这个SIGCHLD信号。
    for (;;)
    {
        //waitpid，有人也用wait，但老师要求大家掌握和使用waitpid即可；
        //waitpid获取子进程的终止状态，这样，子进程就不会成为僵尸进程了；
        //第一次waitpid返回一个大于0的值，表示成功，后边显示 
        //2019/01/14 21:43:38 [alert] 3375: pid = 3377 exited on signal 9(SIGKILL)
        //第二次再循环回来，再次调用waitpid会返回一个0，表示子进程还没结束，
        //然后这里有return来退出；
        //第一个参数：-1，表示等待任何子进程，
        //第二个参数：保存子进程的状态信息
        //第三个参数：提供额外选项，WNOHANG表示不要阻塞，让这个waitpid()立即返回     
        pid = waitpid(-1, &status, WNOHANG);    

        if (pid == 0) //子进程没结束，会立即返回这个数字，但第一次进入for循环应该
                      //不是这个数字，因为一般是子进程退出时会执行到这个函数
        {
            return;
        } 
        if (pid == -1) //这表示这个waitpid调用有错误，有错误也立即返回出去
        {
            err = errno;
            if (err == EINTR) //调用被某个信号中断
            {
                continue;
            }
            //当pid所指示的子进程不存在，或此进程存在，但不是调用进程的子进程，
            //waitpid就会出错返回，这时errno被设置为ECHILD；
            if (err == ECHILD && one)  
            {
                return;
            }
            if (err == ECHILD)  //没有子进程
            {
                ngx_log_error_core(NGX_LOG_INFO, err, 
                    "In func ngx_process_get_status, func waitpid failed!");
                return;
            }
            //其他任何错误
            ngx_log_error_core(NGX_LOG_ALERT, err, 
                "In func ngx_process_get_status, func waitpid failed!");
            return;
        }

        //走到这里，表示成功返回进程id，这里打印一些日志来记录子进程的退出
        one = 1;  //标记waitpid()正常返回
        if (WTERMSIG(status))  //获取使子进程终止的信号编号
        {
            ngx_log_error_core(NGX_LOG_ALERT, 0, 
                "pid = %P exited on signal %d!", 
                pid, WTERMSIG(status)); 
        }
        else
        {
            ngx_log_error_core(NGX_LOG_NOTICE, 0, 
                "pid = %P exited with code %d!", 
                pid, WEXITSTATUS(status)); //WEXITSTATUS()获取子进程传递给exit或
                                           //_exit参数的低八位
        }
    } //end for

    return;
}
