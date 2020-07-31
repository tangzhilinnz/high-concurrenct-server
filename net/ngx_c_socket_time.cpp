
//和网络 中 时间 有关的函数放这里
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

//设置踢出时钟(向multimap表中增加内容)，用户三次握手成功连入，然后我们开启了踢人开关
//[Sock_WaitTimeEnable = 1]，那么本函数被调用
void 
CSocket::AddToTimerQueue(lpngx_connection_t pConn)
{
	//CMemory* p_memory = CMemory::GetInstance();

	time_t cur_time = time(NULL);
	cur_time += m_iWaitTime; //20秒之后的时间

	CLock lock(&m_timequeueMutex); //互斥，因为要操作m_timeQueuemap了
	/*LPSTRUC_MSG_HEADER tmpMsgHeader = 
		(LPSTRUC_MSG_HEADER)p_memory->AllocMemory(lenMsgHeader, false);*/
	LPSTRUC_MSG_HEADER tmpMsgHeader = (LPSTRUC_MSG_HEADER)malloc(lenMsgHeader);
	tmpMsgHeader->pConn = pConn;
	tmpMsgHeader->iCurrsequence = pConn->iCurrsequence;
	//插入的元素被multimap底层的红黑树按键值从小到大的顺序排序
	m_timerQueuemap.insert(std::make_pair(cur_time, tmpMsgHeader));
	m_cur_size_++;  //计时队列尺寸+1
	m_timer_value_ = GetEarliestTime(); //计时队列头部时间值保存到m_timer_value_，
										//是整个队列里最小的值(最近的时间)
	return;
}

//从multimap中取得最早的时间返回去，调用者负责互斥，所以本函数不用互斥，
//调用者确保m_timeQueuemap中一定不为空
time_t 
CSocket::GetEarliestTime()
{
    std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos;	
	pos = m_timerQueuemap.begin();		
	return pos->first;	
}

//从m_timeQueuemap移除最早的时间，并把这个时间所对应的指针返回，
//调用者负责互斥，所以本函数不用互斥，
LPSTRUC_MSG_HEADER 
CSocket::RemoveFirstTimer()
{
	std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos;	
	LPSTRUC_MSG_HEADER p_tmp;
	if(m_cur_size_ <= 0)
	{
		return NULL;
	}
	pos = m_timerQueuemap.begin(); //调用者负责互斥的，这里直接操作没问题的
	p_tmp = pos->second;
	m_timerQueuemap.erase(pos);
	--m_cur_size_;
	return p_tmp;
}

//根据给的当前时间，从m_timeQueuemap找到比这个时间更早的1个节点返回去，
//这些节点都是时间超过了要处理的节点；调用者负责互斥，所以本函数不用互斥
LPSTRUC_MSG_HEADER 
CSocket::GetOverTimeTimer(time_t cur_time)
{
	//CMemory* p_memory = CMemory::GetInstance();
	LPSTRUC_MSG_HEADER ptmp;

	if (m_cur_size_ == 0 || m_timerQueuemap.empty())
		return NULL; //队列为空

	time_t earliesttime = GetEarliestTime();
	//earliesttime = time0 <= time1 <= time2 <= ... <= timen
	//如果earliesttime > cur_time，说明时间队列中没有超时的事件，
	//如果earliesttime <= cur_time，则至少时间队列的头一个事件超时，
	//后面的事件有没有超时需要进一步比较time值和cur_time的大小
	if (earliesttime <= cur_time)
	{
		//删掉m_timerQueuemap的头一个事件，并返回这个事件保存的连接信息
		ptmp = RemoveFirstTimer(); 

		//把这个事件的时间更新后再加到时间队列 
		cur_time = time(NULL);
		time_t newinquetime = cur_time + (m_iWaitTime);
		/*LPSTRUC_MSG_HEADER tmpMsgHeader = 
			(LPSTRUC_MSG_HEADER)p_memory->AllocMemory(lenMsgHeader, false);*/
		LPSTRUC_MSG_HEADER tmpMsgHeader = (LPSTRUC_MSG_HEADER)malloc(lenMsgHeader);
		tmpMsgHeader->pConn = ptmp->pConn;
		tmpMsgHeader->iCurrsequence = ptmp->iCurrsequence;
		//插入的元素被multimap底层的红黑树按键值从小到大的顺序排序
		m_timerQueuemap.insert(std::make_pair(newinquetime, tmpMsgHeader));		
		m_cur_size_++;

		if (m_cur_size_ > 0) //这个判断条件必要，因为以后我们可能在这里扩充别的代码
		{
			//计时队列头部时间值保存到m_timer_value_里
			m_timer_value_ = GetEarliestTime(); 
		}

		return ptmp;
	}

	return NULL;
}

//把指定用户tcp连接从timer表中抠出去
void 
CSocket::DeleteFromTimerQueue(lpngx_connection_t pConn)
{
	std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos, pos2, posend;
	//CMemory* p_memory = CMemory::GetInstance();

	CLock lock(&m_timequeueMutex);

	//因为实际情况可能比较复杂，将来可能还扩充代码等等，所以如下我们遍历整个队列找一圈，
	//而不是找到一次就拉倒，以免出现什么遗漏
	pos = m_timerQueuemap.begin();
	posend = m_timerQueuemap.end();
	while (pos != posend)
	{
		if (pos->second->pConn == pConn)
		{
			pos2 = pos;
			pos++;
			/*p_memory->FreeMemory(pos->second);*/ //释放动态分配的消息头内存
			free(pos->second);
			m_timerQueuemap.erase(pos2);
			--m_cur_size_; //减去一个元素，必然要把尺寸减少1个;								
		}
		else
		{
			++pos;
		}
	}

	if (m_cur_size_ > 0)
	{
		m_timer_value_ = GetEarliestTime();
	}

	return;
}

//清理时间队列中所有内容
void 
CSocket::clearAllFromTimerQueue()
{	
	std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos,posend;

	//CMemory *p_memory = CMemory::GetInstance();	
	pos = m_timerQueuemap.begin();
	posend = m_timerQueuemap.end();    
	for(; pos != posend; ++pos)
	{
		/*p_memory->FreeMemory(pos->second);*/
		free(pos->second);
	}
	m_timerQueuemap.clear();
	m_cur_size_ = 0;
}

//时间队列监视和处理线程，处理到期不发心跳包的用户踢出的线程(静态函数)
void* 
CSocket::ServerTimerQueueMonitorThread(void* threadData)
{
	ThreadItem* pThread = static_cast<ThreadItem*>(threadData);
	CSocket* pSocketObj = pThread->_pThis;

	time_t absolute_time, cur_time;
	int err;

	while (g_stopEvent == 0) //不退出
	{
		//在多线程操作中，若m_cur_size_大于0，一定能保证时间队列有元素		
		if (pSocketObj->m_cur_size_ > 0) 
		{
			cur_time = time(NULL);
	
			{
				//如果所有线程都只读取该变量的话不必加锁，因为仅读取不存在破坏数据的
				//风险，但有线程写该变量的话不管读取还是写入都要加锁(m_timer_value_)
				CLock lock(&pSocketObj->m_timequeueMutex);
				//时间队列中最近发生事情的时间放到absolute_time里
				absolute_time = pSocketObj->m_timer_value_;
			}

			if (absolute_time < cur_time) //说明有事件过期了
			{
				//时间到了，可以处理了
				std::list<LPSTRUC_MSG_HEADER> m_lsIdleList; //保存要处理的消息头
				LPSTRUC_MSG_HEADER result;

				err = pthread_mutex_lock(&pSocketObj->m_timequeueMutex);
				if (err != 0)
				{
					ngx_log_stderr(err,
						"In CSocekt::ServerTimerQueueMonitorThread, "
						"func pthread_mutex_lock failed, "
						"the returned errno is %d!", err);
				}

				while ((result = pSocketObj->GetOverTimeTimer(cur_time)) != NULL) 
				{
					//一次性把所有小于cur_time的超时节点都取出
					m_lsIdleList.push_back(result);
				}

				err = pthread_mutex_unlock(&pSocketObj->m_timequeueMutex);
				if (err != 0)
				{
					ngx_log_stderr(err,
						"In CSocekt::ServerTimerQueueMonitorThread, "
						"func pthread_mutex_unlock failed, "
						"the returned errno is %d!", err);
				}

				LPSTRUC_MSG_HEADER tmpmsg;
				while (!m_lsIdleList.empty())
				{
					tmpmsg = m_lsIdleList.front();
					m_lsIdleList.pop_front();
					//这里需要检查心跳超时问题
					pSocketObj->procPingTimeOutChecking(tmpmsg, cur_time); 
				} 
			}
		} 

		usleep(500 * 1000); //简化问题，每次循环后休息500毫秒
	} 

	return (void*)0;
}

//心跳包检测时间到，该去检测心跳包是否超时的事宜，本函数只是把内存释放，
//子类应该重新事先该函数以实现具体的判断动作
void 
CSocket::procPingTimeOutChecking(LPSTRUC_MSG_HEADER tmpmsg, time_t cur_time)
{
	//CMemory* p_memory = CMemory::GetInstance();
	/*p_memory->FreeMemory(tmpmsg);*/
	free(tmpmsg);
}


