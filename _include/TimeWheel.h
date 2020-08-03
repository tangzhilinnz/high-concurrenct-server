#ifndef __TIME_WHEEL_H_
#define __TIME_WHEEL_H_

#include <pthread.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "ngx_func.h"

typedef char int8;
typedef unsigned char uint8;
typedef uint8 byte;
typedef short int16;
typedef unsigned short uint16;
typedef long int32;
typedef unsigned long uint32;

#define CONFIG_BASE_SMALL 0    // TVN_SIZE=64  TVR_SIZE=256
#define TVN_BITS (CONFIG_BASE_SMALL ? 4 : 6) // 6
#define TVR_BITS (CONFIG_BASE_SMALL ? 6 : 8) // 8
#define TVN_SIZE (1 << TVN_BITS)
#define TVR_SIZE (1 << TVR_BITS)
#define TVN_MASK (TVN_SIZE - 1) // 63
#define TVR_MASK (TVR_SIZE - 1) // 255
#define MAX_TVAL ((unsigned long)((1ULL << (TVR_BITS + 4*TVN_BITS)) - 1))

#define TIME_AFTER(a,b) ((long)(b) - (long)(a) < 0)
#define TIME_BEFORE(a,b) TIME_AFTER(b,a)
#define TIME_AFTER_EQ(a,b) ((long)(a) - (long)(b) >= 0)
#define TIME_BEFORE_EQ(a,b) TIME_AFTER_EQ(b,a)

struct LIST_TIMER
{
    LIST_TIMER* pPrev;
    LIST_TIMER* pNext;
};

struct TIMER_NODE
{
    LIST_TIMER ltTimer;       // ��ʱ��˫����������
    uint32 uExpires;          // ��ʱ������ʱ��
    uint32 uPeriod;           // ��ʱ���������ٴδ����ļ��ʱ�������Ϊ 0����ʾ�ö�ʱ��Ϊһ���Ե�
    void (*timerFn)(void*);   // ��ʱ���ص�����
    void* pParam;             // �ص������Ĳ���
    uint32 isEffective;       // �Ƿ����ʱ�����ϣ�1 �ѹ��ϣ���Ч״̬��0 δ���ϣ���Ч״̬
};

class TimeWheel
{
public:
    //���캯������������
    TimeWheel() : thread_((pthread_t)0), uJiffies_(0) {}
    virtual ~TimeWheel() { DestroyTimeWheel(); }

//private:
    static uint32 init_;
    static pthread_mutex_t  mutexLock_; // ͬ����
    static uint32 uExitFlag_;  // �߳��˳���ʶ(0:Continue, other: Exit)

    pthread_t  thread_; // �߳̾��
    uint32 uJiffies_;   // ��׼ʱ��(��ǰʱ��)����λ������
    //1��ʱ���֡��������ʾ�洢δ���� 0 ~ 255 ����ļ�ʱ����tick ������Ϊ 1 ����
    LIST_TIMER arrListTimer1_[TVR_SIZE]; //256
    //2��ʱ���֡��洢δ���� 256 ~ 256*64-1 ����ļ�ʱ����tick ������Ϊ 256 ����
    LIST_TIMER arrListTimer2_[TVN_SIZE]; //64 
    //3��ʱ���֡��洢δ���� 256*64 ~ 256*64*64-1 ����ļ�ʱ����tick ������Ϊ 256*64 ����
    LIST_TIMER arrListTimer3_[TVN_SIZE];
    //4��ʱ���֡��洢δ���� 256*64*64 ~ 256*64*64*64-1 ����ļ�ʱ����tick ������Ϊ 256*64*64 ����
    LIST_TIMER arrListTimer4_[TVN_SIZE];
    //5��ʱ���֡��洢δ���� 256*64*64*64 ~ 256*64*64*64*64-1 ����ļ�ʱ����tick ������Ϊ 256*64*64*64 ����
    LIST_TIMER arrListTimer5_[TVN_SIZE];

public:
    void SleepMilliseconds(uint32 uMs);
    bool CreateTimeWheel(void);           //����ʱ���ֶ�ʱ������
    void DestroyTimeWheel(void);       //ɾ��ʱ���ֶ�ʱ������

    TIMER_NODE* CreateTimer(              //����һ����ʱ��
        void (*timerFn)(void*), //timerFn �ص�������ַ
        void* pParam,           //pParam �ص������Ĳ���
        uint32 uDueTime,        //uDueTime �״δ����ĳ�ʱʱ����
        uint32 uPeriod);        //uPeriod ��ʱ��ѭ�����ڣ���Ϊ0����ö�ʱ��ֻ����һ��

    int32 ModifyTimer(              //�޸�һ����ʱ��
        TIMER_NODE* lpTimer,    //��Ҫ���޸ĵĶ�ʱ���ڵ�ָ��
        void (*timerFn)(void*), //timerFn �ص�������ַ
        void* pParam,           //pParam �ص������Ĳ���
        uint32 uDueTime,        //uDueTime �״δ����ĳ�ʱʱ����
        uint32 uPeriod);        //uPeriod ��ʱ��ѭ�����ڣ���Ϊ0����ö�ʱ��ֻ����һ��

    int32 InvalidateTimer(TIMER_NODE* lpTimer); //�ö�ʱ��ʧЧ������ɾ��
    int32 DeleteTimer(TIMER_NODE* lpTimer);  //ɾ����ʱ��

private:
    uint32 GetJiffies_old(void); //��ȡ��׼ʱ��(����)
    uint32 GetJiffies(void);
    void ListTimerInsert( //��˫��ѭ��������½����뵽��� pPrev �� pNext ֮��
        LIST_TIMER* newTP, LIST_TIMER* prevTP, LIST_TIMER* nextTP);
    void ListTimerInsertHead(LIST_TIMER* newTP, LIST_TIMER* headTP);
    void ListTimerInsertTail(LIST_TIMER* newTP, LIST_TIMER* headTP);

    //��ʼ��ʱ�����е����� tick����ʼ����ÿ�� tick �е�˫������ֻ��һ��ͷ���
    void InitArrayListTimer(LIST_TIMER* arrListTimer, uint32 nSize);
    //ʹ���½��newTP�滻oldTP��˫��ѭ�������е�λ�á����˫�������н���һ�����oldTP��
    //ʹ��newTP�滻��ͬ��������һ�����newTP
    void ListTimerReplace(LIST_TIMER* oldTP, LIST_TIMER* newTP); 
    //ʹ���½��newTP�滻oldTP��˫��ѭ�������е�λ�á�
    void ListTimerReplaceInit(LIST_TIMER* oldTP, LIST_TIMER* newTP);
    //ɾ��arrListTimer�ϵ����м�ʱ���ڵ�
    void DeleteArrayListTimer(LIST_TIMER* arrListTimer, uint32 uSize);
    //���ݼ�ʱ���Ľ���ʱ���������ʱ���֡��ڸ�ʱ�����ϵ� tick��Ȼ���¼�ʱ�������뵽
    //�� tick ��˫��ѭ�������β��������ǰ��Ҫ������
    void AddTimer(TIMER_NODE* pTmr);
    //����ʱ���� arrlistTimer ��˫��ѭ�����������еļ�ʱ�����ݵ���ʱ����뵽ָ����ʱ������
    uint32 CascadeTimer(LIST_TIMER* arrListTimer, uint32 idx);

    void RunTimer(void);

private:
    static void* ThreadRunTimer(void* pParam); // ��ʱ���̣߳��� 1 ����Ϊ��λ���м�ʱ

    class TimeWheelMutex //�Ի��������м���
    {
    public:
        explicit TimeWheelMutex(pthread_mutex_t* pMutex)
        {
            m_pMutex = pMutex;
            pthread_mutex_lock(m_pMutex);
        }
        ~TimeWheelMutex()
        {
            pthread_mutex_unlock(m_pMutex);
        }
    private:
        pthread_mutex_t* m_pMutex;
    };

#define TimeWheelMutex(x) error "Missing guard object name"
};


#endif