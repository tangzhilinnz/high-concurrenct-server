//和打印格式相关的函数定义放这里
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h> //可变参数头文件
#include <stdint.h> //类型相关头文件

#include "ngx_global.h"
#include "ngx_macro.h"
#include "ngx_func.h"

static u_char* ngx_sprintf_num(u_char* buf, u_char* last, uint64_t ui64, 
    u_char zero, uintptr_t hexadecimal, uintptr_t width, uintptr_t is_neg_num);

//---------------------------------------------------------------------------------------
//对于 nginx 自定义的数据结构进行标准格式化输出，就像printf，vprintf一样，
//该函数相当于针对ngx_vslprintf()函数包装了一下。
u_char* ngx_slprintf(u_char* buf, u_char* last, const char* fmt, ...)
{
    va_list args;
    u_char* p;

    va_start(args, fmt); //使args指向起始的可变参数地址
    p = ngx_vslprintf(buf, last, fmt, args);
    va_end(args);        //释放args 

    return p;
}

//---------------------------------------------------------------------------------------
//和上边的ngx_slprintf非常类似
//类printf()格式化函数，比较安全，max指明了缓冲区结束位置
u_char* ngx_snprintf(u_char* buf, size_t max, const char* fmt, ...)
{
    u_char* p;
    va_list   args;

    va_start(args, fmt); //使args指向起始的可变参数地址
    p = ngx_vslprintf(buf, buf + max, fmt, args);
    va_end(args);        //释放args

    return p;
}

//---------------------------------------------------------------------------------------
//对于nginx自定义的数据结构进行标准格式化输出,就像 printf,vprintf 一样，
//例如，给入参数 "abc = %d",13 ，最终buf里得到的结果是 abc = 13 
//buf：  往这里放数据
//last： 放的数据不要超过这里
//fmt：  一系列可变参数格式都放到这个字符串里面
//支持的格式：(%d/%Xd/%xd)数字，(%s)字符串，(%f)浮点，(%P)pid_t，(%p)地址，(%c)字符
u_char* ngx_vslprintf(u_char* buf, u_char* last, const char* fmt, va_list args)
{
    /*#ifdef _WIN64
        typedef unsigned __int64  uintptr_t;
    #else
        typedef unsigned int uintptr_t;
    #endif*/

    u_char zero; //格式填充符

    //uintptr_t能够存储指针的无符号整数类型，头文件stdint.h
    //任何指向void的有效指针都可以转换为此类型，然后再转换回指针为void
    uintptr_t  width, sign, hex, frac_width, scale, n, is_neg_num;

    int       d;     //保存%c对应的可变参，单个ASCII字符
    int64_t   i64;   //保存%d对应的可变参，typedef signed  long long;
    uint64_t  ui64;  //保存%ud对应的可变参，临时作为%f可变参的整数部分也是可以的 
                     //typedef unsigned  long long;
    u_char*   p;     //保存%s对应的可变参，typedef unsigned char  u_char;
    double    f;     //保存%f对应的可变参
    uint64_t  frac;  //%f可变参数,根据%.2f等，取得小数部分的2位后的内容；

    while (*fmt && buf < last) //每次处理一个字符
    {
        if (*fmt == '%')  //%开头的一般为格式符标志，需作进一步处理 
        {
            //-----------------变量初始化工作开始-----------------

            zero = ' '; //填充符默认是空格符号

            // 若%后面有空格和字符'0'，需要全部跳过，直到%后第一个非空格非'0'符号
            fmt++;
            while (*fmt == ' ' || *fmt == '0')
            {
                if (*fmt == '0') //若%后遇到'0'，则填充符设置为0 
                    zero = '0'; 
                fmt++;
            }

            width = 0; //格式字符%后边如果是个数字，这个数字最终会弄到width里边来，
                       //这东西目前只对数字格式有效，比如%d，%f，%P 
            sign = 1;  //显示的是否是有符号数，这里给1，表示是有符号数，
                       //除非你用%u，这个u表示无符号数 
            hex = 0;   //是否以16进制形式显示(比如地址)：
                       //0不是，1是，并以小写字母显示a-f，2是，并以大写字母显示A-F
            frac_width = 6; //小数点后位数字，一般需要和%.10f配合使用，
                            //这里10就是frac_width；默认设置为6位小数
            i64 = 0;    //用%d对应的可变参中的实际数字，会保存在这里
            ui64 = 0;   //用%ud对应的可变参中的实际数字，会保存在这里 
            d = 0;      //用%c对应的可变参中的单个ASCII字符，会保存在这里     
            is_neg_num = 0; //用于表示数字(f,d,P)是否为负数：0不是，1是
            //fmt已指向'%'字符的下一个非空格非'0'字节位置

            //-----------------变量初始化工作结束-----------------

            //这个while就是判断%后边是否是个数字，如果是个数字，就把这个数字取出来，
            //比如%16，最终这个循环就能够把16取出来弄到width里边去
            //%16d 这里最终width = 16;
            while (*fmt >= '0' && *fmt <= '9')
            {
                //如果%后边接的字符是 '0'--'9'之间的内容，比如%16这种；
                //第一次:width = 1；第二次width = 16，所以整个width = 16；
                width = width * 10 + (*(fmt++) - '0'); //width初始值为零
            }
            //fmt已跳过'%'后面的所有数字字符('0'--'9')

            for (;;) //一些特殊的格式，我们做一些特殊的标记，给一些变量特殊值等等
            {
                switch (*fmt)  //处理一些%之后的特殊字符
                {
                case 'u':       //%u，这个u表示无符号
                    sign = 0;   //标记这是个无符号数
                    fmt++;      //往后走一个字符
                    continue;   //回到for继续判断

                case 'X':       //%X，X表示十六进制，不要单独使用，一般是%Xd
                    hex = 2;    //标记以大写字母显示十六进制中的A-F
                    sign = 0;
                    fmt++;
                    continue;

                case 'x':       //%x，x表示十六进制，不要单独使用，一般是%xd
                    hex = 1;    //标记以小写字母显示十六进制中的a-f
                    sign = 0;
                    fmt++;
                    continue;

                case '.':  //其后边必须跟个数字，须与%f配合使用，比如%.10f表示转换浮
                           //点数时，小数点后必须保证10位数字，不足10位则用0来填补；
                    fmt++; //往后走一个字符
                    frac_width = 0; //frac_width被重置前必须先清零，以便以下算法计
                                    //算新值，或当'.'后没有数字时，用此值作为缺省值
                
                    while (*fmt >= '0' && *fmt <= '9') //提取'.'后所有数字
                    {
                        //frac_width初始值为零
                        frac_width = frac_width * 10 + (*(fmt++) - '0');
                    }
                    break;

                default:
                    break;
                } //end switch(*fmt) 
                break;
            } //end for(;;)

            switch (*fmt)
            {
            case '%': //(a)%%时会遇到这个情形，本意是打印一个%，(b)如果输入格式不完整，
                      //如 "%0123uxXXxu.12%" ，这段代码就会将前面的"%0123uxXXxu.12"
                      //截掉，仅保留第二个'%'字符，且不作为格式输入符号%，而是普通'%'
                      //字符存入buf中，然后回到最外层的while循环重新读取一下个字符
                *(buf++) = '%';
                fmt++;
                continue;

            case 'd': //显示整型数据，如果和u配合使用是%ud,则是显示无符号整型数据
                if (sign)  //如果是有符号数
                {
                    //va_arg():遍历可变参数，var_arg的第二个参数表示遍历的这个可变
                    //的参数的类型
                    i64 = (int64_t)va_arg(args, int);
                }
                else //如何是和%ud配合使用，则本条件就成立
                {
                    ui64 = (uint64_t)va_arg(args, u_int);
                }
                break;  //这break掉，直接跳道switch后边的代码去执行,这种凡是break的，
                        //都不做fmt++【switch后仍旧需要进一步处理】

            case 'i': //转换intptr_t型数据，如果用%ui，则转换的数据类型是uintptr_t
                if (sign)
                {
                    i64 = (int64_t)va_arg(args, intptr_t);
                }
                else
                {
                    ui64 = (uint64_t)va_arg(args, uintptr_t);
                }
                break;

            case 'L':  //转换int64_t型数据，如果用%uL，则转换的数据类型是uint64_t
                if (sign)
                {
                    i64 = va_arg(args, int64_t);
                }
                else
                {
                    ui64 = va_arg(args, uint64_t);
                }
                break;

            case 'p':
                ui64 = (uintptr_t)va_arg(args, void*);
                hex = 2;    //标记以大写字母显示十六进制中的A-F
                sign = 0;   //标记这是个无符号数
                zero = '0'; //前边0填充
                width = 2 * sizeof(void*);
                break;

            case 's': //一般用于显示字符串
                p = va_arg(args, u_char*);

                while (*p && buf < last) //没遇到字符串结束标记，且buf装得下这个参数
                {
                    *(buf++) = *(p++);   //比如"%s","abcdefg"，那abcdefg都被装进来
                }

                fmt++;
                continue; //重新从最外层while开始执行 

            case 'c': //显示单个字符(ASCII  0--127 )
                d = va_arg(args, int);
                if ((d & 0xff) >= 0 && (d & 0xff) <= 127)
                    *(buf++) = (u_char)(d & 0xff);
                else
                    *(buf++) = '?';

                fmt++;
                continue; //重新从最外层while开始执行

            case 'P':  //转换一个pid_t类型(就是int类型)
                i64 = (int64_t)va_arg(args, pid_t);
                sign = 1;
                break;

            case 'f': //一般用于显示double类型，如果要显示小数部分，则要形如 %.5f  
                f = va_arg(args, double);
                if (f < 0)  //负数的处理
                {
                    //*(buf++) = '-'; //单独搞个负号出来
                    is_neg_num = 1;
                    f = -f; //那这里f应该是正数了!
                }
                //走到这里保证f肯定 >= 0【不为负数】
                ui64 = (int64_t)f; //正整数部分给到ui64里
                frac = 0;

                if (frac_width) //如果是%.2f，那么frac_width就会是这里的2
                {
                    scale = 1;  //缩放从1开始
                    for (n = frac_width; n; n--)
                    {
                        scale *= 10; //这可能溢出哦
                    }

                    //把小数部分取出来 ，比如格式 %.2f，对应的参数是12.537
                    // (uint64_t) ((12.537 - (double) 12) * 100 + 0.5);  
                    //      = (uint64_t) (0.537 * 100 + 0.5)  
                    //      = (uint64_t) (53.7 + 0.5) 
                    //      = (uint64_t) (54.2) = 54
                    //如果是"%.6f", 21.378，那么这里frac = 378000
                    frac = (uint64_t)((f - (double)ui64) * scale + 0.5);

                    //进位，比如%.2f ，对应的参数是12.999，那么
                    //(uint64_t) (0.999 * 100 + 0.5)  
                    //      = (uint64_t) (99.9 + 0.5) 
                    //      = (uint64_t) (100.4) = 100
                    //而此时scale == 100，两者正好相等
                    if (frac == scale)
                    {
                        //此时代表小数部分四舍五入后向整数部分个位进1位
                        ui64++;
                        //小数部分归0
                        frac = 0;
                    }

                    //若浮点数带小数部分，即frac_width>0,则浮点数的宽度为
                    //整数部分(若是负数需带负号)+小数部分+小数点，
                    //(ui64.len + frac_width + 1)，因此width-frac_width-1
                    //才是整数部分应该设置的宽度值。
                    if (width > frac_width + 1)
                        width = width - frac_width - 1;
                    else // width <= frac_width + 1
                        width = 0;
                } 
                else //frac_width == 0，不带小数部分(.295)，需要判断是否向整数位四
                     //舍五入进一位
                {
                    ui64 = (int64_t)(f + 0.5); //四舍五入后重新分配整数部分到ui64里
                } 

                printf("--------------------------------------------------------\n");
                printf("integer part = (%s%d)\n", (is_neg_num == 1) ? "-" : "", ui64);
                printf("integer_width = %d\n", width);
                printf("frac = (%d)\n", frac);
                printf("frac_width = %d\n", frac_width);
                printf("--------------------------------------------------------\n");

                //正整数部分，先显示出来，比如“1234567”弄到buf中显示
                buf = ngx_sprintf_num(buf, last, ui64, zero, 0, width, is_neg_num);

                if (frac_width) //指定了显示多少位小数
                {
                    if (buf < last)
                    {
                        *(buf++) = '.'; //因为指定显示多少位小数，先把小数点增加进来
                    }
                    //frac这里是小数部分，显示出来，不够的，后边填充'0'字符
                    buf = ngx_sprintf_num(buf, last, frac, '0', 0, frac_width, 0);
                }
                fmt++;
                continue;  //重新从while开始执行

            //..................................
            //................其他格式符，逐步完善
            //..................................

            default:  //如果输入格式不完整，如 "%0123uxXXxu.12@" ，这段代码就会将
                      //前面的"%0123uxXXxu.12" 截掉，仅保留'@'字符，存入buf中，
                      //然后回到最外层的while循环重新读取一下个字符
                *(buf++) = *(fmt++);
                continue;
            } //end switch(*fmt) 

            //显示%d，%P，%p，%i，%L的，会走下来

            //统一把显示的数字都保存到 ui64 里去；
            if (sign) //显示的是有符号数
            {
                if (i64 < 0)  //这可能是和%d格式对应的要显示的数字，i64类型是int64_t
                {
                    //*(buf++) = '-';  //小于0，自然要把负号先显示出来
                    is_neg_num = 1;
                    ui64 = (uint64_t)-i64; //变成无符号数（正数）
                }
                else //显示正数，i64 >= 0
                {
                    ui64 = (uint64_t)i64;
                }
            }

            //把一个数字 比如"1234567"弄到buffer中显示，如果是要求10位，则前边会填充
            //3个空格比如"   1234567"；注意第5个参数hex，是否以16进制显示，如果你是
            //想以16进制显示一个数字则可以%Xd或者%xd，此时hex = 2或者1
            buf = ngx_sprintf_num(buf, last, ui64, zero, hex, width, is_neg_num);
            fmt++;
        }
        //---------------------------------------------------------------------
        else  //当成正常字符，从源fmt拷贝到目标buf里
        {
            //用fmt当前指向的字符赋给buf当前指向的位置，然后buf往前走一个字符位置，
            //fmt往前走一个字符位置
            *(buf++) = *(fmt++);
        } //end if (*fmt == '%') 
    }  //end while (*fmt && buf < last) 

    return buf;
}

//---------------------------------------------------------------------------------------
//以一个指定宽度把一个数字显示在buf对应的内存中，如果实际显示的数字位数比指定的宽度要小，
//比如指定显示10位，而你实际要显示的只有"1234567"，那结果可能是会显示"   1234567"；
//如果你不指定宽度【参数width=0】，则按实际宽度显示；
//你给进来一个%Xd之类的，还能以十六进制数字格式显示出来。
//buf： 往这里放数据
//last：放的数据不要超过这里
//ui64：显示的数字         
//zero: 显示内容时，格式字符%后边接的是否是个'0',如果是zero = '0'，否则zero = ' '， 
//hexadecimal：是否显示成十六进制，0否，1是，以小写字母显示a-f，2是，以大写字母显示A-F
//width:显示内容时，格式化字符%后接的如果是个数字比如%16，那么width=16，所以这个是希望
//显示的宽度值，如果实际显示的内容不够，则后头用zero在前面填充
//无符号型64位整数，值域为(十进制)：0 -- 18446744073709551615
//有符号型64位整数，值域为(十进制)：-9223372036854775808 -- 9223372036854775807
//无符号型32位整型，值域为(十进制)：0 -- 4294967295
//有符号型32位整型，值域为(十进制)：-214748364 -- 2147483647
//is_neg_num：显示数字是否为负数，若为负数，则is_neg_num取1传入，非负数取0传入
//            is_neg_num为1时，该函数会在数字前面加上符号'-'

static u_char* ngx_sprintf_num(u_char* buf, u_char* last, uint64_t ui64,
    u_char zero, uintptr_t hexadecimal, uintptr_t width, uintptr_t is_neg_num)
{
    //#define NGX_INT64_LEN (sizeof("-9223372036854775808") - 1) = 20，
    //注意这里是sizeof是包括末尾的\0，不是strlen；temp[21]能容纳最大的64位
    //无符号整数的十进制表示(18446744073709551615 需20位字符数组)和对应十
    //六进制表示(ffff ffff ffff ffff 需16位字符数组)；temp[21]也能容纳最
    //小的64位有符号整数的十进制表示(-9223372036854775808 需要20位字符)
    u_char* p, temp[NGX_INT64_LEN + 1];
    size_t      len;
    uint32_t    ui32;

    //跟把一个10进制数显示成16进制有关，即和%xd格式符有关，显示的16进制数中a-f小写
    static u_char   hex[] = "0123456789abcdef";
    //跟把一个10进制数显示成16进制有关，即和%Xd格式符有关，显示的16进制数中A-F大写
    static u_char   HEX[] = "0123456789ABCDEF";

    //NGX_INT64_LEN = 20，所以p指向的是temp[20]那个位置，即数组最后一个元素位置
    p = temp + NGX_INT64_LEN;
    //-------------------------------------------------------------------------
    if (hexadecimal == 0)
    {
        //NGX_MAX_UINT32_VALUE :最大的32位无符号数：十进制是‭4294967295
        if (ui64 <= (uint64_t)NGX_MAX_UINT32_VALUE)
        {
            ui32 = (uint32_t)ui64; //能保存下
            do  //这个循环能够把诸如 7654321这个数字保存成：temp[13]=7,temp[14]=6,
                //temp[15]=5,temp[16]=4,temp[17]=3,temp[18]=2,temp[19]=1，而且
                //temp[0..12]以及temp[20]都是不确定的值
            {
                *(--p) = (u_char)(ui32 % 10 + '0');
            } while (ui32 /= 10); //每次缩小10倍等于去掉屁股后边这个数字
        }
        else
        {
            do
            {
                *(--p) = (u_char)(ui64 % 10 + '0');
            } while (ui64 /= 10); //每次缩小10倍等于去掉屁股后边这个数字
        }

        if (is_neg_num == 1) //若是负数，带上符号位'-'
            *(--p) = '-'; 

    }
    //-------------------------------------------------------------------------
    else if (hexadecimal == 1)  //1是以小写字母(a-f)显示16进制整数
    {
        //比如我显示一个1,234,567【十进制数】，对应的二进制数实际是12D687
        do
        {
            //0xf就是二进制1111，ui64 & 0xf，就等于把一个数的最末尾的4个二进制位取出
            //ui64 & 0xf 其实就能分别得到这个16进制数也就是7,8,6,D,2,1这个数字，转成 
            //(uint32_t)，然后以这个为hex的下标，找到这几个数字的对应的能够显示的字符；
            *(--p) = hex[(uint32_t)(ui64 & 0xf)];
        } while (ui64 >>= 4); //ui64 >>= 4，右移4位，就是除以16，相当于把该16进制数的
                            //最末尾一位干掉，原来是12D687，>>4后是12D68，如此反复，
                            //最终肯定有==0时导致while不成立退出循环
                            // 比如1234567 / 16 = 77160(0x12D68) 
                            //     77160 / 16 = 4822(0x12D6)
                            //     ... ...
    }
    //-------------------------------------------------------------------------
    else // hexadecimal == 2    //2是以大写字母(A-F)显示16进制整数
    {
        //参考else if (hexadecimal == 1)，非常类似
        do
        {
            *(--p) = HEX[(uint32_t)(ui64 & 0xf)];
        } while (ui64 >>= 4);
    }
    //-------------------------------------------------------------------------

    len = (temp + NGX_INT64_LEN) - p;  //求数字宽度，比如"-7654321"这个数字，len=8

    if (is_neg_num == 1 && zero == '0' && buf < last)
    {
        *(buf++) = *(p++); //一个特殊处理，如果填空符zero是'0'而且数字为负，则
                           //填空符'0'添加到'-'后面，其他情况暂不用这个处理。
    }

    while (len++ < width && buf < last)
    {
        *(buf++) = zero;  //填充zero进去到buffer中(往数之前增加)，比如你用格式  
                          //ngx_log_stderr(0, "invalid option: %010d\n", 21); 
                          //显示的结果是：nginx: invalid option: 0000000021 
    }

    len = (temp + NGX_INT64_LEN) - p; //还原这个len，也就是要显示的数字的实际宽度

    if ((buf + len) > last)   //剩余的空间不够拷贝整个数字
    {
        len = last - buf; //剩余的buf有多少我就拷贝多少
    }

    //拷贝空间为*buf、*(buf+1)、...、*(buf+(len-1))共len个字节，
    //返回指针buf+len <= last
    return ngx_cpymem(buf, p, len); //把最新buf+len返回去；
}
