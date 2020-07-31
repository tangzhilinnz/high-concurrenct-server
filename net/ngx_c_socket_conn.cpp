//和网络中连接/连接池有关的函数放这里
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
//#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"


//---------------------------------------------------------------------------------------
//连接池成员函数
ngx_connection_s::ngx_connection_s()
{
    fd = -1;
    instance = 1;
    iCurrsequence = 0;
    int err = pthread_mutex_init(&logicPorcMutex, NULL); //互斥量初始化
    if (err != 0) 
    {
        ngx_log_stderr(err, "In ngx_connection_s::ngx_connection_s(), "
            "pthread_mutex_init(&logicPorcMutex) failed!.");
    }
    err = pthread_mutex_init(&recyConnMutex, NULL); //互斥量初始化
    if (err != 0)
    {
        ngx_log_stderr(err, "In ngx_connection_s::ngx_connection_s(), "
            "pthread_mutex_init(&recyConnMutex) failed!.");
    }
}

//析构函数只做了一件事，释放该连接对象的互斥量成员
ngx_connection_s::~ngx_connection_s() 
{
    int err = pthread_mutex_destroy(&logicPorcMutex); //互斥量释放
    if (err != 0)
    {
        ngx_log_stderr(err, "In ngx_connection_s::~ngx_connection_s(), "
            "pthread_mutex_destroy(&logicPorcMutex) failed!.");
    }
}

//分配出去一个连接的时候初始化一些内容，原来放在ngx_get_connection里做
void 
ngx_connection_s::GetOneToUse()
{
    CConfig* p_config = CConfig::GetInstance();

    instance = !instance;               //instance取反
    ++iCurrsequence;                    //iCurrsequence加1
    curStat = _PKG_HD_INIT;             //收包状态处于初始状态，准备接收数据包头
    precvbuf_hstar = dataHeadInfo;      //包头的起始地址给成dataHeadInfo
    precvbuf = dataHeadInfo;            //收包我要先收到这里来，因为我要先收包头，
                                        //所以收数据的buff直接就是dataHeadInfo
    irecvlen = sizeof(COMM_PKG_HEADER); //这里指定收数据的长度，这里先要求收包头
                                        //这么长字节的数据
    precvMemPointer = NULL;             //既然没new内存，指向的内存地址先给NULL
    psendMemPointer = NULL;             //发送数据头指针记录，先给NULL
    events = 0;                         //epoll事件先给0 

    sendCount = 0;
    sendBufFull = 0;

    timerEntry = NULL;

    /*recvdMsgCount = 0;*/               //收到的消息数刚开始为零      
    /*sentMsgCount = 0; */               //发送的消息数刚开始为零

    //将前一个连接的ip地址字符串清零
    memset(addr_text, 0, sizeof(addr_text));

    //允许校验错误包次数初始化
    const char* isOn = p_config->GetString("AbnormalPKGCheck");
    if (isOn == NULL)
    {
        abnrPKGCheck_admit = 0;
    }
    else
    {
        if (strcasecmp(isOn, "ON") == 0)
        {
            abnrPKGCheck_admit = p_config->GetIntDefault(
                "AbnormalPKGCheck_Admit", 5);
            abnrPKGCheck_admit = ngx_min(abnrPKGCheck_admit, 30000);
            abnrPKGCheck_admit = ngx_max(abnrPKGCheck_admit, 5);
        }
        else
        {
            abnrPKGCheck_admit = 0;
        }
    }
}

//回收一个连接，释放这个连接曾今分配的动态内存
void 
ngx_connection_s::PutOneToFree()
{
    ++iCurrsequence;
    //=============================================================================================
    if (precvMemPointer != NULL) //若曾给这个连接分配过接收数据的内存，则要释放内存
    {
        /*CMemory::GetInstance()->FreeMemory(precvMemPointer);*/
        free(precvMemPointer);
        precvMemPointer = NULL;
    }
    if (psendMemPointer != NULL) //如果发送数据的缓冲区里有内容，则要释放内存
    {
        /*CMemory::GetInstance()->FreeMemory(psendMemPointer);*/
        free(psendMemPointer);
        psendMemPointer = NULL;
    } 

    timerEntry = NULL;
    //=============================================================================================
}

//---------------------------------------------------------------------------------------
//CSocket成员函数

//初始化连接池
void 
CSocket::initconnection()
{
    lpngx_connection_t p_Conn;
    //CMemory* p_memory = CMemory::GetInstance();

    int ilenconnpool = sizeof(ngx_connection_t);
    for (int i = 0; i < m_worker_connections; ++i) //先创建这么多连接，后续不够再加
    {
        //清理内存，因为这里分配内存new char，无法执行构造函数
        /*p_Conn = (lpngx_connection_t)p_memory->AllocMemory(ilenconnpool, true);*/
        p_Conn = (lpngx_connection_t)malloc(ilenconnpool);
        memset(p_Conn, 0, ilenconnpool);
        //手工调用构造函数，因为AllocMemory里无法调用构造函数
        p_Conn = new(p_Conn) ngx_connection_t(); //定位new的主要用途就是反复使用一块
                                                 //较大的动态分配的内存来构造不同类型
                                                 //的对象或者他们的数组
                                                 //释放则显式调用析构函数
                                                 //p_Conn->~ngx_connection_t		
        p_Conn->GetOneToUse();
        m_connectionList.push_back(p_Conn);     //所有连接[不管是否空闲]都放在这个list
        m_freeconnectionList.push_back(p_Conn); //空闲连接会放在这个list
    } 
    //开始这两个列表一样大
    m_free_connection_n = m_total_connection_n = m_connectionList.size();

    return;
}

//最终回收连接池，释放内存
void 
CSocket::clearconnection()
{
    lpngx_connection_t p_Conn;
    //CMemory* p_memory = CMemory::GetInstance();

    while (!m_connectionList.empty())
    {
        p_Conn = m_connectionList.front();
        m_connectionList.pop_front();

        if (p_Conn->fd != -1)
        {
            if (close(p_Conn->fd) == -1)
            {
                ngx_log_error_core(NGX_LOG_ALERT, errno,
                    "In CSocket::clearconnection, func close(%d) failed!",
                    p_Conn->fd);
            }
            p_Conn->fd = -1; //官方nginx这么写，这么写有意义
        }
        p_Conn->PutOneToFree();
        p_Conn->~ngx_connection_t(); //手工调用析构函数，释放该连接对象的互斥量成员

        /*p_memory->FreeMemory(p_Conn);*/
        free(p_Conn);
    }

    m_total_connection_n = 0;
    m_free_connection_n = 0;
    m_recycling_connection_n = 0; 
}

//从连接池中获取一个空闲连接
//当一个客户端连接TCP进入，我希望把这个连接和连接池中的一个连接对象绑到一起，后续可以通
//过这个连接，把这个对象拿到，因为对象里边可以记录各种信息
lpngx_connection_t 
CSocket::ngx_get_connection(int isock)
{
    //该函数在主线程中调用，因为辅助线程在函数 ngx_free_connection 也要访问变量
    //m_freeconnectionList，所以应该临界一下
    CLock lock(&m_freeconnListMutex);

    if (!m_freeconnectionList.empty())
    {
        //有空闲的，自然是从空闲的中摘取，返回第一个元素但不检查元素存在与否
        lpngx_connection_t p_Conn = m_freeconnectionList.front(); 
        m_freeconnectionList.pop_front(); //移除第一个元素但不返回	
        p_Conn->GetOneToUse();
        --m_free_connection_n;
        p_Conn->fd = isock;
        return p_Conn;
    }

    //走到这里，表示没空闲的连接了，那就考虑重新创建一个连接
    //CMemory* p_memory = CMemory::GetInstance();
    /*lpngx_connection_t p_Conn = (lpngx_connection_t)p_memory->
        AllocMemory(sizeof(ngx_connection_t), true);*/

    lpngx_connection_t p_Conn = (lpngx_connection_t)malloc(sizeof(ngx_connection_t));
    memset(p_Conn, 0, sizeof(ngx_connection_t));

    p_Conn = new(p_Conn) ngx_connection_t();
    p_Conn->GetOneToUse();
    m_connectionList.push_back(p_Conn); //入到总表中来，但不能入到空闲表中来，
                                        //因为这个连接即将被使用
    ++m_total_connection_n;
    p_Conn->fd = isock;
    return p_Conn;

    //因为我们要采用延迟释放的手段来释放连接，因此这种 instance就没啥用，
    //这种手段用来处理立即释放才有用，但还是保留
}

//归还参数pConn所代表的连接到到连接池中
void 
CSocket::ngx_free_connection(lpngx_connection_t pConn)
{
    //1）因为辅助线程和主线程可能都要调用此函数，这两个线程会操作同一个变量
    //   m_freeconnectionList，
    //2）主线程会调用ngx_get_connection函数，也会在其中操作变量
    //   m_freeconnectionList
    //所以这里需要互斥锁
    ngx_log_stderr(0, "In CSocket::ngx_free_connection, "
        "connection for [%s] is reclaimed to m_freeconnectionList.",
        pConn->addr_text);

    CLock lock(&m_freeconnListMutex);

    //首先明确一点，所有连接全部都在m_connectionList里；
    pConn->PutOneToFree();

    //扔到空闲连接列表里
    m_freeconnectionList.push_back(pConn);

    //空闲连接数+1
    ++m_free_connection_n;

    return;

    /*
    if (c->ifnewrecvMem == true)
    {
        //我们曾经给这个连接对象分配过内存，则要释放内存        
        CMemory::GetInstance()->FreeMemory(c->pnewMemPointer);
        c->pnewMemPointer = NULL;
        c->ifnewrecvMem = false;  //这行有用？
    }

    c->data = m_pfree_connections;  //回收的节点指向原来串起来的空闲链的链头

    //节点本身也要干一些事
    ++(c->iCurrsequence);    //回收后，该值就增加1，以用于判断某些网络事件是否过期，
                             //一被释放就立即+1也是有必要的

    m_pfree_connections = c; //修改原来空闲链的链头指向新节点
    ++m_free_connection_n;   //空闲连接多1

    return;*/
}


//将要回收的连接放到一个队列中来，后续有专门的线程会处理这个队列中的连接的回收
//有些连接，我们不希望马上释放，要隔一段时间后再释放以确保服务器的稳定，所以，
//我们把这种隔一段时间才释放的连接先放到一个队列中来
void 
CSocket::ngx_recycle_connection(lpngx_connection_t pConn)
{
    //std::list<lpngx_connection_t>::iterator pos;
    //CLock lock(&recyConnMutex); //针对连接回收列表的互斥量，因为在主线程中会
                                      //调用这个函数，操作这个连接回收列表；辅助线
                                      //程ServerRecyConnectionThread也要操作这个
                                      //连接回收列表
    CLock lock(&pConn->recyConnMutex);
    //防止连接被多次扔到回收站中来
    //for (pos = m_recyconnectionList.begin(); pos != m_recyconnectionList.end(); ++pos)
    //{
    //    if ((*pos) == pConn)
    //    {
    //        return;
    //    }
    //}

    //防止连接中的socket_id被重复关闭
    if (pConn->fd != -1)
    {
        if (close(pConn->fd) == -1) 
        {
            ngx_log_error_core(NGX_LOG_ALERT, errno,
                "In CSocket::ngx_recycle_connection, func close(%d) failed!",
                pConn->fd);
        }
        pConn->fd = -1; //官方nginx这么写，这么写有意义
    }
    else return;

    ++pConn->iCurrsequence;

    if (pConn->timerEntry == NULL)
    {
        pConn->timerEntry = 
            timeWheel.CreateTimer(SFreeConnToList, pConn, m_RecyConnectionWaitTime, 0);
    }
    //pConn->timerEntry != NULL indicates that this pConn has been put in the time wheel
    else 
    {

    }

    //pConn->inRecyTime = time(NULL); //记录回收时间
    //m_recyconnectionList.push_back(pConn); //等待ServerRecyConnectionThread线程处理
    //++m_recycling_connection_n;            //待释放连接队列大小+1
    //ngx_log_stderr(0, "CSocket::ngx_recycle_connection() executed, "
    //    "one pConn is put into m_recyconnectionList.");

    return;
}

//=============================================================================================
//处理连接回收的线程函数(为CSocket的静态成员)
void* 
CSocket::ServerRecyConnectionThread(void* threadData)
{
    ThreadItem* pThread = static_cast<ThreadItem*>(threadData);
    CSocket* pSocketObj = pThread->_pThis;

    time_t currtime;
    int err;
    std::list<lpngx_connection_t>::iterator pos, pos2, posend;
    lpngx_connection_t p_Conn;

    while (1)
    {
        //为简化问题，我们直接每次休息200毫秒
        usleep(200 * 1000);

        //不管啥情况，先把这个条件成立时该做的动作做了
        if (pSocketObj->m_recycling_connection_n > 0)
        {
            currtime = time(NULL);

            err = pthread_mutex_lock(&pSocketObj->recyConnMutex);
            if (err != 0)
            {
                ngx_log_stderr(err,
                    "In CSocket::ServerRecyConnectionThread, pthread_mutex_lock"
                    "(&recyConnMutex) failed, errno is %d!", err);
            }

            pos = pSocketObj->m_recyconnectionList.begin();
            posend = pSocketObj->m_recyconnectionList.end();
            while (pos != posend)
            {
                p_Conn = (*pos);
                //没到释放的时间，如果系统不退出，continue，否则就得要强制释放
                if ((p_Conn->inRecyTime + pSocketObj->m_RecyConnectionWaitTime) > currtime
                    && g_stopEvent == 0)
                {
                    pos++;
                    continue; //没到释放的时间
                }

                //这将来可能还要做一些是否能释放的判断[在我们写完发送数据代码之后]
                //... ... ... ...

                //流程走到这里，表示可以释放，那我们就开始释放
                pos2 = pos;
                pos++;
                --pSocketObj->m_recycling_connection_n; //待释放连接队列大小-1
                pSocketObj->m_recyconnectionList.erase(pos2);
                ngx_log_stderr(0, "In CSocket::ServerRecyConnectionThread, "
                    "connection for [%s] is reclaimed to m_freeconnectionList.",
                    p_Conn->addr_text);
                //归还参数pConn所代表的连接到空闲连接列表
                pSocketObj->ngx_free_connection(p_Conn);
            }

            err = pthread_mutex_unlock(&pSocketObj->recyConnMutex);
            if (err != 0)
            {
                ngx_log_stderr(err, "In CSocket::ServerRecyConnectionThread, "
                    "pthread_mutex_unlock(&recyConnMutex) failed, "
                    "errno is %d!", err);
            }
        } 

        if (g_stopEvent == 1) //要退出整个程序，那么肯定要先退出这个循环
        {

            //因为要退出，所以就得硬释放了，不管到没到时间，不管有没有其他不允许
            //释放的需求，都得硬释放
            err = pthread_mutex_lock(&pSocketObj->recyConnMutex);
            if (err != 0)
            {
                ngx_log_stderr(err, "In CSocket::ServerRecyConnectionThread, "
                    "pthread_mutex_lock(&recyConnMutex) failed, "
                    "errno is %d!", err);
            }

            while (!pSocketObj->m_recyconnectionList.empty())
            {
                p_Conn = pSocketObj->m_recyconnectionList.front();
                pSocketObj->m_recyconnectionList.pop_front();
                //归还参数pConn所代表的连接到空闲连接列表
                pSocketObj->ngx_free_connection(p_Conn);
                --pSocketObj->m_recycling_connection_n; //待释放连接队列大小-1
            }

            err = pthread_mutex_unlock(&pSocketObj->recyConnMutex);
            if (err != 0)
                ngx_log_stderr(err, "In CSocket::ServerRecyConnectionThread, "
                    "pthread_mutex_unlock(&recyConnMutex) failed, "
                    "errno is %d!", err);

            break; //整个程序要退出了，所以break;
        }
    }    

    return (void*)0;
}
//=============================================================================================

//用户连入，当accept时，得到的socket在处理中产生失败，则资源用这个函数立即释放连接
//该函数只在worker进程的主线程中调用
void 
CSocket::ngx_close_connection(lpngx_connection_t pConn)
{
    //防止连接中的socket_id被重复关闭
    if (pConn->fd != -1)
    {
        if (close(pConn->fd) == -1)
        {
            ngx_log_error_core(NGX_LOG_ALERT, errno,
                "In CSocket::ngx_close_connection, func close(%d) failed!",
                pConn->fd);
        }
        pConn->fd = -1; //官方nginx这么写，这么写有意义
    }
    //if (pConn->precvMemPointer != NULL) //若曾给这个连接分配过接收数据的内存，则要释放内存
    //{
    //    /*CMemory::GetInstance()->FreeMemory(precvMemPointer);*/
    //    free(pConn->precvMemPointer);
    //    pConn->precvMemPointer = NULL;
    //}
    //if (pConn->psendMemPointer != NULL) //如果发送数据的缓冲区里有内容，则要释放内存
    //{
    //    /*CMemory::GetInstance()->FreeMemory(psendMemPointer);*/
    //    free(pConn->psendMemPointer);
    //    pConn->psendMemPointer = NULL;
    //}
    ++pConn->iCurrsequence;
    ngx_free_connection(pConn); 
    return;
}

