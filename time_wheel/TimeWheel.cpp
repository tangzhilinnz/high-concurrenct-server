#include "TimeWheel.h"

pthread_mutex_t  TimeWheel::mutexLock_ = PTHREAD_MUTEX_INITIALIZER;
uint32 TimeWheel::init_ = 0;
uint32 TimeWheel::uExitFlag_ = 0;

//获取基准时间(毫秒)
uint32
TimeWheel::GetJiffies_old(void)
{
    //int gettimeofday(struct timeval*tv, struct timezone *tz);
    //其参数tv是保存获取时间结果的结构体，参数tz用于保存时区结果：
    //struct timeval{
    //    long int tv_sec;  //seconds
    //    long int tv_usec; //microseconds
    //};
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

//获取基准时间(毫秒)
uint32
TimeWheel::GetJiffies(void)
{
    //int clock_gettime(clockid_t, struct timespec *);
    //struct timespec {
    //    time_t tv_sec; //seconds 
    //    long tv_nsec;  //nanoseconds 
    //};
    struct timespec ts; //精确到纳秒(10的-9次方秒)
    //使用 clock_gettime 函数时，有些系统需连接rt库，加 -lrt 参数，有些不需要连接rt库
    //CLOCK_MONOTONIC 表示从系统启动这一刻起开始计时，不受系统时间被用户改变的影响
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

//将双向循环链表的新结点插入到结点 pPrev 和 pNext 之间
void
TimeWheel::ListTimerInsert(LIST_TIMER* newTP, LIST_TIMER* prevTP, LIST_TIMER* nextTP)
{
    nextTP->pPrev = newTP;
    newTP->pNext = nextTP;
    newTP->pPrev = prevTP;
    prevTP->pNext = newTP;
}

void
TimeWheel::ListTimerInsertHead(LIST_TIMER* newTP, LIST_TIMER* headTP)
{
    ListTimerInsert(newTP, headTP, headTP->pNext);
}

void
TimeWheel::ListTimerInsertTail(LIST_TIMER* newTP, LIST_TIMER* headTP)
{
    ListTimerInsert(newTP, headTP->pPrev, headTP);
}

//使用新结点newTP替换oldTP在双向循环链表中的位置。如果双向链表中仅有一个结点oldTP，
//使用newTP替换后，同样，仅有一个结点newTP
void
TimeWheel::ListTimerReplace(LIST_TIMER* oldTP, LIST_TIMER* newTP)
{
    newTP->pNext = oldTP->pNext;
    newTP->pNext->pPrev = newTP;
    newTP->pPrev = oldTP->pPrev;
    newTP->pPrev->pNext = newTP;
}

//使用新结点newTP替换oldTP在双向循环链表中的位置。
void
TimeWheel::ListTimerReplaceInit(LIST_TIMER* oldTP, LIST_TIMER* newTP)
{
    ListTimerReplace(oldTP, newTP);
    //使用 pNew 替换 pOld 在双向循环链表中的位置后，pOld 结点从链表中独立出来了，
    //所以要让 pOld 指向自己
    oldTP->pNext = oldTP;
    oldTP->pPrev = oldTP;
}

//初始化时间轮中的所有 tick。初始化后，每个 tick 中的双向链表只有一个头结点
void
TimeWheel::InitArrayListTimer(LIST_TIMER* arrListTimer, uint32 nSize)
{
    uint32 i;
    for (i = 0; i < nSize; i++)
    {
        arrListTimer[i].pPrev = &arrListTimer[i];
        arrListTimer[i].pNext = &arrListTimer[i];
    }
}

//删除arrListTimer上的所有计时器节点
void
TimeWheel::DeleteArrayListTimer(LIST_TIMER* arrListTimer, uint32 uSize)
{
    LIST_TIMER listTmr, * pListTimer;
    TIMER_NODE* pTmr;
    uint32 idx;

    for (idx = 0; idx < uSize; idx++)
    {
        ListTimerReplaceInit(&arrListTimer[idx], &listTmr);
        pListTimer = listTmr.pNext;
        while (pListTimer != &listTmr)
        {
            //offsetof(s,m) ((size_t)&(((s*)0)->m))
            pTmr = (TIMER_NODE*)((uint8*)pListTimer - offsetof(TIMER_NODE, ltTimer));
            pListTimer = pListTimer->pNext;
            free(pTmr);
        }
    }
}

//根据计时器的结束时间计算所属时间轮、在该时间轮上的 tick、
//然后将新计时器结点插入到该 tick 的双向循环链表的尾部，调用前需要被互斥
void
TimeWheel::AddTimer(TIMER_NODE* pTmr)
{
    LIST_TIMER* pHead;
    uint32 i, uDueTime, uExpires;

    uExpires = pTmr->uExpires; //= uJiffies_ + uDueTime
    uDueTime = uExpires - uJiffies_; 
    if (uDueTime < TVR_SIZE)   //idx < 256
    {
        i = uExpires & TVR_MASK; // expires & 255
        pHead = &arrListTimer1_[i];
    }
    else if (uDueTime < 1 << (TVR_BITS + TVN_BITS)) //idx < 256*64
    {
        i = (uExpires >> TVR_BITS) & TVN_MASK; //(expires>>8) & 63
        pHead = &arrListTimer2_[i];
    }
    else if (uDueTime < 1 << (TVR_BITS + 2 * TVN_BITS)) //idx < 256*64*64
    {
        i = (uExpires >> (TVR_BITS + TVN_BITS)) & TVN_MASK; //(expires>>14) & 63
        pHead = &arrListTimer3_[i];
    }
    else if (uDueTime < 1 << (TVR_BITS + 3 * TVN_BITS)) //idx < 256*64*64*64
    {
        i = (uExpires >> (TVR_BITS + 2 * TVN_BITS)) & TVN_MASK; //(expires>>20) & 63
        pHead = &arrListTimer4_[i];
    }
    else if ((signed long)uDueTime < 0)
    {
        /* Can happen if you add a timer with uExpires == uJiffies_,
         * or you set a timer to go off in the past
         */
        pHead = &arrListTimer1_[(uJiffies_ & TVR_MASK)];
    }
    else // 256*64*64*64 <= idx <= 256*64*64*64*64 - 1
    {
        /* If the timeout is larger than 0xffffffff on 64-bit
         * architectures then we use the maximum timeout:
         */
        if (uDueTime > 0xffffffffUL)
        {
            uDueTime = 0xffffffffUL;
            uExpires = uDueTime + uJiffies_;
        }
        i = (uExpires >> (TVR_BITS + 3 * TVN_BITS)) & TVN_MASK; //(expires>>26) & 63
        pHead = &arrListTimer5_[i];
    }

    ListTimerInsertTail(&(pTmr->ltTimer), pHead);
}

//遍历时间轮 arrlistTimer 的双向循环链表，将其中的计时器根据到期时间加入到指定的时间轮中
uint32
TimeWheel::CascadeTimer(LIST_TIMER* arrListTimer, uint32 idx)
{
    LIST_TIMER listTmr, * pListTimer;
    TIMER_NODE* pTmr;

    ListTimerReplaceInit(&arrListTimer[idx], &listTmr);
    pListTimer = listTmr.pNext;
    //遍历双向循环链表，添加定时器
    while (pListTimer != &listTmr)
    {
        pTmr = (TIMER_NODE*)((uint8*)pListTimer - offsetof(TIMER_NODE, ltTimer));
        pListTimer = pListTimer->pNext;
        AddTimer(pTmr);
    }

    return idx;
}

void
TimeWheel::RunTimer(void)
{
    //TVN_MASK= 63( 111 111 )
#define INDEX(N) ((uJiffies_ >> (TVR_BITS + (N) * TVN_BITS)) & TVN_MASK)
    uint32 idx, uJiffies;
    LIST_TIMER listTmrExpire, * pListTmrExpire;
    TIMER_NODE* pTmr;

    uJiffies = GetJiffies();

    pthread_mutex_lock(&mutexLock_);

    while (TIME_AFTER_EQ(uJiffies, uJiffies_))
    {
        //unint32共32bit，idx为当前时间的低8bit，INDEX(0)为次6bit，INDEX(1)为再次6bit，
        //INDEX(2)为再次6bit，INDEX(3)为高6bit
        idx = uJiffies_ & TVR_MASK; //255
        if (!idx &&
            !CascadeTimer(arrListTimer2_, INDEX(0)) &&
            !CascadeTimer(arrListTimer3_, INDEX(1)) &&
            !CascadeTimer(arrListTimer4_, INDEX(2)))
            CascadeTimer(arrListTimer5_, INDEX(3));

        //新结点 pListTmrExpire 替换 arrListTimer1[idx] 后，双向循环链表 arrListTimer1[idx] 
        //就只有它自己一个结点了。pListTmrExpire 成为双向循环链表的入口
        pListTmrExpire = &listTmrExpire;
        ListTimerReplaceInit(&arrListTimer1_[idx], pListTmrExpire);
        //遍历时间轮arrListTimer1的双向循环链表，执行该链表所有定时器的回调函数
        pListTmrExpire = pListTmrExpire->pNext;
        while (pListTmrExpire != &listTmrExpire)
        {
            pTmr = (TIMER_NODE*)((uint8*)pListTmrExpire - offsetof(TIMER_NODE, ltTimer));
            pListTmrExpire = pListTmrExpire->pNext;
            pTmr->timerFn(pTmr->pParam);

            if (pTmr->uPeriod != 0)
            {
                pTmr->uExpires = uJiffies_ + pTmr->uPeriod;
                AddTimer(pTmr);
            }
            else free(pTmr);
        }

        uJiffies_++;
    }

    pthread_mutex_unlock(&mutexLock_);
}

// 计时器线程，以 1 毫秒为单位进行计时
void*
TimeWheel::ThreadRunTimer(void* pParam)
{
    TimeWheel* pTimeWheel;

    pTimeWheel = (TimeWheel*)pParam;
    if (pTimeWheel == NULL) return NULL;

    while (!uExitFlag_)
    {
        pTimeWheel->RunTimer();
        pTimeWheel->SleepMilliseconds(1); //线程睡1毫秒
    }

    return NULL;
}

//睡uMs毫秒
void
TimeWheel::SleepMilliseconds(uint32 uMs)
{
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = uMs * 1000;  // tv.tv_usec 单位是微秒
    select(0, NULL, NULL, NULL, &tv);
}

//创建定时器管理器
bool
TimeWheel::CreateTimeWheel(void)
{
    if (init_ == 1) return false;

    thread_ = (pthread_t)0;
    uExitFlag_ = 0;

    int err = pthread_mutex_init(&mutexLock_, NULL);
    if (err != 0)
    {
        /*ngx_log_stderr(err, "In TimeWheel::CreateTimeWheel, "
            "func pthread_mutex_init for mutexLock_ failed!");*/
        return false;
    }

    InitArrayListTimer(arrListTimer1_, 
        sizeof(arrListTimer1_) / sizeof(arrListTimer1_[0]));
    InitArrayListTimer(arrListTimer2_,
        sizeof(arrListTimer2_) / sizeof(arrListTimer2_[0]));
    InitArrayListTimer(arrListTimer3_,
        sizeof(arrListTimer3_) / sizeof(arrListTimer3_[0]));
    InitArrayListTimer(arrListTimer4_,
        sizeof(arrListTimer4_) / sizeof(arrListTimer4_[0]));
    InitArrayListTimer(arrListTimer5_,
        sizeof(arrListTimer5_) / sizeof(arrListTimer5_[0]));

    uJiffies_ = GetJiffies();
    err = pthread_create(&thread_, NULL, ThreadRunTimer, this);
    if (err != 0)
    {
        //创建线程有错
        /*ngx_log_stderr(err, "In TimeWheel::CreateTimeWheel, "
            "func pthread_create failed to create TimeWheel thread!";*/
        return false;
    }

    init_ = 1;
    return true;
}

//删除定时器管理器
void
TimeWheel::DestroyTimeWheel(void)
{
    if (init_ == 0) return;

    uExitFlag_ = 1;

    if (thread_ != (pthread_t)0)
    {
        pthread_join(thread_, NULL);
        thread_ = (pthread_t)0;
    }

    DeleteArrayListTimer(arrListTimer1_,
        sizeof(arrListTimer1_) / sizeof(arrListTimer1_[0]));
    DeleteArrayListTimer(arrListTimer2_,
        sizeof(arrListTimer2_) / sizeof(arrListTimer2_[0]));
    DeleteArrayListTimer(arrListTimer3_,
        sizeof(arrListTimer3_) / sizeof(arrListTimer3_[0]));
    DeleteArrayListTimer(arrListTimer4_,
        sizeof(arrListTimer4_) / sizeof(arrListTimer4_[0]));
    DeleteArrayListTimer(arrListTimer5_,
        sizeof(arrListTimer5_) / sizeof(arrListTimer5_[0]));

    pthread_mutex_destroy(&mutexLock_);
}

//创建一个定时器
TIMER_NODE*
TimeWheel::CreateTimer(
    void (*timerFn)(void*), //timerFn 回调函数地址
    void* pParam,           //pParam 回调函数的参数
    uint32 uDueTime,        //uDueTime 首次触发的超时时间间隔
    uint32 uPeriod)       //uPeriod 定时器循环周期，若为0，则该定时器只运行一次
{
    TIMER_NODE* pTmr = NULL;
    if (NULL == timerFn || init_ == 0) return NULL;

    pTmr = (TIMER_NODE*)malloc(sizeof(TIMER_NODE));
    if (pTmr != NULL)
    {
        pTmr->uPeriod = uPeriod;
        pTmr->timerFn = timerFn;
        pTmr->pParam = pParam;

        pthread_mutex_lock(&mutexLock_);
        pTmr->uExpires = uJiffies_ + uDueTime;
        AddTimer(pTmr);
        pthread_mutex_unlock(&mutexLock_);
    }

    return pTmr;
}

//修改一个定时器
int32
TimeWheel::ModifyTimer(            
    TIMER_NODE* lpTimer,    //需要被修改的定时器节点指针
    void (*timerFn)(void*), //新的timerFn 回调函数地址
    void* pParam,           //新的pParam 回调函数的参数
    uint32 uDueTime,        //新的uDueTime 首次触发的超时时间间隔
    uint32 uPeriod)    //新的uPeriod 定时器循环周期，若为0，则该定时器只运行一次
{
    LIST_TIMER* pListTmr;
    //TIMER_NODE* pTmr;

    if (init_ == 1 && NULL != lpTimer)
    {
        pthread_mutex_lock(&mutexLock_);
        lpTimer->uPeriod = uPeriod;
        lpTimer->timerFn = timerFn;
        lpTimer->pParam = pParam;
        pListTmr = &lpTimer->ltTimer;
        pListTmr->pPrev->pNext = pListTmr->pNext;
        pListTmr->pNext->pPrev = pListTmr->pPrev;
        lpTimer->uExpires = uJiffies_ + uDueTime;
        AddTimer(lpTimer);
        pthread_mutex_unlock(&mutexLock_);
        return 0;
    }
    else
        return -1;
}

//删除定时器
int32
TimeWheel::DeleteTimer(TIMER_NODE* lpTimer)
{
    LIST_TIMER* pListTmr;

    if (init_ == 1 && NULL != lpTimer)
    {
        pthread_mutex_lock(&mutexLock_);
        pListTmr = &lpTimer->ltTimer;
        pListTmr->pPrev->pNext = pListTmr->pNext;
        pListTmr->pNext->pPrev = pListTmr->pPrev;
        free(lpTimer);
        pthread_mutex_unlock(&mutexLock_);
        return 0;
    }
    else
        return -1;
}