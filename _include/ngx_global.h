
#ifndef __NGX_GBLDEF_H__
#define __NGX_GBLDEF_H__

#include <signal.h>

#include "ngx_c_slogic.h"
#include "ngx_c_threadpool.h"
#include "ngx_c_crc32.h"
#include "mem_pool.h"
#include "TimeWheel.h"
//#include "tzmalloc.h"

// 一些比较通用的定义放在这里
//一些全局变量的外部声明也放在这里

//类型定义--------------------------------------------------------------------------------


//用于存放从配置文件nginx.conf读取的item
typedef struct _CConfItem
{
	char ItemName[50];
	char ItemContent[500];
}CConfItem, *LPCConfItem;

//和运行日志相关 
typedef struct _ngx_log_t
{
	int log_level;  //日志级别 或者日志类型，ngx_macro.h里分0-8共9个级别
	int fd;	        //日志文件描述符
}ngx_log_t;

//外部全局量声明(在nginx.cpp中定义的全局变量)
extern char**  g_os_argv;        //原始命令行参数数组,在main中会被赋值
extern char*   gp_envmem ;       //指向自己分配的env环境变量的内存
extern size_t  g_envneedmem;     //环境变量所占内存大小
extern char*   gp_argv;          //指向自己分配的argv(argv[1]开始)内存
extern size_t  g_argvlen;        //主程序命令行参数所占内存大小(除argv[0]外)
extern size_t  g_argvneedmem;    //主程序命令行参数所占内存大小(包含argv[0]外)
extern int     g_daemonized;     //守护进程标记，标记是否启用了守护进程模式，
							     //0：未启用，1：启用了
extern CLogicSocket g_socket;    //socket全局对象
extern CThreadPool g_threadpool; //thread pool全局对象

//外部全局变量声明(在nginx.cpp中定义的全局变量)
extern pid_t         ngx_pid;
extern pid_t         ngx_parent;

//外部全局变量声明(在ngx_log.cpp中定义的全局变量)
extern ngx_log_t     ngx_log;

//外部全局变量声明(在nginx.cpp中定义的全局变量)
extern int           ngx_process;
extern sig_atomic_t  ngx_reap;
extern int           g_stopEvent;

//外部全局变量声明(在nginx.cpp中定义的全局变量)
extern CCRC32* pCRC32;

//外部全局变量声明(在nginx.cpp中定义的全局变量)
//extern TzMemPool<_SPN64K, _LARGE>* pMalloc;
extern MemPool memPool;
extern TimeWheel timeWheel;
extern int g_wrongPKGAdmit;

#endif
