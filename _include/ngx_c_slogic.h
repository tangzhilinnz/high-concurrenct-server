#ifndef __NGX_C_SLOGIC_H__
#define __NGX_C_SLOGIC_H__

#include <sys/socket.h>
#include "ngx_c_socket.h"

//处理逻辑和通讯的子类，继承CSocket类中的成员有：
//protected:
//	size_t		lenPkgHeader;	//sizeof(COMM_PKG_HEADER);		
//	size_t      lenMsgHeader;    //sizeof(STRUC_MSG_HEADER);
//  void msgSend(char* psendbuf);   //把数据扔到待发送对列中
//public:
//	virtual bool Initialize();            //初始化函数[父进程中执行]
//	virtual bool Initialize_subproc();    //初始化函数[子进程中执行]
//	virtual void Shutdown_subproc();      //退出清理函数[子进程中执行]
//
//	virtual void threadRecvProcFunc(char* pMsgBuf); 	
//	char* outMsgRecvQueue();        									
//	int ngx_epoll_init();       
//  int ngx_epoll_oper_event(int fd, uint32_t eventtype, uint32_t flag, 
//		int bcaction, lpngx_connection_t pConn);
//	int ngx_epoll_process_events(int timer); 

class CLogicSocket : public CSocket   //继承自父类CScoekt
{
public:
	CLogicSocket();                   //构造函数
	virtual ~CLogicSocket();          //释放函数
	virtual bool Initialize();        //初始化函数

public:
	//通用收发数据相关函数
	void SendNoBodyPkgToClient(LPSTRUC_MSG_HEADER pMsgHeader, unsigned short iMsgCode);

	//各种业务逻辑相关函数都在之类
	bool _HandleRegister(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, 
		char* pPkgBody, unsigned short iBodyLength);
	bool _HandleLogIn(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, 
		char* pPkgBody, unsigned short iBodyLength);
	bool _HandleTest(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader,
		char* pPkgBody, unsigned short iBodyLength);
	bool _HandlePing(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, 
		char* pPkgBody, unsigned short iBodyLength);
	//void _HandleWrongPKG(LPSTRUC_MSG_HEADER pMsgHeader);

public:
	virtual void threadRecvProcFunc(char* pMsgBuf);
};

#endif
