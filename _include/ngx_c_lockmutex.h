
#ifndef __NGX_LOCKMUTEX_H__
#define __NGX_LOCKMUTEX_H__

#include <pthread.h> 
#include <errno.h>

class CLock
{
public:
	explicit CLock(pthread_mutex_t* pMutex)
	{
		m_pMutex = pMutex;
		int err = pthread_mutex_lock(m_pMutex); //加锁互斥量
		if (err != 0)
		{
			ngx_log_stderr(err, "In CLock::CLock, "
				"func pthread_mutex_lock failed，the returned errno is %d!", err);
		}
	}

	~CLock()
	{
		int err = pthread_mutex_unlock(m_pMutex); //解锁互斥量
		if (err != 0)
		{
			ngx_log_stderr(err, "In CLock::~CLock, "
				"func pthread_cond_wait failed，the returned errno is %d!", err);

		}
	}

private:
	pthread_mutex_t* m_pMutex;
};

#define CLock(x)    error "Missing guard object name"
#endif
