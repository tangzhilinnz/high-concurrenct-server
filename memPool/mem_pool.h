#ifndef __MEM_POOL_H__
#define __MEM_POOL_H__

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>


using u_char = unsigned char;
#define Align 16
#define MIN(a, b) ((a) > (b) ? (b) : (a))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, mi, ma) MAX(MIN(v, ma), mi)
#define Mem_Align(d)    (((d) + (Align - 1)) & ~(Align - 1))
#define Mem_Align_Ptr(p, a)                                                   \
    (u_char *) (((uintptr_t) (p) + ((uintptr_t) a - 1)) & ~((uintptr_t) a - 1))

//内存分配考虑字节对齐时的单位(platform word)
//const size_t ALIGNMENT = sizeof(unsigned long);
//const size_t PAGE_SIZE = 4096 * 2;
//const size_t MAX_CLASS_SIZE = 512;
static const size_t __SPAN_SIZE = 1024 * 1024;
static const size_t __SPAN_NUM = 800;
//static const size_t __CLASS_TYPE = 32;

struct CLASS_UNIT
{
    int _activity; //该classSize被分配活跃度
    int _classSize; //所缓存的对象大小
    int _classNum;  //所缓存的对象总数量
    int _currentNum; //分配对象链表数量
    u_char* _currentHead; //指向可分配对象链表头
    u_char* _currentTail; //指向可分配对象链表尾
    u_char* _depsoitHead; //指向对象的缓存链表头，一般可分配对象超过256会被缓存到这个链表
                         //若对象缓存链表超过256，则对象缓存链表会被回收到centralCache
    u_char* _depsoitTail; //指向对象的缓存链表尾
};

struct THREAD_CACHE
{
    pthread_t   _Tid;       //记录该线程id
    THREAD_CACHE* _next;    //指向下一个线程
    THREAD_CACHE* _prev;    //指向下一个线程
    int _indexForLastIdle;  //指向上一次回收最不活跃的class
    int _reclaimCount;      //回收机制触发的计数变量
    CLASS_UNIT _freeList[32]; //线程缓存各种 classSize的链表
};

struct CENTRAL_UNIT
{
    int _classSize; //所缓存的对象大小
    int _classNum;  //所缓存的对象数量
    int _batchSize; //所缓存的对象块大小(暂固定为256)  
    int _batchNum;  //所缓存的对象块数量(初始化为256)
    int _scrapNum;  //不完整batch的大小
    u_char* _batchHead; //线程取用class的头指针
    u_char* _batchTail; //线程取用class的尾指针
    u_char* _scrapHead; //不完整batch的头指针
    u_char* _scrapTail; //不完整batch的尾指针
    int atomicLock;  //原子锁
};

struct SPAN_TAG
{
    int _spanNO;      //span的序号
    int _classType;   //span所分割的小块内存大小
    u_char* _begin;   //span的起始地址
    u_char* _end;    //span的结束地址 
};

struct SPAN_HEAP
{   
    bool isExhausted;
    int _spanTotalNum;
    int _spanLeftNum;   //还剩下的span数量
    int _spanSize;
    int _curIndex; //span heap 还未使用的span下标
    SPAN_TAG _freeHeap[__SPAN_NUM];
    u_char* _MEM_HEAD;
    u_char* _MEM_TAIL;
};


class MemPool
{
public:
    MemPool() {}
    virtual ~MemPool() {}

//private:
    enum { __CLASS_TYPE_NUM = 32 };
    enum { __BATCH_SIZE = 256 };

    static int __classType[__CLASS_TYPE_NUM];
    static SPAN_HEAP __spanHeap; 
    static CENTRAL_UNIT __freeList[__CLASS_TYPE_NUM];
    static u_char* __memHead;  //内存池起始地址
    static u_char* __memTail;  //内存池结束地址

    static bool __init;

    static __thread  THREAD_CACHE* __threadlocal_data
        __attribute__((tls_model("initial-exec")));

    static pthread_key_t __threadlocal_key;

    static pthread_mutex_t __threadCacheMutex;
    static pthread_spinlock_t  __centralCacheSpin; //内存分配释放锁

    static THREAD_CACHE* __thread_heaps;
    static int __thread_heap_num;
    static u_char __atomicLock;

//private:
    bool CreatPool();
    void InitSpanHeap(u_char* memHead, u_char* memTail);
    bool InitCentralCache();
    bool SetClass(int classSize, int unitNum); // one unit size is 256*256*classSize

    void* Malloc(size_t size);
    void Free(void* p);

    void SetThreadCache();
    THREAD_CACHE* AllocThreadCache();

private:
    static void DeleteThreadCache(void* ptr);


private:
    class LockMutex
    {
    public:
        explicit LockMutex(pthread_mutex_t* pMutex)
        {
            m_pMutex = pMutex;
            pthread_mutex_lock(m_pMutex);
        }
        ~LockMutex()
        {
            pthread_mutex_unlock(m_pMutex);
        }
    private:
        pthread_mutex_t* m_pMutex;
    };

#define LockMutex(x) error "Missing guard object name"

    class LockSpin
    {
    public:
        explicit LockSpin(pthread_spinlock_t* mpSpin)
        {
            m_pSpin = mpSpin;
            pthread_spin_lock(m_pSpin);
        }

        ~LockSpin()
        {
            pthread_spin_unlock(m_pSpin);
        }

    private:
        pthread_spinlock_t* m_pSpin;
    };

#define LockSpin(x) error "Missing guard object name"

};



#endif