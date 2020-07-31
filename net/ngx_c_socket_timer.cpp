//������ �� ��ʱ�� �йصĺ���������
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>    //uintptr_t
#include <stdarg.h>    //va_start....
#include <unistd.h>    //STDERR_FILENO��
#include <sys/time.h>  //gettimeofday
#include <time.h>      //localtime_r
#include <fcntl.h>     //open
#include <errno.h>     //errno
#include <sys/socket.h>
#include <sys/ioctl.h> //ioctl
#include <arpa/inet.h>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
//#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"


void //��̬����
CSocket::SFreeConnToList(void* pConnVoid)
{
	lpngx_connection_t pConn;
	pConn = (lpngx_connection_t)pConnVoid;
	g_socket.ngx_free_connection(pConn);
}