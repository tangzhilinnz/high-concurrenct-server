#ifndef __TZ_MEM_POOL_H__
#define __TZ_MEM_POOL_H__

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>

enum CAPACITY_E
{
	_CAP1 = 1,
	_CAP2 = 2,
	_CAP3 = 3,
	_CAP4 = 4,
	_CAP5 = 5,
	_CAP20 = 10,
};

// Select one of SPAN_SIZE_E value as each span size
// _SPN16K    <====>   span size: 16 * 1024(16KB)
// _SPN32K    <====>   span size: 32 * 1024(32KB)
// _SPN64K    <====>   span size: 64 * 1024(64KB)
// _SPN128K   <====>   span size: 128 * 1024(128KB)
// _SPN256K   <====>   span size: 256 * 1024(256KB)
// _SPN512K   <====>   span size: 512 * 1024(512KB)
// _SPN1M     <====>   span size: 1024 * 1024(1MB)
enum SPAN_SIZE_E
{
	_SPN16K = 16 * 1024,
	_SPN32K = 32 * 1024,
	_SPN64K = 64 * 1024,
	_SPN128K = 128 * 1024,
	_SPN256K = 256 * 1024,
	_SPN512K = 512 * 1024,
	_SPN1M = 1024 * 1024
};

// Select one of T_POOLSIZE value as the pool size,
// the default value is medium
// _SMALL    <====>   pool size: 100
// _MEDIUM   <====>   pool size: 200
// _LARGE    <====>   pool size: 300
// _HUGE     <====>   pool size: 300
enum SPAN_NUM_E
{
	_SMALL = 200,
	_MEDIUM = 400,
	_LARGE = 800,
	_HUGE = 1600,
	_EX = 3200,
};

using u_char = unsigned char;

//������(�ص�����)������
typedef void(*__pool_cleanup_pfunc)(void* data);
struct POOL_CLEANUP_S
{
	__pool_cleanup_pfunc _handler;  //����ָ�룬������������Ļص�����
	void* _data;                   //���ݸ��ص������Ĳ���
	POOL_CLEANUP_S* _next_cleanup; //���е�cleanup���������������һ��������
};

//����С���ڴ���ڴ�ص�ͷ��������Ϣ
struct POOL_SPAN_S
{
	u_char* _begin;            //С���ڴ�ص���ʼ��ַ
	u_char* _last;             //С���ڴ�ؿ����ڴ����ʼ��ַ
	u_char* _end;              //С���ڴ�ؿ����ڴ��ĩβ��ַ��һλ
	u_char* _reset;		       //С���ڴ�صĻ��ձ��λ
	int32_t _ptimes;          //С���ڴ��ѷ�������
	int32_t _rtimes;
	int16_t _recycle;
	POOL_SPAN_S* _next_span;   //����capacity�ڵ��ڴ�鶼������һ��
	POOL_SPAN_S* _next_chunk;  //����capacity�ڴ�鶼������һ��
};

//�߳��ڴ�����������
struct THREAD_CACHE_S
{
	pthread_t   _Tid;
	POOL_SPAN_S* _Tcurrent;
	POOL_SPAN_S* _Tdeposit;
	int32_t _Tdeposit_size;

	/*u_char* _Tcurrent1;
	u_char* _Tdeposit1;
	int32_t _Tdeposit_size1;*/

	THREAD_CACHE_S* _next;
	THREAD_CACHE_S* _prev;
};

#define MIN(a, b) ((a) > (b) ? (b) : (a))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, mi, ma) MAX(MIN(v, ma), mi)
#define ngx_align(d, a)    (((d) + (a - 1)) & ~(a - 1))
#define ngx_align_ptr(p, a)                                                   \
    (u_char *) (((uintptr_t) (p) + ((uintptr_t) a - 1)) & ~((uintptr_t) a - 1))

//�ڴ���俼���ֽڶ���ʱ�ĵ�λ(platform word)
const size_t ALIGNMENT = sizeof(unsigned long);
const size_t PAGE_SIZE = 4096;
//�ڴ�ؿɷ�������ռ䣬���������С���ڴ�ֱ����malloc����
const size_t MAX_ALLOC_FROM_POOL = PAGE_SIZE - 1;
//�ڴ�ش�С����16�ֽڽ��ж���
const size_t POOL_ALIGNMENT = 16;

const size_t POINTER_SIZE = sizeof(POOL_SPAN_S*);
const size_t POOL_SPAN_SIZE = sizeof(POOL_SPAN_S);


template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
class TzMemPool
{
private:
	TzMemPool() //��ֹ����һ��ʵ��
	{
		__cleanup = nullptr;
		__Head = nullptr;
		__Tail = nullptr;
	}
	TzMemPool(const TzMemPool&) {} //��ֹ��������һ��ʵ��
	TzMemPool& operator=(const TzMemPool&) { return *this; } //��ֹ��ֵ����һ��ʵ��

	static TzMemPool* m_instance; //��̬˽�г�Ա

	class CRecycle
	{//Obj has no special access to the members of CRecycle and vice versa. but
	 //Ƕ������Է�����Χ��ľ�̬��Ա��������ʹ���ķ���Ȩ����˽�е�(��TzMemPool::m_instance)
	public:
		~CRecycle()
		{
			if (TzMemPool::m_instance)
			{
				// ��������Ψһ����������������~CMemory()
				delete TzMemPool::m_instance;
				TzMemPool::m_instance = NULL;
			}
		}
	};

	class LockGuardSpin
	{
	public:
		explicit LockGuardSpin(pthread_spinlock_t* mpSpin)
		{
			m_pSpin = mpSpin;
			pthread_spin_lock(m_pSpin);
		}

		~LockGuardSpin()
		{
			pthread_spin_unlock(m_pSpin);
		}

	private:
		pthread_spinlock_t* m_pSpin;
	};

#define LockGuardSpin(x) error "Missing guard object name"

	class LockGuard
	{
	public:
		explicit LockGuard(pthread_mutex_t* pMutex)
			//explicit CLockGuard(pthread_spinlock_t* mpSpin)
		{
			m_pMutex = pMutex;
			pthread_mutex_lock(m_pMutex);

			//m_pSpin = mpSpin;
			//pthread_spin_lock(m_pSpin);
		}

		~LockGuard()
		{
			pthread_mutex_unlock(m_pMutex);
			//pthread_spin_unlock(m_pSpin);
		}

	private:
		pthread_mutex_t* m_pMutex;
		//pthread_spinlock_t*   m_pSpin;
	};

#define LockGuard(x) error "Missing guard object name"

public:
	virtual ~TzMemPool()
	{
		TzDestroyPool();
		/*ngx_log_stderr(0, "~TzDestroyPool() executed, "
			"global class TzMemPool object was detroyed!");*/
		return;
	}
	static TzMemPool* GetInstance() //������
	{
		if (m_instance == nullptr)
		{
			m_instance = new TzMemPool();  //��һ�ε���Ӧ�÷�����������
			static CRecycle recycle;  //�ֲ���̬����ֻ�ᱻ����һ�Σ��ڵ��õ�ʱ���죬
									  //��main����ִ����Ϻ󣬳����˳�֮ǰ������
		}
		return m_instance;
	}

public:
	size_t TzGetFreepoolSize();
	bool   TzCreatePool(); 
	//void*  TzMalloc(size_t size);  //�����ڴ��ֽڶ���
	//void*  TznMalloc(size_t size); //�������ڴ��ֽڶ���	
	//void*  TzcMalloc(size_t size); //����TzMallocʵ���ڴ���䲢��ʼ��0
	void   TzDestroyPool();
	void   TzFree(void* p);
	POOL_CLEANUP_S*  //��ӻص������������
		TzCleanupAdd(__pool_cleanup_pfunc handler, void* data = nullptr);

private:
	enum { MAX_ALLOC_SIZE = MAX_ALLOC_FROM_POOL };

private:
	u_char* __Head;
	u_char* __Tail;
	POOL_CLEANUP_S* __cleanup;
	
	static pthread_spinlock_t  __pool_spin; //�ڴ�����ͷ���
	static pthread_mutex_t     __pool_mutex;

	static POOL_SPAN_S   __Span_list[__spannum];
	static POOL_SPAN_S* __Free_pool;
	static size_t  __Free_pool_size;

	static u_char* __Free_pool1;
	static size_t  __Free_pool_size1;

	static bool     __init;

public:
	static int32_t  __capacity;
	static int32_t  __movebits;

	static __thread  THREAD_CACHE_S* __threadlocal_data
		__attribute__((tls_model("initial-exec")));
	static pthread_key_t __threadlocal_key;

public:
	void* TzMalloc(size_t size);

	//void* TzAllocLarge(size_t size);

	static void  TzDeleteThreadCache(void* ptr);
	static THREAD_CACHE_S* TzAllocThreadCache();
	static void TzSetThreadCache();
	static void TzSetCapacity();

public:
	static THREAD_CACHE_S* __thread_heaps;
	static int __thread_heap_count;
};

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
/*typename */TzMemPool<__spansize, __spannum>*
TzMemPool<__spansize, __spannum>::m_instance = nullptr;

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
__thread  THREAD_CACHE_S* TzMemPool<__spansize, __spannum>::__threadlocal_data
__attribute__((tls_model("initial-exec"))) = nullptr;

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
pthread_key_t TzMemPool<__spansize, __spannum>::__threadlocal_key;

//�ڴ�ط����ͷ���(����)
template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
pthread_spinlock_t TzMemPool<__spansize, __spannum>::__pool_spin;

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
pthread_mutex_t  TzMemPool<__spansize, __spannum>::__pool_mutex;

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
POOL_SPAN_S   TzMemPool<__spansize, __spannum>::__Span_list[__spannum];

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
POOL_SPAN_S* TzMemPool<__spansize, __spannum>::__Free_pool = nullptr;

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
size_t  TzMemPool<__spansize, __spannum>::__Free_pool_size = 0;


template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
u_char* TzMemPool<__spansize, __spannum>::__Free_pool1 = nullptr;

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
size_t  TzMemPool<__spansize, __spannum>::__Free_pool_size1 = 0;


template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
bool TzMemPool<__spansize, __spannum>::__init = false;

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
int  TzMemPool<__spansize, __spannum>::__capacity = 0;

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
int  TzMemPool<__spansize, __spannum>::__movebits = 0;

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
THREAD_CACHE_S* TzMemPool<__spansize, __spannum>::__thread_heaps = nullptr;

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
int TzMemPool<__spansize, __spannum>::__thread_heap_count = 0;


template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
size_t
TzMemPool<__spansize, __spannum>::TzGetFreepoolSize()
{
	return __Free_pool_size;
}

//�����ڴ��
template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
bool
TzMemPool<__spansize, __spannum>::TzCreatePool() 
{
	if (__init)
	{
		/*ngx_log_stderr(0, "TzMemPool::TzCreatePool can only be "
			"called once for initializing the mem pool!");*/
		return false;
	}

	int err;
	err = pthread_mutex_init(&__pool_mutex, NULL);
	if (err != 0)
	{
		/*ngx_log_stderr(err, "In ngx_mem_pool::ngx_create_pool, "
			"func pthread_mutex_init for __pool_mutex failed!");*/
		return false;
	}
	err = pthread_spin_init(&__pool_spin, 0);
	if (err != 0)
	{
		/*ngx_log_stderr(err, "In TzMemPool::TzCreatePool, "
			"func pthread_spin_init for __pool_spin failed!");*/
		return false;
	}

	__Head = (u_char*)malloc(__spansize * __spannum);
	if (__Head == nullptr)
	{
		return false;
	}
	__Tail = __Head + __spansize * __spannum - 1;

	u_char* p_begin = __Head;
	for (int i = 0; i < __spannum; i++)
	{
		__Span_list[i]._begin = p_begin;
		__Span_list[i]._last = p_begin;
		__Span_list[i]._end = p_begin + (__spansize - 1 - 128); //128 is for buffer zone
		__Span_list[i]._reset = __Span_list[i]._end - MAX_ALLOC_SIZE;
		__Span_list[i]._ptimes = 0; //At first, there is no allocation
		__Span_list[i]._rtimes = 0;
		__Span_list[i]._recycle = 0;
		__Span_list[i]._next_span = nullptr;
		__Span_list[i]._next_chunk = nullptr;

		p_begin += __spansize;
	}

	__Free_pool = nullptr;
	__Free_pool_size = __spannum / _CAP5;

	/*__Free_pool1 = nullptr;
	__Free_pool_size1 = __spannum;
	u_char* p_begin1 = __Head;*/
	for (int i = 0; i < __Free_pool_size; i++)
	{
		POOL_SPAN_S* pchunk = nullptr;
		for (int j = 0; j < _CAP5; j++)
		{
			__Span_list[i * _CAP5 + j]._next_span = pchunk;
			pchunk = &__Span_list[i * _CAP5 + j];
		}

		pchunk->_next_chunk = __Free_pool;
		__Free_pool = pchunk;
	}


	int spansize = __spansize;
	do
	{
		spansize = spansize >> 1;
		++__movebits;

	} while (spansize > 1);

	TzSetCapacity();

	pthread_key_create(&__threadlocal_key, TzDeleteThreadCache);
	__init = true; //�ڴ���ѽ�������ʼ��

	return true;
}

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
void
TzMemPool<__spansize, __spannum>::TzSetCapacity()
{
	if (__thread_heap_count == 0)
	{
		__capacity = _CAP5;
	}
	else
	{
		int ratio = _LARGE / (4 * __thread_heap_count);

		if (ratio >= _CAP5) __capacity = _CAP5;
		else if (ratio >= _CAP4) __capacity = _CAP4;
		else if (ratio >= _CAP3) __capacity = _CAP3;
		else if (ratio >= _CAP2) __capacity = _CAP2;
		else __capacity = _CAP1;
	}
	//__capacity = _CAP20;
}

////���ڴ������size��С���ڴ棬�����ڴ��ֽڶ���
//template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
//void*
//TzMemPool<__spansize, __spannum>::TzMalloc(size_t size)
//{
//	if (size <= MAX_ALLOC_SIZE)
//	{
//		return TzAllocSmall(/*capacity, */size, 1);
//	}
//	return TzAllocLarge(size);
//}
//
////���ڴ������size��С���ڴ棬�������ڴ��ֽڶ���
//template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
//void*
//TzMemPool<__spansize, __spannum>::TznMalloc(size_t size)
//{
//	if (size <= MAX_ALLOC_SIZE)
//	{
//		return TzAllocSmall(size, 0);
//	}
//	return TzAllocLarge(size);
//}
//
////���õ���TzMallocʵ���ڴ���䣬���ǻ��ʼ��0
//template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
//void*
//TzMemPool<__spansize, __spannum>::TzcMalloc(size_t size)
//{
//	void* p;
//	p = TzMalloc(size);
//	if (p)
//	{
//		memset(p, 0, size);
//	}
//	return p;
//}

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
void
TzMemPool<__spansize, __spannum>::TzSetThreadCache()
{
	if (__init == false)
	{
		/*ngx_log_stderr(0, "This mem pool must be created first by calling "
			"ngx_create_pool, program exited");*/
		printf("This mem pool must be created first by calling "
			"ngx_create_pool, program exited\n");
		exit(-1);
	}

	LockGuardSpin lock(&__pool_spin); //����

	THREAD_CACHE_S* tc = TzAllocThreadCache();

	TzSetCapacity();
	//let __threadlocal_data point to this THREAD_CACHE_S
	__threadlocal_data = tc;
	pthread_setspecific(__threadlocal_key, tc);
}

//this function shoud be locked by calling function when calling
template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
THREAD_CACHE_S*
TzMemPool<__spansize, __spannum>::TzAllocThreadCache()
{
	u_char* m;
	POOL_SPAN_S* p;
	THREAD_CACHE_S* tc;

	tc = (THREAD_CACHE_S*)malloc(sizeof(THREAD_CACHE_S));
	if (tc == nullptr)
	{
		/*ngx_log_stderr(0, ""Exception occurred in mem allocation, "
			"program exited!*/
		printf("Exception occurred in mem allocation, program exited\n");
		exit(-1);
	}

	pthread_t const me = pthread_self();

	tc->_Tid = me;
	tc->_Tcurrent = nullptr;
	tc->_Tdeposit = nullptr;
	tc->_Tdeposit_size = 0;

	/*tc->_Tcurrent1 = nullptr;
	tc->_Tdeposit1 = nullptr;
	tc->_Tdeposit_size1 = 0;*/

	tc->_next = __thread_heaps;
	tc->_prev = nullptr;

	__thread_heaps = tc;
	if (tc->_next != nullptr)
		(tc->_next)->_prev = tc;

	++__thread_heap_count;

	return tc;
}

//С���ڴ����
template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
void* 
TzMemPool<__spansize, __spannum>::TzMalloc(size_t size/*, unsigned int align*/)
{
	if (size > 4095)
	{
		//return TzAllocLarge(size);
		return malloc(size);
	}

	if (__threadlocal_data == nullptr) 
	{ 
		TzSetThreadCache(); 
		//printf("yes ==== tid %d\n", pthread_self());
	}
	THREAD_CACHE_S* tc = __threadlocal_data;

	if (tc->_Tcurrent == nullptr)
	{
		//printf("YES\n");
		if (tc->_Tdeposit_size != 0)
		{
			tc->_Tcurrent = tc->_Tdeposit;
			tc->_Tdeposit = nullptr;
			tc->_Tdeposit_size = 0;
			//printf("YES1--tid%d\n", pthread_self());
		}
		else
		{
			LockGuardSpin lock(&__pool_spin);
			//LockGuard lock(&__pool_mutex);
			//if (__Free_pool != nullptr)
			if (__Free_pool_size != 0)
			{
				tc->_Tcurrent = __Free_pool;
				__Free_pool = __Free_pool->_next_chunk;
				--__Free_pool_size;
				tc->_Tcurrent->_next_chunk = nullptr;
			}
			else
			{
				printf("Large\n");
				return malloc(size);
			}
			//printf("YES----TID %d\n", pthread_self());
		}
	}

	u_char* m;
	POOL_SPAN_S* p;
	p = tc->_Tcurrent;
	m = p->_last;
	/*if (align)
	{
		m = ngx_align_ptr(m, ALIGNMENT);
	}*/

	size = ngx_align(size, ALIGNMENT) /*+ 8*/;
	//m = ngx_align_ptr(m, ALIGNMENT);
	
	p->_last = m + size;

	__sync_fetch_and_add(&p->_ptimes, 1);
	if (p->_last > p->_reset)
	{
		__sync_fetch_and_sub(&p->_ptimes, 16);
		tc->_Tcurrent = tc->_Tcurrent->_next_span;
	}

	//if (p->_last <= p->_reset)
	//{
	//	while (__sync_lock_test_and_set(&p->_recycle, 1))
	//	{
	//		usleep(0);
	//	}
	//	++p->_ptimes;
	//	__sync_lock_release(&p->_recycle);
	//}
	//else
	//{
	//	while (__sync_lock_test_and_set(&p->_recycle, 1))
	//	{
	//		usleep(0);
	//	}
	//	p->_ptimes -= 15;
	//	__sync_lock_release(&p->_recycle);
	//	tc->_Tcurrent = tc->_Tcurrent->_next_span;
	//}

	return m /*+ 8*/;
}

//����ڴ����
//template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
//void*
//TzMemPool<__spansize, __spannum>::TzAllocLarge(size_t size)
//{
//	return malloc(size);
//}

//�ͷ��ڴ��
template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
void
TzMemPool<__spansize, __spannum>::TzFree(void* p)
{
	//pthread_t m = pthread_self();
	if (p == nullptr)  return;

	if (reinterpret_cast<uintptr_t>(p) > reinterpret_cast<uintptr_t>(__Tail) ||
		reinterpret_cast<uintptr_t>(p) < reinterpret_cast<uintptr_t>(__Head))
	{
		free(p); //����mallocֱ�ӷ�����ڴ�
		return;
	}

	//printf("_pooldeposit_size %d\n", __threadlocal_data->_pooldeposit_size);
	//printf("_poolcapacity %d\n", __threadlocal_data->_poolcapacity);

	//������Ǵ��ڴ�ط����
	int64_t index = ((u_char*)p - __Head) >> __movebits;
	POOL_SPAN_S* pdata = &__Span_list[index];

	//int tmp;
	//while (__sync_lock_test_and_set(&pdata->_recycle, 1))
	//{
	//	usleep(0);
	//}
	//tmp = --pdata->_ptimes;
	//__sync_lock_release(&pdata->_recycle);


	if (__sync_sub_and_fetch(&pdata->_ptimes, 1) == -16)
	//if (tmp == -16)
	{
		//printf("yes\n");
		pdata->_ptimes = 0;
		//pdata->_rtimes = 0;
		//pdata->_recycle = 0;

		pdata->_last = pdata->_begin;
		if (__threadlocal_data == nullptr) TzSetThreadCache();
		THREAD_CACHE_S* tc = __threadlocal_data;
		if (tc->_Tdeposit_size < __capacity)
		{
			//printf("memmanage->_pooldeposit_size %d\n", memmanage->_pooldeposit_size);
			//printf("memmanage->_poolcapacity %d\n", memmanage->_poolcapacity);
			pdata->_next_span = tc->_Tdeposit;
			tc->_Tdeposit = pdata;
			++tc->_Tdeposit_size;
		}
		//_pooldeposit_size has exceeded the _poolcapacity limit, and we need
		//to return all deposit pools to __Free_pool
		else // memmanage->_pooldeposit_size == __capacity
		{
			//printf("YES----TID %d\n", pthread_self());
			{
				LockGuardSpin lock(&__pool_spin);
				//pthread_spin_lock(&__pool_spin);
				tc->_Tdeposit->_next_chunk = __Free_pool;
				__Free_pool = tc->_Tdeposit;
				++__Free_pool_size;
				//pthread_spin_unlock(&__pool_spin);
			}
			pdata->_next_span = nullptr;
			tc->_Tdeposit = pdata;
			tc->_Tdeposit_size = 1;
		}
	}
	return;
}

template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
void
TzMemPool<__spansize, __spannum>::TzDeleteThreadCache(void* ptr)
{
	//printf("YES--delete\n");
	THREAD_CACHE_S* tc = reinterpret_cast<THREAD_CACHE_S*>(ptr);
	POOL_SPAN_S* p, * p1;
	p = tc->_Tcurrent;

	if (tc->_next != nullptr)
		tc->_next->_prev = tc->_prev;
	if (tc->_prev != nullptr)
		tc->_prev->_next = tc->_next;
	if (__thread_heaps == tc)
		__thread_heaps = tc->_next;
	--__thread_heap_count;

	if (p != nullptr)
	{
		//if (p->_ptimes > 0)
		if (__sync_sub_and_fetch(&p->_ptimes, 16) > -16)
		{
			p = p->_next_span;
		}
		else
		{
			p->_ptimes = 0;
			p->_last = p->_begin;
		}
	}

	LockGuardSpin lock(&__pool_spin);

	if (p != nullptr)
	{
		p->_next_chunk = __Free_pool;
		__Free_pool = p;
		++__Free_pool_size;
	}

	if (tc->_Tdeposit != nullptr)
	{
		tc->_Tdeposit->_next_chunk = __Free_pool;
		__Free_pool = tc->_Tdeposit;
		++__Free_pool_size;
	}
	//printf("test : %d\n", test);

	free(ptr);
}

//�ڴ�ص����ٺ���
template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
void
TzMemPool<__spansize, __spannum>::TzDestroyPool()
{
	POOL_CLEANUP_S* c, * cn;

	for (c = __cleanup; c; /* void */)
	{
		if (c->_handler)
		{
			c->_handler(c->_data);
		}

		cn = c;
		c = c->_next_cleanup;
		free(cn);
	}

	THREAD_CACHE_S* ptc, * ptc1;
	for (ptc = __thread_heaps; ptc;)
	{
		ptc1 = ptc->_next;
		free(ptc);
		ptc = ptc1;
	}

	free(__Head);

	int err;
	err = pthread_spin_destroy(&__pool_spin);
	if (err != 0)
	{
		/*ngx_log_stderr(err, "In ngx_mem_pool::ngx_destroy_pool(), "
			"func pthread_spin_destroy for __pool_spin failed!");*/
	}
}

//��ӻص������������
template<SPAN_SIZE_E __spansize, SPAN_NUM_E __spannum>
POOL_CLEANUP_S* 
TzMemPool<__spansize, __spannum>::TzCleanupAdd(__pool_cleanup_pfunc handler,
	void* data)
{
	if (handler == nullptr)  return nullptr;

	POOL_CLEANUP_S* c;

	c = (POOL_CLEANUP_S*)malloc(sizeof(POOL_CLEANUP_S));

	if (c == nullptr)
	{
		return nullptr;
	}

	if (data)
	{
		/*c->data = malloc(size);
		if (c->data == nullptr)
		{
			return nullptr;
		}*/
		c->_data = data;
	}

	//CLockGuard lock(&__cleanup_mutex); //����ʹ�߳�ͬ��
	c->_handler = handler;
	c->_next_cleanup = __cleanup;
	__cleanup = c;

	return c;
}

#endif
