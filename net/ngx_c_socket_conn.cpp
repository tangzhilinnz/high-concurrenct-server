﻿//和网络中连接/连接池有关的函数放这里
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
    //pRecvBuff = NULL;
    //recvMsgHead = NULL;
    //recvMsgRead = NULL;
    unpackMsgQue.head = NULL;
    unpackMsgQue.tail = NULL;
    unpackMsgQue.size = 0;

    int err = pthread_mutex_init(&logicPorcMutex, NULL); //互斥量初始化
    if (err != 0) 
    {
        ngx_log_stderr(err, "In ngx_connection_s::ngx_connection_s(), "
            "pthread_mutex_init(&logicPorcMutex) failed!.");
    }
    //err = pthread_mutex_init(&recyConnMutex, NULL); //互斥量初始化
    //if (err != 0)
    //{
    //    ngx_log_stderr(err, "In ngx_connection_s::ngx_connection_s(), "
    //        "pthread_mutex_init(&recyConnMutex) failed!.");
    //}

    timerEntryPing = timeWheel.CreateTimer(
        CSocket::PingTimeout, this, CSocket::m_iWaitTime, 0);
    if (timerEntryPing == NULL)
        ngx_log_stderr(err, "In ngx_connection_s::ngx_connection_s(), "
            "CreateTimer(timerEntryPing) failed!.");

    timerEntryRecy = timeWheel.CreateTimer(
        CSocket::SetConnToIdle, this, CSocket::m_RecyConnWaitTime, 0);
    if (timerEntryRecy == NULL)
        ngx_log_stderr(err, "In ngx_connection_s::ngx_connection_s(), "
            "CreateTimer(timerEntryRecy) failed!.");
}

//析构函数只做了一件事，释放该连接对象的互斥量成员和定时器
ngx_connection_s::~ngx_connection_s() 
{
    int err = pthread_mutex_destroy(&logicPorcMutex); //互斥量释放
    if (err != 0)
    {
        ngx_log_stderr(err, "In ngx_connection_s::~ngx_connection_s(), "
            "pthread_mutex_destroy(&logicPorcMutex) failed!.");
    }
    //err = pthread_mutex_destroy(&recyConnMutex); //互斥量释放
    //{
    //    ngx_log_stderr(err, "In ngx_connection_s::~ngx_connection_s(), "
    //        "pthread_mutex_destroy(&recyConnMutex) failed!.");
    //}
    if (!timerEntryPing) timeWheel.DeleteTimer(timerEntryPing);
    timerEntryPing = NULL;
    if (!timerEntryRecy) timeWheel.DeleteTimer(timerEntryRecy);
    timerEntryRecy = NULL;
}

//分配出去一个连接的时候初始化一些内容，原来放在ngx_get_connection里做
void 
ngx_connection_s::GetOneToUse()
{
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

    pIOthread = NULL;

    sendCount = 0;
    sendBufFull = 0;
    timerStatus = 0;

    memset(addr_text, 0, sizeof(addr_text)); //将前一个连接的ip地址字符串清零
    memset(eventFlags, 0, sizeof(eventFlags)); //将前一个连接的事件
                                               //待定标志清零
    //=========================================================================
    //允许校验错误包的次数，g_wrongPKGAdmit是一个全局变量，在main函数开始时已初始化
    wrongPKGAdmit = g_wrongPKGAdmit;
    //=========================================================================
}

//回收一个连接，释放这个连接曾今分配的动态内存
void 
ngx_connection_s::PutOneToFree()
{
    ++iCurrsequence;
    //=========================================================================
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
    if (pRecvBuff != NULL) //若分配过数据接收缓冲区，则需要释放
    {
        free(pRecvBuff);
        pRecvBuff = NULL;
        recvMsgHead = NULL;
        recvMsgRead = NULL;
        //buffLength = 0;
    }

    nextConn = NULL;
    //=========================================================================
}

//-----------------------------------------------------------------------------
//CSocket成员函数

//初始化连接池
void 
CSocket::initconnection()
{
    lpngx_connection_t pConn;
    //CMemory* p_memory = CMemory::GetInstance();

    int iLenConnPool = sizeof(ngx_connection_t);
    for (int i = 0; i < m_worker_connections; ++i) //先创建这么多连接，后续不够再加
    {
        //清理内存，因为这里分配内存new char，无法执行构造函数
        pConn = (lpngx_connection_t)malloc(iLenConnPool);

        if (pConn != NULL)
        {
            memset(pConn, 0, iLenConnPool);
            //手工调用构造函数，因为AllocMemory里无法调用构造函数
            pConn = new(pConn) ngx_connection_t(); //定位new的主要用途就是反复使用一块
                                                   //较大的动态分配的内存来构造不同类型
                                                   //的对象或者他们的数组
                                                   //释放则显式调用析构函数
                                                   //pConn->~ngx_connection_t		
            pConn->GetOneToUse();
            m_connectionList.push_back(pConn);     //所有连接[不管是否空闲]都放在这个list
            m_freeconnectionList.push_back(pConn); //空闲连接会放在这个list
        }
    }

    m_free_connection_n = m_total_connection_n = m_connectionList.size();

    return;
}

//=============================================================================
//最终回收连接池，释放内存
void 
CSocket::clearconnection()
{
    lpngx_connection_t pConn;
    //CMemory* p_memory = CMemory::GetInstance();

    while (!m_connectionList.empty())
    {
        pConn = m_connectionList.front();
        m_connectionList.pop_front();

        if (pConn->fd != -1)
        {
            if (close(pConn->fd) == -1)
            {
                ngx_log_error_core(NGX_LOG_ALERT, errno,
                    "In CSocket::clearconnection, func close(%d) failed!",
                    pConn->fd);
            }
            pConn->fd = -1; //官方nginx这么写，这么写有意义
        }
        pConn->PutOneToFree();
        pConn->~ngx_connection_t(); //手工调用析构函数，释放该连接对象的互斥量和定时器

        free(pConn);
    }

    m_total_connection_n = 0;
    m_free_connection_n = 0;

    return;
}
//=============================================================================

//取连接池保护函数
bool 
CSocket::ConnListProtection()
{
    if (onlineUserCount >= m_worker_connections)
    {
        ngx_log_stderr(0,
            "Reach the maximum number(%d) of clients allowed by the system, "
            "connection request failed!", m_worker_connections);
        /*if (close(s) == -1)
        {
            ngx_log_error_core(NGX_LOG_ALERT, errno,
                "In CSocekt::ngx_event_accept, close(%d) failed!", s);
        }*/
        return true;
    }
    else //onlineUserCount < m_worker_connections
    {
        //某些恶意用户可能连接上来发了1条数据就迅速断开，或者连上后当刚申请了连接池就断开，
        //这样会频繁调用函数ngx_get_connection，造成短时间内产生大量连接消耗。比如系统允
        //许2048个客户连接，但连接池却有2048 * 5个连接，如此大的容量表明在短时间内产生了
        //大批量连接的申请和释放，导致大量连接进入idle状态，无法及时被回收到
        //m_freeconnectionList，从而无法通过函数ngx_free_connection及时回收给系统；
        //这样会使系统内存消耗激增，所以必须控制，在此设置m_total_connection_n的最大值为
        //m_worker_connections * 5
        if (m_total_connection_n >= (m_worker_connections * 5))
        {
            ngx_log_stderr(0,
                "The number of connections in m_connectionList has exceeded the "
                "maximum value(%d), connection request failed!", 
                m_worker_connections * 5);
            return true;
        }

        return false;
    }
}

//从连接池中获取一个空闲连接(已经在ngx_epoll_init为监听socket消耗两个连接，
//所以可用于客户连接的只有m_worker_connections - 2 个连接
//当一个客户端连接TCP进入，我希望把这个连接和连接池中的一个连接对象绑到一起，后续可以通
//过这个连接，把这个对象拿到，因为对象里边可以记录各种信息
lpngx_connection_t 
CSocket::ngx_get_connection(int isock)
{
    //该函数在主线程中调用，因为辅助线程在函数 ngx_free_connection 也要访问变量
    //m_freeconnectionList，所以应该临界一下
    CLock lock(&freeConnListMutex);

    if (!m_freeconnectionList.empty())
    {
        //有空闲的，自然是从空闲的中摘取，返回第一个元素但不检查元素存在与否
        lpngx_connection_t pConn = m_freeconnectionList.front(); 
        m_freeconnectionList.pop_front(); //移除第一个元素但不返回	
        pConn->GetOneToUse();
        --m_free_connection_n;
        pConn->fd = isock;

        pConn->pRecvBuff = (char*)malloc(connBuffSize);
        pConn->recvMsgHead = pConn->pRecvBuff;
        pConn->recvMsgRead = pConn->pRecvBuff;
        //pConn->buffLength = 0;
        if (pConn->pRecvBuff == NULL)
            return NULL;
        //=======================================test====================================
        /*ngx_log_stderr(0, "the size of m_freeconnectionList after one fetch: %d", 
            m_free_connection_n);
        ngx_log_stderr(0, "the total size of m_connectionList: %d",
            m_total_connection_n);*/
        //=======================================test====================================
        return pConn;
    }

    //if (freeConnList.head2 != NULL)
    //{
    //    //有空闲的，自然是从空闲的中摘取，返回第一个元素但不检查元素存在与否
    //    lpngx_connection_t pConn = freeConnList.head2;
    //    freeConnList.head2 = (freeConnList.head2)->nextConn;
    //    if (freeConnList.head2 == NULL)
    //        freeConnList.tail2 = NULL;
    //    --freeConnList.size2;
    //    pConn->GetOneToUse();
    //    pConn->fd = isock;
    //    //=======================================test====================================
    //    ngx_log_stderr(0, "the size of freeConnList after one fetch: %d", 
    //        freeConnList.size2);
    //    //=======================================test====================================
    //    return pConn;
    //}

    //走到这里，表示没空闲的连接了，那就考虑重新创建一个连接
    lpngx_connection_t pConn = (lpngx_connection_t)malloc(sizeof(ngx_connection_t));
    memset(pConn, 0, sizeof(ngx_connection_t));

    pConn = new(pConn) ngx_connection_t();
    pConn->GetOneToUse();
    m_connectionList.push_back(pConn); //入到总表中来，但不能入到空闲表中来，
                                       //因为这个连接即将被使用
    ++m_total_connection_n;
    pConn->fd = isock;

    pConn->pRecvBuff = (char*)malloc(connBuffSize);
    pConn->recvMsgHead = pConn->pRecvBuff;
    pConn->recvMsgRead = pConn->pRecvBuff;
    //pConn->buffLength = 0;
    if (pConn->pRecvBuff == NULL)
        return NULL;
    //=======================================test====================================
    /*ngx_log_stderr(0, "the size of m_freeconnectionList after one fetch: %d",
        m_free_connection_n);
    ngx_log_stderr(0, "the total size of m_connectionList: %d",
        m_total_connection_n);*/
    //=======================================test====================================

    return pConn;
    //因为我们要采用延迟释放的手段来释放连接，因此这种 instance就没啥用，
    //这种手段用来处理立即释放才有用，但还是保留
}

//归还参数pConn所代表的连接到到连接池中
void 
CSocket::ngx_free_connection(lpngx_connection_t pConn)
{
    //释放缓存在unpackMsgQue的解包消息，这些消息还未放入收消息队列中
    MsgQueueRelease(&(pConn->unpackMsgQue));
    pConn->PutOneToFree();
    //1）因为辅助线程和主线程可能都要调用此函数，这两个线程会操作同一个变量
    //   freeConnList，
    //2）主线程会调用ngx_get_connection函数，也会在其中操作变量
    //   freeConnList
    //所以这里需要互斥锁
    //ngx_log_stderr(0, "In CSocket::ngx_free_connection, "
    //    "connection for [%s] is reclaimed to m_freeconnectionList.",
    //    pConn->addr_text);

    //首先明确一点，所有连接全部都在m_connectionList里
    CLock lock(&freeConnListMutex);

    //当自由连接链表有富裕的连接，将超过m_worker_connections之后的连接都直接free掉，
    //只保持自由连接链表中有m_worker_connections个连接即可；之所以会出现连接数量大
    //于m_worker_connections，是因为某一时刻，客户接入过多，超过m_worker_connections，
    //导致系统又动态分配了连接供使用。当连接的客户数减少时就会多出来
    if (m_free_connection_n < m_worker_connections)
    {
        m_freeconnectionList.push_back(pConn);
        ++m_free_connection_n; //空闲连接数+1
        return;
    }

    //将该连接free掉，归还内存给操作系统
    m_connectionList.remove(pConn);
    --m_total_connection_n;
    if (pConn->fd != -1)
    {
        if (close(pConn->fd) == -1)
        {
            ngx_log_error_core(NGX_LOG_ALERT, errno,
                "In CSocket::ngx_free_connection, func close(%d) failed!",
                pConn->fd);
        }
        pConn->fd = -1; //官方nginx这么写，这么写有意义
    }
    pConn->~ngx_connection_t(); //手工调用析构函数，释放该连接对象的互斥量和定时器
    free(pConn);
}

//将要回收的连接放到一个队列中来，后续有专门的线程会处理这个队列中的连接的回收
//有些连接，我们不希望马上释放，要隔一段时间后再释放以确保服务器的稳定，所以，
//我们把这种隔一段时间才释放的连接先放到一个队列中来
void 
CSocket::ngx_recycle_connection(lpngx_connection_t pConn)
{
    int* patomicLock = &(pConn->pIOthread->atomicLock);
    CONN_EVENT_QUEUE* pconnEventQue = &(pConn->pIOthread->connEventQueue);

    if (pConn == NULL) return;

    //ATOMIC LCOK for this conn's thread connEventQueue 
    while (__sync_lock_test_and_set(patomicLock, 1)) { usleep(0); }

    //防止该连的socket_id被重复关闭
    if (pConn->eventFlags[E_CLOSE_CONN] == 1)
    {
        //release ATOMIC LCOK FOR this conn's thread connEventQueue 
        __sync_lock_release(patomicLock);
        return;
    }
    //这里动态分配了内存，记得释放
    CONN_EVENT* pconnEvent = (CONN_EVENT*)malloc(sizeof(CONN_EVENT));
    pconnEvent->eType = E_CLOSE_CONN;
    pconnEvent->pConn = pConn;
    pconnEvent->next = NULL;
    pconnEvent->prev = NULL;

    if (pconnEventQue->ptail == NULL)
    {
        pconnEventQue->phead = pconnEvent;
        pconnEventQue->ptail = pconnEvent;
    }
    else
    {
        pconnEventQue->ptail->next = pconnEvent;
        pconnEvent->prev = pconnEventQue->ptail;
        pconnEventQue->ptail = pconnEvent;
    }

    ++pconnEventQue->size;
    pConn->eventFlags[E_CLOSE_CONN] = 1;

    //release ATOMIC LCOK FOR this conn's thread connEventQueue 
    __sync_lock_release(patomicLock);

    ThreadEventNotify(pConn);

    return;

    //while (__sync_lock_test_and_set(&connLOCK, 1)) 
    //{ usleep(0); }
    ////防止连接中的socket_id被重复关闭
    //if (pConn->fd == -1)
    //{
    //    __sync_lock_release(&connLOCK); //release ATOMIC LCOK FOR recyConnQueue
    //    return;
    //}
    //if (close(pConn->fd) == -1)
    //{
    //    ngx_log_error_core(NGX_LOG_ALERT, errno,
    //        "In CSocket::ngx_recycle_connection, func close(%d) failed!",
    //         pConn->fd);
    //}
    //pConn->nextConn = NULL;
    //++recyConnQueue.size2;
    //if (recyConnQueue.tail2 == NULL)
    //{
    //    recyConnQueue.head2 = pConn;
    //    recyConnQueue.tail2 = pConn;
    //}
    //else
    //{
    //    (recyConnQueue.tail2)->nextConn = pConn;
    //    recyConnQueue.tail2 = pConn;
    //}
    //pConn->fd = -1; //官方nginx这么写，这么写有意义  
    //++pConn->iCurrsequence;
    //__sync_lock_release(&connLOCK); //release ATOMIC LCOK FOR recyConnQueue
    ////将信号量的值+1，这样其他卡在sem_wait的就可以走下去；sem_post是给信号量的值加上
    ////一个1，它是一个原子操作，即同时对同一个信号量做加1操作的两个线程是不会冲突的
    //if (sem_post(&semRecyConnQueue) == -1)
    //{
    //    ngx_log_stderr(0, "In CSocket::ngx_recycle_connection, "
    //        "func sem_post(&semRecyConnQueue) failed.");
    //}
}

//用户连入，当accept时，得到的socket在处理中产生失败，则资源用这个函数立即释放连接
//该函数只在worker进程的主线程中调用，并在加入epoll之前
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

    ++pConn->iCurrsequence;
    ngx_free_connection(pConn); 
    return;
}


//=============================================================================================
//处理连接回收的线程函数(为CSocket的静态成员)
//void* 
//CSocket::ServerRecyConnThread(void* threadData)
//{
//    ThreadItem* pThread = static_cast<ThreadItem*>(threadData);
//    CSocket* pSocketObj = pThread->_pThis;
//
//    int err;
//    lpngx_connection_t pConn, pConn1;
//
//    while (1)
//    {
//        if (sem_wait(/*&pSocketObj->*/&semRecyConnQueue) == -1)
//        {
//            //sem_wait成功返回0，信号量的值会减去1；
//            //错误的话信号量的值不改动，返回-1，errno设定来标识错误
//            //error type: 
//            //EINTR: The call was interrupted by a signal handler; see signal(7)
//            //EINVAL: semEventSendQueue is not a valid semaphores           
//            if (errno != EINTR) //EINTR不算错误
//                ngx_log_stderr(errno, "In CSocekt::ServerRecyConnThread, "
//                    "func sem_wait(&semRecyConnQueue) failed!.");
//        }
//
//        if (g_stopEvent != 0) //要求整个进程退出
//            break;
//
//        if (recyConnQueue.size2 > 0)
//        {
//            while (__sync_lock_test_and_set(&connLOCK, 1))
//            { usleep(0); } //ATOMIC LCOK FOR recyConnQueue
//
//            pConn = recyConnQueue.head2;
//            recyConnQueue.head2 = NULL;
//            recyConnQueue.tail2 = NULL;
//            recyConnQueue.size2 = 0;
//
//            //release ATOMIC LCOK FOR recyConnQueue
//            __sync_lock_release(&connLOCK);
//
//            while (pConn)
//            {
//                pConn1 = pConn;
//                pConn = pConn->nextConn; //指向下一个连接
//                if (pConn1->timerStatus == 0)
//                {
//                    //首先让心跳包定时器失效(无论有没有创建，都可以调用)，
//                    //再启动连接的idle定时器
//                    //if (pSocketObj->m_ifkickTimeCount == 1)
//                    timeWheel.DisableTimer(pConn1->timerEntryPing);
//                    timeWheel.StartTimer(pConn1->timerEntryRecy);
//                    pConn1->timerStatus = 1; //将timerStatus置为1，回收状态
//                    //===============================test======================
//                    //ngx_log_stderr(0, "pConn->timerStatus == 1");
//                    //===============================test======================
//                    //onlineUserCount--; //连入用户数量-1
//                }
//                else if (pConn1->timerStatus == 1)
//                {
//                    //pConn->timerStatus == 1 为回收状态，直接把连接回收到自由链表
//                    pSocketObj->ngx_free_connection(pConn1);
//                }
//            }
//        }
//
//        //while (__sync_lock_test_and_set(&connLOCK, 1))
//        //{ usleep(0); } //ATOMIC LCOK FOR recyConnQueue
//        //if (recyConnQueue.head2 == NULL)
//        //    pConn = NULL;
//        //else
//        //{
//        //    pConn = recyConnQueue.head2;
//        //    recyConnQueue.head2 = (recyConnQueue.head2)->nextConn;
//        //    if (recyConnQueue.head2 == NULL)
//        //        recyConnQueue.tail2 = NULL;
//        //    --recyConnQueue.size2;
//        //}
//        ////release ATOMIC LCOK FOR recyConnQueue
//        //__sync_lock_release(/*&pSocketObj->*/&connLOCK);
//        //if (pConn)
//        //{
//        //    if (pConn->timerStatus == 0)
//        //    {
//        //        //首先让心跳包定时器失效(无论有没有创建，都可以调用)，再创建连接的idle定时器
//        //        timeWheel.DisableTimer(pConn->timerEntryPing);
//        //        pConn->timerEntryRecy = timeWheel.CreateTimer(SetConnToIdle,
//        //            pConn, pSocketObj->m_RecyConnWaitTime, 0);
//        //        pConn->timerStatus = 1; //将timerStatus置为1，回收状态
//        //        --pSocketObj->onlineUserCount; //连入用户数量-1
//        //    }
//        //    else if (pConn->timerStatus == 1)
//        //    {
//        //        //pConn->timerStatus == 1 为回收状态，直接把连接回收到自由链表
//        //        pSocketObj->ngx_free_connection(pConn);
//        //    }
//        //}
//    }
//    return (void*)0;
//}
