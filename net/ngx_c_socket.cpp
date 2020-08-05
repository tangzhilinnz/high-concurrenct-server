//和网络 有关的函数放这里
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
#include <sys/ioctl.h> //ioctl
#include <arpa/inet.h>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
//#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"


int CSocket::recvLOCK = 0;
int CSocket::sendLOCK = 0;
int CSocket::connLOCK = 0;

ATOMIC_QUEUE  CSocket::recvMsgQueue;  //must be atomic locked when handling
ATOMIC_QUEUE  CSocket::sendMsgQueue;  //must be atomic locked when handling
ATOMIC_QUEUE  CSocket::sendingQueue;  //no need for atomic lock while only 
                                      //handled by send thread
ATOMIC_QUEUE2 CSocket::recyConnQueue; //must be atomic locked when handling

sem_t CSocket::semRecyConnQueue;  //the semaphore for handling recyConnQueue

//构造函数
CSocket::CSocket()
{
    //配置相关
    m_worker_connections = 1;      //epoll连接最大项数
    m_ListenPortCount = 2;         //默认监听2个端口
    m_RecyConnectionWaitTime = 60; //等待这么些秒后才回收连接

    //epoll相关
    m_epollhandle = -1;            //epoll返回的句柄

    //一些和网络通讯有关的常用变量值，供后续频繁使用时提高效率
    lenPkgHeader = sizeof(COMM_PKG_HEADER);  //包头的sizeof值(占用的字节数)
    lenMsgHeader = sizeof(STRUC_MSG_HEADER); //消息头的sizeof值(占用的字节数)
    //lenListHook = sizeof(LIST_HOOK); //发消息送队列和接受消息队列使用的钩子长度

    //各种队列相关
    m_iRecvMsgQueueCount = 0;     //收消息队列大小
    m_iSendMsgQueueCount = 0;     //发消息队列大小

    //=============================================================================================
    recvMsgQueue.head = NULL;
    recvMsgQueue.tail = NULL;
    recvMsgQueue.size = 0;

    sendMsgQueue.head = NULL;
    sendMsgQueue.tail = NULL;
    sendMsgQueue.size = 0;
    
    sendingQueue.head = NULL;
    sendingQueue.tail = NULL;
    sendingQueue.size = 0;

    recyConnQueue.head2 = NULL;
    recyConnQueue.tail2 = NULL;
    recyConnQueue.size2 = 0;

    //freeConnList.head2 = NULL;
    //freeConnList.tail2 = NULL;
    //freeConnList.size2 = 0;
    //=============================================================================================

    m_total_connection_n = 0;     //连接池总连接数
    m_free_connection_n = 0;      //连接池空闲连接数
    //m_recycling_connection_n = 0; //待释放连接队列大小

    return;
}

//释放函数
CSocket::~CSocket()
{
    //释放必须的内存
    //(1)监听端口相关内存的释放
    ngx_close_listening_sockets();

    Shutdown_subproc();

    ngx_log_stderr(0, "~CSocket() executed, "
        "global object g_socket's super class part was detroyed!");

    return;
}

//各种清理函数----------------------------------------------------------------------------
//清理TCP接收消息队列
//=============================================================================================
void CSocket::clearMsgRecvQueue()
{
    //char* sTmpMempoint;
    ////CMemory* p_memory = CMemory::GetInstance();
    ////临界与否，将来有线程池再考虑临界问题
    //while (!m_MsgRecvQueue.empty())
    //{
    //    sTmpMempoint = m_MsgRecvQueue.front();
    //    m_MsgRecvQueue.pop_front();
    //    /*p_memory->FreeMemory(sTmpMempoint);*/
    //    free(sTmpMempoint);
    //}

    LPSTRUC_MSG_HEADER msg = NULL;
    while (recvMsgQueue.head) //直接依次释放接收队列中的所有消息
    {
        msg = recvMsgQueue.head;
        recvMsgQueue.head = recvMsgQueue.head->nextMsg;
        free(msg);
    }
    recvMsgQueue.tail = NULL;
    recvMsgQueue.size = 0;
}
//=============================================================================================
//清理TCP发送消息队列
void CSocket::clearMsgSendQueue()
{
    //char* sTmpMempoint;
    ////CMemory* p_memory = CMemory::GetInstance();
    //while (!m_MsgSendQueue.empty())
    //{
    //    sTmpMempoint = m_MsgSendQueue.front();
    //    m_MsgSendQueue.pop_front();
    //    /*p_memory->FreeMemory(sTmpMempoint);*/
    //    free(sTmpMempoint);
    //}

    LPSTRUC_MSG_HEADER msg = NULL;
    while (sendMsgQueue.head) //直接依次释放发送队列中的所有消息
    {
        msg = sendMsgQueue.head;
        sendMsgQueue.head = sendMsgQueue.head->nextMsg;
        free(msg);
    }
    sendMsgQueue.tail = NULL;
    sendMsgQueue.size = 0;

    while (sendingQueue.head) //直接依次释放正在发送队列中的所有消息
    {
        msg = sendingQueue.head;
        sendingQueue.head = sendingQueue.head->nextMsg;
        free(msg);
    }
    sendingQueue.tail = NULL;
    sendingQueue.size = 0;
}
//=============================================================================================

//初始化函数, 在主进程中调用(即fork()子进程之前)
//成功返回true，失败返回false
bool 
CSocket::Initialize()
{
    ReadConf();  //读配置项
    bool reco = ngx_open_listening_sockets(); //打开监听端口
    return reco;
}

//worker子进程中需要执行的初始化函数
bool 
CSocket::Initialize_subproc()
{
    //=============================================================================================
    int err;
    //收息互斥量初始化
    err = pthread_mutex_init(&m_recvMessageQueueMutex, NULL);
    if (err != 0)
    {
        ngx_log_stderr(err, "In CSocket::Initialize_subproc(), "
            "pthread_mutex_init(&m_recvMessageQueueMutex) failed!");
        return false;
    }
    //=============================================================================================
    //发消息互斥量初始化
    err = pthread_mutex_init(&m_sendMessageQueueMutex, NULL);
    if (err != 0)
    {
        ngx_log_stderr(err, "In CSocket::Initialize_subproc(), "
            "pthread_mutex_init(&m_sendMessageQueueMutex) failed!");
        return false;
    }
    //=============================================================================================
    //空闲连接相关互斥量初始化
    err = pthread_mutex_init(&freeConnListMutex, NULL);
    if (err != 0)
    {
        ngx_log_stderr(err, "In CSocket::Initialize_subproc(), "
            "pthread_mutex_init(&freeConnListMutex) failed!");
        return false;
    }
    //=============================================================================================
    //连接回收队列相关互斥量初始化
    //err = pthread_mutex_init(&recyConnMutex, NULL);
    //if (err != 0)
    //{
    //    ngx_log_stderr(err, "In CSocket::Initialize_subproc(), "
    //        "pthread_mutex_init(&recyConnMutex) failed!");
    //    return false;
    //}
    //=============================================================================================

    //初始化发消息相关信号量，信号量用于进程/线程之间的同步，虽然互斥量
    //[pthread_mutex_lock]和条件变量[pthread_cond_wait]都是线程之间的同步手段，
    //但这里用信号量实现则更容易理解，更容易简化问题，使用书写的代码短小且清晰；
    //第二个参数=0，表示信号量在线程之间共享，如果非0，表示在进程之间共享
    //第三个参数=0，表示信号量的初始值，为0时，调用sem_wait()就会卡在那里卡着
    /*if (sem_init(&m_semEventSendQueue, 0, 0) == -1)
    {
        ngx_log_stderr(errno, "In CSocket::Initialize_subproc(), "
            "sem_init(&m_semEventSendQueue) failed!");
        return false;
    }*/

    if (sem_init(&semRecyConnQueue, 0, 0) == -1)
    {
        ngx_log_stderr(errno, "In CSocket::Initialize_subproc(), "
            "sem_init(&m_semEventSendQueue) failed!");
        return false;

    }
    //=============================================================================================

    //创建线程----------------------------------------------------------------------------
    ThreadItem* pSendQueue;     //专门用来发送数据的线程
    m_threadVector.push_back(pSendQueue = new ThreadItem(this)); 
    err = pthread_create(&pSendQueue->_Handle, NULL, 
        ServerSendQueueThread, pSendQueue); 
    if(err != 0)
    {
        ngx_log_stderr(err, "In CSocket::Initialize_subproc(), "
            "func pthread_create_1 failed!");
        return false;
    }

    ThreadItem* pRecyConn;      //专门用来回收连接的线程
    m_threadVector.push_back(pRecyConn = new ThreadItem(this));
    err = pthread_create(&pRecyConn->_Handle, NULL,
        ServerRecyConnThread, pRecyConn);
    if (err != 0)
    {
        ngx_log_stderr(err, "In CSocket::Initialize_subproc(), "
            "func pthread_create_2 failed!");
        return false;
    }

    return true;
}

//关闭退出函数[需要在worker子进程中执行]
void
CSocket::Shutdown_subproc()
{
    //(1)把干活线程停止掉，注意系统应该尝试通过设置g_stopEvent = 1来启动整个项目停止
    //用到信号量sem_post的，可能还需要调用一下sem_post
    g_stopEvent = 1;
    //if (sem_post(&m_semEventSendQueue) == -1)  //让ServerSendQueueThread线程运行
    //{
    //    ngx_log_stderr(0, "In CSocekt::Shutdown_subproc, "
    //        "func sem_post(&m_semEventSendQueue) failed!");
    //}

    if (sem_post(&semRecyConnQueue) == -1)  //让ServerRecyConnThread线程运行
    {
        ngx_log_stderr(0, "In CSocekt::Shutdown_subproc, "
            "func sem_post(&semRecyConnQueue) failed!");
    }

    std::vector<ThreadItem*>::iterator iter;
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        pthread_join((*iter)->_Handle, NULL); //等待一个线程终止
    }

    //(2)释放一下new出来的ThreadItem[辅助线程池中的ThreadItem对象]   
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        if (*iter)
            delete* iter;
    }

    m_threadVector.clear();

    //(3)队列相关
    clearMsgSendQueue();
    clearMsgRecvQueue();
    clearconnection();

    //=============================================================================================
    //(4)多线程相关
    int err;
    err = pthread_mutex_destroy(&m_recvMessageQueueMutex); //收消息互斥量释放
    if (err != 0)
    {
        ngx_log_stderr(err, "In CSocket::Shutdown_subproc(), "
            "pthread_mutex_destroy(&m_recvMessageQueueMutex) failed!");
    }
    //=============================================================================================
    err = pthread_mutex_destroy(&m_sendMessageQueueMutex); //发消息互斥量释放
    if (err != 0)
    {
        ngx_log_stderr(err, "In CSocket::Shutdown_subproc(), "
            "pthread_mutex_destroy(&m_sendMessageQueueMutex) failed!");
    }
    //=============================================================================================
    err = pthread_mutex_destroy(&freeConnListMutex);       //连接相关互斥量释放
    if (err != 0)
    {
        ngx_log_stderr(err, "In CSocket::Shutdown_subproc(), "
            "pthread_mutex_destroy(&freeConnListMutex) failed!");
    }
    //err = pthread_mutex_destroy(&recyConnMutex);    //连接回收队列相关的互斥量释放
    //if (err != 0)
    //{
    //    ngx_log_stderr(err, "In CSocket::Shutdown_subproc(), "
    //        "pthread_mutex_destroy(&recyConnMutex) failed!");
    //}
    //=============================================================================================
    //if (sem_destroy(&m_semEventSendQueue) == -1)            //发消息相关线程信号量释放
    //{
    //    ngx_log_stderr(errno, "In CSocket::Shutdown_subproc(), "
    //        "sem_destroy(&m_semEventSendQueue) failed!");
    //}
    if (sem_destroy(&semRecyConnQueue) == -1)
    {
        ngx_log_stderr(errno, "In CSocket::Shutdown_subproc(), "
            "sem_destroy(&semRecyConnQueue) failed!");
    }
    //=============================================================================================
}

//专门用于读各种配置项
void 
CSocket::ReadConf()
{
    CConfig* p_config = CConfig::GetInstance();
    m_worker_connections = p_config->GetIntDefault("worker_connections", 
        m_worker_connections);                  //epoll连接的最大项数
    m_ListenPortCount = p_config->GetIntDefault("ListenPortCount", 
        m_ListenPortCount);                     //取得要监听的端口数量
    m_RecyConnectionWaitTime = p_config->GetIntDefault(
        "Sock_RecyConnectionWaitTime", 
        m_RecyConnectionWaitTime);              //回收连接等待时间

    //是否开启踢人时钟，1：开启   0：不开启
    m_ifkickTimeCount = p_config->GetIntDefault("Sock_WaitTimeEnable", 0);
    //多少秒检测一次是否 心跳超时，只有当Sock_WaitTimeEnable = 1时，本项才有用
    m_iWaitTime = p_config->GetIntDefault("Sock_MaxWaitTime", m_iWaitTime);                         	
    m_iWaitTime = (m_iWaitTime > 5000) ? m_iWaitTime : 5000;

    return;
}

//监听端口[支持多个端口]，这里遵从nginx的函数命名
bool 
CSocket::ngx_open_listening_sockets()
{
    int                isock;         //socket
    struct sockaddr_in serv_addr;     //服务器的地址结构体(系统定义)
    int                iport;         //端口
    char               strinfo[100];  //临时字符串 

    memset(&serv_addr, 0, sizeof(serv_addr)); //先初始化
    serv_addr.sin_family = AF_INET;           //选择协议族为IPV4
    //监听本地所有的IP地址，INADDR_ANY表示的是一个服务器上所有的网卡(服务器可能不止一
    //个网卡，一个网卡可以设置多个IP地址)多个本地ip地址都进行绑定端口号，进行侦听。
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    //中途用到一些配置信息
    CConfig* p_config = CConfig::GetInstance();

    for (int i = 0; i < m_ListenPortCount; i++)
    {
        //系统函数socket，成功返回非负描述符，出错返回-1
        //参数1：AF_INET：使用ipv4协议，一般就这么写
        //参数2：SOCK_STREAM：使用TCP，表示可靠连接，
        //       还有一个UDP套接字，表示不可靠连接
        //参数3：给0，固定用法
        isock = socket(AF_INET, SOCK_STREAM, 0);
        if (isock == -1)
        {
            ngx_log_stderr(errno, "In CSocket::ngx_open_listening_sockets, "
                "func socket(i=%d) failed.", i);
            //ngx_close_listening_sockets(); //~CSocket函数中调用这个函数
            return false; //false表示主程序也将退出
        }

        //setsockopt（）:设置一些套接字参数选项；
        //参数2：表示级别，和参数3配套使用，即参数3如果确定了，参数2就确定了;
        //参数3：允许重用本地地址
        //设置SO_REUSEADDR，主要是解决TIME_WAIT这个状态导致bind()失败的问题
        int reuseaddr = 1;  //1:打开对应的设置项
        if (setsockopt(isock, SOL_SOCKET, SO_REUSEADDR,
            (const void*)&reuseaddr, sizeof(reuseaddr)) == -1)
        {
            ngx_log_stderr(errno, "In CSocket::ngx_open_listening_sockets, "
                "func setsockopt(i=%d) failed.", i);
            //ngx_close_listening_sockets(); //~CSocket函数中调用这个函数
            close(isock); //关闭这个未成功建立的监听isock
            return false; //false表示主程序也将退出
        }

        //设置该socket为非阻塞
        if (setnonblocking(isock) == false)
        {
            ngx_log_stderr(errno, "In CSocket::ngx_open_listening_sockets, "
                "func setnonblocking(i=%d) falied.", i);
            //ngx_close_listening_sockets(); //~CSocket函数中调用这个函数
            close(isock); //关闭这个未成功建立的监听isock
            return false; //false表示主程序也将退出
        }

        //设置本服务器要监听的地址和端口，这样客户端才能连接到该地址和端口并发送数据        
        strinfo[0] = 0;
        sprintf(strinfo, "ListenPort%d", i);
        iport = p_config->GetIntDefault(strinfo, 10000); //给默认值10000
        //htons从主机字节顺序转变成网络字节顺序，in_port_t就是uint16_t，这里是将
        //int(32bits)类型转换成unsigned short int(16bits)类型
        serv_addr.sin_port = htons((in_port_t)iport); 
                            
        //绑定服务器地址结构体
        if (bind(isock, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) == -1)
        {
            ngx_log_stderr(errno, "In CSocket::ngx_open_listening_sockets, "
                "func bind(i=%d) failed.", i);
            //ngx_close_listening_sockets(); //~CSocket函数中调用这个函数
            close(isock); //关闭这个未成功建立的监听isock
            return false; //false表示主程序也将退出
        }

        //开始监听
        if (listen(isock, NGX_LISTEN_BACKLOG) == -1)
        {
            ngx_log_stderr(errno, "In CSocket::ngx_open_listening_sockets, "
                "func listen(i=%d) failed.", i);
            //ngx_close_listening_sockets(); //~CSocket函数中调用这个函数
            close(isock); //关闭这个未成功建立的监听isock
            return false; //false表示主程序也将退出
        }

        //层层筛选，能走下来的isock已经被监听，就可以放到列表里
        lpngx_listening_t p_listensocketitem = new ngx_listening_t;
        memset(p_listensocketitem, 0, sizeof(ngx_listening_t));
        p_listensocketitem->port = iport;  //记录下所监听的端口号
        p_listensocketitem->fd = isock;    //套接字句柄保存下来   
        ngx_log_error_core(NGX_LOG_INFO, 0, 
            "Listening port%d succeeded!", iport); 
        m_ListenSocketList.push_back(p_listensocketitem); //加入到队列中
    }

    if (m_ListenSocketList.size() <= 0)  //不可能一个端口都不监听吧
    {
        return false;
    }

    return true;
} 

//设置socket连接为非阻塞模式【这种函数的写法很固定】：
//非阻塞，不断调用，检测返回值，但拷贝数据的时候是阻塞的
bool 
CSocket::setnonblocking(int sockfd)
{
    // 方法1：
    int nb = 1; //0：清除，1：设置  
    if (ioctl(sockfd, FIONBIO, &nb) == -1) //FIONBIO：设置/清除非阻塞I/O标记，
                                           //0：清除，1：设置
    {
        return false;
    }

    return true;

    // 方法2：稍复杂
    /*
    //fcntl：file control【文件控制】相关函数，执行各种描述符控制操作
    //参数1：所要设置的描述符，这里是套接字【也是描述符的一种】
    int opts = fcntl(sockfd, F_GETFL); //用F_GETFL先获取描述符的一些标志信息
    if(opts < 0)
    {
        ngx_log_stderr(errno,
            "First fcntl in CSocket::setnonblocking() failed.");
        return false;
    }
    opts |= O_NONBLOCK; //把非阻塞标记加到原来的标记上，标记这是个非阻塞套接字
                        //关闭非阻塞：opts &= ~O_NONBLOCK，然后再F_SETFL即可
    if(fcntl(sockfd, F_SETFL, opts) < 0)
    {
        ngx_log_stderr(errno,
            "Second fcntl in CSocket::setnonblocking() failed.");
        return false;
    }
    return true;*/
}

//关闭所有监听套接字队列中的socket，并释放与之绑定的ngx_listening_s对象
void 
CSocket::ngx_close_listening_sockets()
{
    std::vector<lpngx_listening_t>::iterator pos;
    for (pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos)
    {
        //Can not call close twice for one opened fd,
        //The first call should return 0; 
        //the second call should return -1, and set errno to EBADF
        if (close((*pos)->fd) == -1)
        {
            ngx_log_error_core(NGX_LOG_ALERT, errno,
                "In CSocket::ngx_close_listening_sockets(), "
                "func close(%d) failed!", 
                (*pos)->fd);
        }

        ngx_log_error_core(NGX_LOG_INFO, 0,
            "Listening port%d closed!", (*pos)->port); //显示一些信息到日志中

        delete (*pos); //一定要把指针指向的内存干掉，不然内存泄漏
    }

    m_ListenSocketList.clear();

    return;
}

//=============================================================================================
//将一个待发送消息入到发消息队列中，处理收消息线程池中的每个线程会调用此函数
void 
CSocket::msgSend(char* psendbuf)
{
    //CLock lock(&m_sendMessageQueueMutex);
    //m_MsgSendQueue.push_back(psendbuf);
    //++m_iSendMsgQueueCount;   //原子操作
    ////将信号量的值+1，这样其他卡在sem_wait的就可以走下去；sem_post是给信号量的值加上
    ////一个1，它是一个原子操作，即同时对同一个信号量做加1操作的两个线程是不会冲突的
    //if (sem_post(&m_semEventSendQueue) == -1)                                            
    //{
    //    ngx_log_stderr(0, "In CSocket::msgSend, "
    //        "func sem_post(&m_semEventSendQueue) failed.");
    //}
    if (psendbuf == NULL) return;

    LPSTRUC_MSG_HEADER msg = reinterpret_cast<LPSTRUC_MSG_HEADER>(psendbuf);
    msg->preMsg = NULL;
    msg->nextMsg = NULL;
    lpngx_connection_t p_Conn = msg->pConn;

    //发送消息队列过大也可能给服务器带来风险
    //if (sendMsgQueue.size + sendingQueue.size > 100000)
    //{
    //    //发送队列过大，比如客户端恶意不接受数据，就会导致这个队列越来越大
    //    //那么考虑为了服务器安全，不处理一些数据的发送，等处理线程直接干掉，
    //    //虽然有可能导致客户端出现问题，但总比服务器不稳定要好很多
    //    return;
    //}

    //if (p_Conn->sendCount > 1000)
    //{
    //    //这个连接存在只发不收的异常，导致服务器该连接的发送缓冲区已经溢出，
    //    //需要关闭该异常连接
    //    //=========================================================================================
    //    ngx_log_stderr(0, "In CSocekt::msgSend, [tid = %d]"
    //        "one connection sendCount(%d) exceeded 1000, "
    //        "the server will close the connection!",
    //        pthread_self() ,p_Conn->sendCount);
    //    ngx_recycle_connection(p_Conn);
    //    //=========================================================================================
    //    free(psendbuf);
    //    return;
    //}

    //__sync_fetch_and_add(&p_Conn->sendCount, 1);

    while (__sync_lock_test_and_set(&sendLOCK, 1))
    { usleep(0);}

    ++sendMsgQueue.size;
    ++p_Conn->sendCount;

    if (sendMsgQueue.tail == NULL)
    {
        sendMsgQueue.head = msg;
        sendMsgQueue.tail = msg;
    }
    else
    {
        (sendMsgQueue.tail)->nextMsg = msg;
        msg->preMsg = sendMsgQueue.tail;
        sendMsgQueue.tail = msg;
    }

    __sync_lock_release(&sendLOCK);

    return;
}
//=============================================================================================

//---------------------------------------------------------------------------------------
//(1)epoll功能初始化，子进程中进行，本函数被ngx_worker_process_init()所调用
int 
CSocket::ngx_epoll_init()
{
    //(1)很多内核版本不处理epoll_create的参数，只要该参数>0即可
    //创建一个epoll对象，创建了一个红黑树，还创建了一个双向链表
    m_epollhandle = epoll_create(m_worker_connections); //直接以epoll连接的最大项
                                                        //数为参数，肯定是>0的； 
    if (m_epollhandle == -1)
    {
        ngx_log_stderr(errno,
            "In CSocket::ngx_epoll_init(), func epoll_create failed!");
        exit(2); //这是致命问题，直接退，资源由系统释放，这里不刻意释放，比较麻烦
    }

    //(2)创建连接池，后续用于处理所有客户端的连接
    initconnection();

    //(3)遍历所有 监听socket，为每个 监听socket 增加一个连接池中的连接
    //(即让每个 监听socket 和一段内存绑定，以方便记录该sokcet相关数据、状态等)
    std::vector<lpngx_listening_t>::iterator pos;
    for (pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos)
    {
        //从连接池中获取一个空闲连接对象
        lpngx_connection_t p_Conn = ngx_get_connection((*pos)->fd);
        p_Conn->listening = (*pos); //连接对象和监听对象关联，通过连接对象找监听对象
        (*pos)->p_connection = p_Conn; //监听对象和连接对象关联，通过监听对象找连接对象

        //对监听端口的读事件设置处理方法，因为监听端口是用来等对方连接时发送三路握手的，
        //所以监听端口关心的就是读事件
        p_Conn->rhandler = &CSocket::ngx_event_accept;

        //往监听socket上增加监听事件，从而开始让监听端口履行其职责，如果不加这行，
        //虽然端口能连上，但不会触发ngx_epoll_process_events()里边的epoll_wait()
        //往下走
        if (ngx_epoll_oper_event(
            (*pos)->fd,           //socekt句柄
            EPOLL_CTL_ADD,        //事件类型，这里是增加
            EPOLLIN | EPOLLRDHUP, //增加标志，EPOLLIN(可读)，EPOLLRDHUP(TCP连接的远端关闭或半关闭)
            0,                    //对于事件类型为增加的，不需要这个参数
            p_Conn                //连接池中的连接 
        ) == -1)
        {
            exit(2); //有问题，直接退出，资源由系统释放，日志已经写过了
        }
    }

    return 1;
}


//(2)对epoll事件的具体操作
//返回值：成功返回1，失败返回-1；
int 
CSocket::ngx_epoll_oper_event(
    int                fd,        //句柄，一个socket
    uint32_t           eventtype, //EPOLL_CTL_ADD，EPOLL_CTL_MOD，EPOLL_CTL_DEL
    uint32_t           flag,      //标志，具体含义取决于eventtype
    int                bcaction,  //对于事件类型为EPOLL_CTL_MOD，需要这个参数，0(增加)1(去掉)2(覆盖)
    lpngx_connection_t pConn      //pConn：一个连接指针，EPOLL_CTL_ADD时增加到红黑树
                                  //中去，将来epoll_wait时能取出来用
)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    if (eventtype == EPOLL_CTL_ADD) //往红黑树中增加节点；
    {
        //红黑树从无到有增加节点，不管原来是啥标记，直接覆盖
        ev.events = flag;      
        pConn->events = flag;  //这个连接本身也记录这个标记
    }
    else if (eventtype == EPOLL_CTL_MOD)
    {
        //节点已经在红黑树中，修改节点的事件信息
        ev.events = pConn->events;  //先把标记恢复回来
        if (bcaction == 0)
        {
            //增加某个标记            
            ev.events |= flag;
        }
        else if (bcaction == 1)
        {
            //去掉某个标记
            ev.events &= ~flag;
        }
        else if (bcaction == 2)
        {
            //完全覆盖某个标记            
            ev.events = flag;  
        }

        pConn->events = ev.events; //记录该标记
    }
    else
    {
        //删除红黑树中节点，socket关闭时 该节点会自动从红黑树移除
        return 1;  //先直接返回1表示成功，，目前无需求，所以将来扩展
    }

    //系统函数epoll_ctl，在EPOLL_CTL_ADD或EPOLL_CTL_MOD模式均会用参数的epoll_event
    //对象覆盖红黑树节点上的epoll_event对象，因此ev.data.ptr在此需要重新赋值，否则为
    //空指针，系统会崩溃。查看内核源代码，发现
    /*
    SYSCALL_DEFINE4(epoll_ctl, int, epfd, int, op, int, fd,
            struct epoll_event __user*, event)
    {
        ... ... ... ... ... ...
        epi = ep_find(ep, tf.file, fd);
        error = -EINVAL;
        switch (op) {
        ... ... ... ... ... ...
        case EPOLL_CTL_MOD:
            if (epi) {
                if (!(epi->event.events & EPOLLEXCLUSIVE)) {
                    epds.events |= EPOLLERR | EPOLLHUP;
                    error = ep_modify(ep, epi, &epds);
                }
            } else
                error = -ENOENT;
            break;
        }
        ... ... ... ... ... ...
    }
    -------------------------------------------------------------------------------------
    static int ep_modify(struct eventpoll *ep, struct epitem *epi,
            const struct epoll_event* event)
    {
      ... ... ... ... ... ...
      epi->event.events = event->events; //need barrier below
      epi->event.data = event->data; //protected by mtx
      ... ... ... ... ... ...
    }*/

    //因为指针的最后一位(二进制位)肯定不是1，所以和pConn->instance做|运算；
    //到时候通过一些编码，既可以取得c的真实地址，又可以把此时此刻的
    //pConn->instance值取到。比如pConn是个地址，可能的值是 0x00af0578，
    //对应的二进制是‭101011110000010101111000‬，而|1后是0x00af0579
    ev.data.ptr = (void*)((uintptr_t)pConn | pConn->instance);

    if (epoll_ctl(m_epollhandle, eventtype, fd, &ev) == -1)
    {
        ngx_log_stderr(errno, "In CSocket::ngx_epoll_oper_event, "
            "func epoll_ctl(%d,%ud,%ud,%d) failed!", 
            fd, eventtype, flag, bcaction);
        return -1;
    }

    return 1;
}

//开始获取发生的事件消息
//参数unsigned int timer：epoll_wait()阻塞的时长，单位是毫秒；
//返回值1：正常返回，0：有问题返回，但不管是正常还是问题返回，都应该保持进程继续运行
//本函数被ngx_process_events_and_timers()调用，其在子进程的死循环中被反复调用
int 
CSocket::ngx_epoll_process_events(int timer)
{
    //等待事件，事件会返回到m_events里，最多返回NGX_MAX_EVENTS个事件；
    //如果两次调用epoll_wait的事件间隔比较长，则可能在epoll的双向链表中，积累了多个
    //事件，所以调用epoll_wait，可能取到多个事件
    //阻塞timer这么长时间除非：
    //a)阻塞时间到达
    //b)阻塞期间收到事件(比如新用户连入)会立刻返回
    //c)调用时有事件也会立刻返回
    //d)如果来个信号，比如你用kill -1 pid测试
    //如果timer为-1则一直阻塞，如果timer为0则立即返回，即便没有任何事件
    //返回值：有错误发生返回-1，错误在errno中，比如你发个信号过来，就返回-1，错误信息是
    //(4: Interrupted system call)
    //如果你等待的是一段时间，并且超时了timer，则返回0；
    //如果返回>0则表示成功捕获到这么多个事件(返回值里)
    //epoll_wait第三个参数maxevents的值不能大于大于epoll_create时的size
    int events = epoll_wait(m_epollhandle, m_events, NGX_MAX_EVENTS, timer);

    if (events == -1)
    {
        //有错误发生，发送某个信号给本进程就可以导致这个条件成立，而且错误码根据观察是4
        //#define EINTR 4，EINTR错误的产生：当阻塞于某个慢系统调用的一个进程捕获某个
        //信号且相应信号处理函数返回时，该系统调用可能返回一个EINTR错误。
        //例如：在socket服务器端，设置了信号捕获机制，有子进程，当在父进程阻塞于慢系统
        //调用时由父进程捕获到了一个有效信号时，内核会致使accept返回一个EINTR错误
        //(被中断的系统调用)。
        if (errno == EINTR)
        {
            //信号所致，直接返回，一般认为这不是毛病，但还是打印下日志记录一下，
            //因为一般也不会人为给worker进程发送消息
            ngx_log_error_core(NGX_LOG_INFO, errno,
                "In CSocket::ngx_epoll_process_events, func epoll_wait failed!");

            return 1;  //正常返回
        }
        else
        {
            //这被认为应该是有问题，记录日志
            ngx_log_error_core(NGX_LOG_ALERT, errno,
                "In CSocket::ngx_epoll_process_events, func epoll_wait failed!");

            return 0;  //非正常返回 
        }
    }

    if (events == 0) //超时，但没事件来
    {
        if (timer != -1)
        {
            //要求epoll_wait阻塞一定的时间而不是一直阻塞，属于阻塞到时间了，则正常返回
            return 1;
        }
        //无限等待，所以不存在超时，但却没返回任何事件，这应该不正常有问题        
        ngx_log_error_core(NGX_LOG_ALERT, 0,
            "In CSocket::ngx_epoll_process_events, func epoll_wait failed, "
            "no timeout and no event!");

        return 0; //非正常返回 
    }

    //会惊群，一个telnet上来，4个worker进程都会被惊动，都执行下边这个
    //ngx_log_stderr(errno, "惊群测试1:%d",events); 

    //走到这里，就是属于有事件收到了
    lpngx_connection_t p_Conn;
    uintptr_t          instance;
    uint32_t           revents;

    for (int i = 0; i < events; ++i) //注意events才是返回的实际事件数量
    {
        p_Conn = (lpngx_connection_t)(m_events[i].data.ptr);
        instance = (uintptr_t)p_Conn & 1; //将地址的最后一位取出来，用instance变量
                                          //标识，见ngx_epoll_oper_event，该值是当
                                          //时随着连接池中的连接一起给进来的，取得的
                                          //是你当时调用ngx_epoll_oper_event的时候，
                                          //这个连接里边的instance变量的值
        p_Conn = (lpngx_connection_t)((uintptr_t)p_Conn & (uintptr_t)~1);

        //过滤过期事件1
        if (p_Conn->fd == -1) //当关联一个连接池中的连接对象时，这个套接字值是要给到
                       //p_Conn->fd；ngx_close_connection或ngx_recycle_connection
                              //函数中会把文件p_Conn关闭，并且把p_Conn会被设置为-1
        {
            //假如用epoll_wait取得三个事件，处理第一个事件时，因为业务需要，我们把这个
            //连接关闭，p_Conn->fd会被设置为-1；第二个事件照常处理；假如第三个事件也跟
            //第一个事件对应的是同一个连接，那这个条件就成立；这属于过期事件，不处理
            ngx_log_error_core(NGX_LOG_DEBUG, 0, 
                "In CSocket::ngx_epoll_process_events, "
                "an expiration event was encountered, fd==-1:%p.", p_Conn);
            continue;
        }
        //过滤过期事件的2
        if (p_Conn->instance != instance)
        {
            //instance标志判断事件是否过期
            //a)处理第一个事件时，因为业务需要，我们把这个连接【假设套接字为50】关闭，
            //  同时设置c->fd = -1；并且调用ngx_close_connection将该连接归还给连接池；
            //b)处理第二个事件，恰好第二个事件是建立新连接事件，调用ngx_get_connection
            //  从连接池中取出的连接非常可能就是刚刚释放的第一个事件对应的连接池中的连接；
            //c)又因为a中套接字50被释放了，所以会被操作系统拿来复用，复用给了b)；
            //d)当处理第三个事件时，第三个事件其实已经过期的，不应该处理，此时连接池中的
            //该连接实际上已经被用作第二个事件对应的socket上了；依靠instance标志位能够
            //解决这个问题，当调用ngx_get_connection从连接池中获取一个新连接时，我们把
            //instance标志位置反。所以这个条件如果成立，说明这个连接已经关闭后又重启了
            ngx_log_error_core(NGX_LOG_DEBUG, 0, 
                "In CSocket::ngx_epoll_process_events, "
                "an expiration event was encountered, instance reset:%p.", p_Conn);

            continue;
        } //注意：过滤过期事件的2用于即关即用型连接池的过期事件判断，保留当学习

        //能走到这里，这些事件都没过期，就正常开始处理
        revents = m_events[i].events; //取出事件类型
        /*
        if (revents & (EPOLLERR | EPOLLHUP)) //例如对方close掉套接字，这里会感应到
                                       //换句话说：如果发生了错误或者客户端非正常断连
        {
            revents |= EPOLLIN | EPOLLOUT; //EPOLLIN：表示对应的链接上有数据可以读出，
                                           //TCP链接的远端主动关闭连接，也相当于可
                                           //读事件，因为本服务器要处理发送来的FIN包
                                           //EPOLLOUT：表示对应的连接上可以写入数据
                                           //发送写准备好            
        }*/
        if (revents & EPOLLIN) //如果是读事件
        {
            //一个客户端新连入，对端正常关闭，已连接的socket发送数据来，这个会成立           
            (this->*(p_Conn->rhandler))(p_Conn); //如果新连接进入，这里执行的应该是
                                                 //CSocket::ngx_event_accept           
                                  //如果是已经连入，发送数据到这里，则这里执行的应该是 
                                              //CSocket::ngx_read_request_handler
        }

        if (revents & EPOLLOUT) //如果是写事件
        {
            //客户端关闭，如果服务器端挂着一个写通知事件，则这里个条件是可能成立的
            if (revents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) 
            {
                //EPOLLERR：对应的连接发生错误 8(1000) 
                //EPOLLHUP：对应的连接被挂起 16(0001 0000)
                //EPOLLRDHUP：表示TCP连接的远端关闭或者半关闭连接 8192(0010 0000 0000 0000)
                //8221 = ‭0010 0000 0001 1101‬：包括EPOLLRDHUP，EPOLLHUP，EPOLLERR
                ngx_log_stderr(errno, "In CSocekt::ngx_epoll_process_events, "
                    "EPOLLOUT set up and (EPOLLERR|EPOLLHUP|EPOLLRDHUP) set up, "
                    "event=%Xd.", revents);
            }
            else
            {
                //如果有数据没有发送完，由系统驱动来发送，CSocekt::ngx_write_request_handler
                //其他情况则是，CSocekt::ngx_null_request_handler
                (this->*(p_Conn->whandler))(p_Conn);   
            }
        }
    }

    return 1;
}

void //从sendingQueue消息队列中弹出指定消息
CSocket::popSendingQueueMsg(LPSTRUC_MSG_HEADER msg) 
{
    //发送队列无消息或者指定消息为空
    if (sendingQueue.head == NULL || msg == NULL) return; 
    //发送队列只有一条消息
    if (sendingQueue.head == sendingQueue.tail)
    {
        sendingQueue.head = NULL;
        sendingQueue.tail = NULL;
        sendingQueue.size = 0;
        return;
    }

    //以下发送队列中已经至少有两条消息
    if (msg == sendingQueue.head) //当指定消息为第一条消息时
    {
        sendingQueue.head = sendingQueue.head->nextMsg;
        sendingQueue.head->preMsg = NULL;
        --sendingQueue.size;
    }
    else if (msg == sendingQueue.tail) //当指定消息为最后一条消息时
    {
        sendingQueue.tail = sendingQueue.tail->preMsg;
        sendingQueue.tail->nextMsg = NULL;
        --sendingQueue.size;
    }
    else //当指定消息为其他情况时
    {
        (msg->preMsg)->nextMsg = msg->nextMsg;
        (msg->nextMsg)->preMsg = msg->preMsg;
        --sendingQueue.size;
    }

    return;
}

//---------------------------------------------------------------------------------------
//处理发送消息队列的线程，被声明为静态函数
void* 
CSocket::ServerSendQueueThread(void* threadData)
{
    ThreadItem* pThread = static_cast<ThreadItem*>(threadData);
    CSocket* pSocketObj = pThread->_pThis;
    int err;
    char* pMsgBuf;
    LPSTRUC_MSG_HEADER	pMsgHeader;
    LPCOMM_PKG_HEADER   pPkgHeader;
    lpngx_connection_t  p_Conn;
    unsigned short      itmp;
    ssize_t             sendsize;
    //int                sem_value;
    int msgHeaderLength = pSocketObj->lenMsgHeader;
    int pkgHeaderLength = pSocketObj->lenPkgHeader;

    LPSTRUC_MSG_HEADER msg = NULL;
    LPSTRUC_MSG_HEADER msg1 = NULL;

    //=================================test variables====================================
    struct timespec tmv1, tmv2;
    double tm = 0;
    int count = 0;
    //=================================test variables====================================

    while (g_stopEvent == 0) //不退出
    {
        //如果信号量值>0，则减1并走下去，否则在这里卡着；
        //为了让信号量值+1，对于完整正确的消息，消息处理线程会最终在CSocekt::msgSend，
        //或 worker进程的主线程在CSocekt::ngx_write_request_handler 中调用sem_post
        //让信号量加1，就达到了让sem_wait走下去的目的；
        //如果被某个信号中断，sem_wait也可能过早的返回，错误为EINTR；
        //整个程序退出之前，也要sem_post一下，确保如果本线程卡在sem_wait，也能走
        //下去从而让本线程成功返回结束
        //if (sem_wait(&pSocketObj->m_semEventSendQueue) == -1)
        //{
        //    //sem_wait成功返回0，信号量的值会减去1；
        //    //错误的话信号量的值不改动，返回-1，errno设定来标识错误
        //    //error type: 
        //    //EINTR: The call was interrupted by a signal handler; see signal(7)
        //    //EINVAL: m_semEventSendQueue is not a valid semaphores           
        //    if (errno != EINTR) //EINTR不算错误
        //        ngx_log_stderr(errno, "In CSocekt::ServerSendQueueThread, "
        //            "func sem_wait failed!.");
        //}
        //
        //if (g_stopEvent != 0) //要求整个进程退出
        //    break;

        //一般走到这里都表示需要处理数据收发了
        //if (pSocketObj->m_iSendMsgQueueCount > 0) //原子的 
        //{
        //    //因为我们要操作发送消息对列m_MsgSendQueue，所以这里要临界
        //    err = pthread_mutex_lock(&pSocketObj->m_sendMessageQueueMutex);             
        //    if (err != 0)
        //    {
        //        ngx_log_stderr(err, "In CSocekt::ServerSendQueueThread, "
        //            "func pthread_mutex_lock failed, "
        //            "the returned errno is %d!", err);
        //        continue;
        //    }
        //    pos = pSocketObj->m_MsgSendQueue.begin();
        //    posend = pSocketObj->m_MsgSendQueue.end();
        //    while (pos != posend)
        //    {
        //        pMsgBuf = (*pos); //消息头+包头+包体，但是不发送消息头给客户端
        //        pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf; //指向消息头
        //        p_Conn = pMsgHeader->pConn;
        //        //包过期，ngx_recycle_connection回收连接到回收连接队列，iCurrsequence
        //        //会增加1；ngx_free_connection将这个连接从回收连接队列挪到空闲连接队列，
        //        //iCurrsequence加1；ngx_get_connection启用这个连接，iCurrsequence再
        //        //加1。只要下面条件成立，肯定是客户端连接已断，后续要发送的数据直接删除
        //        if (p_Conn->iCurrsequence != pMsgHeader->iCurrsequence)
        //        {
        //            //丢弃此消息，小心处理该消息的删除
        //            pos2 = pos;
        //            pos++;
        //            pSocketObj->m_MsgSendQueue.erase(pos2);
        //            --pSocketObj->m_iSendMsgQueueCount; //发送消息队列容量少1		
        //            /*p_memory->FreeMemory(pMsgBuf);*/
        //            free(pMsgBuf);
        //            continue;
        //        } 
        //        //=============================================================================================
        //        //if (pMsgHeader->msgSeqNum > p_Conn->sentMsgCount)
        //        //{
        //        //    //还未轮到该消息的发送，先跳过
        //        //    pos++;
        //        //    continue;
        //        //}
        //        //if (pMsgHeader->isToSendPKG == false)
        //        //{
        //        //    //废包(仅用于维持发包顺序)，丢弃
        //        //    pos2 = pos;
        //        //    pos++;
        //        //    pSocketObj->m_MsgSendQueue.erase(pos2);
        //        //    --pSocketObj->m_iSendMsgQueueCount; //发送消息队列容量少1		
        //        //    p_memory->FreeMemory(pMsgBuf);
        //        //    ++p_Conn->sentMsgCount; //发送消息数量增加1
        //        //    continue;
        //        //}
        //        //=============================================================================================
        //        //走到这里，便可以发送消息，发送完消息要从发送队列里删除
        //        pPkgHeader = //指向包头
        //            (LPCOMM_PKG_HEADER)(pMsgBuf + msgHeaderLength);
        //        p_Conn->psendMemPointer = pMsgBuf; //发送后释放用的，这段内存是new出来的
        //        pos2 = pos;
        //        pos++;
        //        pSocketObj->m_MsgSendQueue.erase(pos2);
        //        --pSocketObj->m_iSendMsgQueueCount;	
        //        //要发送的数据的缓冲区指针，因为发送数据不一定全部都能发送出去，需要记
        //        //录数据发送到了哪里，需要知道下次数据从哪里开始发送
        //        p_Conn->psendbuf = (char*)pPkgHeader;
        //        //包头+包体 长度，打包时用了htons，所以这里得用ntohs转回来
        //        itmp = ntohs(pPkgHeader->pkgLen);        
        //        p_Conn->isendlen = itmp; //剩余有多少数据还没发送
        //        //这里是重点，我们采用epoll水平触发的策略，能走到这里的，都应该是还没
        //        //有投递写事件到epoll中
        //        //epoll水平触发发送数据的改进方案：
        //        //开始不把socket写事件通知加入到epoll，当我需要写数据的时候，直接调用
        //        //write/send发送数据；
        //        //如果返回了EAGAIN[发送缓冲区满了，需要等待可写事件才能继续往缓冲区里
        //        //写数据]，我再把写事件通知加入到epoll，
        //        //此时，就变成了在epoll驱动下写数据，全部数据发送完毕后，再把写事件通
        //        //知从epoll中干掉；
        //        //优点：数据不多的时候，可以避免epoll的写事件的增加/删除，提高了程序的
        //        //执行效率；                         
        //        //直接调用write或者send发送数据
        //        ngx_log_stderr(0, "Data to be sent(%ud) for %s.",
        //            p_Conn->isendlen, p_Conn->addr_text);
        //
        //        sendsize = pSocketObj->sendproc(
        //            p_Conn, 
        //            p_Conn->psendbuf, 
        //            p_Conn->isendlen);
        //        
        //        if (sendsize > 0)
        //        {
        //            if (sendsize == p_Conn->isendlen) //成功发送出去整条数据
        //            {
        //                /*p_memory->FreeMemory(p_Conn->psendMemPointer);*/ //释放内存
        //                free(p_Conn->psendMemPointer);
        //                p_Conn->psendMemPointer = NULL;
        //                //++p_Conn->sentMsgCount; //发送消息数量增加1
        //                ngx_log_stderr(0, "In CSocekt::ServerSendQueueThread, "
        //                    "a message has been sent successfully!"); 
        //            }
        //            else //sendsize < p_Conn->isendlen，
        //                //数据没有全部发送完毕(EAGAIN)，可能因为发送缓冲区满了
        //            {
        //                p_Conn->whandler = &CSocket::ngx_write_request_handler;
        //                //发送到了哪里，剩余多少，记录下来，方便下次sendproc时使用
        //                p_Conn->psendbuf = p_Conn->psendbuf + sendsize;
        //                p_Conn->isendlen = p_Conn->isendlen - sendsize;
        //                //发送缓冲区已满，现在我要依赖epoll事件通知，在worker进程的
        //                //主线程中来发送数据了
        //                if (pSocketObj->ngx_epoll_oper_event(
        //                    p_Conn->fd,    //socket句柄
        //                    EPOLL_CTL_MOD, //事件类型，这里是添加 写通知
        //                    EPOLLOUT,      //EPOLLOUT：可写的时候通知我
        //                    0,             //事件类型为EPOLL_CTL_MOD，0(增加)1(去掉)2(覆盖)
        //                    p_Conn         //连接池中的连接
        //                ) == -1)
        //                {
        //                    //设置失败
        //                    ngx_log_stderr(errno, "In CSocekt::ServerSendQueueThread, "
        //                        "func ngx_epoll_oper_event failed!");
        //                }
        //                ngx_log_stderr(0, "In CSocekt::ServerSendQueueThread，"
        //                    "some data has not been sent completely due to send "
        //                    "buffer being full, actually send %d, left %d to send!", 
        //                    p_Conn->isendlen, sendsize);
        //            }
        //
        //            continue;  //继续处理其他消息                    
        //        }
        //        else if (sendsize == 0)
        //        {
        //            //发送0个字节，首先因为我发送的内容不是0个字节的；
        //            //然后如果发送缓冲区满则返回的应该是-1，而错误码应该是EAGAIN，
        //            //这种情况按对端关闭了socket处理，我就把这个发送的包丢弃了，
        //            //等待recv来做断开socket以及回收资源
        //            //如果对方关闭连接出现send==0，那么这个日志可能会常出现
        //            ngx_log_stderr(errno,"In CSocekt::ServerSendQueueThread, "
        //                "func sendproc oddly returned 0!"); 
        //
        //            /*p_memory->FreeMemory(p_Conn->psendMemPointer);*/ //释放内存
        //            free(p_Conn->psendMemPointer);
        //            p_Conn->psendMemPointer = NULL;
        //            continue;
        //        }
        //        else if (sendsize == -1)
        //        {
        //            //一个字节都没发出去，说明发送缓冲区当前正好是满的
        //            //发送缓冲区已满，现在我要依赖epoll事件通知来发送数据了
        //            p_Conn->whandler = &CSocket::ngx_write_request_handler;
        //            if (pSocketObj->ngx_epoll_oper_event(
        //                p_Conn->fd,    //socket句柄
        //                EPOLL_CTL_MOD, //事件类型，这里是添加 写通知
        //                EPOLLOUT,      //EPOLLOUT：可写的时候通知我
        //                0,             //事件类型为EPOLL_CTL_MOD，0(增加)1(去掉)2(覆盖)
        //                p_Conn         //连接池中的连接
        //            ) == -1)
        //            {
        //                //设置失败
        //                ngx_log_stderr(errno, "In CSocekt::ServerSendQueueThread, "
        //                    "func ngx_epoll_oper_event_2 failed!");
        //            }
        //            continue;
        //        }
        //        else //sendsize == -2
        //        {
        //            //能走到这里的，应该就是返回值-2了，一般就认为对端断开了，
        //            //把这个发送的包丢弃，等待recv来做断开socket以及回收资源
        //            /*p_memory->FreeMemory(p_Conn->psendMemPointer);*/ //释放内存
        //            free(p_Conn->psendMemPointer);
        //            p_Conn->psendMemPointer = NULL;
        //            continue;
        //        }
        //    } //end while(pos != posend)
        //
        //    //=========================================================================================
        //    //如果发消息队列还有数据，而信号量已经为零，这时线程可能返回后睡眠，造成
        //    //消息滞留，所以在此判断信号量是否增加以保持线程运行
        //    //sem_getvalue(&pSocketObj->m_semEventSendQueue, &sem_value);
        //    //if (sem_value == 0 && !pSocketObj->m_MsgSendQueue.empty())
        //    //{
        //    //    //sem_post是给信号量的值加上一个1，它是一个原子操作，即同时对同一个信
        //    //    //号量做加1操作的两个线程是不会冲突的
        //    //    sem_post(&pSocketObj->m_semEventSendQueue);
        //    //}
        //    //=========================================================================================
        //    err = pthread_mutex_unlock(&pSocketObj->m_sendMessageQueueMutex);
        //    if (err != 0)
        //    {
        //        ngx_log_stderr(err, "In CSocekt::ServerSendQueueThread, "
        //            "func pthread_mutex_unlock failed, "
        //            "the returned errno is %d!", err);
        //    }
        //    //=========================================================================================
        //} //end if(pSocketObj->m_iSendMsgQueueCount > 0)

        usleep(200); //每次睡眠500微妙，避免cpu高占

        //若消息队列不为空，则一次性取出其所有消息放入正在发送消息队列，因为正在发送消息
        //队列只在发消息线程中处理，所以不需要加锁；
        //对于正在发送消息队列上的所有消息，能够被这轮线程循环处理的消息会被弹出该队列
        //进行处理，不能被处理的继续留在队列中等待下一轮循环进行处理(下一轮新加入的消息
        //会连接在上一轮未处理消息的末尾，所以上一轮未处理的消息会被第二轮循环优先处理)
        if (pSocketObj->sendMsgQueue.size > 0)
        {
            while (__sync_lock_test_and_set(&sendLOCK, 1))
            { usleep(0); }

            if ((pSocketObj->sendingQueue).tail == NULL) //sendingQueue is empty
            {
                (pSocketObj->sendingQueue).head = 
                    (pSocketObj->sendMsgQueue).head;
                (pSocketObj->sendingQueue).tail =
                    (pSocketObj->sendMsgQueue).tail;
                (pSocketObj->sendingQueue).size =
                    (pSocketObj->sendMsgQueue).size;
            }
            else 
            {
                (pSocketObj->sendingQueue).tail->nextMsg = 
                    (pSocketObj->sendMsgQueue).head;
                (pSocketObj->sendMsgQueue).head->preMsg =
                    (pSocketObj->sendingQueue).tail;
                (pSocketObj->sendingQueue).tail =
                    (pSocketObj->sendMsgQueue).tail;
                (pSocketObj->sendingQueue).size += 
                    (pSocketObj->sendMsgQueue).size;
            }

            //msg = (pSocketObj->sendMsgQueue).head;
            (pSocketObj->sendMsgQueue).head = NULL;
            (pSocketObj->sendMsgQueue).tail = NULL;
            (pSocketObj->sendMsgQueue).size = 0;

            __sync_lock_release(&sendLOCK);

        }

        msg = (pSocketObj->sendingQueue).head;
        while (msg)
        {
            pMsgBuf = reinterpret_cast<char*>(msg);
            pMsgHeader = msg; //指向消息头
            p_Conn = pMsgHeader->pConn;
            //包过期，ngx_recycle_connection回收连接到回收连接队列，iCurrsequence
            //会增加1；ngx_free_connection将这个连接从回收连接队列挪到空闲连接队列，
            //iCurrsequence加1；ngx_get_connection启用这个连接，iCurrsequence再
            //加1。只要下面条件成立，肯定是客户端连接已断，后续要发送的数据直接删除
            if (p_Conn->iCurrsequence != pMsgHeader->iCurrsequence)
            {
                //丢弃此消息，小心处理该消息的删除
                msg1 = msg;
                msg = msg->nextMsg;
                pSocketObj->popSendingQueueMsg(msg1);
                free(msg1);
                continue;
            }

            if (p_Conn->sendBufFull > 0)
            {
                //当服务器某一个连接的发送缓冲区已满，再积累500条消息就关闭此连接
                if (p_Conn->sendCount > 500) 
                {
                    ngx_log_stderr(0, "connection[%s] sendCount(%d) exceeded 500, "
                        "the server will close the connection soon for safty!",
                        p_Conn->addr_text, p_Conn->sendCount);
                    pSocketObj->ngx_recycle_connection(p_Conn);
                }
                //靠系统驱动来发送消息，所以这里不能再发送
                msg = msg->nextMsg;
                continue;
            }

            //--p_Conn->sendCount; //该连接的发送队列中消息数减1
            __sync_fetch_and_sub(&p_Conn->sendCount, 1); //需要原子锁临界，很重要

            //走到这里，便可以发送消息，发送完消息要从发送队列里删除
            pPkgHeader = //指向包头
                (LPCOMM_PKG_HEADER)(pMsgBuf + msgHeaderLength);
            p_Conn->psendMemPointer = pMsgBuf; //发送后释放这段动态分配内存          
            //要发送的数据的缓冲区指针，因为发送数据不一定全部都能发送出去，需要记
            //录数据发送到了哪里，需要知道下次数据从哪里开始发送
            p_Conn->psendbuf = (char*)pPkgHeader;
            //包头+包体 长度，打包时用了htons，所以这里得用ntohs转回来
            itmp = ntohs(pPkgHeader->pkgLen);
            p_Conn->isendlen = itmp; //剩余有多少数据还没发送
            msg1 = msg;
            msg = msg->nextMsg;
            pSocketObj->popSendingQueueMsg(msg1);
            //这里是重点，我们采用epoll水平触发的策略，能走到这里的，都应该是还没
            //有投递写事件到epoll中
            //epoll水平触发发送数据的改进方案：
            //开始不把socket写事件通知加入到epoll，当我需要写数据的时候，直接调用
            //write/send发送数据；
            //如果返回了EAGAIN[发送缓冲区满了，需要等待可写事件才能继续往缓冲区里
            //写数据]，我再把写事件通知加入到epoll，
            //此时，就变成了在epoll驱动下写数据，全部数据发送完毕后，再把写事件通
            //知从epoll中干掉；
            //优点：数据不多的时候，可以避免epoll的写事件的增加/删除，提高了程序的
            //执行效率；                         
            //直接调用write或者send发送数据
            /*ngx_log_stderr(0, "Data to be sent(%ud) for %s.",
                p_Conn->isendlen, p_Conn->addr_text);*/

            sendsize = pSocketObj->sendproc(
                p_Conn,
                p_Conn->psendbuf,
                p_Conn->isendlen);

            if (sendsize > 0)
            {
                if (sendsize == p_Conn->isendlen) //成功发送出去整条数据
                {
                    free(p_Conn->psendMemPointer); //释放内存
                    p_Conn->psendMemPointer = NULL;
                    //p_Conn->sendBufFull = 0; //这行多余
                    /*ngx_log_stderr(0, "In CSocekt::ServerSendQueueThread, "
                        "a message has been sent successfully!");*/

                    if (count == 0)
                    {
                        timespec_get(&tmv1, 1);
                    }
              
                    if (++count > 5000)
                    {
                        count = 0;
                        timespec_get(&tmv2, 1);
                        tm += (tmv2.tv_sec - tmv1.tv_sec) * 1000;
                        tm += (tmv2.tv_nsec * 0.000001 - tmv1.tv_nsec * 0.000001);
                        ngx_log_stderr(0, "time consuming: %f ms\n",tm);
                        tm = 0;
                    }

                }
                else //sendsize < p_Conn->isendlen，
                    //数据没有全部发送完毕(EAGAIN)，可能因为发送缓冲区满了
                {
                    //发送到了哪里，剩余多少，记录下来，方便下次sendproc时使用
                    p_Conn->psendbuf = p_Conn->psendbuf + sendsize;
                    p_Conn->isendlen = p_Conn->isendlen - sendsize;
                    //因为发送缓冲区慢了，需要通过epoll事件来驱动消息的继续发送
                    p_Conn->sendBufFull = 1; //标记发送缓冲区满了

                    //发送缓冲区已满，现在我要依赖epoll事件通知，在worker进程的
                    //主线程中来发送数据了
                    if (pSocketObj->ngx_epoll_oper_event(
                        p_Conn->fd,    //socket句柄
                        EPOLL_CTL_MOD, //事件类型，这里是添加 写通知
                        EPOLLOUT,      //EPOLLOUT：可写的时候通知我
                        0,             //事件类型为EPOLL_CTL_MOD，0(增加)1(去掉)2(覆盖)
                        p_Conn         //连接池中的连接
                    ) == -1)
                    {
                        //设置失败
                        ngx_log_stderr(errno, "In CSocekt::ServerSendQueueThread, "
                            "func ngx_epoll_oper_event failed!");
                    }
                    ngx_log_stderr(0, "In CSocekt::ServerSendQueueThread，"
                        "some data has not been sent completely due to send "
                        "buffer being full, actually send %d, left %d to send!",
                        p_Conn->isendlen, sendsize);
                }

                continue;  //继续处理其他消息                    
            }

            else if (sendsize == 0)
            {
                //发送0个字节，首先因为我发送的内容不是0个字节的；
                //然后如果发送缓冲区满则返回的应该是-1，而错误码应该是EAGAIN，
                //这种情况按对端关闭了socket处理，我就把这个发送的包丢弃了，
                //等待recv来做断开socket以及回收资源
                //如果对方关闭连接出现send==0，那么这个日志可能会常出现
                ngx_log_stderr(errno, "In CSocekt::ServerSendQueueThread, "
                    "func sendproc oddly returned 0!");

                free(p_Conn->psendMemPointer);
                p_Conn->psendMemPointer = NULL;
                //p_Conn->sendBufFull = 0; //这行多余
                continue;
            }
            else if (sendsize == -1)
            {
                //一个字节都没发出去，说明发送缓冲区当前正好是满的
                //发送缓冲区已满，现在我要依赖epoll事件通知来发送数据了
                p_Conn->sendBufFull = 1;
                if (pSocketObj->ngx_epoll_oper_event(
                    p_Conn->fd,    //socket句柄
                    EPOLL_CTL_MOD, //事件类型，这里是添加 写通知
                    EPOLLOUT,      //EPOLLOUT：可写的时候通知我
                    0,             //事件类型为EPOLL_CTL_MOD，0(增加)1(去掉)2(覆盖)
                    p_Conn         //连接池中的连接
                ) == -1)
                {
                    //设置失败
                    ngx_log_stderr(errno, "In CSocekt::ServerSendQueueThread, "
                        "func ngx_epoll_oper_event_2 failed!");
                }

                ngx_log_stderr(0, "In CSocekt::ServerSendQueueThread，"
                    "some data has not been sent due to send buffer being full, "
                    "epoll is set up");

                continue;
            }
            else //sendsize == -2
            {
                //能走到这里的，应该就是返回值-2了，一般就认为对端断开了，
                //把这个发送的包丢弃，等待recv来做断开socket以及回收资源
                free(p_Conn->psendMemPointer);
                p_Conn->psendMemPointer = NULL;
                //p_Conn->sendBufFull = 0; //这行多余
                continue;
            }
        }
        
    } //end while
    return (void*)0;
}

//epoll_event结构体一般用在epoll机制中，其定义如下：

/*
struct epoll_event
{
  uint32_t events;      //Epoll events 
  epoll_data_t data;    //User data variable
} __attribute__ ((__packed__));

typedef union epoll_data
{
  void  * ptr;
  int fd;
  uint32_t u32;
  uint64_t u64;
} epoll_data_t;
*/

