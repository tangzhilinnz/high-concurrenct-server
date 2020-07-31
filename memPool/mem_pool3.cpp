#include "mem_pool.h"

//================================initialize static variable=============================
int MemPool::__classType[] = { 
	16,  32,  48,  64,  80,  96,  112, 128, 
	144, 160, 176, 192, 208, 224, 240, 256, 
	272, 288, 304, 320, 336, 352, 368, 384, 
	400, 416, 432, 448, 464, 480, 496, 512,
};

SPAN_HEAP MemPool::__spanHeap;

SPAN_TAG* MemPool::__freeSpanHeap[__SPAN_NUM];

CENTRAL_UNIT MemPool::__freeList[__CLASS_TYPE_NUM];

u_char* MemPool::__memHead = nullptr;  //内存池起始地址

u_char* MemPool::__memTail = nullptr;  //内存池结束地址

bool MemPool::__init = false;

__thread  THREAD_CACHE* MemPool::__threadlocal_data 
__attribute__((tls_model("initial-exec"))) = nullptr;

pthread_key_t  MemPool::__threadlocal_key;

pthread_mutex_t MemPool::__threadCacheMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_spinlock_t MemPool::__centralCacheSpin;

THREAD_CACHE* MemPool::__thread_heaps = nullptr;;
int MemPool::__thread_heap_num = 0;
//u_char MemPool::__atomicLock = 0;

//================================initialize static variable=============================

bool
MemPool::CreatPool()
{
	if (__init)
	{
		/*ngx_log_stderr(0, "MemPool::CreatPool can only be "
			"called once for initializing the mem pool!");*/
		printf("MemPool::CreatPool can only be "
			"called once for initializing the mem pool!/n");
		return false;
	}

	int err = pthread_spin_init(&__centralCacheSpin, 0);
	if (err != 0)
	{
		/*ngx_log_stderr(err, "In MemPool::CreatePool, "
			"func pthread_spin_init for __centralCacheSpin failed!");*/
		return false;
	}

	//====================apply for a large block of memmory as base mem====================
	__memHead = (u_char*)malloc(__SPAN_SIZE * __SPAN_NUM);
	if (__memHead == nullptr) return false;
	__memTail = __memHead + __SPAN_SIZE * __SPAN_NUM - 1;
	//====================apply for a large block of memmory as base mem====================
	
	InitSpanHeap(/*__memHead, __memTail*/);
	if (!InitCentralCache()) return false;

	pthread_key_create(&__threadlocal_key, DeleteThreadCache);

	__init = true; //内存池已建立并初始化
	return true;
}

void 
MemPool::InitSpanHeap(/*u_char* memHead, u_char* memTail*/)
{
	__spanHeap.isExhausted = false;

	//__spanHeap._MEM_HEAD = memHead;
	//__spanHeap._MEM_TAIL = memTail;

	//__spanHeap._spanSize = __SPAN_SIZE;
	__spanHeap._spanTotalNum = __SPAN_NUM;
	__spanHeap._spanLeftNum = __SPAN_NUM;
	__spanHeap._curIndex = 0;

	//建立__SPAN_NUM个大小为__SPAN_SIZE的SPAN结构，每一个SPAN信息记录在数组
	//__spanHeap._freeHeap里面，方便查询和使用
	u_char* begin = __memHead;
	for (int i = 0; i < __SPAN_NUM; ++i)
	{
		__spanHeap._freeHeap[i]._begin = begin;
		__spanHeap._freeHeap[i]._end = begin + __SPAN_SIZE - 1;
		//__spanHeap._freeHeap[i]._spanNO = i;
		__spanHeap._freeHeap[i]._classType = -1; //0 indicates that it has not been used
		__freeSpanHeap[i] = &__spanHeap._freeHeap[i];

		begin += __SPAN_SIZE;
	}
}

bool 
MemPool::InitCentralCache()
{
	for (int i = 0; i < __CLASS_TYPE_NUM; ++i)
	{
		//__freeList[i].atomicLock = 0;
		__freeList[i]._batchNum = 0;
		__freeList[i]._batchSize = __BATCH_SIZE;
		__freeList[i]._classNum = 0;
		__freeList[i]._classSize = __classType[i];
		__freeList[i]._batchHead = nullptr;
		__freeList[i]._batchTail = nullptr;
		__freeList[i]._scrapHead = nullptr;
		__freeList[i]._scrapTail = nullptr;
		__freeList[i]._scrapNum = 0;
	}

	//初始化所有的class type为一个单位size，即32种class size，每一种大小需要内存空间
	//256*256*classSize， 这样一个需要 528MB 内存空间
	/*for (int i = 0; i < __CLASS_TYPE_NUM; ++i)
	{
		if (!SetClass(__classType[i], 1)) return false;
	}*/

	//if (!SetClass(64, 100)) return false;
	//if (!SetClass(512, 4)) return false;
	//if (!SetClass(32, 100)) return false;
	if (!SetClass(32, 10)) return false;
	if (!SetClass(48, 10)) return false;
	if (!SetClass(64, 10)) return false;
	if (!SetClass(80, 10)) return false;
	if (!SetClass(96, 10)) return false;
	if (!SetClass(112, 10)) return false;
	if (!SetClass(128, 10)) return false;

	return true;
}

bool 
MemPool::SetClass(int classSize, int unitNum)
{
	//one unit size is 256*256*classSize, so each class type requires a memory
	//of size classSize >> 4 MB(or classSize >> 4 spans) for one unit, 
	//e.g. classSize 16 needs 1M(1 span), classSize 32 needs 2M(2 spans), 
	//classSize 48 needs 3M(3 spans), ..., classSize 512 needs 32M(32 spans).
	if (__spanHeap.isExhausted) 
	{
		printf("In SetClass, span heap has been exhausted, "
			"and class setting cannot be available!\n");
		return false;
	}

	bool exist = false;
	for (int i = 0; i < __CLASS_TYPE_NUM; i++)
	{
		if (__classType[i] == classSize)
		{
			exist = true;
			break;
		}
	}
	if (!exist)
	{
		printf("In SetClass, the classSize %d you set doen not exist!\n", 
			classSize);
		return false;
	}

	int spanNumRequired = (classSize >> 4) * unitNum;
	if (__spanHeap._spanLeftNum < spanNumRequired) 
	{
		printf("In SetClass, there is no enough space left for setting "
			"classSize(%d*%dunit) in the span heap!\n", classSize, unitNum);
		return false;
	}
	__spanHeap._spanLeftNum -= spanNumRequired;

	if (__spanHeap._spanLeftNum == 0) 
		__spanHeap.isExhausted = true;
	for (int i = __spanHeap._curIndex; i < __spanHeap._curIndex + spanNumRequired; i++)
	{
		__spanHeap._freeHeap[i]._classType = (classSize >> 4) - 1;
	}

	int index = (classSize >> 4) - 1;
	u_char* pCutting = __spanHeap._freeHeap[__spanHeap._curIndex]._begin;
	u_char* pBatch = nullptr;
	for (int i = 0; i < 256 * unitNum; i++)
	{
		//首先连接batch tail
		*reinterpret_cast<u_char**>(pCutting + sizeof(u_char*)) =
			__freeList[index]._batchTail;
		__freeList[index]._batchTail = pCutting;
		//然后串接batch的每一个class，一共256个
		pBatch = nullptr;
		for (int j = 0; j < __BATCH_SIZE; j++)
		{
			*reinterpret_cast<u_char**>(pCutting) = pBatch;
			pBatch = pCutting;
			pCutting += classSize;
		}
		//最后连接batch head
		*reinterpret_cast<u_char**>(pBatch + sizeof(u_char*)) = 
			__freeList[index]._batchHead;
		__freeList[index]._batchHead = pBatch;
	}
	__freeList[index]._batchNum += 256 * unitNum;
	__freeList[index]._classNum += 256 * unitNum * __BATCH_SIZE;

	__spanHeap._curIndex += spanNumRequired;

	return true;
}

void 
MemPool::SetThreadCache()
{
	if (__init == false)
	{
		/*ngx_log_stderr(0, "This mem pool must be created first by calling "
			"ngx_create_pool, program exited");*/
		printf("This mem pool must be created first by calling "
			"CreatPool, program exited\n");
		exit(-1);
	}

	LockMutex lock(&__threadCacheMutex); //加锁
	THREAD_CACHE* tc = AllocThreadCache();
	//let __threadlocal_data point to this THREAD_CACHE_S
	__threadlocal_data = tc;
	pthread_setspecific(__threadlocal_key, tc);
}

THREAD_CACHE* 
MemPool::AllocThreadCache()
{
	THREAD_CACHE* tc;

	tc = (THREAD_CACHE*)malloc(sizeof(THREAD_CACHE));
	if (tc == nullptr)
	{
		/*ngx_log_stderr(0, ""Exception occurred in mem allocation, "
			"program exited!*/
		printf("Exception occurred in mem allocation, program exited\n");
		exit(-1);
	}

	pthread_t const me = pthread_self();

	//struct CLASS_UNIT {
	//	int _activity; //该classSize被分配活跃度
	//	int _classSize; //所缓存的对象大小
	//	int _classNum;  //所缓存的对象总数量
	//	int _currentNum; //分配对象链表数量
	//	u_char* _currentHead; //指向可分配对象链表头
	//	u_char* _currentTail; //指向可分配对象链表尾
	//	u_char* _depsoit; //指向对象的缓存链表，一般可分配对象超过256会被缓存到这个链表
	//					  //若对象缓存链表超过256，则对象缓存链表会被回收到centralCache
	//};
	//struct THREAD_CACHE {
	//	pthread_t   _Tid;       //记录该线程id
	//	THREAD_CACHE* _next;    //指向下一个线程
	//	THREAD_CACHE* _prev;    //指向下一个线程
	//	int _indexForLastIdle;  //指向上一次回收最不活跃的class
	//	int _reclaimCount;      //回收机制触发的计数变量
	//	CLASS_UNIT _freeList[32]; //线程缓存各种 classSize的链表
	//};

	tc->_Tid = me;
	tc->_indexForLastIdle = -1;
	tc->_reclaimCount = 500;

	for (int i = 0; i < 32; i++)
	{
		tc->_freeList[i]._activity = 0;
		tc->_freeList[i]._classSize = __classType[i];
		tc->_freeList[i]._classNum = 0;
		tc->_freeList[i]._currentNum = 0;
		tc->_freeList[i]._currentHead = nullptr;
		tc->_freeList[i]._currentTail = nullptr;
		tc->_freeList[i]._depsoitHead = nullptr;
		tc->_freeList[i]._depsoitTail = nullptr;
	}

	tc->_next = __thread_heaps;
	tc->_prev = nullptr;

	__thread_heaps = tc;
	if (tc->_next != nullptr)
		(tc->_next)->_prev = tc;

	++__thread_heap_num;

	return tc;
}

void* 
MemPool::Malloc(size_t size)
{
	if (size > 512)
	{
		printf("directly malloc: %d Bytes\n", size);
		return malloc(size);
	}

	if (__threadlocal_data == nullptr) SetThreadCache();
	//THREAD_CACHE* tc = __threadlocal_data;

	int index = (Mem_Align(size) >> 4) - 1;
	//int index = (size +15) / 16 - 1;

	CLASS_UNIT* pClass = &(__threadlocal_data->_freeList[index]);

	if (pClass->_currentHead == nullptr)
	{
		//printf("Yes\n");

		if (pClass->_depsoitHead != nullptr)
		{
			pClass->_currentHead = pClass->_depsoitHead;
			pClass->_currentTail = pClass->_depsoitTail;
			pClass->_depsoitHead = nullptr;
			pClass->_depsoitTail = nullptr;
			pClass->_currentNum = __BATCH_SIZE;
		}
		else //(tc->_freeList[index])._depsoitHead == nullptr
		{
			/*while (__sync_lock_test_and_set(&(__freeList[index].atomicLock), 1))
			{ usleep(0); }*/
			/*while (__sync_lock_test_and_set(&__atomicLock, 1))
			{usleep(0);}*/

			LockSpin lock(&__centralCacheSpin);
			//LockMutex lock(&__threadCacheMutex);

			CENTRAL_UNIT* pUnit = &__freeList[index];
			if (pUnit->_batchNum != 0)
			{
				pClass->_currentHead = pUnit->_batchHead;
				pClass->_currentTail = pUnit->_batchTail;
				pClass->_currentNum += __BATCH_SIZE;
				//(tc->_freeList[index])._classNum += __BATCH_SIZE;
				pUnit->_batchHead =
					*reinterpret_cast<u_char**>(pUnit->_batchHead + 8);
				pUnit->_batchTail =
					*reinterpret_cast<u_char**>(pUnit->_batchTail + 8);
				--pUnit->_batchNum;
				//__freeList[index]._classNum -= __BATCH_SIZE;
			}
			else //__freeList[index]._batchNum == 0
			{
				//__sync_lock_release(&(__freeList[index].atomicLock));
				//__sync_lock_release(&__atomicLock);
				printf("directly malloc: %d Bytes\n", size);
				return malloc(size);
			}

			//__sync_lock_release(&__atomicLock);
			//__sync_lock_release(&(__freeList[index].atomicLock));
		}
	}

	void* m = pClass->_currentHead;

	pClass->_currentHead = *reinterpret_cast<u_char**>(pClass->_currentHead);
	//pClass->_currentHead = *((u_char**)pClass->_currentHead);
	if (pClass->_currentHead == nullptr)
		pClass->_currentTail = nullptr;
	--pClass->_currentNum;
	//--(tc->_freeList[index])._classNum;
	//++(tc->_freeList[index])._activity;

	return m;
}

void 
MemPool::Free(void* p)
{
	//int indexTag, index;

	if (reinterpret_cast<uintptr_t>(p) > reinterpret_cast<uintptr_t>(__memTail) ||
		reinterpret_cast<uintptr_t>(p) < reinterpret_cast<uintptr_t>(__memHead))
	{
		free(p); //属于malloc直接分配的内存
		printf("directly free\n");
		return;
	}

	//这后面是从内存池分配的
	if (__threadlocal_data == nullptr) SetThreadCache();
	//THREAD_CACHE* tc = __threadlocal_data;

	int indexTag = ((u_char*)p - __memHead) >> 20;
	int index = /*__spanHeap._freeHeap[indexTag]._classType*/
		__freeSpanHeap[indexTag]->_classType;

	CLASS_UNIT* pClass = &(__threadlocal_data->_freeList[index]);

	if (pClass->_currentNum < __BATCH_SIZE)
	{
		*reinterpret_cast<u_char**>(p) = pClass->_currentHead;
		//*((u_char**)p) = pClass->_currentHead;
		pClass->_currentHead = (u_char*)p;
		++pClass->_currentNum;

		if (pClass->_currentTail == nullptr)
			pClass->_currentTail = (u_char*)p;
	}

	else //(tc->_freeList[index])._currentNum == __BATCH_SIZE
	{
		if (pClass->_depsoitHead == nullptr)
		{
			pClass->_depsoitHead = pClass->_currentHead;
			pClass->_depsoitTail = pClass->_currentTail;

			*reinterpret_cast<u_char**>(p) = nullptr;
			pClass->_currentHead = (u_char*)p;
			pClass->_currentTail = (u_char*)p;
			pClass->_currentNum = 1;
			//++(tc->_freeList[index])._classNum;
		}
		else //(tc->_freeList[index])._depsoitHead != nullptr
		{
			/*while (__sync_lock_test_and_set(&(__freeList[index].atomicLock), 1))
			{ usleep(0); }*/
			/*while (__sync_lock_test_and_set(&__atomicLock, 1))
			{usleep(0);}*/
			{
				LockSpin lock(&__centralCacheSpin);
				//LockMutex lock(&__threadCacheMutex);
				CENTRAL_UNIT* pUnit = &__freeList[index];

				*reinterpret_cast<u_char**>(pClass->_currentHead + 8) =
					pUnit->_batchHead;
				pUnit->_batchHead = pClass->_currentHead;

				*reinterpret_cast<u_char**>(pClass->_currentTail + 8) =
					pUnit->_batchTail;
				pUnit->_batchTail = pClass->_currentTail;

				++pUnit->_batchNum;
				//__freeList[index]._classNum += __BATCH_SIZE;
			}
			//__sync_lock_release(&__atomicLock);
			//__sync_lock_release(&(__freeList[index].atomicLock));

			*reinterpret_cast<u_char**>(p) = nullptr;
			pClass->_currentHead = (u_char*)p;
			pClass->_currentTail = (u_char*)p;
			pClass->_currentNum = 1;
			//(tc->_freeList[index])._classNum -= __BATCH_SIZE;
		}
	}

	//return;
}

void 
MemPool::DeleteThreadCache(void* ptr)
{

}