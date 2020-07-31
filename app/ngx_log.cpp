//和日志相关的函数放之类
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>    //uintptr_t
#include <stdarg.h>    //va_start....
#include <unistd.h>    //STDERR_FILENO等
#include <sys/time.h>  //gettimeofday
#include <time.h>      //localtime_r
#include <fcntl.h>     //open
#include <errno.h>     //errno

#include "ngx_global.h"
#include "ngx_macro.h"
#include "ngx_func.h"
#include "ngx_c_conf.h"

//全局量

//错误等级，和ngx_macro.h里定义的日志等级宏是一一对应关系(static表示只用于本文件)
static u_char err_levels[][20]  = 
{
    //字符串常量的最后由系统自动加上一个'\0'
    {"stderr"},    //0：控制台错误
    {"emerg"},     //1：紧急
    {"alert"},     //2：警戒
    {"crit"},      //3：严重
    {"error"},     //4：错误
    {"warn"},      //5：警告
    {"notice"},    //6：注意
    {"info"},      //7：信息
    {"debug"}      //8：调试
};

ngx_log_t   ngx_log; //ngx_log_t的定义在ngx_global.h

//---------------------------------------------------------------------------------------
//描述：通过可变参数组合出字符串【支持...省略号形参】，自动往字符串最末尾增加换行符\n，
//往标准错误上输出这个字符串；如果err不为0，表示有错误，会将该错误编号以及对应的错误信
//息一并放到组合出的字符串中一起显示；
//调用格式比如：ngx_log_stderr(0, "invalid option: \"%s\",%d", "testinfo", 123);

void ngx_log_stderr(int err, const char* fmt, ...)
{
    va_list args;                          //创建一个va_list类型变量 

    u_char  errstr[NGX_MAX_ERROR_STR + 1]; //NGX_MAX_ERROR_STR = 2048
    //这块有必要加，至少在va_end处理之前有必要，否则字符串没有结束标记不行的；
    memset(errstr, 0, sizeof(errstr));

    u_char* p, * last;

    last = errstr + NGX_MAX_ERROR_STR;   //last指向errstr最后一个有效元素

    //p指向errstr中字符串"nginx: "的下一个位置                                            
    p = ngx_cpymem(errstr, "nginx: ", 7);   // p = errstr + 7

    va_start(args, fmt);    //使args指向起始的可变参数
    p = ngx_vslprintf(p, last, fmt, args);  //组合出这个字符串保存在errstr里
    va_end(args);   //释放args

    if (err)  //如果错误代码不是0，表示有错误发生，将错误代码和错误信息显示出来
    {
        p = ngx_log_errno(p, last, err);
    }

    //插入换行符到末尾，哪怕覆盖到last指向的内容(即errstr[2048]保存\0)
    if (p > last)
    {
        p = last;
    }
    *(p++) = '\n'; //增加个换行符    

    //往标准错误(一般是屏幕)输出信息连带最后加的换行符，同时若日志文件成功打开，则
    //也会往日志文件中打印这条消息
    write(STDERR_FILENO, errstr, p - errstr);
    if (ngx_log.fd > STDERR_FILENO) //如果系统打开有效的日志文件，这个条件肯定成立，
                                    //此时也才有意义将这个信息写到日志文件
    {
        //errstr本身已经带\n，ngx_log_error_core函数还会在errstr后包一个\n，
        //所以写到日志中会有一个空行多出来
        err = 0;    //不要再次把错误信息弄到字符串里，否则字符串里重复了
        ngx_log_error_core(NGX_LOG_STDERR, err, (const char*)errstr);
    }

    return;
}

//---------------------------------------------------------------------------------------
//描述：给一段内存，一个错误编号，我要组合出一个字符串，形如：(错误编号: 错误原因)，
//放到给的这段内存中去，需保证整个信息我能装到buf中，否则全部抛弃 
//buf：是个内存，要往这里保存数据
//last：放的数据不要超过这里
//err：错误编号，我们是要取得这个错误编号对应的错误字符串，保存到buf中
u_char *ngx_log_errno(u_char *buf, u_char *last, int err)
{
    //char *strerror(int errnum);该函数返回一个指向错误字符串的指针，
    //该错误字符串描述了错误errnum，根据资料不会返回NULL;
    char* p_error_info = strerror(err); 
    size_t len = strlen(p_error_info);

    //printf("%s\n", p_error_info); //debug

    //然后我还要插入一些字符串：" (%d:) "
    char leftstr[10] = {0}; 
    sprintf(leftstr, " (%d: ", err);
    size_t leftlen = strlen(leftstr);

    char rightstr[] = ") "; 
    size_t rightlen = strlen(rightstr);
    
    size_t extralen = leftlen + rightlen; //左右的额外宽度
    if ((buf + len + extralen) <= last)
    {
        //保证整个我装得下，我就装，否则我全部抛弃 
        buf = ngx_cpymem(buf, leftstr, leftlen);
        buf = ngx_cpymem(buf, p_error_info, len);
        buf = ngx_cpymem(buf, rightstr, rightlen);
    }

    return buf;
}

//---------------------------------------------------------------------------------------
//往日志文件中写日志，代码中有自动加换行符，所以调用时字符串不用刻意加\n；
//定向为标准错误，则直接往屏幕上写日志，比如日志文件打不开，则会直接定位到标准错误，
//此时日志就打印到屏幕上，参考ngx_log_init()
//level：一个等级数字，我们把日志分成一些等级，以方便管理、显示、过滤等等，如果这个等
//级数字比配置文件中的等级数字"LogLevel"大，那么该条信息不被写到日志文件中
//err：是个错误代码，如果不是0，就应该转换成显示对应的错误信息,一起写到日志文件中，
//ngx_log_error_core(5, 8, "这个XXX工作的有问题,显示的结果是=%s","YYYY");
void ngx_log_error_core(int level, int err, const char* fmt, ...)
{
    u_char* last;
    u_char* p;  //指向当前要拷贝数据到其中的内存位置
    va_list args;

    u_char  errstr[NGX_MAX_ERROR_STR + 1];  
    memset(errstr, 0, sizeof(errstr));

    last = errstr + NGX_MAX_ERROR_STR;

    struct timeval   tv;   //struct timeval结构体在time.h中定义
                           //struct timeval {
                           //    time_t        tv_sec;     /* seconds */
                           //    suseconds_t   tv_usec; /* microseconds */
                           //};

    struct tm   tm;   //tm结构，这本质上是一个结构体，里面包含了各时间字段
                      //struct tm {
                      //    int tm_sec;  /* seconds after the minute  [0,59] */
                      //    int tm_min;   /* minutes after the hour  [0,59] */
                      //    int tm_hour;  /* hours since midnight  [0,23] */
                      //    int tm_mday;  /* day of the month  [1,31] */
                      //    int tm_mon;   /* months since January  [0,11] */
                      //    int tm_year;  /* years since 1900 */
                      //    int tm_wday;  /* days since Sunday  [0,6] */
                      //    int tm_yday;  /* days since January 1  [0,365] */
                      //    int tm_isdst;  /* daylight savings time flag */
                      //};

    time_t  sec;  //time_t类型，这本质上是一个长整数，表示从
                  //1970-01-01 00:00:00到目前计时时间的秒数

    memset(&tv, 0, sizeof(struct timeval));
    memset(&tm, 0, sizeof(struct tm));

    gettimeofday(&tv, NULL);  //获取当前时间，返回自1970-01-01 00:00:00到现在经历
                              //的秒数(第二个参数是时区，一般不关心)        

    sec = tv.tv_sec;          //秒
    localtime_r(&sec, &tm);   //把参数1的time_t转换为本地时间，保存到参数2中去，
                              //带_r的是线程安全的版本，尽量使用
    tm.tm_mon++;              //月份要调整下正常
    tm.tm_year += 1900;       //年份要调整下才正常

    u_char strcurrtime[40] = { 0 }; //strcurrtime是局部变量，所以在这里等效于：
                                    //  u_char strcurrtime[40];
                                    //  strcurrtime[0] = 0;
                                    //  memset(strcurrtime + 1, 0, 39);
                                     
    ngx_slprintf(strcurrtime,
        (u_char*)-1,    //得到很大一个指针                     
        "%4d/%02d/%02d %02d:%02d:%02d",     
        tm.tm_year, tm.tm_mon,
        tm.tm_mday, tm.tm_hour,
        tm.tm_min, tm.tm_sec);

    //日期增加进来，得到形如："2019/01/08 20:26:07"
    p = ngx_cpymem(errstr, strcurrtime, strlen((const char*)strcurrtime)); 
    //日志级别增加进来，得到形如："2019/01/08 20:26:07 [crit] "
    p = ngx_slprintf(p, last, " [%s] ", err_levels[level]); 
    //支持%P格式，进程id增加进来，得到形如： "2019/01/08 20:50:15 [crit] 2037: "
    p = ngx_slprintf(p, last, "PID_%P: ", ngx_pid);                             

    va_start(args, fmt);    //使args指向起始的可变参数地址 
    p = ngx_vslprintf(p, last, fmt, args);  
    va_end(args);   //释放args                        

    if (err)  //如果错误代码不是0，表示有错误发生
    {
        //错误代码和错误信息也要显示出来
        p = ngx_log_errno(p, last, err);
    }
   
    if (p > last)
    {
        p = last;                             
    }
    *p++ = '\n'; //增加个换行符       

    //这么写代码是图方便：随时可以把流程弄到while后边去；大家可以借鉴一下这种写法
    ssize_t n;
    while (1)
    {
        if (level > ngx_log.log_level)
        {
            //要打印的这个日志的等级太落后（等级数字太大，比配置文件中的数字大)
            //这种日志就不打印了
            break;
        }
        //磁盘是否满了的判断，先算了，还是由管理员保证这个事情吧； 

        //写日志文件        
        n = write(ngx_log.fd, errstr, p - errstr);  //文件写入成功后，如果中途
        if (n == -1)
        {
            //写失败有问题
            if (errno == ENOSPC) //写失败，且原因是磁盘没空间了
            {
                //磁盘没空间了
                //没空间还写个毛线啊
                //先do nothing吧；
            }
            else
            {
                //这是有其他错误，那么我考虑把这个错误显示到标准错误设备吧；
                if (ngx_log.fd != STDERR_FILENO) //当前是定位到文件的，则条件成立
                {
                    n = write(STDERR_FILENO, errstr, p - errstr);
                }
            }
        }
        break;
    } 

    return;
}

//---------------------------------------------------------------------------------------
//描述：日志初始化，就是把日志文件打开，若打开成功，在main函数里面关闭该日志文件
void ngx_log_init()
{
    ngx_log.fd = -1; //全局变量ngx_log：-1(表示日志文件尚未打开)

    u_char* p_log_name = NULL;
    size_t nlen;

    //从配置文件中读取和日志相关的配置信息
    CConfig* p_config = CConfig::GetInstance();
    p_log_name = (u_char*)p_config->GetString("Log");
    if (p_log_name == NULL)
    {
        //没读到，就要给个缺省的路径文件名了，"logs/error_defualt.log"
        p_log_name = (u_char*)NGX_ERROR_LOG_PATH; 
    }

    //缺省日志等级为6【注意】，如果读失败，就给缺省日志等级
    ngx_log.log_level = p_config->GetIntDefault("LogLevel", NGX_LOG_NOTICE);

    //只写打开|追加到末尾|文件不存在则创建【这个需要跟第三参数指定文件访问权限】
    //mode = 0644：文件访问权限，6:110，4:100 (用户：读写，用户所在组：读，其他：读)
    ngx_log.fd = open((const char*)p_log_name, O_WRONLY|O_APPEND|O_CREAT, 0644);
    if (ngx_log.fd == -1)  //如果有错误，则直接定位到标准错误上去 
    {
        ngx_log_stderr(errno, 
            "[alert] can not open error log file: open() \"%s\" failed", 
            p_log_name);
        ngx_log.fd = STDERR_FILENO; //直接定位到标准错误去了        
    }

    return;
}
