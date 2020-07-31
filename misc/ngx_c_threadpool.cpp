//和线程池有关的函数放这里
#include <stdarg.h>
#include <unistd.h>  //usleep

#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_threadpool.h"
//#include "ngx_c_memory.h"
#include "ngx_macro.h"

//静态成员初始化
//#define PTHREAD_MUTEX_INITIALIZER ((pthread_mutex_t) -1)
pthread_mutex_t CThreadPool::m_pthreadMutex = PTHREAD_MUTEX_INITIALIZER; 
//#define PTHREAD_COND_INITIALIZER ((pthread_cond_t) -1)
pthread_cond_t CThreadPool::m_pthreadCond = PTHREAD_COND_INITIALIZER;    
bool CThreadPool::m_shutdown = false; //刚开始标记整个线程池的线程是不退出的      

//构造函数
CThreadPool::CThreadPool()
{
    m_iRunningThreadNum = 0; //正在运行的线程，开始给个0；原子的对象给0，
                             //可以直接赋值当整型变量来用
    m_iLastEmgTime = time(NULL); //上次报告线程不够用了的时间；

    return;
}

//析构函数
CThreadPool::~CThreadPool()
{
    StopAll();

    ngx_log_stderr(0, "~CThreadPool() executed, "
        "global object g_threadpool was detroyed!");

    return;
}

//创建线程池中的线程，要手工调用，不在构造函数里调用了
//返回值：所有线程都创建成功则返回true，出现错误则返回false
bool 
CThreadPool::Create(int threadNum)
{
    ThreadItem* pNew;
    int err;

    m_iThreadNum = threadNum; //保存要创建的线程数量    

    for (int i = 0; i < m_iThreadNum; ++i)
    {
        //创建(new)一个新线程对象并入到容器中  
        m_threadVector.push_back(pNew = new ThreadItem(this));
        //创建线程，错误不返回到errno，一般返回错误码
        err = pthread_create(&pNew->_Handle, NULL, ThreadFunc, pNew);
        if (err != 0)
        {
            //创建线程有错
            ngx_log_stderr(err, "CThreadPool::Create failed to create "
                "thread_%d，the returned errno is %d!", i, err);
            return false;
        }
        else
        {
            //创建线程成功
            ngx_log_stderr(0, "CThreadPool::Create succeeded to create "
                "thread_%d, tid=%ui", i, pNew->_Handle);
        }
    }

    //我们必须保证每个线程都启动并运行到pthread_cond_wait，本函数才返回，只有这样，
    //这些线程才能进行后续的正常工作，依靠每个ThreadItem对象中的ifrunning来判断，
    //该值被初始化为false，从false变为true即可知道线程已运行到pthread_cond_wait
    std::vector<ThreadItem*>::iterator iter;
lblfor:
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        if ((*iter)->ifrunning == false) //这说明仍有没启动完全的线程，跳转继续
        {
            usleep(100 * 1000); //单位是微妙，100*1000=100毫秒
            goto lblfor;
        }
    }

    return true;
}

//线程入口函数，当用pthread_create()创建线程后，这个ThreadFunc函数都会被立即执行；
//这个是静态成员函数，是不存在this指针的
//唤醒丢失--------------------------------------------------------------------------------
//如果worker进程生成了2个及以上提取消息的线程，当一个pthread_cond_signal发送激活信号
//的时候，总是有至少一个线程处于3种状态的一种：
//  a) waiting in the mutex queue(A)
//  b) waiting in the condition variable queue(B)
//  c) unlocked the mutex(C)
//无论处于何种状态，都能保证本次pthread_cond_signal激活线程的效果(既保证当消息队列中有
//消息插入后能够被一个线程及时提取)，状态a)和c)会取得互斥锁并循环回第二个while读取消息
//队列直至其为空才重新进入pthread_cond_wait状态；而状态b)保证这次pthread_cond_signal
//至少有一个保底线程可以被激活来提取消息(首先需要拿到锁，这时其他线程可能还没有处在阻塞
//等待状态，无法收到激活通知)。所以无论是哪种状态，都可以保证pthread_cond_signal来的时
//候，消息队列不会没有线程去处理，即遗留消息。
void* 
CThreadPool::ThreadFunc(void* threadData)
{  
    ThreadItem* pThread = static_cast<ThreadItem*>(threadData);
    CThreadPool* pThreadPoolObj = pThread->_pThis; //&g_threadpool

    char* jobbuf = NULL;
    int err;

    //CMemory* p_memory = CMemory::GetInstance();

    pthread_t tid = pthread_self(); //获取线程自身id，方便调试打印信息等 
    while (true)
    {
        //线程用pthread_mutex_lock()函数去锁定指定的mutex变量，若该mutex已经被另外一
        //个线程锁定了，该调用将会阻塞线程在此直到mutex被解锁。  
        err = pthread_mutex_lock(&m_pthreadMutex); //A
        if (err != 0)
        {
            ngx_log_stderr(err, "In CThreadPool::ThreadFunc, "
                "func pthread_mutex_lock failed，the returned errno is %d!", err);
        }

        //因为：pthread_cond_wait()是个值得注意的函数，调用一次pthread_cond_signal()
        //可能会唤醒多个，惊群：官方描述是至少一个pthread_cond_signal在多处理器上可能
        //同时唤醒多个线程
        //pthread_cond_wait函数，如果只有一条消息唤醒了两个线程干活，那么其中有一个
        //线程拿不到消息，那如果不用while写，就会出问题，所以被惊醒后必须再次用while拿消息，
        //拿到才走下来；在执行Create函数时，g_socket.outMsgRecvQueue()总是处于empty状态
        while ((jobbuf = g_socket.outMsgRecvQueue()) == NULL && m_shutdown == false)
        {
            //如果这个pthread_cond_wait被唤醒【被唤醒后程序执行流程往下走的前提是拿到了锁
            //--官方：pthread_cond_wait()返回时，互斥量再次被锁住】，那么会立即再次执行
            //g_socket.outMsgRecvQueue()，如果拿到了一个NULL，则继续在这里wait着();
            if (pThread->ifrunning == false)
                pThread->ifrunning = true; //标记为true了才允许调用StopAll()：测试
                                           //中发现如果Create()和StopAll()紧挨着调用，
                                        //就会导致线程混乱，所以每个线程必须执行到这里，
                                        //才认为是启动成功了；

            //执行pthread_cond_wait()的时候，会卡在这里，且m_pthreadMutex会被释放掉
            //服务器程序刚初始化的时候，每个worker进程的所有线程必然是卡在这里等待的
            err = pthread_cond_wait(&m_pthreadCond, &m_pthreadMutex); //B
            if (err != 0)
            {
                ngx_log_stderr(err, "In CThreadPool::ThreadFunc, "
                    "func pthread_cond_wait failed，the returned errno is %d!",
                    err);

            }
        }

        //能走下来的，必然是拿到了的消息队列中的数据或者 m_shutdown == true
        err = pthread_mutex_unlock(&m_pthreadMutex); //C
        if (err != 0)
        {
            ngx_log_stderr(err, "In CThreadPool::ThreadFunc, "
                "func pthread_mutex_unlock failed，the returned errno is %d!",
                err);
        }

        //先判断线程退出这个条件
        if (m_shutdown)
        {
            if (jobbuf != NULL)
            {
                //可能会成立尤其是当要退出的时候
                /*p_memory->FreeMemory(jobbuf);*/
                free(jobbuf);
            }
            break;
        }

        //能走到这里的，就是有数据可以处理
        ++pThreadPoolObj->m_iRunningThreadNum; //原子+1，记录正在干活的线程数量增加1
        g_socket.threadRecvProcFunc(jobbuf);   //处理消息队列中来的消息
        /*p_memory->FreeMemory(jobbuf);*/          //释放消息内存 
        free(jobbuf);
        --pThreadPoolObj->m_iRunningThreadNum; //原子-1，记录正在干活的线程数量减少1
    } 

    return (void*)0; //线程结束
}

//停止所有线程，等待线程池中所有线程结束，该函数返回后，应该是所有线程池中线程都结束了
void 
CThreadPool::StopAll()
{
    //(1)已经调用过，就不要重复调用了
    if (m_shutdown == true)
    {
        return;
    }
    m_shutdown = true;

    //(2)唤醒等待该条件(卡在pthread_cond_wait()的的所有线程)，一定要在改变条件状态
    //以后再给线程发信号
    int err = pthread_mutex_lock(&m_pthreadMutex);
    if (err != 0)
    {
        //pthread_mutex_lock()调用失败
        ngx_log_stderr(err, "In CThreadPool::StopAll(), "
            "func pthread_mutex_lock failed, the returned errno is %d!", err);
        m_shutdown = false; //重新置位false，以便再次调用StopAll
        return;
    }
    int err1 = pthread_cond_broadcast(&m_pthreadCond);
    err = pthread_mutex_unlock(&m_pthreadMutex);
    if (err != 0)
    {
        //pthread_mutex_unlock()调用失败
        ngx_log_stderr(err, "In CThreadPool::StopAll(), "
            "func pthread_mutex_unlock failed, the returned errno is %d!", err);
        m_shutdown = false; //重新置位false，以便再次调用StopAll
        return;
    }
    if (err1 != 0)
    {
        //pthread_cond_broadcast()调用失败
        ngx_log_stderr(err, "In CThreadPool::StopAll(), "
            "func pthread_cond_broadcast failed, the returned errno is %d!", err);
        m_shutdown = false; //重新置位false，以便再次调用StopAll
        return;
    }

    //(3)等待所有线程返回    
    std::vector<ThreadItem*>::iterator iter;
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        pthread_join((*iter)->_Handle, NULL); //等待一个线程终止
    }

    //流程走到这里，那么所有的线程池中的线程肯定都返回了
    pthread_mutex_destroy(&m_pthreadMutex);
    pthread_cond_destroy(&m_pthreadCond);

    //(4)释放一下new出来的ThreadItem【线程池中的线程对象】    
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        if (*iter)
            delete* iter;
    }
    m_threadVector.clear();

    ngx_log_stderr(0, "CThreadPool::StopAll() succeeded to terminate all "
        "threads in the thread pool!");

    return;
}

//一个worker进程中，只有一个线程(worker进程的主线程)中调用了Call，所以不存在多个线
//程调用Call的情形，所以就避免了多个线程调用pthread_cond_signal造成冲突的潜在问题
void 
CThreadPool::Call(/*int irmqc*/)
{
    //唤醒一个等待该条件的线程，也就是可以唤醒卡在pthread_cond_wait的线程
    //int err = pthread_cond_signal(&m_pthreadCond); 
    //if (err != 0)
    //{
    //    ngx_log_stderr(err, "In CThreadPool::Call, "
    //        "func pthread_cond_signal failed, the returned errno is %d!", err);
    //    return;
    //}
 
    //(1)如果当前的工作线程全部都忙，则要报警
    if(m_iThreadNum == m_iRunningThreadNum) //线程池中线程总量，跟当前正在干活的线
                                            //程数量一样，说明所有线程都忙碌起来，
                                            //线程不够用了
    {        
        //线程不够用了
        time_t currtime = time(NULL);
        if(currtime - m_iLastEmgTime > 10) //最少间隔10秒钟才报一次线程池中线程不
                                           //够用的问题
        {
            //两次报告之间的间隔必须超过10秒
            m_iLastEmgTime = currtime;  //更新时间
            //写日志，通知这种紧急情况给用户，用户要考虑增加线程池中线程数量了
            ngx_log_stderr(0,"In CThreadPool::Call, there is no free thread "
                "in the pool, consider expanding the thread pool!");
        }
    } 

    return;
}
