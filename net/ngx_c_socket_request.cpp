//和网络  中 客户端请求数据有关的代码
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
#include <pthread.h>   //多线程

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
//#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"  //自动释放互斥量的一个类
#include "ngx_c_crc32.h"

//=============================================================================
//通知并唤醒该连接所在的epoll，该连接有上事件需要处理
void
CSocket::ThreadEventNotify(lpngx_connection_t pConn)
{
    uint64_t one = 1;
    //int epollHandle = pConn->pIOthread->epollHandle;
    int evtfd = pConn->pIOthread->evtfd;
    ssize_t n = write(evtfd, &one, sizeof(uint64_t));
    if (n != sizeof(uint64_t))
    {
        ngx_log_stderr(errno, "In CSocekt::ThreadEventNotify(), "
            "write() falied to write 8 bytes!");
    }
}

//epoll被唤醒后，调用该连接的 EventRespond()读取收到的通知
void
CSocket::ThreadEventRespond(lpngx_connection_t pConn)
{
    uint64_t u;

    int evtfd = pConn->pIOthread->evtfd;
    ssize_t n = read(evtfd, &u, sizeof(uint64_t));
    if (n != sizeof(uint64_t))
    {
        ngx_log_stderr(errno, "In CSocekt::ThreadEventRespond(), "
            "read() falied to read 8 bytes!");
    }
    //============test=========================================
    ngx_log_stderr(0, "In CSocekt::ThreadEventRespond(), " 
        "read %ud (0x%xd) from evtfd\n", u, u);
}

//used to release message queues of MESSAGE_QUEUE type
//in some scenarios may need critical lock applied by the caller
void
CSocket::MsgQueueRelease(MESSAGE_QUEUE* pMsgQueue)
{   
    LPSTRUC_MSG_HEADER pTmpMsg;

    if (pMsgQueue == NULL) return;

    while (pMsgQueue->head)
    {
        pTmpMsg = pMsgQueue->head;
        pMsgQueue->head = pMsgQueue->head->nextMsg;
        free(pTmpMsg);
    }
    pMsgQueue->tail = NULL;
    pMsgQueue->size = 0;
    return;
}

//当连接上有数据来的时候，本函数会被EpollProcessIO(-1)所调用，
//官方的类似函数为ngx_http_wait_request_handler();
void 
CSocket::ngx_read_request_handler(lpngx_connection_t pConn)
{
    //====================================test=================================
    ssize_t n = 0;
    n = recvproc(pConn, pConn->pRecvBuff, connBuffSize); //n <= connBuffSize 
    pConn->recvMsgRead = pConn->pRecvBuff;
    //pConn->buffLength += n;
    //ngx_log_stderr(0, "recv Length: %d", n);

    while (n > 0 && pConn->fd != -1)
    {
        //成功收到了一些字节(n>0)，就要开始判断收到了多少数据了
        if (pConn->curStat == _PKG_HD_INIT)
        {
            //收到的消息长度正好大于或等于一个包头长度
            if (n >= lenPkgHeader) 
            {
                //recvMsgRead移动一个包头长度，读取一个包头长度，所以剩余数据长度需
                //减去lenPkgHeader
                memcpy(pConn->precvbuf, pConn->recvMsgRead, lenPkgHeader);
                pConn->recvMsgRead = pConn->recvMsgRead + lenPkgHeader;
                n -= lenPkgHeader;
                ngx_read_request_handler_PKGcheck(pConn); //检验是不是真包头
                //continue;
            }
            else //n < lenPkgHeader 收到消息长度不足一个包头长度
            {
                memcpy(pConn->precvbuf, pConn->recvMsgRead, n);
                pConn->curStat = _PKG_HD_RECVING; //包头不完整，继续接收包头
                pConn->precvbuf = pConn->precvbuf + n; //注意收后续包的内存往后走
                pConn->irecvlen = pConn->irecvlen - n; //包头要收的内容也相应减少
                n = 0;
                //continue;
            }
            continue;
        }

        else if (pConn->curStat == _PKG_HD_RECVING)
        {
            //实际收到的宽度大于或等于要求收到的宽度
            if (n >= pConn->irecvlen)
            {
                memcpy(pConn->precvbuf, pConn->recvMsgRead, pConn->irecvlen);
                pConn->recvMsgRead = pConn->recvMsgRead + pConn->irecvlen;
                n -= pConn->irecvlen;
                ngx_read_request_handler_PKGcheck(pConn); //检验是不是真包头
            }
            else //n < lenPkgHeader 收到消息长度仍然不足一个包头长度
            {
                memcpy(pConn->precvbuf, pConn->recvMsgRead, n);
                //pConn->curStat = _PKG_HD_RECVING; //包头不完整，继续接收包头中	
                pConn->precvbuf = pConn->precvbuf + n; //注意收后续包的内存往后走
                pConn->irecvlen = pConn->irecvlen - n; //包头要收的内容也相应减少
                n = 0;
            }
            continue;
        }

        else if (pConn->curStat == _PKG_HD_CHECKING)
        {
            memcpy(pConn->precvbuf, pConn->recvMsgRead, pConn->irecvlen);
            memcpy(pConn->dataHeadInfo, pConn->dataHeadInfo + 1, lenPkgHeader);
            ++pConn->recvMsgHead;
            --n;
            ngx_read_request_handler_PKGcheck(pConn); //检验是不是真包头
            continue;
        }

        else if (pConn->curStat == _PKG_BD_INIT)
        {
            //包头已接收完毕并验证为真包头，开始接收包体
            if (n >= pConn->irecvlen)
            {
                //收到的宽度等于要收的宽度，包体也收完整了
                memcpy(pConn->precvbuf, pConn->recvMsgRead, pConn->irecvlen);
                pConn->recvMsgRead = pConn->recvMsgRead + pConn->irecvlen;
                n -= pConn->irecvlen;
                ngx_read_request_handler_proc_plast(pConn);
            }
            else //n < pConn->irecvlen
            {
                //收到的宽度小于要收的宽度
                memcpy(pConn->precvbuf, pConn->recvMsgRead, n);
                pConn->curStat = _PKG_BD_RECVING;
                pConn->precvbuf = pConn->precvbuf + n;
                pConn->irecvlen = pConn->irecvlen - n;
                n = 0;
            }
            continue;
        }

        else if (pConn->curStat == _PKG_BD_RECVING)
        {
            //包头已接收完毕并验证为真包头，开始接收包体
            if (n >= pConn->irecvlen)
            {
                //收到的宽度等于要收的宽度，包体也收完整了
                memcpy(pConn->precvbuf, pConn->recvMsgRead, pConn->irecvlen);
                pConn->recvMsgRead = pConn->recvMsgRead + pConn->irecvlen;
                n -= pConn->irecvlen;
                ngx_read_request_handler_proc_plast(pConn);
            }
            else //n < pConn->irecvlen
            {
                //收到的宽度小于要收的宽度
                memcpy(pConn->precvbuf, pConn->recvMsgRead, n);
                //pConn->curStat = _PKG_BD_RECVING;
                pConn->precvbuf = pConn->precvbuf + n;
                pConn->irecvlen = pConn->irecvlen - n;
                n = 0;
            }
            continue;
        }      
    }

    //连接已关闭，释放 解包后未放入收消息队列的消息
    if (pConn->fd == -1) MsgQueueRelease(&(pConn->unpackMsgQue));
    //将解包的消息放入收消息队列(pConn->fd != -1)
    else
    {
        if (pConn->unpackMsgQue.size > 0)
            inMsgRecvQueue(&(pConn->unpackMsgQue));
    }

    return;

    ////收包，注意我们用的第二个和第三个参数，我们用的始终是这两个参数，因此我们必须保证
    ////c->precvbuf指向正确的收包位置，保证c->irecvlen指向正确的收包宽度
    ////c->precvbuf初始指向c->dataHeadInfo
    //ssize_t reco = recvproc(pConn, pConn->precvbuf, pConn->irecvlen);
    //if (reco <= 0)
    //{
    //    return;      
    //}
    ////成功收到了一些字节(reco>0)，就要开始判断收到了多少数据了     
    //if (pConn->curStat == _PKG_HD_INIT) 
    //{
    //    //lenPkgHeader == c->irecvlen
    //    if (reco == lenPkgHeader) //正好收到完整包头，进行校验 
    //    {
    //        ngx_read_request_handler_PKGcheck(pConn); //校验包头函数
    //    }
    //    else //reco < lenPkgHeader == c->irecvlen
    //    {
    //        //收到的包头不完整--我们不能预料每个包的长度，也不能预料各种拆包/粘包情况，
    //        //所以收到不完整包头(也算是缺包)是很可能的
    //        pConn->curStat = _PKG_HD_RECVING; //包头不完整，继续接收包头中	
    //        pConn->precvbuf = pConn->precvbuf + reco; //注意收后续包的内存往后走
    //        pConn->irecvlen = pConn->irecvlen - reco; //要收的内容也相应减少
    //    }
    //}
    //else if (pConn->curStat == _PKG_HD_RECVING) //包头不完整，接收包头中
    //{
    //    if (reco == pConn->irecvlen) //要求收到的宽度和我实际收到的宽度相等
    //    {
    //        //包头收完整了，进行校验
    //        ngx_read_request_handler_PKGcheck(pConn); //校验包头函数
    //    }
    //    else //reco < c->irecvlen
    //    {
    //        //包头还是没收完整，继续收包头
    //        //c->curStat = _PKG_HD_RECVING; 
    //        pConn->precvbuf = pConn->precvbuf + reco; //注意收后续包的内存往后走
    //        pConn->irecvlen = pConn->irecvlen - reco; //要收的内容也相应减少
    //    }
    //}
    //else if (pConn->curStat == _PKG_HD_CHECKING) //校验包头
    //{
    //    ngx_read_request_handler_PKGcheck(pConn); //校验包头函数
    //}
    //else if (pConn->curStat == _PKG_BD_INIT)
    //{
    //    //包头刚好收完，准备接收包体
    //    if (reco == pConn->irecvlen)
    //    {
    //        //收到的宽度等于要收的宽度，包体也收完整了
    //        ngx_read_request_handler_proc_plast(pConn);
    //    }
    //    else //reco < c->irecvlen
    //    {
    //        //收到的宽度小于要收的宽度
    //        pConn->curStat = _PKG_BD_RECVING;
    //        pConn->precvbuf = pConn->precvbuf + reco;
    //        pConn->irecvlen = pConn->irecvlen - reco;
    //    }
    //}
    //else if (pConn->curStat == _PKG_BD_RECVING)
    //{
    //    //接收包体中，包体不完整，继续接收中
    //    if (reco == pConn->irecvlen)
    //    {
    //        //包体收完整了
    //        ngx_read_request_handler_proc_plast(pConn);
    //    }
    //    else //reco < c->irecvlen
    //    {
    //        //包体没收完整，继续收
    //        pConn->precvbuf = pConn->precvbuf + reco;
    //        pConn->irecvlen = pConn->irecvlen - reco;
    //    }
    //}  
}

//接收数据专用函数--引入这个函数是为了方便，如果断线，错误之类的，这里直接释放连接池中
//连接，然后直接关闭socket，以免在其他函数中还要重复的干这些事
//参数PConn： 连接池中相关连接
//参数buff：  接收数据的缓冲区
//参数buflen：要接收的数据大小
//返回值：-1，则是有问题发生并且在这里把问题处理完毕，调用本函数的调用者可以直接return
//       >0，则是表示实际收到的字节数
ssize_t //ssize_t是有符号整型，在32位机器上等同与int，在64位机器上等同与long int，
        //size_t就是无符号型的ssize_t
CSocket::recvproc(lpngx_connection_t PConn, char* buff, ssize_t buflen)
{
    ssize_t n;

    n = recv(PConn->fd, buff, buflen, 0); //recv()系统函数，最后一个参数flag，一般为0；     
    if (n == 0)
    {
        //客户端关闭(应该是正常完成了4次挥手)，直接回收连接，关闭socket即可 
        ngx_log_stderr(0, "connection was closed by the client(by TCP four waves)");
        //=====================================================================
        ngx_recycle_connection(PConn);
        //=====================================================================
        return -1;
    }
    //客户端没断，走这里 
    if (n < 0) //这被认为有错误发生
    {
        //EAGAIN和EWOULDBLOCK[这个应该常用在http上]，表示没收到数据，一般来讲，
        //在ET模式下会出现这个错误，因为ET模式下是不停的recv肯定有一个时刻收到这
        //个errno，但LT模式下一般是来事件才收，所以不该出现这个返回值
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            //LT模式不该出现这个errno，而且这个其实也不是错误，所以不当做错误处理
            ngx_log_stderr(errno, "In CSocket::recvproc, condition(errno == "
                "EAGAIN || errno == EWOULDBLOCK) was unexpectedly established!");
            return -1; //不当做错误处理，只是简单返回
        }
        //EINTR错误的产生：当阻塞于某个慢系统调用的一个进程捕获某个信号且相应信号处理
        //函数返回时，该系统调用可能返回一个EINTR错误。例如：在socket服务器端，设置了
        //信号捕获机制，有子进程，当在父进程阻塞于慢系统调用时由父进程捕获到了一个有效
        //信号时，内核会致使accept返回一个EINTR错误(被中断的系统调用)。
        if (errno == EINTR)  //这个不算错误，参考官方nginx
        {
            //LT模式不该出现这个errno，而且这个其实也不是错误，所以不当做错误处理
            ngx_log_stderr(errno, "In CSocket::recvproc, condition(errno == "
                "EINTR) was unexpectedly established!");
            return -1; //不当做错误处理，只是简单返回
        }

        //所有走下来的错误，都认为异常：意味着我们要关闭客户端套接字并回连接       
        if (errno == ECONNRESET)  //#define ECONNRESET 104 
                                  /* Connection reset by peer */
        {
            //如果客户端非正常关闭socket连接，像关闭整个运行程序(应该是直接给服务
            //器发送rst包而不是4次挥手包完成连接断开)，会产生这个错误，linux环境
            //104(ECONNRESET)--
            //1 接收端recv或者read，对端已经关闭连接，recv/read返回该错误
            //2 对端重启连接，还未建立连接
            //3 发送端已经断开连接，但是调用send会触发这个错误
            ngx_log_stderr(errno, "In CSocket::recvproc, an error occurred "
                "while receiving data!");
        }
        else
        {
            //能走到这里的，都表示错误，打印错误信息到屏幕上和日志
            ngx_log_stderr(errno, "In CSocket::recvproc, an error occurred "
                "while receiving data!");
        }

        ngx_log_stderr(errno, "connection was closed by the client(Abnormally)!");
        //=====================================================================
        //这种真正的错误就要，直接关闭套接字，释放连接池中连接了
        ngx_recycle_connection(PConn);
        //=====================================================================
        return -1;
    }

    //能走到这里的，就认为收到了有效数据，返回收到的字节数
    return n; 
}

//包头收完整后的处理，进行校验
void 
CSocket::ngx_read_request_handler_PKGcheck(lpngx_connection_t pConn)
{
    //CCRC32* p_crc32 = CCRC32::GetInstance();

    LPCOMM_PKG_HEADER pPkgHeader;
    //包头完整接收后，包头信息肯定是在precvbuf_hstar里，将precvbuf_hstar从char*类型
    //转换为LPCOMM_PKG_HEADER类型(_COMM_PKG_HEADER*)
    //pPkgHeader = (LPCOMM_PKG_HEADER)pConn->precvbuf_hstar;
    pPkgHeader = (LPCOMM_PKG_HEADER)pConn->dataHeadInfo;

    //进行包头的CRC32效验，判定此包是否为错误包，恶意包或畸形包(非常重要)
    int crc32_header = ntohl(pPkgHeader->crc32_h); //4字节数据的网络序转主机序
    /*int calcrc_h = p_crc32->Get_CRC((unsigned char*)pPkgHeader,
        lenPkgHeader - sizeof(int));*/ //计算纯包头的crc值

    int calcrc_h = CRC32((unsigned char*)pPkgHeader, lenPkgHeader - sizeof(int));

    if (calcrc_h != crc32_header) //服务器端根据包头计算crc_h值，和客户端传递
                                  //过来的包头中的crc32_h信息比较
    {
        //ngx_log_stderr(0, "%d", pConn->wrongPKGAdmit);
        if ((pConn->wrongPKGAdmit)-- <= 0)
        {
            ngx_log_stderr(0, "In CSocket::ngx_read_request_handler_PKGcheck, "
                "the number of packet check failures has exceeded the allowed value, "
                "the connection is closed for safety!");
            //=================================================================
            ngx_recycle_connection(pConn); //丢弃此包，关闭连接后退出该函数 
            //=================================================================
            return;
        }

        //if (pConn->curStat == _PKG_HD_CHECKING)
        //{
        //    if (pConn->precvbuf_hstar - pConn->dataHeadInfo > 180)
        //    {
        //        ngx_memcpy(pConn->dataHeadInfo, pConn->precvbuf_hstar, lenPkgHeader);
        //        pConn->precvbuf_hstar = pConn->dataHeadInfo;
        //    }
        //    pConn->precvbuf = pConn->precvbuf_hstar + lenPkgHeader;
        //    ++pConn->precvbuf_hstar;
        //    ngx_log_stderr(0, "In CSocket::ngx_read_request_handler_PKGcheck, "
        //        "a malicious, malformed or error packet is found!");
        //    return;
        //}
        ////准备开始逐一校验后续数据有无正确的包头，校验无效的数据都丢弃
        //pConn->curStat = _PKG_HD_CHECKING;
        //pConn->precvbuf = pConn->precvbuf_hstar + lenPkgHeader;
        //++pConn->precvbuf_hstar;
        //pConn->irecvlen = 1;
        //ngx_log_stderr(0, "In CSocket::ngx_read_request_handler_PKGcheck, "
        //    "a malicious, malformed or error packet is found!");

        if (pConn->curStat != _PKG_HD_CHECKING)
        {
            pConn->curStat = _PKG_HD_CHECKING;
            pConn->irecvlen = 1;
            pConn->precvbuf = pConn->dataHeadInfo + lenPkgHeader;
            ngx_log_stderr(0, "In CSocket::ngx_read_request_handler_PKGcheck, "
            "a malicious, malformed or error packet is found!");
        }
    }
    else
    {
        ngx_read_request_handler_proc_pfirst(pConn);
        //pConn->curStat = _PKG_BD_INIT;
    }
    return;
}

//包头进行校验后，进行包处理阶段1(pfirst)
void 
CSocket::ngx_read_request_handler_proc_pfirst(lpngx_connection_t pConn)
{
    //CMemory* p_memory = CMemory::GetInstance();

    LPCOMM_PKG_HEADER pPkgHeader;
    //包头校验成功后，包头信息肯定是在precvbuf_hstar里，将precvbuf_hstar从char*类型
    //转换为LPCOMM_PKG_HEADER类型(_COMM_PKG_HEADER*)
    //pPkgHeader = (LPCOMM_PKG_HEADER)pConn->precvbuf_hstar; 
    pPkgHeader = (LPCOMM_PKG_HEADER)pConn->dataHeadInfo;
    unsigned short e_pkgLen;
    e_pkgLen = ntohs(pPkgHeader->pkgLen); //2字节数据的网络序转主机序

    //错误包的初步判断，筛查客户端程序设计是否有漏洞，造成数据包长度超出规定范围
    if (e_pkgLen < lenPkgHeader)
    {
        //伪造包/或者错误包，整个包长是包头+包体，就算包体为0字节，
        //那么至少e_pkgLen == lenPkgHeader = sizeof(COMM_PKG_HEADER)
        //遇到这样的畸形或者错误包最安全的方式是直接关闭连接，检查数据包设计漏洞
        ngx_log_stderr(0, "In CSocket::ngx_read_request_handler_proc_pfirst, "
            "one packet length is smaller than the packet header length, "
            "the connection is closed for safety!");
        //=====================================================================
        ngx_recycle_connection(pConn);
        //=====================================================================
    }
    else if (e_pkgLen > (_PKG_MAX_LENGTH - 1000)) //客户端发来包长度>29000，恶意包
    {
        //恶意包，太大，认定非法用户，废包【包头中说这个包总长度这么大，这不行】
        //遇到这样的畸形或者错误包最安全的方式是直接关闭连接，检查数据包设计漏洞
        ngx_log_stderr(0, "In CSocket::ngx_read_request_handler_proc_pfirst, "
            "one packet length is greater than the upper limit, "
            "the connection is closed for safety!");
        //=====================================================================
        ngx_recycle_connection(pConn);
        //=====================================================================
    }
    else
    {
        //无明显错误的包头，后续消息处理线程将进一步校验消息包是否合理   
        //=====================================================================
        //分配内存，长度(消息头+包头+包体)
        char* pTmpBuffer = (char*)malloc(lenMsgHeader + e_pkgLen);
        //ngx_log_stderr(0, "=%d!", lenMsgHeader + e_pkgLen);
        pConn->precvMemPointer = pTmpBuffer;  //内存开始指针
        //=====================================================================

        //a)先填写消息头内容
        LPSTRUC_MSG_HEADER ptmpMsgHeader = (LPSTRUC_MSG_HEADER)pTmpBuffer;
        //ptmpMsgHeader->nextMsg = NULL; //刚开始每一个消息头不指向任何其他消息
        ptmpMsgHeader->pConn = pConn;
        ptmpMsgHeader->iCurrsequence = pConn->iCurrsequence; //收到包时的连接池中连
                                                            //接序号记录到消息头里来
        //b)再填写包头内容
        pTmpBuffer += lenMsgHeader;                 //往后跳，跳过消息头，指向包头
        memcpy(pTmpBuffer, pPkgHeader, lenPkgHeader); //直接把收到的包头拷贝进来
        if (e_pkgLen == lenPkgHeader)
        {
            //该报文只有包头无包体【我们允许一个包只有包头，没有包体】
            //这相当于收完整了，则直接入消息队列待后续业务逻辑线程去处理
            ngx_read_request_handler_proc_plast(pConn);
        }
        else //e_pkgLen > lenPkgHeader
        {
            //开始收包体，注意我的写法
            pConn->curStat = _PKG_BD_INIT; //当前状态改变，包头刚好收完，准备接收包体	    
            pConn->precvbuf = pTmpBuffer + lenPkgHeader; //pTmpBuffer指向包头，
                                                  //+lenPkgHeader后指向包体位置
            pConn->irecvlen = e_pkgLen - lenPkgHeader; //e_pkgLen(包头+包体)- 
                                                     //lenPkgHeader(包头)=包体
        }
    }
    return;
}

//收到一个完整包后的处理(plast表示最后阶段)
void 
CSocket::ngx_read_request_handler_proc_plast(lpngx_connection_t pConn)
{
    //把这段内存放到消息队列中来；
    /*int irmqc = 0;*/  //消息队列当前信息数量
    //=========================================================================
    //inMsgRecvQueue(pConn->precvMemPointer/*, irmqc*/); //返回消息队列当前信息数量irmqc，
                                           //是调用本函数后的消息队列中消息数量
    //激发唤醒线程池中的某个线程来处理业务逻辑
    //g_threadpool.Call(/*irmqc*/);

    LPSTRUC_MSG_HEADER msg = 
        reinterpret_cast<LPSTRUC_MSG_HEADER>(pConn->precvMemPointer);
    //刚开始每一个消息头不指向任何其他消息
    msg->preMsg = NULL; 
    msg->nextMsg = NULL;

    //pConn->unpackMsgQue;

    ++((pConn->unpackMsgQue).size);

    //ngx_log_stderr(0, "=%d", (pConn->unpackMsgQue).size);

    if ((pConn->unpackMsgQue).tail == NULL)
    {
        (pConn->unpackMsgQue).head = msg;
        (pConn->unpackMsgQue).tail = msg;
    }
    else
    {
        ((pConn->unpackMsgQue).tail)->nextMsg = msg;
        msg->preMsg = (pConn->unpackMsgQue).tail;
        (pConn->unpackMsgQue).tail = msg;
    }

    //=========================================================================
    pConn->precvMemPointer = NULL;
    pConn->curStat =  _PKG_HD_INIT; //收包状态机恢复为原始态，为收下一个包做准备  
    //pConn->precvbuf_hstar = pConn->dataHeadInfo; //设置好收包包头的起始位置
    pConn->precvbuf = pConn->dataHeadInfo; //设置好收包的存储位置
    pConn->irecvlen = lenPkgHeader; //设置好要接收数据的大小
    return;
}

//当收到一个完整包之后，将完整包入消息队列，这个包在服务器端应该是 消息头+包头+包体 格式
//参数：返回接收消息队列当前信息数量irmqc，因为临界着，所以这个值也是OK的；
void 
CSocket::inMsgRecvQueue(/*char* buf*/MESSAGE_QUEUE* pMsgQueue)
{
    //=========================================================================
    //CLock lock(&m_recvMessageQueueMutex);  //自动加锁解锁很方便，不需要手工去解锁了
     //收入接收消息队列前，对包的消息头做一个记录，标记此消息的序号
    //LPSTRUC_MSG_HEADER ptmpMsgHeader = (LPSTRUC_MSG_HEADER)buf;
    //ptmpMsgHeader->msgSeqNum = (ptmpMsgHeader->pConn)->recvdMsgCount;
    //++(ptmpMsgHeader->pConn)->recvdMsgCount; //+1代表成功收到一条消息并入队列
    //m_MsgRecvQueue.push_back(buf);	       //入消息队列
    //++m_iRecvMsgQueueCount;                //收消息队列数字+1，个人认为用变量更方便
                                           //一点，比 m_MsgRecvQueue.size()高效
    //irmqc = m_iRecvMsgQueueCount;          //接收消息队列当前信息数量保存到irmqc
    //=========================================================================
    int err;
    ////因为是多线程操作，所以对于每一个pConn，其pConn->precvMemPointer可能会在被传入
    ////前一刻被置空(当pConn被ngx_recycle_connection回收)，在此必须检查buf是否为空
    //if (buf == NULL) return; 
    //LPSTRUC_MSG_HEADER msg = reinterpret_cast<LPSTRUC_MSG_HEADER>(buf);
    //msg->preMsg = NULL;
    //msg->nextMsg = NULL;
    //while (__sync_lock_test_and_set(&recvLOCK, 1)) //ATOMIC LCOK FOR READ
    //{ usleep(0); }
    //++recvMsgQueue.size;
    //if (recvMsgQueue.tail == NULL)
    //{
    //    recvMsgQueue.head = msg;
    //    recvMsgQueue.tail = msg;
    //}
    //else
    //{
    //    (recvMsgQueue.tail)->nextMsg = msg;
    //    msg->preMsg = recvMsgQueue.tail;
    //    recvMsgQueue.tail = msg;
    //}

    if (pMsgQueue->head == NULL) return;

    while (__sync_lock_test_and_set(&recvLOCK, 1)) //ATOMIC LCOK FOR READ
    { usleep(0); }

    if (recvMsgQueue.tail == NULL) //recvMsgQueue is empty
    {
        recvMsgQueue.head = pMsgQueue->head;
        recvMsgQueue.tail = pMsgQueue->tail;
    }
    else //recvMsgQueue.tail != NULL
    {
        (recvMsgQueue.tail)->nextMsg = pMsgQueue->head;
        (pMsgQueue->head)->preMsg = recvMsgQueue.tail;
        recvMsgQueue.tail = pMsgQueue->tail;
    }

    recvMsgQueue.size += pMsgQueue->size;
    pMsgQueue->head = NULL;
    pMsgQueue->tail = NULL;
    pMsgQueue->size = 0;
    //ngx_log_stderr(err, "= %d!", recvMsgQueue.size);

    //唤醒一个等待该条件的线程，也就是可以唤醒卡在pthread_cond_wait的线程
    err = pthread_cond_signal(&(CThreadPool::m_pthreadCond));
    if (err != 0)
    {
        ngx_log_stderr(err, "In CThreadPool::Call, "
            "func pthread_cond_signal failed, the returned errno is %d!", err);
        return;
    }

    __sync_lock_release(&recvLOCK); //ATOMIC LCOK FOR READ

    return;
}

//专用于处理消息的线程(线程池)从消息队列中把一个包提取出来处理
char* 
CSocket::outMsgRecvQueue()
{
    //=========================================================================

    //CLock lock(&m_recvMessageQueueMutex); //互斥
    //if (m_MsgRecvQueue.empty())
    //{
    //    return NULL;       
    //}
    //char* sTmpMsgBuf = m_MsgRecvQueue.front(); //返回第一个元素但不检查元素存在与否
    //m_MsgRecvQueue.pop_front();                //移除第一个元素但不返回	
    //--m_iRecvMsgQueueCount;                    //收消息队列数字-1
    //return sTmpMsgBuf;
    //if (recvMsgQueue.size == 0)
    //{ return NULL; }

    char* pMsg;
    while (__sync_lock_test_and_set(&recvLOCK, 1)) //ATOMIC LCOK FOR READ
    { usleep(0); }

    if (recvMsgQueue.head == NULL)
    {
        __sync_lock_release(&recvLOCK); //ATOMIC LCOK FOR READ
        return NULL;
    }

    pMsg = reinterpret_cast<char*>(recvMsgQueue.head);
    recvMsgQueue.head = (recvMsgQueue.head)->nextMsg;
    if (recvMsgQueue.head == NULL)
        recvMsgQueue.tail = NULL;
    else
        (recvMsgQueue.head)->preMsg = NULL;

    --recvMsgQueue.size;
    //ngx_log_stderr(0, "= %d!", recvMsgQueue.size);
    //if (recvMsgQueue.head == NULL)
    //{
    //    ngx_log_stderr(0, "yes head == NULl");
    //}
    //else
    //{
    //    ngx_log_stderr(0, "no head != NULl");
    //}

    __sync_lock_release(&recvLOCK); //ATOMIC LCOK FOR READ

    return pMsg;
    //=========================================================================
}

//-----------------------------------------------------------------------------
//发送数据专用函数，返回本次发送的字节数
//返回>0，成功发送了一些字节
//=0，估计对方断了
//-1，errno == EAGAIN ，本方发送缓冲区满了
//-2，errno != EAGAIN != EWOULDBLOCK != EINTR ，一般我认为都是对端断开的错误
//ssize_t是有符号整型，在32位机器上等同与int，在64位机器上等同与long int，size_t
//就是无符号型的ssize_t
ssize_t
CSocket::sendproc(lpngx_connection_t PConn, char* buff, ssize_t size)
{
    ssize_t n;

    for (;;)
    {
        n = send(PConn->fd, buff, size, 0); //send系统函数，最后一个参数flag，一般为0； 
        if (n > 0) //成功发送了一些数据
        {
            //发送成功一些数据，但发送了多少，我们这里不关心，也不需要再次send
            //这里有两种情况
            //(1)n == size也就是想发送多少都发送成功了，这表示完全发完毕了；
            //(2)n < size没法送完毕，发送缓冲区满了，直接返回
            return n; //返回本次发送的字节数
        }

        if (n == 0)
        {
            //send返回0？ 一般recv返回0表示断开，send返回0，当做正常处理
            //我个人认为send返回0，要么你发送的字节是0，要么对端可能断开
            //网上资料：send=0表示超时，对方主动关闭了连接过程；
            //遵循一个原则，连接断开，我们并不在send动作里处理，集中到recv处理，
            //否则send，recv都处理连接断开会乱套，因为send函数的调用再另一个线程
            //连接断开epoll会通知并且recvproc里会处理，不在这里处理
	    //========================================================================
	    //ngx_recycle_connection(PConn);
	    //========================================================================
            return 0;
        }

        //n == -1 and errno has been set 
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return -1; //内核缓冲区满，这个不算错误
        }
        else if (errno == EINTR)
        {
            //这个应该也不算错误 ，收到某个信号导致send产生这个错误？
            //参考官方的写法，打印个日志，等下次for循环重新send试一次了
            ngx_log_stderr(errno, "In CSocket::sendproc, send() failed!");
            return -1;
        }
        else
        {
            //send函数原型：
            //int send(int s, const void *msg, int len, unsigned int flag);
            //errno == EBADF：   参数s非合法的socket处理代码
            //errno == EFAULT：  参数中有一指针指向无法存取的内存空间
            //errno == ENOTSOCK：参数s为一文件描述符，非socket

            //走到这里表示的是其他errno，都表示错误，错误也不断开socket，我也依然等待
            //recvproc来统一处理断开，因为引入多线程，如果sendproc和recvproc同时处
            //理断开，很难处理好
            
	    //=====================================================================
	    //ngx_recycle_connection(PConn);
	    //=====================================================================				
	    return -2;
        }
    }
}

//设置数据发送时的写处理函数，当数据可写时epoll通知我们，在
//int CSocekt::EpollProcessIO(int timer)中调用此函数
//数据没法送完毕，要继续发送
void 
CSocket::ngx_write_request_handler(lpngx_connection_t pConn)
{
    //CMemory* p_memory = CMemory::GetInstance();

    ssize_t sendsize = sendproc(pConn, pConn->psendbuf, pConn->isendlen);

    if (sendsize > 0 && sendsize != pConn->isendlen)
    {
        //没有全部发送完毕，那么记录发送到了哪里，剩余多少，方便下次sendproc时使用
        pConn->psendbuf = pConn->psendbuf + sendsize;
        pConn->isendlen = pConn->isendlen - sendsize;
        return;
    }

    else if (sendsize == -1)
    {
        //可能是信号中断，发送缓冲区满不太可能，因为只有可以发送数据时才有通知
        ngx_log_stderr(errno, "In CSocekt::ngx_write_request_handler, "
            "condition sendsize == -1 is set by some signal interruption!");
        return;
    }

    else if (sendsize > 0 && sendsize == pConn->isendlen) //成功发送完毕
    {
        //如果是成功的发送完毕数据，则把写事件通知从epoll中干掉吧；
        //如果是断线，系统会自动把连接节点从红黑树中干掉，不用管
        //pConn->whandler = &CSocket::ngx_null_request_handler;
        if (ngx_epoll_oper_event(
            pConn->pIOthread->epollHandle,
            pConn->fd,      //socket句柄
            EPOLL_CTL_MOD,  //这里是修改，因为我们准备减去写通知
            EPOLLOUT,       //EPOLLOUT：可写的时候通知我
            1,              //事件类型为EPOLL_CTL_MOD，0(增加)1(去掉)2(覆盖)
            pConn           //连接池中的连接
        ) == -1)
        {
            //设置失败，就回收连接
            ngx_recycle_connection(pConn); //在工作进程的主线程中调用
            ngx_log_stderr(errno, "In CSocekt::ngx_write_request_handler, "
                "func ngx_epoll_oper_event failed!");
        }
        //=====================================================================
        /*p_memory->FreeMemory(pConn->psendMemPointer);*/ //释放内存
        free(pConn->psendMemPointer);
        //=====================================================================
        pConn->psendMemPointer = NULL;
        //++pConn->sentMsgCount; //成功发送消息后，sentMsgCount加1进行统计
        /*ngx_log_stderr(0, "In CSocekt::ngx_write_request_handler, "
            "a message has been sent successfully!");*/
        pConn->sendBufFull = 0;
    }
    
    else if (sendsize == -2 || sendsize == 0) //对端断开了，执行收尾工作
    {
        /*p_memory->FreeMemory(pConn->psendMemPointer);*/ //释放内存
        free(pConn->psendMemPointer);
        pConn->psendMemPointer = NULL;
        pConn->sendBufFull = 0;
    }

    //让发送辅助线程往下走，判断是否还有该连接的其他未发送消息
    //sem_post是给信号量的值加上一个1，它是一个原子操作，即同时对同一个信号量做加1操作
    //的两个线程是不会冲突的
    //=========================================================================
    if (sem_post(&semEventSendQueue) == -1)
    {
        ngx_log_stderr(0, "In CSocekt::ngx_write_request_handler, "
            "func sem_post(&semEventSendQueue) failed!");
    }
    //=========================================================================
    return;
}

//-----------------------------------------------------------------------------
//消息处理线程主函数，专门处理各种接收到的TCP消息
//pMsgBuf：发送过来的消息缓冲区，消息本身是自解释的，通过包头可以计算整个包长
//         消息本身格式【消息头+包头+包体】 
void 
CSocket::threadRecvProcFunc(char* pMsgBuf) { return; }
