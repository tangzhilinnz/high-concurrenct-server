#ifndef __NGX_MACRO_H__
#define __NGX_MACRO_H__

#include "ngx_global.h"

//各种#define宏定义相关的定义放这里

#define LF     (u_char) '\n'
#define CR     (u_char) '\r'
#define CRLF   "\r\n"

#define NGX_MAX_ERROR_STR   2048   //显示的错误信息最大数组长度

//简单功能函数----------------------------------------------------------------------------

//ngx_cpymem返回的是目标(拷贝数据后)的下一位，连续复制多段数据时方便，
//注意#define写法，n这里用()包着，防止出现什么错误
//void *memcpy(void *destin, const void *source, unsigned n) 
//从源source中拷贝n个字节到目标destin中，返回一个指向目标存储区destin的指针
#define ngx_memcpy(dst, src, n)   (void) memcpy(dst, src, n)
#define ngx_cpymem(dst, src, n)	  (((u_char *) memcpy(dst, src, n)) + (n)) 

//void* memmove(void* dest, const void* source, size_t count)
//memmove用于从source拷贝count个字符到dest，如果目标区域和源区域有重叠的话，
//memmove能够保证源串在被覆盖之前将重叠区域的字节拷贝到目标区域中
#define ngx_memmove(dst, src, n)   (void) memmove(dst, src, n)
#define ngx_movemem(dst, src, n)   (((u_char *) memmove(dst, src, n)) + (n))

#define ngx_abs(value)       (((value) >= 0) ? (value) : - (value))
#define ngx_max(val1, val2)  ((val1 < val2) ? (val2) : (val1))
#define ngx_min(val1, val2)  ((val1 > val2) ? (val2) : (val1))

//数字相关--------------------------------------------------------------------------------

//最大的32位无符号数：十进制是‭4294967295‬
#define NGX_MAX_UINT32_VALUE   (uint32_t) 0xffffffff   
// NGX_INT64_LEN == 20
#define NGX_INT64_LEN          (sizeof("-9223372036854775808") - 1)     

//日志相关--------------------------------------------------------------------------------

//我们把日志一共分成九个等级【级别从高到低，数字最小的级别最高，数字大的级别最低】，
//以方便管理、显示、过滤等等；最高级别日志(NGX_LOG_STDERR)，日志的内容不再写入
//log参数指定的文件，而是会直接将日志输出到标准错误设备比如控制台屏幕
#define NGX_LOG_STDERR            0    //控制台错误【stderr】
#define NGX_LOG_EMERG             1    //紧急 【emerg】
#define NGX_LOG_ALERT             2    //警戒 【alert】
#define NGX_LOG_CRIT              3    //严重 【crit】
#define NGX_LOG_ERR               4    //错误 【error】：属于常用级别
#define NGX_LOG_WARN              5    //警告 【warn】：属于常用级别
#define NGX_LOG_NOTICE            6    //注意 【notice】
#define NGX_LOG_INFO              7    //信息 【info】
#define NGX_LOG_DEBUG             8    //调试 【debug】：最低级别

//定义缺省日志存放的路径和文件名，即当nginx.conf里面没有配置Log。
#define NGX_ERROR_LOG_PATH	"logs/error_defualt.log"  

//进程相关--------------------------------------------------------------------------------

//标记当前进程类型
#define NGX_PROCESS_MASTER     0  //master进程，管理进程
#define NGX_PROCESS_WORKER     1  //worker进程，工作进程

//和数据包校验有关的宏定义
#define CRC32(str, len)   pCRC32->Get_CRC(str, len)

//和内存分配有关的全局变量
#define VER            0  /* modify VER to choose the version here */

#if (VER == 1)
#define malloc(size)	memPool.Malloc(size)
#define free(ptr)		memPool.Free(ptr)	
#endif
//.......其他待扩展


#endif
