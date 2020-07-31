#ifndef __NGX_LOCKMUTEX_H__
#define __NGX_LOCKMUTEX_H__

#include <pthread.h> 
#include <errno.h>

class CLockGuard
{
public:
	explicit CLockGuard(pthread_mutex_t* pMutex)
	//explicit CLockGuard(pthread_spinlock_t* mpSpin)
	{
		m_pMutex = pMutex;
		pthread_mutex_lock(m_pMutex);

		//m_pSpin = mpSpin;
		//pthread_spin_lock(m_pSpin);
	}

	~CLockGuard()
	{
		pthread_mutex_unlock(m_pMutex);
		//pthread_spin_unlock(m_pSpin);
	}

private:
	pthread_mutex_t*    m_pMutex;
	//pthread_spinlock_t*   m_pSpin;
};

#define CLockGuard(x) error "Missing guard object name"
#endif
