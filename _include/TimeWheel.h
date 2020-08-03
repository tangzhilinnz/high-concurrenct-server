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
    LIST_TIMER ltTimer;       // 定时器双向链表的入口
    uint32 uExpires;          // 定时器到期时间
    uint32 uPeriod;           // 定时器触发后，再次触发的间隔时长。如果为 0，表示该定时器为一次性的
    void (*timerFn)(void*);   // 定时器回调函数
    void* pParam;             // 回调函数的参数
    uint32 isEffective;       // 是否挂在时间轮上，1 已挂上，有效状态；0 未挂上，无效状态
};

class TimeWheel
{
public:
    //构造函数，析构函数
    TimeWheel() : thread_((pthread_t)0), uJiffies_(0) {}
    virtual ~TimeWheel() { DestroyTimeWheel(); }

//private:
    static uint32 init_;
    static pthread_mutex_t  mutexLock_; // 同步锁
    static uint32 uExitFlag_;  // 线程退出标识(0:Continue, other: Exit)

    pthread_t  thread_; // 线程句柄
    uint32 uJiffies_;   // 基准时间(当前时间)，单位：毫秒
    //1级时间轮。在这里表示存储未来的 0 ~ 255 毫秒的计时器。tick 的粒度为 1 毫秒
    LIST_TIMER arrListTimer1_[TVR_SIZE]; //256
    //2级时间轮。存储未来的 256 ~ 256*64-1 毫秒的计时器。tick 的粒度为 256 毫秒
    LIST_TIMER arrListTimer2_[TVN_SIZE]; //64 
    //3级时间轮。存储未来的 256*64 ~ 256*64*64-1 毫秒的计时器。tick 的粒度为 256*64 毫秒
    LIST_TIMER arrListTimer3_[TVN_SIZE];
    //4级时间轮。存储未来的 256*64*64 ~ 256*64*64*64-1 毫秒的计时器。tick 的粒度为 256*64*64 毫秒
    LIST_TIMER arrListTimer4_[TVN_SIZE];
    //5级时间轮。存储未来的 256*64*64*64 ~ 256*64*64*64*64-1 毫秒的计时器。tick 的粒度为 256*64*64*64 毫秒
    LIST_TIMER arrListTimer5_[TVN_SIZE];

public:
    void SleepMilliseconds(uint32 uMs);
    bool CreateTimeWheel(void);           //创建时间轮定时管理器
    void DestroyTimeWheel(void);       //删除时间轮定时管理器

    TIMER_NODE* CreateTimer(              //创建一个定时器
        void (*timerFn)(void*), //timerFn 回调函数地址
        void* pParam,           //pParam 回调函数的参数
        uint32 uDueTime,        //uDueTime 首次触发的超时时间间隔
        uint32 uPeriod);        //uPeriod 定时器循环周期，若为0，则该定时器只运行一次

    int32 ModifyTimer(              //修改一个定时器
        TIMER_NODE* lpTimer,    //需要被修改的定时器节点指针
        void (*timerFn)(void*), //timerFn 回调函数地址
        void* pParam,           //pParam 回调函数的参数
        uint32 uDueTime,        //uDueTime 首次触发的超时时间间隔
        uint32 uPeriod);        //uPeriod 定时器循环周期，若为0，则该定时器只运行一次

    int32 InvalidateTimer(TIMER_NODE* lpTimer); //让定时器失效，但不删除
    int32 DeleteTimer(TIMER_NODE* lpTimer);  //删除定时器

private:
    uint32 GetJiffies_old(void); //获取基准时间(毫秒)
    uint32 GetJiffies(void);
    void ListTimerInsert( //将双向循环链表的新结点插入到结点 pPrev 和 pNext 之间
        LIST_TIMER* newTP, LIST_TIMER* prevTP, LIST_TIMER* nextTP);
    void ListTimerInsertHead(LIST_TIMER* newTP, LIST_TIMER* headTP);
    void ListTimerInsertTail(LIST_TIMER* newTP, LIST_TIMER* headTP);

    //初始化时间轮中的所有 tick。初始化后，每个 tick 中的双向链表只有一个头结点
    void InitArrayListTimer(LIST_TIMER* arrListTimer, uint32 nSize);
    //使用新结点newTP替换oldTP在双向循环链表中的位置。如果双向链表中仅有一个结点oldTP，
    //使用newTP替换后，同样，仅有一个结点newTP
    void ListTimerReplace(LIST_TIMER* oldTP, LIST_TIMER* newTP); 
    //使用新结点newTP替换oldTP在双向循环链表中的位置。
    void ListTimerReplaceInit(LIST_TIMER* oldTP, LIST_TIMER* newTP);
    //删除arrListTimer上的所有计时器节点
    void DeleteArrayListTimer(LIST_TIMER* arrListTimer, uint32 uSize);
    //根据计时器的结束时间计算所属时间轮、在该时间轮上的 tick、然后将新计时器结点插入到
    //该 tick 的双向循环链表的尾部，调用前需要被互斥
    void AddTimer(TIMER_NODE* pTmr);
    //遍历时间轮 arrlistTimer 的双向循环链表，将其中的计时器根据到期时间加入到指定的时间轮中
    uint32 CascadeTimer(LIST_TIMER* arrListTimer, uint32 idx);

    void RunTimer(void);

private:
    static void* ThreadRunTimer(void* pParam); // 计时器线程，以 1 毫秒为单位进行计时

    class TimeWheelMutex //对互斥量进行加锁
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