#include <stdio.h>
//#include "Timer.h"
#include "TimeWheel.h"

void TimerFun(void* pParam)
{
    TimeWheel* pTW;
    pTW = (TimeWheel*)pParam;
    printf("Timer expire! Jiffies: %lu\n", pTW->uJiffies_);
}

int main(void)
{
    TimeWheel timeWheel;
    TIMER_NODE* pTn;
    if (!timeWheel.CreateTimeWheel())
    {
        printf("Failed to create time wheel, exit program!\n");
        exit(-2);
    }

    timeWheel.CreateTimer(TimerFun, &timeWheel, 2000, 0);


    pTn = timeWheel.CreateTimer(TimerFun, &timeWheel, 4000, 1000);
    timeWheel.SleepMilliseconds(10001);
    timeWheel.ModifyTimer(pTn, TimerFun, &timeWheel, 2000, 2000);
    timeWheel.SleepMilliseconds(10001);
    timeWheel.DeleteTimer(pTn);
    timeWheel.SleepMilliseconds(3000);
    timeWheel.DestroyTimeWheel();
    return 0;
}