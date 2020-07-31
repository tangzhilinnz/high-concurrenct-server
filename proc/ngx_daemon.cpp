//和守护进程相关
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>     //errno
#include <sys/stat.h>
#include <fcntl.h>

#include "ngx_func.h"
#include "ngx_macro.h"
#include "ngx_c_conf.h"

//描述：守护进程初始化，创建一个守护进程
//执行失败：返回-1，子进程：返回0，父进程：返回1
int ngx_daemon()
{
    //(1)fork()一个子进程出来
    switch (fork())  //fork()出来这个子进程才会成为新的master进程
    {
    case -1:
        //创建子进程失败
        ngx_log_error_core(NGX_LOG_EMERG, errno, 
            "In ngx_daemon, func fork() failed!");
        return -1; //执行失败：返回-1
    case 0:
        //子进程，走到这里直接break;
        break;
    default:
        //回到主流程去释放一些资源
        return 1;  //父进程直接返回1；
    }

    //只有fork出来的子进程才能走到这个流程
    ngx_parent = ngx_pid;   //ngx_pid是原来父进程的id，因为这里是子进程，所以
                            //子进程的ngx_parent设置为原来父进程的pid
    ngx_pid = getpid();     //当前子进程的id要重新取得
    
    //(2)脱离终端，终端关闭，将跟此子进程无关
    if (setsid() == -1)  
    {
        ngx_log_error_core(NGX_LOG_EMERG, errno, 
            "In ngx_daemon, func setsid() failed!");
        return -1;
    }
    //备注：进程属于一个进程组，进程组号(GID)就是进程组长的进程号(PID)。登录会话
    //(session)可以包含多个进程组。这些进程组共享一个控制终端。这个控制终端通常是创建
    //进程的登录终端。控制终端，登录会话和进程组通常是从父进程继承下来的。当进程是会话
    //组长时会导致setsid()调用失败。setsid()调用成功后，进程成为新的会话组长和新的进
    //程组长，并与原来的登录会话和进程组脱离。由于会话过程对控制终端的独占性，进程同时
    //与控制终端脱离。

    //(3)设置为0，不要让它来限制文件权限，以免引起混乱
    umask(0); 

    //(4)打开黑洞设备，以读写方式打开
    int fd = open("/dev/null", O_RDWR);
    if (fd == -1) 
    {
        ngx_log_error_core(NGX_LOG_EMERG, errno, 
            "In ngx_daemon, func open(\"/dev/null\") failed!");
        return -1;
    }
    if (dup2(fd, STDIN_FILENO) == -1) //先关闭STDIN_FILENO[这是规定，已经打开的描述
                                      //符，改动之前，先close]，类似于指针指向null，
                                      //让/dev/null成为标准输入；dup2相当于把参数1
                                      //指向的内容赋给参数2，让参数2也指向此内容
    {
        ngx_log_error_core(NGX_LOG_EMERG, errno, 
            "In ngx_daemon, func dup2(STDIN) falied!");
        return -1;
    }
    if (dup2(fd, STDOUT_FILENO) == -1) //再关闭STDOUT_FILENO，类似于指针指向null，
                                       //让/dev/null成为标准输出；
    {
        ngx_log_error_core(NGX_LOG_EMERG, errno, 
            "In ngx_daemon, func dup2(STDOUT) failed!");
        return -1;
    }
    if (fd > STDERR_FILENO)   //fd应该是3(0，1，2已被系统占用)，这个应该成立
     {
        if (close(fd) == -1)  //释放资源这样这个文件描述符就可以被复用；不然
                              //这个数字(文件描述符)会被一直占着；
        {
            ngx_log_error_core(NGX_LOG_EMERG, errno, 
                "In ngx_daemon, func close(fd) failed!");
            return -1;
        }
    }

    return 0; //子进程返回0
}