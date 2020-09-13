//������ �� ��ʱ�� �йصĺ���������
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>    //uintptr_t
#include <stdarg.h>    //va_start....
#include <unistd.h>    //STDERR_FILENO��
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


void //��ʱ���ں�ֱ�ӽ������ӷŵ�recyConnQueue�У��û����̴߳���
CSocket::SetConnToIdle(void* pConnVoid)
{
	lpngx_connection_t pConn;
	pConn = (lpngx_connection_t)pConnVoid;
    //if (pConn == NULL) return;

    while (__sync_lock_test_and_set(&connLOCK, 1)) //ATOMIC LCOK FOR recyConnQueue
    { usleep(0); }

    pConn->nextConn = NULL;
    ++recyConnQueue.size2;
    if (recyConnQueue.tail2 == NULL)
    {
        recyConnQueue.head2 = pConn;
        recyConnQueue.tail2 = pConn;
    }
    else
    {
        (recyConnQueue.tail2)->nextConn = pConn;
        recyConnQueue.tail2 = pConn;
    }

    __sync_lock_release(&connLOCK); //release ATOMIC LCOK FOR recyConnQueue

    //���ź�����ֵ+1��������������sem_wait�ľͿ�������ȥ��sem_post�Ǹ��ź�����ֵ����
    //һ��1������һ��ԭ�Ӳ�������ͬʱ��ͬһ���ź�������1�����������߳��ǲ����ͻ��
    if (sem_post(&semRecyConnQueue) == -1)
    {
        ngx_log_stderr(0, "In CSocket::ngx_recycle_connection, "
            "func sem_post(&semRecyConnQueue) failed.");
    }

    //=======================================test====================================
    //ngx_log_stderr(0, "SetConnToIdle executed!");
    //=======================================test====================================
}

void 
CSocket::PingTimeout(void* pConnVoid)
{
	lpngx_connection_t pConn;
	pConn = (lpngx_connection_t)pConnVoid;
    if (pConn == NULL) return;

    while (__sync_lock_test_and_set(&connLOCK, 1)) //ATOMIC LCOK FOR recyConnQueue
    { usleep(0); }

    //��ֹ�����е�socket_id���ظ��ر�
    if (pConn->fd == -1)
    {
        __sync_lock_release(&connLOCK); //release ATOMIC LCOK FOR recyConnQueue
        return;
    }

    if (close(pConn->fd) == -1)
    {
        ngx_log_error_core(NGX_LOG_ALERT, errno,
            "In CSocket::ngx_recycle_connection, func close(%d) failed!",
            pConn->fd);
    }

    pConn->nextConn = NULL;
    ++recyConnQueue.size2;
    if (recyConnQueue.tail2 == NULL)
    {
        recyConnQueue.head2 = pConn;
        recyConnQueue.tail2 = pConn;
    }
    else
    {
        (recyConnQueue.tail2)->nextConn = pConn;
        recyConnQueue.tail2 = pConn;
    }

    pConn->fd = -1; //�ٷ�nginx��ôд����ôд������  
    ++pConn->iCurrsequence;

    __sync_lock_release(&connLOCK); //release ATOMIC LCOK FOR recyConnQueue

    //���ź�����ֵ+1��������������sem_wait�ľͿ�������ȥ��sem_post�Ǹ��ź�����ֵ����
    //һ��1������һ��ԭ�Ӳ�������ͬʱ��ͬһ���ź�������1�����������߳��ǲ����ͻ��
    if (sem_post(&semRecyConnQueue) == -1)
    {
        ngx_log_stderr(0, "In CSocket::PingTimeout, "
            "func sem_post(&semRecyConnQueue) failed.");
    }

    pConn->pIOthread->connCount--; //ӵ��pConn���ӵ�IO�߳�������-1
    onlineUserCount--; //�����û�����-1
    int tmp = onlineUserCount;  
    ngx_log_stderr(0, "In CSocket::PingTimeout, "
        "connection for [%s] is closed! The onlineUserCount: %d",
	    pConn->addr_text, tmp);
}

void //ÿ��5�룬����Ļ��ӡ�����������Ϣ
CSocket::PrintInfo(void* pConnVoid)
{
    int tmp = onlineUserCount;
    ngx_log_stderr(0, "============================ My Server ==========================");
    for (int i = 0; i < IOThreadCount; i++)
    {
        int tmp = IOThreads[i].connCount;
        ngx_log_stderr(0, "Thread_%d: has %d conns", i, tmp);
    }
    ngx_log_stderr(0, "onlineUserCount: %d", tmp);
    ngx_log_stderr(0, "recvMsgQueue Size: %d", recvMsgQueue.size);
    ngx_log_stderr(0, "sendMsgQueue Size: %d", sendMsgQueue.size);
    ngx_log_stderr(0, "sendingQueue Size: %d", sendingQueue.size);
    ngx_log_stderr(0, "m_free_connection_n: %d", m_free_connection_n);
    ngx_log_stderr(0, "m_total_connection_n: %d", m_total_connection_n);
    ngx_log_stderr(0, "============================ My Server ==========================");
}
