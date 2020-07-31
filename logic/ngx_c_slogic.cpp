//和网络以及逻辑处理 有关的函数放这里
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
#include "ngx_c_crc32.h"
#include "ngx_c_slogic.h"  
#include "ngx_logiccomm.h"  
#include "ngx_c_lockmutex.h" 

//定义成员函数指针
typedef bool (CLogicSocket::*handler)(
    lpngx_connection_t pConn,       //连接池中连接的指针
    LPSTRUC_MSG_HEADER pMsgHeader,  //消息头指针
    char *pPkgBody,                 //包体指针
    unsigned short iBodyLength);    //包体长度

//用来保存成员函数指针的数组
static const handler statusHandler[] = 
{
    //数组前5个元素，保留，以备将来增加一些基本服务器功能
    NULL,                           //【0】：下标从0开始
    NULL,                           //【1】：下标从0开始
    NULL,                           //【2】：下标从0开始
    NULL,                           //【3】：下标从0开始
    NULL,                           //【4】：下标从0开始

    //开始处理具体的业务逻辑
    &CLogicSocket::_HandleRegister, //【5】：实现注册功能
    &CLogicSocket::_HandleLogIn,    //【6】：实现登录功能
    &CLogicSocket::_HandleTest,     //【7】：实现测试功能
    //..................
};

//整个命令有多少个，编译时即可知道
#define AUTH_TOTAL_COMMANDS sizeof(statusHandler)/sizeof(handler) 

//size_t		lenPkgHeader;	//sizeof(COMM_PKG_HEADER);		
//size_t        lenMsgHeader;    //sizeof(STRUC_MSG_HEADER);
//在CLogicSocket类的父类CSocket的构造函数中已初始化

//构造函数
CLogicSocket::CLogicSocket() {}
//析构函数
CLogicSocket::~CLogicSocket() 
{
    ngx_log_stderr(0, "~CLogicSocket() executed, "
        "global object g_socket was detroyed!");
}

//初始化函数, 在ngx_master_process_cycle之前调用(fork子进程之前)
//成功返回true，失败返回false
bool 
CLogicSocket::Initialize()
{
    //做一些和本类相关的初始化工作
    //....日后根据需要扩展     
    bool bParentInit = CSocket::Initialize(); //调用父类的同名函数
    return bParentInit;
}

//处理收到的数据包
//pMsgBuf：消息头+包头+包体
void 
CLogicSocket::threadRecvProcFunc(char* pMsgBuf)
{
    LPSTRUC_MSG_HEADER pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf; //消息头
    lpngx_connection_t p_Conn = pMsgHeader->pConn; //消息头中藏着连接对象指针

    //先判断客户端断是否断开，再检查数据包的其他项
    //ngx_recycle_connection回收连接到回收队列，iCurrsequence会增加1；
    //ngx_free_connection将这个连接从回收连接队列挪到空闲连接队列，
    //iCurrsequence加1；ngx_get_connection启用这个连接，iCurrsequence再加1

    //=========================================================================================
    if (p_Conn->iCurrsequence != pMsgHeader->iCurrsequence)
    {
        return; //丢弃，客户端断开了
    }

    LPCOMM_PKG_HEADER  pPkgHeader = //包头
        (LPCOMM_PKG_HEADER)(pMsgBuf + lenMsgHeader); 
    void* pPkgBody = NULL; //指向包体的指针
    unsigned short pkglen = ntohs(pPkgHeader->pkgLen); //客户端指明的包宽度
                                                       //包头+包体
    if (lenPkgHeader == pkglen) //所有从网络上收到的2字节数据，
                                   //都要用ntohs转成本机序
    {
        //没有包体，只有包头，crc值给0，判断是否为0不用从网络序转主机序
        if (pPkgHeader->crc32 != 0) 
        {
            ngx_log_stderr(0, "In CLogicSocket::threadRecvProcFunc, "
                "CRC check failed and data discarded!");
            //_HandleWrongPKG(pMsgHeader);
            return; //crc错，直接丢弃
        }
        pPkgBody = NULL;
    }
    else //lenPkgHeader < pkglen
    {
        //有包体，走到这里
        int crc32_body = ntohl(pPkgHeader->crc32); //4字节数据的网络序转主机序
        pPkgBody =                                 //跳过消息头和包头，指向包体
            (void*)(pMsgBuf + lenMsgHeader + lenPkgHeader);

        //计算crc值判断包的完整性        
        /*int calccrc = CCRC32::GetInstance()->Get_CRC((unsigned char*)pPkgBody,
            pkglen - lenPkgHeader);*/    //计算纯包体的crc值

        int calccrc = CRC32((unsigned char*)pPkgBody, pkglen - lenPkgHeader);

        if (calccrc != crc32_body) //服务器端根据包体计算crc值，和客户端传递
                                          //过来的包头中的crc32信息比较
        {
            ngx_log_stderr(0, "In CLogicSocket::threadRecvProcFunc, "
                "CRC check failed and data discarded!");
            //_HandleWrongPKG(pMsgHeader);
            return; //crc错，直接丢弃
        }
    }

    //包crc校验OK才能走到这里    	
    unsigned short imsgCode = ntohs(pPkgHeader->msgCode); //消息代码拿出来

    //一些判断
    //(1)判断消息码，防止客户端恶意侵害我们服务器，发送一个不在服务器处理范围内的消息码
    if (imsgCode >= AUTH_TOTAL_COMMANDS) //unsigned short imsgCode无符号数不可能<0
    {
        ngx_log_stderr(0, "In CLogicSocket::threadRecvProcFunc, "
            "invalid imsgCode=%d!", imsgCode);
        //_HandleWrongPKG(pMsgHeader);
        return; //丢弃恶意包或者错误包
    }

    //能走到这里的，包没过期，不恶意，那继续判断是否有相应的处理函数
    //(2)有对应的消息处理函数吗
    if (statusHandler[imsgCode] == NULL) //用imsgCode的方式可以使查找要执行的成员函
                                         //数效率特别高
    {
        ngx_log_stderr(0, "In CLogicSocket::threadRecvProcFunc, "
            "no corresponding handler function for imsgCode=%d!", imsgCode);
        //_HandleWrongPKG(pMsgHeader);
        return;  //没有相关的处理函数
    }

    //一切正确，可以放心大胆的处理了
    //(3)调用消息码对应的成员函数来处理
    (this->*statusHandler[imsgCode])
        (p_Conn, pMsgHeader, (char*)pPkgBody, pkglen - lenPkgHeader);

    return;

    //=========================================================================================
}

//---------------------------------------------------------------------------------------
//处理各种业务逻辑

//void 
//CLogicSocket::_HandleWrongPKG(LPSTRUC_MSG_HEADER pMsgHeader)
//{
//    //将一个只带消息头的废包插入发送消息队列，当消息发送辅助线程处理到该废包时，
//    //直接丢弃，并对该消息头所带pConn的sentMsgCount成员加1，表示线程会继续按照
//    //与收消息的顺序来处理发送消息，即先收就先发的原则，不会造成消息的跳跃发送；
//    //如果客户端发送消息已经入收消息队列，再被判断为错包从而弃之的概率不大，因此
//    //将错包的消息头插入发送消息队列的情况偶尔会发生，对程序执行效率影响不大；
//    //对于某个客户端连接，只要程序不会长时间阻塞于某条消息的发送处理，整个程序就
//    //能迅速按照收包顺序依次发送这些收包的回应包，因为后一个回应包能否发送依赖于
//    //前一个回应包的成功发送
//    CMemory* p_memory = CMemory::GetInstance();
//
//    char* p_sendbuf = (char*)p_memory->AllocMemory(lenMsgHeader, false);
//
//    pMsgHeader->isToSendPKG = false;
//    memcpy(p_sendbuf, pMsgHeader, lenMsgHeader); //消息头直接拷贝
//
//    msgSend(p_sendbuf);  
//}

bool 
CLogicSocket::_HandleRegister(
    lpngx_connection_t pConn, 
    LPSTRUC_MSG_HEADER pMsgHeader, 
    char* pPkgBody, 
    unsigned short iBodyLength)
{
    //(1)首先判断包体的合法性
    if (pPkgBody == NULL) //具体根据客户端服务器约定，如果约定这个命令[msgCode]必须
                          //带包体，如果不带包体，就认为是恶意包，错包，不处理    
    {
        ngx_log_stderr(0, "In CLogicSocket::_HandleRegister, "
            "no packet body and data discarded!");
        //_HandleWrongPKG(pMsgHeader);
        return false;
    }

    int iRecvLen = sizeof(STRUCT_REGISTER);
    if (iRecvLen != iBodyLength) //发送过来的结构大小不对，认为是恶意包，错包，不处理
    {
        ngx_log_stderr(0, "In CLogicSocket::_HandleRegister, "
            "packet body length mismatch and data discarded!");
        //_HandleWrongPKG(pMsgHeader);
        return false;
    }

    //(2)对于同一个用户，可能同时发送来多个请求过来，造成多个线程同时为该用户服务，
    //比如以网游为例，用户要在商店中买A物品，又买B物品，而用户的钱只够买A或者B，
    //不够同时买A和B；那如果用户发送购买命令过来买了一次A，又买了一次B，如果是两
    //个线程来执行同一个用户的这两次不同的购买命令，很可能造成这个用户购买成功了A，
    //又购买成功了B。所以，为了稳妥起见，针对某个用户的命令，我们一般都要互斥，增加
    //互斥的变量logicPorcMutex于ngx_connection_s结构中
    //CLock lock(&pConn->logicPorcMutex);

    //(3)取得了整个发送过来的数据
    LPSTRUCT_REGISTER p_RecvInfo = (LPSTRUCT_REGISTER)pPkgBody;

    //(4)这里可能要考虑 根据业务逻辑，进一步判断收到的数据的合法性，当前该玩家的状态是
    //否适合收到这个数据等等【比如如果用户没登陆，它就不适合购买物品等等】，这里自己发
    //挥，自己根据业务需要来扩充代码... ... ... ... ...

    //(5)给客户端返回数据时，一般也是返回一个结构，这个结构内容具体由客户端/服务器协商，
    //这里我们就以给客户端也返回同样的STRUCT_REGISTER结构来举例    
    LPCOMM_PKG_HEADER pPkgHeader;
    //CMemory* p_memory = CMemory::GetInstance();
    //CCRC32*  p_crc32 = CCRC32::GetInstance();
    int iSendLen = sizeof(STRUCT_REGISTER);

    //iSendLen = 65000; //unsigned最大也就是这个值

    //a)分配要发送出去的包的内存
    /*char* p_sendbuf = (char*)p_memory->AllocMemory (
        lenMsgHeader + lenPkgHeader + iSendLen, false);*/
    char* p_sendbuf = (char*)malloc(lenMsgHeader + lenPkgHeader + iSendLen);
    //b)填充消息头
    memcpy(p_sendbuf, pMsgHeader, lenMsgHeader); //消息头直接拷贝
    //c)填充包头
    pPkgHeader = (LPCOMM_PKG_HEADER)(p_sendbuf + lenMsgHeader); //指向包头
    pPkgHeader->msgCode = _CMD_REGISTER; //消息代码，统一在ngx_logiccomm.h中定义
    pPkgHeader->msgCode = htons(pPkgHeader->msgCode); //htons主机序转网络序 
    pPkgHeader->pkgLen = htons(lenPkgHeader + iSendLen); //包头+包体 尺寸 
    //d)填充包体
    LPSTRUCT_REGISTER p_sendInfo = (LPSTRUCT_REGISTER)(
        p_sendbuf + lenMsgHeader + lenPkgHeader);	//跳过消息头+包头，就是包体
    //这里根据需要，填充要发回给客户端的内容，int类型要使用htonl转，short类型要使用htons转

    //e)包体内容全部确定好后，计算包体的crc32值
    //pPkgHeader->crc32 = p_crc32->Get_CRC((unsigned char*)p_sendInfo, iSendLen);
    pPkgHeader->crc32 = CRC32((unsigned char*)p_sendInfo, iSendLen);
    pPkgHeader->crc32 = htonl(pPkgHeader->crc32);

    //pPkgHeader->crc32_h = p_crc32->Get_CRC((unsigned char*)pPkgHeader, lenPkgHeader - sizeof(int));
    pPkgHeader->crc32_h = CRC32((unsigned char*)pPkgHeader, lenPkgHeader - sizeof(int));
    pPkgHeader->crc32_h = htonl(pPkgHeader->crc32_h);

    //usleep(300 * 1000);

    //f)发送数据包
    msgSend(p_sendbuf);
    /*
    if (ngx_epoll_oper_event(
        pConn->fd,          //socekt句柄
        EPOLL_CTL_MOD,      //事件类型，这里是修改(添加)
        EPOLLOUT,           //这里代表要添加的标志，EPOLLOUT(可写)
        0,                  //对于事件类型为EPOLL_CTL_MOD，需要这个参数，0(增加)1(去掉)2(覆盖)
        pConn               //连接池中的连接
    ) == -1)
    {
        ngx_log_stderr(errno, "In CLogicSocket::_HandleRegister, "
            "ngx_epoll_oper_event(%d,%ud,%ud,%d) failed!", 
            pConn->fd, EPOLL_CTL_MOD, EPOLLOUT, 0);
        return false;
    }*/

    //ngx_log_stderr(0, "CLogicSocket::_HandleRegister executed!");
    return true;
}

bool 
CLogicSocket::_HandleLogIn(
    lpngx_connection_t pConn, 
    LPSTRUC_MSG_HEADER pMsgHeader, 
    char* pPkgBody, 
    unsigned short iBodyLength)
{
    //(1)首先判断包体的合法性
    if (pPkgBody == NULL) //具体根据客户端服务器约定，如果约定这个命令[msgCode]必须
                          //带包体，如果不带包体，就认为是恶意包，不处理    
    {
        ngx_log_stderr(0, "In CLogicSocket::_HandleLogIn, "
            "no packet body and data discarded!");
        //_HandleWrongPKG(pMsgHeader);
        return false;
    }

    int iRecvLen = sizeof(STRUCT_LOGIN);
    if (iRecvLen != iBodyLength) //发送过来的结构大小不对，认为是恶意包，不处理
    {
        ngx_log_stderr(0, "In CLogicSocket::_HandleLogIn, "
            "packet body length mismatch and data discarded!");
        //_HandleWrongPKG(pMsgHeader);
        return false;
    }
    
    //(2)对于同一个用户，可能同时发送来多个请求过来，造成多个线程同时为该用户服务，
    //比如以网游为例，用户要在商店中买A物品，又买B物品，而用户的钱只够买A或者B，
    //不够同时买A和B；那如果用户发送购买命令过来买了一次A，又买了一次B，如果是两
    //个线程来执行同一个用户的这两次不同的购买命令，很可能造成这个用户购买成功了A，
    //又购买成功了B。所以，为了稳妥起见，针对某个用户的命令，我们一般都要互斥，增加
    //互斥的变量logicPorcMutex于ngx_connection_s结构中
    //CLock lock(&pConn->logicPorcMutex);

    //(3)取得了整个发送过来的数据
    LPSTRUCT_LOGIN p_RecvInfo = (LPSTRUCT_LOGIN)pPkgBody;

    //(4)这里可能要考虑 根据业务逻辑，进一步判断收到的数据的合法性，当前该玩家的状态是
    //否适合收到这个数据等等【比如如果用户没登陆，它就不适合购买物品等等】，这里自己发
    //挥，自己根据业务需要来扩充代码... ... ... ... ...

    //(5)给客户端返回数据时，一般也是返回一个结构，这个结构内容具体由客户端/服务器协商，
    //这里我们就以给客户端也返回同样的STRUCT_LOGIN结构来举例    
    LPCOMM_PKG_HEADER pPkgHeader;
    //CMemory* p_memory = CMemory::GetInstance();
    //CCRC32* p_crc32 = CCRC32::GetInstance();
    int iSendLen = sizeof(STRUCT_LOGIN);

    //a)分配要发送出去的包的内存
    /*char* p_sendbuf = (char*)p_memory->AllocMemory(
        lenMsgHeader + lenPkgHeader + iSendLen, false);*/
    char* p_sendbuf = (char*)malloc(lenMsgHeader + lenPkgHeader + iSendLen);
    //b)填充消息头
    memcpy(p_sendbuf, pMsgHeader, lenMsgHeader); //消息头直接拷贝
    //c)填充包头
    pPkgHeader = (LPCOMM_PKG_HEADER)(p_sendbuf + lenMsgHeader); //指向包头
    pPkgHeader->msgCode = _CMD_LOGIN; //消息代码，统一在ngx_logiccomm.h中定义
    pPkgHeader->msgCode = htons(pPkgHeader->msgCode); //htons主机序转网络序 
    pPkgHeader->pkgLen = htons(lenPkgHeader + iSendLen); //包头+包体 尺寸 
    //d)填充包体
    LPSTRUCT_LOGIN p_sendInfo = (LPSTRUCT_LOGIN)(
        p_sendbuf + lenMsgHeader + lenPkgHeader);	//跳过消息头+包头，就是包体
    //这里根据需要，填充要发回给客户端的内容，int类型要使用htonl转，short类型要使用htons转

    //e)包体内容全部确定好后，计算包体的crc32值
    //pPkgHeader->crc32 = p_crc32->Get_CRC((unsigned char*)p_sendInfo, iSendLen);
    pPkgHeader->crc32 = CRC32((unsigned char*)p_sendInfo, iSendLen);
    pPkgHeader->crc32 = htonl(pPkgHeader->crc32);

    //pPkgHeader->crc32_h = p_crc32->Get_CRC((unsigned char*)pPkgHeader, lenPkgHeader - sizeof(int));
    pPkgHeader->crc32_h = CRC32((unsigned char*)pPkgHeader, lenPkgHeader - sizeof(int));
    pPkgHeader->crc32_h = htonl(pPkgHeader->crc32_h);

    //usleep(2000*1000);

    //f)发送数据包
    msgSend(p_sendbuf);

    //ngx_log_stderr(0, "CLogicSocket::_HandleLogIn executed!");
    return true;
}

bool 
CLogicSocket::_HandleTest(
    lpngx_connection_t pConn,
    LPSTRUC_MSG_HEADER pMsgHeader,
    char* pPkgBody,
    unsigned short iBodyLength)
{
    //(1)首先判断包体的合法性
    if (pPkgBody == NULL) //具体根据客户端服务器约定，如果约定这个命令[msgCode]必须
                          //带包体，如果不带包体，就认为是恶意包，不处理    
    {
        ngx_log_stderr(0, "In CLogicSocket::_HandleTest, "
            "no packet body and data discarded!");
        //_HandleWrongPKG(pMsgHeader);
        return false;
    }

    int iRecvLen = sizeof(STRUCT_TEST);
    if (iRecvLen != iBodyLength) //发送过来的结构大小不对，认为是恶意包，不处理
    {
        ngx_log_stderr(0, "In CLogicSocket::_HandleTest, "
            "packet body length mismatch and data discarded!");
        //_HandleWrongPKG(pMsgHeader);
        return false;
    }

    //(2)对于同一个用户，可能同时发送来多个请求过来，造成多个线程同时为该用户服务，
    //比如以网游为例，用户要在商店中买A物品，又买B物品，而用户的钱只够买A或者B，
    //不够同时买A和B；那如果用户发送购买命令过来买了一次A，又买了一次B，如果是两
    //个线程来执行同一个用户的这两次不同的购买命令，很可能造成这个用户购买成功了A，
    //又购买成功了B。所以，为了稳妥起见，针对某个用户的命令，我们一般都要互斥，增加
    //互斥的变量logicPorcMutex于ngx_connection_s结构中
    //CLock lock(&pConn->logicPorcMutex);

    //(3)取得了整个发送过来的数据
    LPSTRUCT_TEST p_RecvInfo = (LPSTRUCT_TEST)pPkgBody;

    //(4)这里可能要考虑 根据业务逻辑，进一步判断收到的数据的合法性，当前该玩家的状态是
    //否适合收到这个数据等等【比如如果用户没登陆，它就不适合购买物品等等】，这里自己发
    //挥，自己根据业务需要来扩充代码... ... ... ... ...

    //(5)给客户端返回数据时，一般也是返回一个结构，这个结构内容具体由客户端/服务器协商，
    //这里我们就以给客户端也返回同样的STRUCT_TEST结构来举例    
    LPCOMM_PKG_HEADER pPkgHeader;
    //CMemory* p_memory = CMemory::GetInstance();
    //CCRC32* p_crc32 = CCRC32::GetInstance();
    int iSendLen = strlen(pPkgBody) + 1;

    //a)分配要发送出去的包的内存
    /*char* p_sendbuf = (char*)p_memory->AllocMemory(
        lenMsgHeader + lenPkgHeader + iSendLen, false);*/
    char* p_sendbuf = (char*)malloc(lenMsgHeader + lenPkgHeader + iSendLen);
    //b)填充消息头
    memcpy(p_sendbuf, pMsgHeader, lenMsgHeader); //消息头直接拷贝
    //c)填充包头
    pPkgHeader = (LPCOMM_PKG_HEADER)(p_sendbuf + lenMsgHeader); //指向包头
    pPkgHeader->msgCode = _CMD_TEST; //消息代码，统一在ngx_logiccomm.h中定义
    pPkgHeader->msgCode = htons(pPkgHeader->msgCode); //htons主机序转网络序 
    pPkgHeader->pkgLen = htons(lenPkgHeader + iSendLen); //包头+包体 尺寸 
    //d)填充包体
    LPSTRUCT_LOGIN p_sendInfo = (LPSTRUCT_LOGIN)(
        p_sendbuf + lenMsgHeader + lenPkgHeader);	//跳过消息头+包头，就是包体

    memcpy(p_sendInfo, pPkgBody, iSendLen); //包体直接拷贝


    //e)包体内容全部确定好后，计算包体的crc32值
    //pPkgHeader->crc32 = p_crc32->Get_CRC((unsigned char*)p_sendInfo, iSendLen);
    pPkgHeader->crc32 = CRC32((unsigned char*)p_sendInfo, iSendLen);
    pPkgHeader->crc32 = htonl(pPkgHeader->crc32);

    //pPkgHeader->crc32_h = p_crc32->Get_CRC((unsigned char*)pPkgHeader, lenPkgHeader - sizeof(int));
    pPkgHeader->crc32_h = CRC32((unsigned char*)pPkgHeader, lenPkgHeader - sizeof(int));
    pPkgHeader->crc32_h = htonl(pPkgHeader->crc32_h);

    //f)发送数据包
    msgSend(p_sendbuf);

    //ngx_log_stderr(0, "CLogicSocket::_HandleTest executed!");
    return true;
}