//和设置课执行程序标题名称，重新分配argv和env内存相关的放这里
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>  //env
#include <string.h>

#include "ngx_global.h"
//extern char** g_os_argv;     //原始命令行参数数组,在main中会被赋值
//extern char* gp_envmem;      //指向自己分配的env环境变量的内存
//extern size_t g_envneedmem;  //环境变量所占内存大小
//extern char* gp_argv;        //指向自己分配的argv(argv[1]开始)内存
//extern size_t g_argvlen;     //主程序命令行参数所占内存大小(除argv[0]外)
//extern size_t g_argvneedmem; //主程序命令行参数所占内存大小(包含argv[0]外)
//这些全局变量在nginx.cpp中定义和初始化

//设置可执行程序标题相关函数：分配内存，并且把argv、env变量拷贝到新内存中来
void ngx_init_setproctitle()
{    
    int i;

    //统计命令行参数所占的内存。注意判断argv[i]是否为空作为命令行参数结束标记
    //g_os_argv = (char **) argv; 排除argv[0]，可执行程序名称
    for (i = 1; g_os_argv[i]; i++)
    {
        //+1是因为末尾有\0，是占实际内存位置，strlen函数返回值不算末尾的\0
        g_argvlen += strlen(g_os_argv[i]) + 1;
    }

    g_argvneedmem = g_argvlen + strlen(g_os_argv[0]) + 1;

    //这里无需判断gp_argv == NULL,有些编译器new会返回NULL，有些会报异常，
    //如果在重要的地方new失败了，你无法收场，让程序失控崩溃，助你发现问题为好； 
    gp_argv = new char[g_argvlen];
    memset(gp_argv, 0, g_argvlen);  //内存要清空防止出现问题

    char* ptmp1 = gp_argv;

    //把原来的内存内容(argv[1]及其之后的命令行参数)搬到新地方来
    for (i = 1; g_os_argv[i]; i++)
    {
        size_t size = strlen(g_os_argv[i]) + 1;
        strcpy(ptmp1, g_os_argv[i]); //把原命令行参数内容拷贝到新内存
        g_os_argv[i] = ptmp1; //然后还要让argv[i]指针变量指向这段新内存
        ptmp1 += size;
    }

    //统计环境变量所占的内存。注意判断environ[i]是否为空作为环境变量结束标记
    for (i = 0; environ[i]; i++) 
    {
        //+1是因为末尾有\0,是占实际内存位置，strlen函数返回值不算末尾的\0
        g_envneedmem += strlen(environ[i]) + 1;
    } 
    //这里无需判断gp_envmem == NULL,有些编译器new会返回NULL，有些会报异常，
    //如果在重要的地方new失败了，你无法收场，让程序失控崩溃，助你发现问题为好； 
    gp_envmem = new char[g_envneedmem];
    memset(gp_envmem, 0, g_envneedmem);  //内存要清空防止出现问题

    char *ptmp2 = gp_envmem;

    //把原来的内存内容搬到新地方来
    for (i = 0; environ[i]; i++)
    {
        size_t size = strlen(environ[i]) + 1;
        strcpy(ptmp2, environ[i]); //把原环境变量内容拷贝到新内存
        environ[i] = ptmp2; //然后还要让新环境变量指向这段新内存
        ptmp2 += size;
    }
    return;
}

//设置可执行程序标题
void ngx_setproctitle(const char *title)
{
    //注意：我们的标题长度，不会长到原始标题和原始环境变量都装不下，否则不处理
    
    //(1)计算新标题长度（把末尾的\0也算进来）
    size_t ititlelen = strlen(title) /*+ 1*/; 

    //(2)计算总的原始的argv那块内存的总长度【包括各种参数及环境变量】
    size_t esy = g_argvneedmem + g_envneedmem;
    if( esy < ititlelen)
    {
        //argv和environ总和都存不下
        return;
    }

    //空间够保存标题的，够长，存得下，继续走下来     

    //(3)把标题弄进来，注意原来的命令行参数都已搬家，可以继续使用这些命令行参数,
    char *ptmp = g_os_argv[0]; //让ptmp指向g_os_argv所指向的内存
    strcpy(ptmp, title);
    ptmp += ititlelen; //跳过标题

    //(4)把剩余的原argv以及environ所占的内存全部清0，否则会出现在ps的cmd列可能还
    //会残余一些没有被覆盖的内容，内存总和减去标题字符串长度剩余的大小就是要清0的
    size_t setlen = esy - ititlelen; 
    //memset(ptmp, 0, setlen);
    return;
}