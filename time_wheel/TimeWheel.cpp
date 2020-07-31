#include "TimeWheel.h"

pthread_mutex_t  TimeWheel::mutexLock_ = PTHREAD_MUTEX_INITIALIZER;
uint32 TimeWheel::init_ = 0;
uint32 TimeWheel::uExitFlag_ = 0;

//��ȡ��׼ʱ��(����)
uint32
TimeWheel::GetJiffies_old(void)
{
    //int gettimeofday(struct timeval*tv, struct timezone *tz);
    //�����tv�Ǳ����ȡʱ�����Ľṹ�壬����tz���ڱ���ʱ�������
    //struct timeval{
    //    long int tv_sec;  //seconds
    //    long int tv_usec; //microseconds
    //};
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

//��ȡ��׼ʱ��(����)
uint32
TimeWheel::GetJiffies(void)
{
    //int clock_gettime(clockid_t, struct timespec *);
    //struct timespec {
    //    time_t tv_sec; //seconds 
    //    long tv_nsec;  //nanoseconds 
    //};
    struct timespec ts; //��ȷ������(10��-9�η���)
    //ʹ�� clock_gettime ����ʱ����Щϵͳ������rt�⣬�� -lrt ��������Щ����Ҫ����rt��
    //CLOCK_MONOTONIC ��ʾ��ϵͳ������һ����ʼ��ʱ������ϵͳʱ�䱻�û��ı��Ӱ��
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

//��˫��ѭ��������½����뵽��� pPrev �� pNext ֮��
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

//ʹ���½��newTP�滻oldTP��˫��ѭ�������е�λ�á����˫�������н���һ�����oldTP��
//ʹ��newTP�滻��ͬ��������һ�����newTP
void
TimeWheel::ListTimerReplace(LIST_TIMER* oldTP, LIST_TIMER* newTP)
{
    newTP->pNext = oldTP->pNext;
    newTP->pNext->pPrev = newTP;
    newTP->pPrev = oldTP->pPrev;
    newTP->pPrev->pNext = newTP;
}

//ʹ���½��newTP�滻oldTP��˫��ѭ�������е�λ�á�
void
TimeWheel::ListTimerReplaceInit(LIST_TIMER* oldTP, LIST_TIMER* newTP)
{
    ListTimerReplace(oldTP, newTP);
    //ʹ�� pNew �滻 pOld ��˫��ѭ�������е�λ�ú�pOld ���������ж��������ˣ�
    //����Ҫ�� pOld ָ���Լ�
    oldTP->pNext = oldTP;
    oldTP->pPrev = oldTP;
}

//��ʼ��ʱ�����е����� tick����ʼ����ÿ�� tick �е�˫������ֻ��һ��ͷ���
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

//ɾ��arrListTimer�ϵ����м�ʱ���ڵ�
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

//���ݼ�ʱ���Ľ���ʱ���������ʱ���֡��ڸ�ʱ�����ϵ� tick��
//Ȼ���¼�ʱ�������뵽�� tick ��˫��ѭ�������β��������ǰ��Ҫ������
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

//����ʱ���� arrlistTimer ��˫��ѭ�����������еļ�ʱ�����ݵ���ʱ����뵽ָ����ʱ������
uint32
TimeWheel::CascadeTimer(LIST_TIMER* arrListTimer, uint32 idx)
{
    LIST_TIMER listTmr, * pListTimer;
    TIMER_NODE* pTmr;

    ListTimerReplaceInit(&arrListTimer[idx], &listTmr);
    pListTimer = listTmr.pNext;
    //����˫��ѭ��������Ӷ�ʱ��
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
        //unint32��32bit��idxΪ��ǰʱ��ĵ�8bit��INDEX(0)Ϊ��6bit��INDEX(1)Ϊ�ٴ�6bit��
        //INDEX(2)Ϊ�ٴ�6bit��INDEX(3)Ϊ��6bit
        idx = uJiffies_ & TVR_MASK; //255
        if (!idx &&
            !CascadeTimer(arrListTimer2_, INDEX(0)) &&
            !CascadeTimer(arrListTimer3_, INDEX(1)) &&
            !CascadeTimer(arrListTimer4_, INDEX(2)))
            CascadeTimer(arrListTimer5_, INDEX(3));

        //�½�� pListTmrExpire �滻 arrListTimer1[idx] ��˫��ѭ������ arrListTimer1[idx] 
        //��ֻ�����Լ�һ������ˡ�pListTmrExpire ��Ϊ˫��ѭ����������
        pListTmrExpire = &listTmrExpire;
        ListTimerReplaceInit(&arrListTimer1_[idx], pListTmrExpire);
        //����ʱ����arrListTimer1��˫��ѭ������ִ�и��������ж�ʱ���Ļص�����
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

// ��ʱ���̣߳��� 1 ����Ϊ��λ���м�ʱ
void*
TimeWheel::ThreadRunTimer(void* pParam)
{
    TimeWheel* pTimeWheel;

    pTimeWheel = (TimeWheel*)pParam;
    if (pTimeWheel == NULL) return NULL;

    while (!uExitFlag_)
    {
        pTimeWheel->RunTimer();
        pTimeWheel->SleepMilliseconds(1); //�߳�˯1����
    }

    return NULL;
}

//˯uMs����
void
TimeWheel::SleepMilliseconds(uint32 uMs)
{
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = uMs * 1000;  // tv.tv_usec ��λ��΢��
    select(0, NULL, NULL, NULL, &tv);
}

//������ʱ��������
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
        //�����߳��д�
        /*ngx_log_stderr(err, "In TimeWheel::CreateTimeWheel, "
            "func pthread_create failed to create TimeWheel thread!";*/
        return false;
    }

    init_ = 1;
    return true;
}

//ɾ����ʱ��������
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

//����һ����ʱ��
TIMER_NODE*
TimeWheel::CreateTimer(
    void (*timerFn)(void*), //timerFn �ص�������ַ
    void* pParam,           //pParam �ص������Ĳ���
    uint32 uDueTime,        //uDueTime �״δ����ĳ�ʱʱ����
    uint32 uPeriod)       //uPeriod ��ʱ��ѭ�����ڣ���Ϊ0����ö�ʱ��ֻ����һ��
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

//�޸�һ����ʱ��
int32
TimeWheel::ModifyTimer(            
    TIMER_NODE* lpTimer,    //��Ҫ���޸ĵĶ�ʱ���ڵ�ָ��
    void (*timerFn)(void*), //�µ�timerFn �ص�������ַ
    void* pParam,           //�µ�pParam �ص������Ĳ���
    uint32 uDueTime,        //�µ�uDueTime �״δ����ĳ�ʱʱ����
    uint32 uPeriod)    //�µ�uPeriod ��ʱ��ѭ�����ڣ���Ϊ0����ö�ʱ��ֻ����һ��
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

//ɾ����ʱ��
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