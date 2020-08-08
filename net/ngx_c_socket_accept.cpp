//和网络中接受连接accept有关的函数放这里
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>    //uintptr_t
#include <stdarg.h>    //va_start....
#include <unistd.h>    //STDERR_FILENO等
#include <sys/time.h>  //gettimeofday
#include <time.h>      //localtime_r
#include <fcntl.h>     //open
#include <errno.h>     //errno
#include <sys/socket.h>
#include <sys/ioctl.h> //ioctl
#include <arpa/inet.h>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"

//建立新连接专用函数，当新连接进入时，本函数会被ngx_epoll_process_events所调用，
//函数的参数pConnL是一个和监听socket关联的连接对象
void 
CSocket::ngx_event_accept(lpngx_connection_t pConnL)
{
    //因为listen套接字上用的不是ET，而是LT，意味着客户端连入如果我要不处理，这个函数会
    //被多次调用，所以，这里可以不必多次accept()，可以只执行一次accept()；
    //这也可以避免本函数被卡太久，注意，本函数应该尽快返回，以免阻塞程序运行
    struct sockaddr    mysockaddr;    //远端服务器的socket地址
    socklen_t          socklen;       //数据类型socklen_t和int应该具有相同的长度
    int                err;
    int                level;
    int                s;
    static int         use_accept4 = 1;   //我们先认为能够使用accept4()函数
    lpngx_connection_t newc;              //代表连接池中的一个连接(注意这是指针)

    //ngx_log_stderr(0,"这是几个\n"); 这里会惊群，也就是说，epoll技术本身有惊群的问题

    socklen = sizeof(mysockaddr);

    do   //用do，跳到while后边去方便
    {
        if (use_accept4)
        {
            //因为listen套接字是非阻塞的，所以即便已完成连接队列为空，accept4()也不会
            //卡在这里；从内核获取一个用户端连接，最后一个参数SOCK_NONBLOCK表示返回一
            //个非阻塞的socket，节省一次ioctl(设置为非阻塞)调用
            s = accept4(pConnL->fd, &mysockaddr, &socklen, SOCK_NONBLOCK);
        }
        else
        {
            //因为listen套接字是非阻塞的，所以即便已完成连接队列为空，
            //accept()也不会卡在这里，非阻塞I/O函数，会立即返回一个结果；
            s = accept(pConnL->fd, &mysockaddr, &socklen);
        }

        //惊群，有时候不一定完全惊动所有4个worker进程，可能只惊动其中2个等等，其中一个
        //成功其余的accept4()都会返回-1；错误(11: Resource temporarily unavailable) 
        //所以参考资料：https://blog.csdn.net/russell_tao/article/details/7204260
        //其实，在linux2.6内核上，accept系统调用已经不存在惊群了(至少我在2.6.18内核版
        //本上已经不存在)。大家可以写个简单的程序试下，在父进程中bind，listen，然后fork
        //出子进程，所有的子进程都accept这个监听句柄。这样，当新连接过来时，大家会发现，
        //仅有一个子进程返回新建的连接，其他子进程继续休眠在accept调用上，没有被唤醒。

        //ngx_log_stderr(0,"测试惊群问题，看惊动几个worker进程%d\n",s); 
        //我的结论是：accept4可以认为基本解决惊群问题，但似乎并没有完全解决，有时候还
        //会惊动其他的worker进程
    
        if(s == -1)
        {
            ngx_log_stderr(
                0, "thundering herd test: In CSocket::ngx_event_accept, "
                "func accept failed(pid = %d)!", ngx_pid); 
        }
        else
        {
            ngx_log_stderr(
                0, "thundering herd test: In CSocket::ngx_event_accept, "
                "func accept succeeded(pid = %d)!", ngx_pid);
        }      

        if (s == -1)
        {
            err = errno;

            //对accept、send和recv而言，事件未发生时errno通常被设置成EAGAIN(再来一次)
            //或者EWOULDBLOCK(期待阻塞)
            if (err == EAGAIN || errno == EWOULDBLOCK) //accept()没准备好
            {
                //除非用一个循环不断的accept()取走所有的连接，不然一般不会有这个错误
                //我们这里只取一个连接，也就是accept一次
                return;
            }
            level = NGX_LOG_ALERT;

            //ECONNABORTED错误发生在对方意外关闭套接字之后，即主机中的软件放弃了一个
            //已建立的连接；由于超时或者其它失败而中止接连，例如用户插拔网线就可能出现
            //这个错误
            if (err == ECONNABORTED)
            {
                //该错误被描述为"software caused connection abort"。原因在于当服务
                //和客户进程在完成用于TCP连接的三次握手后，客户TCP却发送了一个RST复位，
                //在服务进程看来，就在该连接已由TCP排队(已完成连接队列)，等着服务进程
                //调用accept的时候RST却到达了。POSIX规定此时的errno值必须ECONNABORTED。
                //源自Berkeley的实现完全在内核中处理中止的连接，服务进程将永远不知道该
                //中止的发生。服务器进程一般可以忽略该错误，直接再次调用accept。
                level = NGX_LOG_ERR;
            }

            //EMFILE：进程的fd已用尽(已达到系统所允许单一进程所能打开的文件/套接字总数)
            //可参考：https://blog.csdn.net/sdn_prc/article/details/28661661 以及 
            //https://bbs.csdn.net/topics/390592927
            //ulimit -n，看看文件描述符限制，如果是1024的话，需要改大；打开的文件句柄数
            //过多，把系统的fd软限制和硬限制都抬高。
            //ENFILE这个errno的存在，表明一定存在system-wide resource limits，而不仅
            //仅有process-specific resource limits。按照常识，process-specific 
            //resource limits，一定受限于system-wide resource limits。
            else if (err == EMFILE || err == ENFILE)
            {
                level = NGX_LOG_CRIT;
            }

            if (use_accept4)
            {
                ngx_log_error_core(level, errno,
                    "In CSocket::ngx_event_accept, func accept4 failed!");
            }

            if (use_accept4 && err == ENOSYS) //accept4函数没实现
            {
                use_accept4 = 0;  //标记不使用accept4函数，改用accept函数
                continue;         //回去重新用accept函数
            }

            if (err == ECONNABORTED)  //对方关闭套接字
            {
                //这个错误因为可以忽略，所以不用干啥
            }

            if (err == EMFILE || err == ENFILE)
            {
                //这个官方做法是先把读事件从listen socket上移除，然后再弄个定时器，
                //定时器到了则继续执行该函数，但是定时器到了有个标记，会把读事件增加
                //到listen socket上去；
            }

            if (!use_accept4)
            {
                ngx_log_error_core(level, errno,
                    "In CSocket::ngx_event_accept, func accept failed!");
            }

            return;
        }
        //走到这里的，表示accept4/accept成功了 

        ////用户连接数过多，要关闭该用户socket，因为现在也没分配连接，所以直接关闭即可
        //if (onlineUserCount >= m_worker_connections)
        //{
        //    ngx_log_stderr(0,
        //        "exceeded the maximum number(%d) of connections allowed by the system, "
        //        "connection request failed for socket(%d)!", m_worker_connections, s);
        //    if (close(s) == -1)
        //    {
        //        ngx_log_error_core(NGX_LOG_ALERT, errno,
        //            "In CSocekt::ngx_event_accept, close(%d) failed!", s);
        //    }
        //    return;
        //}

        //申请连接之前，首先进行连接对象池和客户连入控制
        if (ConnListProtection()) 
        {
            if (close(s) == -1)
            {
                ngx_log_error_core(NGX_LOG_ALERT, errno,
                    "In CSocekt::ngx_event_accept, close(%d) failed!", s);
            }
            return;
        }

        ngx_log_stderr(0, "accept(%d) succeeded!", s);  //s这里就是一个句柄了
        newc = ngx_get_connection(s); //这是针对新连入用户的连接，和监听套接字所对应
                                      //的连接是两个不同的socket
        if (newc == NULL)
        {
            //连接池中连接不够用，那么就得把这个socekt直接关闭并返回了，因为在
            //ngx_get_connection()中已经写日志了，所以这里不需要写日志了
            if (close(s) == -1)
            {
                ngx_log_error_core(NGX_LOG_ALERT, errno, 
                    "In CSocekt::ngx_event_accept, close(%d) failed!", s);
            }
            ngx_log_stderr(0,
                "there is no free connection in m_freeconnectionList, "
                "connection request failed for socket(%d)!", s);
            return;
        }

        //成功的拿到了连接池中的一个连接
        memcpy(&newc->s_sockaddr, &mysockaddr, socklen); //拷贝客户端地址到连接对象
        //将收到的地址弄成字符串，格式形如"192.168.1.126:40904"或者"192.168.1.126"      
        ngx_sock_ntop(&newc->s_sockaddr, 1, (u_char*)newc->addr_text, 100);
        ngx_log_stderr(0, "Established connection with ip:%s", newc->addr_text);

        if (!use_accept4)
        {
            //如果不是用accept4函数取得的socket，就要设置为非阻塞，accept4函数已在其
            //参数中直接设置为非阻塞
            if (setnonblocking(s) == false)
            {
                //设置非阻塞失败，就要回收连接池中的连接并关闭socket，可以立即回收这个
                //连接，无需延迟，因为其上还没有数据收发，谈不到业务逻辑因此无需延迟
                ngx_close_connection(newc); 
                return; 
            }
        }

        newc->listening = pConnL->listening; //连接对象和监听对象关联，方便通过连接对          

        //设置数据来时的读处理函数
        newc->rhandler = &CSocket::ngx_read_request_handler;
        //设置数据发送时的写处理函数
        newc->whandler = &CSocket::ngx_write_request_handler;

        //客户端应该主动发送第一次的数据，这里将读事件加入epoll监控                                                      
        if (ngx_epoll_oper_event(
            s,                    //socekt句柄
            EPOLL_CTL_ADD,        //事件类型，这里是增加
            EPOLLIN | EPOLLRDHUP, //增加标志，EPOLLIN(可读)，EPOLLRDHUP(TCP连接的远端关闭或半关闭)
            0,                    //对于事件类型为增加的，不需要这个参数
            newc                  //连接池中的连接
        ) == -1)
        {
            //增加事件失败，失败日志在ngx_epoll_add_event中写过了，因此这里不多写
            ngx_close_connection(newc); //回收连接池中的连接，并关闭socket
            return; //直接返回
        }

        /*
        else
        {
            //打印下发送缓冲区大小
            int           n;
            socklen_t     len;
            len = sizeof(int);
            getsockopt(s,SOL_SOCKET,SO_SNDBUF, &n, &len);
            ngx_log_stderr(0,"发送缓冲区的大小为%d!",n); //87040

            n = 0;
            getsockopt(s,SOL_SOCKET,SO_RCVBUF, &n, &len);
            ngx_log_stderr(0,"接收缓冲区的大小为%d!",n); //374400

            int sendbuf = 2048;
            if (setsockopt(s, SOL_SOCKET, SO_SNDBUF,(const void *) &sendbuf,n) == 0)
            {
                ngx_log_stderr(0,"发送缓冲区大小成功设置为%d!",sendbuf);
            }

             getsockopt(s,SOL_SOCKET,SO_SNDBUF, &n, &len);
            ngx_log_stderr(0,"发送缓冲区的大小为%d!",n); //87040
        }
        */

        if (m_ifkickTimeCount == 1) //若开启心跳包检测，需要创建心跳包ping-pong定时器
        {
            /*newc->timerEntryPing = timeWheel.CreateTimer(
                PingTimeout, newc, m_iWaitTime, 0);*/
            timeWheel.StartTimer(newc->timerEntryPing);
        }

        ++onlineUserCount;  //连入用户数量+1 
        break;  //一般就是循环一次就跳出去

    } while (1);

    return;
}
