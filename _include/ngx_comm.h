
#ifndef __NGX_COMM_H__
#define __NGX_COMM_H__

//宏定义----------------------------------------------------------------------------------
#define _PKG_MAX_LENGTH     30000  //每个包的最大长度(包头+包体)不超过这个数字，
								   //实际是包头加包体长度不大于这个值-1000(29000)

//通信 收包状态定义
#define _PKG_HD_INIT         0  //初始状态，准备接收数据包头
#define _PKG_HD_RECVING      1  //接收包头中，包头不完整，继续接收中
#define _PKG_BD_INIT         2  //包头校验成功，准备接收包体
#define _PKG_BD_RECVING      3  //接收包体中，包体不完整，继续接收中
#define _PKG_HD_CHECKING	 4  //包头刚好收完，第一次校验失败后进入
								//包头校验状态
//一条消息被成功放到消息队列，则处理后直接回到_PKG_HD_INIT状态

#define _PKG_HEADER_BUFSIZE  sizeof(COMM_PKG_HEADER) + 10 /*200*/
//因为我要先收包头，定义一个固定大小的数组专门用来
//收包头，这个数字大小一定要大于sizeof(COMM_PKG_HEADER)

//结构定义--------------------------------------------------------------------------------
#pragma pack (1) //对齐方式，1字节对齐(结构之间成员不做任何字节对齐：紧密排列)

//一些和网络通讯相关的结构放在这里
//包头结构
typedef struct _COMM_PKG_HEADER
{
	unsigned short pkgLen;  //报文总长度(包头+包体)--2字节，2字节可以表示的最大数字为
						    //6万多，我们定义_PKG_MAX_LENGTH 30000，所以用pkgLen足
						    //够保存下整个包(包头+包体)的长度
	unsigned short msgCode; //消息类型代码--2字节，用于区别每个不同的命令(不同的消息)
	int            crc32;   //CRC32效验--4字节，为了防止收发数据中(包体)出现收到内容
							//和发送内容不一致的情况，引入这个字段做一个基本的校验用
	int			   crc32_h; //CRC32效验--4字节，包头的CRC32效验，用于错误包，恶意包，
							//畸形包的甄别；正常的包都能被连续完整地读取(包头+包体)，
							//因此从包头就能判断此包是否正常，相当于一种加密措施
}COMM_PKG_HEADER, *LPCOMM_PKG_HEADER;

#pragma pack() //取消指定对齐，恢复缺省对齐

//用于表示连接的事件类型
enum eventConnType
{
	E_CLOSE_CONN = 0, //关闭连接事件
	//E_RECYLE_CONN,    //回收连接事件

	E_MAX_TYPE,		  //连接事件的总数
};

#endif
