#ifndef HTTPCONNECT_H
#define HTTPCONNECT_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "locker.h"
#include <sys/uio.h>
#include "logger.h"
#include <map>
#include "sqlpool.h"

//将任务封装成类传给请求队列
class http_connect
{
public:
    static int m_epollfd;                   //所有的socket事件都被注册到同一个epoll对象中
    static int m_usernum;                   //用来统计用户的数量
    static const int FILENAME_LEN = 200;    //文件名的最大长度
    static const int READ_BUFFER_SIZE=2048; //定义读缓冲区的大小
    static const int WRITE_BUFFER_SIZE=1024;//定义写缓冲区的大小

    // HTTP请求方法，但我们只支持GET
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
    
    /*
        解析客户端请求时，著状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续请求客户数据
        GET_REQUEST         :   表示获得了一个完整的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求，获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    
    // 从状态机的3种可能状态，即行的读取状态，分别表示
    // 1.读取到一个完整的行2.行出错3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

    http_connect(){};
    ~http_connect(){};
    void process();                                 //处理客户端的请求
    void init(int m_sockfd,const struct sockaddr_in &addr,int close_log,string user, string passwd, string sqlname);//初始化新接收的连接
    void close_connect();                           //关闭连接
    bool read();                                    //非阻塞的读数据
    bool write();                                   //非阻塞的写数据
    
    //主状态机
    void init();                                            //初始化连接的其他的信息
    HTTP_CODE process_read();                               //解析HTTP请求
    bool process_write( HTTP_CODE ret );                    // 填充HTTP应答

    HTTP_CODE prase_request_line(char *test);               //解析HTTP请求首行
    HTTP_CODE prase_headers(char *test);                    //解析HTTP请求头
    HTTP_CODE prase_content(char *test);                    //解析HTTP请求体
    HTTP_CODE do_request();                                 //对数据进行处理
    char *get_line(){return m_readbuf+m_start_line;};
    //从状态机
    LINE_STATUS prase_line();                               //获得HTTP的行数据

    // 这一组函数被process_write调用以填充HTTP应答。
    void unmap();
    bool add_response( const char* format, ... );
    bool add_content( const char* content );
    bool add_content_type();
    bool add_status_line( int status, const char* title );
    bool add_headers( int content_length );
    bool add_content_length( int content_length );
    bool add_linger();
    bool add_blank_line();

    int m_sockfd;                                   //该HTTP连接的socket
    struct sockaddr_in m_address;                   //通信的socket地址
private:
    char m_readbuf[READ_BUFFER_SIZE];               //读缓冲区
    int m_read_idx;                                 //标记读缓冲区中已经被读取的数据的下一个字节
    int m_check_index;                              //当前正在分析的字符在读缓冲区的位置
    int m_start_line;                               //当前正在解析的行的起始位置

    CHECK_STATE m_check_state;                      //当前主状态机所处的状态 
    METHOD m_method;                                //请求方法


    char m_real_file[ FILENAME_LEN ];
    char *m_url;                                    //请求目标文件的文件名
    char *m_version;                                //协议版本，只支持HTTP1.1
    char *m_host;                                   //主机名
    int m_content_length;                           // HTTP请求的消息总长度
    bool m_linger;                                  //判断HTTP请求是否要保持连接

    char m_writebuf[WRITE_BUFFER_SIZE];     //写缓冲区
    int m_write_idx;                        //写缓冲区中待发送的字节数
    char* m_file_address;                   //客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;                //目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息 
    struct iovec m_iv[2];                   //我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。 
    int m_iv_count;

    //int m_TRIGMode;
    int m_close_log;

    int bytes_to_send;              // 将要发送的数据的字节数
    int bytes_have_send;            // 已经发送的字节数

public:
    MYSQL *mysql;
    int m_state;                                            //读为0, 写为1
    void initmysql_result(connection_pool *connPool);       //将数据库中的用户名和密码载入到服务器的map中来
    char *m_string;                                         //存储请求头数据
    int cgi;                                                //是否启用的POST
    map<string, string> m_users;                            //用户名和密码
    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
