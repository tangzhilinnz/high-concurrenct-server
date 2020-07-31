#ifndef __NGX_C_CRC32_H__
#define __NGX_C_CRC32_H__

#include <stddef.h>  //NULL

class CCRC32
{// 单例类
private:
	CCRC32();
public:
	~CCRC32();
private:
	static CCRC32* m_instance;

public:	
	static CCRC32* GetInstance() 
	{
		if(m_instance == NULL)
		{
			//锁
			if(m_instance == NULL)
			{				
				m_instance = new CCRC32();
				//局部静态对象只会被构造一次，在调用的时候构造，在main函数执行完毕后才析构
				static CRecycle crl;
			}
			//放锁
		} 
		return m_instance;
	}	

private:
	//类中套类，它的唯一工作就是在析构函数中删除CCRC32的实例，被定义为私有内嵌类，
	//以防该类被在其他地方滥用
	class CRecycle
	{// CCRC32 has no special access to the members of CRecycle and vice versa.but
	 //嵌套类可以访问外围类的静态成员变量，即使它的访问权限是私有的(如CCRC32::m_instance)
	public:				
		~CRecycle()
		{
			if (CCRC32::m_instance)
			{	
				//会调用这个唯一分配对象的析构函数~CCRC32()
				delete CCRC32::m_instance; 
				CCRC32::m_instance = NULL;				
			}
		}
	};
	//-----------------------------------------------------------------------------------
public:
	void Init_CRC32_Table();
    unsigned int Reflect(unsigned int ref, char ch); //Reflects CRC bits in the
												     //lookup table
    int Get_CRC(unsigned char* buffer, unsigned int dwSize);
    
public:
    unsigned int crc32_table[256]; //Lookup table arrays
};

#endif


