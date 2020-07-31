
#ifndef __NGX_CONF_H__
#define __NGX_CONF_H__

#include <vector>
#include <mutex>

#include "ngx_global.h"  //一些全局/通用定义

//std::mutex resource_mutex; // c++11, define a mutex

//类名可以遵照一定的命名规则规范，比如老师这里，第一个字母是C，后续的单词首字母大写
class CConfig
{// 单例类
private:
	CConfig();
	CConfig(const CConfig&) {}
	CConfig& operator=(const CConfig&) { return *this; }
public:
	~CConfig();
private:
	static CConfig* m_instance;

public:	
	static CConfig* GetInstance() 
	{	
		// 1.如果if(m_instance != NULL)成立，则肯定表示_instance已经被new过了；
		// 2.如果if(m_instance == NULL)成立，则不一定表示_instance没被new过，因为
		// 如果一个线程if(m_instance == NULL)通过以后，还没有执行new语句，又被切换
		// 到另一个线程执行if(m_instance == NULL)，可以通过并且执行完new语句，然后
		// 又被系统切换回来接着执行一次new语句，就一共执行了两次，显然m_instance == NULL
		// 不代表_instance没被new过，所以需要双重锁定保证安全和效率
		if(m_instance == NULL)
		{
			//std::unique_lock<std::mutex> mymutex(resource_mutex); // c++自动加锁
			//锁
			if(m_instance == NULL)
			{					
				m_instance = new CConfig();
				//局部静态对象只会被构造一次，在调用的时候构造，在main函数执行完毕后才析构
				static CRecycle crl; 
			}
			//放锁		
		}
		return m_instance;
	}	

private:
	//类中套类，它的唯一工作就是在析构函数中删除CConfig的实例，被定义为私有内嵌类，
	//以防该类被在其他地方滥用
	class CRecycle 
	{// CConfig has no special access to the members of CRecycle and vice versa.but
	 //嵌套类可以访问外围类的静态成员变量，即使它的访问权限是私有的(如CConfig::m_instance)
	public:				
		~CRecycle()
		{
			if (CConfig::m_instance)
			{						
				delete CConfig::m_instance; //会调用这个唯一分配对象的析构函数~CConfig()
				CConfig::m_instance = NULL;				
			}
		}
	};
//---------------------------------------------------------------------------------------
public:
    bool Load(const char *pconfName); //装载配置文件
	const char *GetString(const char *p_itemname);
	int  GetIntDefault(const char *p_itemname,const int def);

public:
	std::vector<LPCConfItem> m_ConfigItemList; //存储配置信息的列表
};

#endif
