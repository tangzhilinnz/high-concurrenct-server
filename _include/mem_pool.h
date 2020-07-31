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

//�ڴ���俼���ֽڶ���ʱ�ĵ�λ(platform word)
//const size_t ALIGNMENT = sizeof(unsigned long);
//const size_t PAGE_SIZE = 4096 * 2;
//const size_t MAX_CLASS_SIZE = 512;
static const size_t __SPAN_SIZE = 1024 * 1024;
static const size_t __SPAN_NUM = 800;
//static const size_t __CLASS_TYPE = 32;

struct CLASS_UNIT
{
    int _activity; //��classSize�������Ծ��
    int _classSize; //������Ķ����С
    int _classNum;  //������Ķ���������
    int _currentNum; //���������������
    u_char* _currentHead; //ָ��ɷ����������ͷ
    u_char* _currentTail; //ָ��ɷ����������β
    u_char* _depsoitHead; //ָ�����Ļ�������ͷ��һ��ɷ�����󳬹�256�ᱻ���浽�������
                         //�����󻺴�������256������󻺴�����ᱻ���յ�centralCache
    u_char* _depsoitTail; //ָ�����Ļ�������β
};

struct THREAD_CACHE
{
    pthread_t   _Tid;       //��¼���߳�id
    THREAD_CACHE* _next;    //ָ����һ���߳�
    THREAD_CACHE* _prev;    //ָ����һ���߳�
    int _indexForLastIdle;  //ָ����һ�λ������Ծ��class
    int _reclaimCount;      //���ջ��ƴ����ļ�������
    CLASS_UNIT _freeList[32]; //�̻߳������ classSize������
};

struct CENTRAL_UNIT
{
    int _classSize; //������Ķ����С
    int _classNum;  //������Ķ�������
    int _batchSize; //������Ķ�����С(�ݹ̶�Ϊ256)  
    int _batchNum;  //������Ķ��������(��ʼ��Ϊ256)
    int _scrapNum;  //������batch�Ĵ�С
    u_char* _batchHead; //�߳�ȡ��class��ͷָ��
    u_char* _batchTail; //�߳�ȡ��class��βָ��
    u_char* _scrapHead; //������batch��ͷָ��
    u_char* _scrapTail; //������batch��βָ��
    int atomicLock;  //ԭ����
};

struct SPAN_TAG
{
    int _spanNO;      //span�����
    int _classType;   //span���ָ��С���ڴ��С
    u_char* _begin;   //span����ʼ��ַ
    u_char* _end;    //span�Ľ�����ַ 
};

struct SPAN_HEAP
{   
    bool isExhausted;
    int _spanTotalNum;
    int _spanLeftNum;   //��ʣ�µ�span����
    int _spanSize;
    int _curIndex; //span heap ��δʹ�õ�span�±�
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
    static u_char* __memHead;  //�ڴ����ʼ��ַ
    static u_char* __memTail;  //�ڴ�ؽ�����ַ

    static bool __init;

    static __thread  THREAD_CACHE* __threadlocal_data
        __attribute__((tls_model("initial-exec")));

    static pthread_key_t __threadlocal_key;

    static pthread_mutex_t __threadCacheMutex;
    static pthread_spinlock_t  __centralCacheSpin; //�ڴ�����ͷ���

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