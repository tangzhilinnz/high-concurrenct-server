#ifndef __NGX_LOCKSPIN_H__
#define __NGX_LOCKSPIN_H__

#include <pthread.h> 
#include <errno.h>

class CLockGuardSpin
{
public:
	explicit CLockGuardSpin(pthread_spinlock_t* mpSpin)
	{
		m_pSpin = mpSpin;
		pthread_spin_lock(m_pSpin);
	}

	~CLockGuardSpin()
	{
		pthread_spin_unlock(m_pSpin);
	}

private:
	pthread_spinlock_t*   m_pSpin;
};

#define CLockGuardSpin(x) error "Missing guard object name"
#endif