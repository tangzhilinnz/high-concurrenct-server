
//系统头文件放上边
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

//自定义头文件放下边,因为g++中用了-I参数，所以这里用<>也可以
#include "ngx_func.h"     //函数声明
#include "ngx_c_conf.h"   //和配置文件处理相关的类,名字带c_表示和类有关
#include "ngx_global.h"   //一些全局/通用定义
#include "ngx_macro.h"

//静态成员定义及赋值 
CConfig* CConfig::m_instance = NULL;

//构造函数
CConfig::CConfig() {}

//析构函数
CConfig::~CConfig()
{    
	std::vector<LPCConfItem>::iterator pos;	
	for(pos = m_ConfigItemList.begin(); pos != m_ConfigItemList.end(); ++pos)
	{		
		delete (*pos);
	}
	m_ConfigItemList.clear(); 

    ngx_log_stderr(0, "~CConfig() executed, "
        "dynamically allocated object(singleton pattern) was detroyed!");

    return;
}

//装载配置文件
bool CConfig::Load(const char *pconfName) 
{   
    FILE *fp;
    fp = fopen(pconfName,"r");
    if(fp == NULL) return false;

    //每一行配置文件读出来都放这里
    char linebuf[501];   //每行配置都不要太长，保持<500字符内，防止出现问题
    
    //走到这里，文件打开成功 
    //feof是C语言标准库函数，其原型在stdio.h中，其功能是检测流上的文件结束符，
    //如果文件结束，则返回非0值，否则返回0，所以没有结束则条件成立
    while (!feof(fp))
    {
        //从文件中读数据，每次读一行，一行最多不要超过500个字符
        if (fgets(linebuf, 500, fp) == NULL)
            continue;

        if (linebuf[0] == 0) //空行
            continue;

        //处理注释行
        if (*linebuf == ';' || *linebuf == ' ' || *linebuf == '#' ||
            *linebuf == '\t' || *linebuf == '\n')
            continue;

    lblprocstring:
        //屁股后边若有换行，回车，空格，tab等都截取掉
        if (strlen(linebuf) > 0)
        {
            // 10-->LF(UNIX/Linux采用换行符LF表示下一行)
            // 13-->CR(苹果机(MAC OS系统)则采用回车符CR表示下一行)
            // (Dos和windows采用回车+换行CR/LF表示下一行)
            // 32-->(space), 9-->('\t'水平制表符(HT))
            if (linebuf[strlen(linebuf) - 1] == 10 || linebuf[strlen(linebuf) - 1] == 13 ||
                linebuf[strlen(linebuf) - 1] == 32 || linebuf[strlen(linebuf) - 1] == 9)
            {
                linebuf[strlen(linebuf) - 1] = 0;
                goto lblprocstring;
            }
        }
        if (linebuf[0] == 0)
            continue;
        if (*linebuf == '[') //[开头的也不处理
            continue;

        //这种 “ListenPort = 5678”走下来；
        //在参数linebuf所指向的字符串中搜索第一次出现字符=（一个无符号字符）的位置，
        //返回一个指向该字符串中第一次出现该字符的指针，如果不包含则返回NULL空指针
        char* ptmp = strchr(linebuf, '=');
        if (ptmp != NULL) // ptmp == NULL, 配置字符串里无等号，放弃读取存入
        {
            LPCConfItem p_confitem = new CConfItem;
            memset(p_confitem, 0, sizeof(CConfItem));
            //等号左侧的拷贝到p_confitem->ItemName
            strncpy(p_confitem->ItemName, linebuf, (int)(ptmp - linebuf));
            //等号右侧的拷贝到p_confitem->ItemContent
            strcpy(p_confitem->ItemContent, ptmp + 1);

            Rtrim(p_confitem->ItemName);
            Ltrim(p_confitem->ItemName);
            Rtrim(p_confitem->ItemContent);
            Ltrim(p_confitem->ItemContent);

            //如果p_confitem->ItemName或者p_confitem->ItemContent其中之一为空串，
            //则抛弃这条配置，首先释放new出来的内存，然后回到最外层while读取新配置
            if (p_confitem->ItemName != NULL && 
                strlen(p_confitem->ItemName) == 0)
            {
                delete p_confitem;
                continue;
            }

            if (p_confitem->ItemContent != NULL && 
                strlen(p_confitem->ItemContent) == 0)
            {
                delete p_confitem;
                continue;
            }

            m_ConfigItemList.push_back(p_confitem); //内存要释放，因为是new出来的 
        }
    }

    fclose(fp); //关闭配置文件，这步不可忘记

    return true;
}

//根据ItemName获取配置信息字符串，不修改不用互斥
const char* CConfig::GetString(const char* p_itemname)
{
    std::vector<LPCConfItem>::iterator pos;
    for (pos = m_ConfigItemList.begin(); pos != m_ConfigItemList.end(); ++pos)
    {
        //strncasecmp用来比较参数s1和s2字符串，比较时会自动忽略大小写的差异
        if (strcasecmp((*pos)->ItemName, p_itemname) == 0)
            return (*pos)->ItemContent;
    }

    return NULL;
}

//根据ItemName获取数字类型配置信息，不修改不用互斥
int CConfig::GetIntDefault(const char* p_itemname, const int def)
{
    std::vector<LPCConfItem>::iterator pos;
    for (pos = m_ConfigItemList.begin(); pos != m_ConfigItemList.end(); ++pos)
    {
        //strncasecmp用来比较参数s1和s2字符串，比较时会自动忽略大小写的差异
        if (strcasecmp((*pos)->ItemName, p_itemname) == 0)
        {
            char* p_tmp = (*pos)->ItemContent;
            for (int i = 0; i < strlen(p_tmp); i++)
            {
                if (p_tmp[i] >= '0' && p_tmp[i] <= '9')
                    continue;
                else //存在非数字字符，直接返回缺省值def
                    return def;
            }

            //atoi(表示 ascii to integer)是把字符串转换成整型数的一个函数
            return atoi((*pos)->ItemContent);
        }
    }

    return def;
}
