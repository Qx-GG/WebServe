#include "http_connect.h"
#include <fstream>
#include <mysql/mysql.h>

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

locker m_lock;
map<string, string> users;

void http_connect::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//初始化静态成员变量
int http_connect::m_epollfd=-1;
int http_connect::m_usernum=0;

//网站的根目录
const char* doc_root="/home/qinxin/WebServer/root";

//将文件描述符设置为非阻塞
int setnonblocking(int fd)
{
    int old_flag=fcntl(fd,F_GETFL);
    int new_flag=old_flag|O_NONBLOCK;
    fcntl(fd,F_SETFL,new_flag);
    return old_flag;
}

//向epoll中添加需要监听的文件描述符
void addfd(int epollfd,int fd,bool one_shot)
{
    epoll_event event;
    event.data.fd=fd;
    event.events=EPOLLIN | EPOLLRDHUP | EPOLLET;
    if(one_shot)
    {
        // 防止同一个任务被不同的线程处理
        event.events|=EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);

    //设置文件描述符为非阻塞，因为可能epoll在ET模式
    setnonblocking(fd);
}

//从epoll中删除监听的文件描述符
void delfd(int epollfd,int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

//修改epoll中监听的文件描述符,主要是重置epoll中的该文件描述符的EPOLLONESHOT,确保下一次可读时，EPOLLIN事件能够被触发 
void modfd(int epollfd,int fd,int ev)
{
    epoll_event event;
    event.data.fd=fd;
    event.events=ev|EPOLLONESHOT|EPOLLRDHUP|EPOLLET;
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

//初始化新接收的连接
 void http_connect::init(int m_sockfd,const struct sockaddr_in &addr,int close_log,string user,string passwd,string sqlname)
 {
    this->m_sockfd=m_sockfd;
    this->m_address=addr;

    //设置端口复用
    int optval=1;
    setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEPORT,&optval,sizeof(optval));

    //将新的socket加入epoll
    addfd(m_epollfd,m_sockfd,true);

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    m_close_log = close_log;

    //总的用户数+1
    m_usernum++;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());
    init();
 }

 void http_connect::init()
 {
    bytes_to_send = 0;
    bytes_have_send = 0;

    m_check_state=CHECK_STATE_REQUESTLINE;//初始状态为解析请求首行
    m_check_index=0;
    m_start_line=0;
    m_read_idx=0;
    m_method=GET;
    m_url=0;
    m_version=0;
    m_content_length = 0;
    m_linger=false;
    m_host=0;
    m_write_idx=0;
    m_state=0;
    cgi=0;
    mysql=NULL;

    memset(m_readbuf, '\0', READ_BUFFER_SIZE);
    memset(m_writebuf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
 }


 //关闭连接
 void http_connect::close_connect()
 {
    if(m_sockfd!=-1)
    {
        delfd(m_epollfd,m_sockfd);
        m_sockfd=-1;
        m_usernum--;
    }
 }

//一次性把数据全部读取完
 bool http_connect::read()
 {
    //如果读取的数据量大于读缓冲区的容量大小
    if(m_read_idx>=READ_BUFFER_SIZE)
    {
        return false;
    }
    //定义已经读取到的字节数
    int bytes_read=0;

    //循环读取客户数据，直到无数据可读或者对方关闭连接
    while(true)
    {
        bytes_read=recv(m_sockfd,m_readbuf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        if(bytes_read==-1)
        {
            if(errno==EAGAIN||errno==EWOULDBLOCK)
            {
                //没有数据
                break;
            }
            else
            {
                return false;
            }
        }
        else if(bytes_read==0)
        {
            //对方关闭连接
            return false; 
        }
        m_read_idx+=bytes_read;
    }
    return true;
 }

//主状态机
//解析HTTP请求
http_connect::HTTP_CODE http_connect::process_read()
{
    //定义初始状态
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;
    char *text=NULL;
    while(((m_check_state==CHECK_STATE_CONTENT)&&(line_status==LINE_OK))||((line_status=prase_line())==LINE_OK))//解析到一行完整的数据或者解析到请求体
    {
        //获取一行数据
        text=get_line();
        m_start_line=m_check_index;
        printf( "got 1 http line: %s\n", text);
        LOG_INFO("%s", text);
        switch(m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
                {
                    ret=prase_request_line(text);
                    if(ret==BAD_REQUEST)
                    {
                        return BAD_REQUEST;
                    }
                    break;
                }

            case CHECK_STATE_HEADER:
                {
                    ret=prase_headers(text);
                    if(ret==BAD_REQUEST)
                    {
                        return BAD_REQUEST;
                    }
                    else if(ret==GET_REQUEST)
                    {
                        return do_request();
                    }
                    break;
                }

            case CHECK_STATE_CONTENT:
            {
                ret=prase_content(text);
                if(ret==GET_REQUEST)
                {
                    return do_request();
                }
                line_status=LINE_OPEN;
                break;
            }

            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

//解析HTTP请求首行:获得请求方法，目标URL，HTTP版本
http_connect::HTTP_CODE http_connect::prase_request_line(char *text)
{
    // GET /index.html HTTP/1.1
    m_url=strpbrk(text," \t");//m_url-->/index.html HTTP/1.1
    if (! m_url) 
    { 
        return BAD_REQUEST;
    }
    *m_url++='\0';  // GET\0/index.html HTTP/1.1
    char *method=text;
    if(strcasecmp(method,"GET")==0)
    {
        m_method=GET;
    }
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
    {
        return BAD_REQUEST;
    }
    m_url += strspn(m_url, " \t");
    m_version=strpbrk(m_url," \t");
    if(!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++='\0';//m_url-->/\0;
    m_version += strspn(m_version, " \t");
    if(strcasecmp(m_version,"HTTP/1.1")!=0)
    {
        return BAD_REQUEST;
    }

    if(strncasecmp(m_url,"http://",7)==0)
    {
        m_url+=7;
        m_url=strchr(m_url,'/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if(!m_url||m_url[0]!='/')
    {
        return BAD_REQUEST;
    }
    if (strlen(m_url) == 1)
        {
            strcat(m_url, "judge.html");
        }
    m_check_state=CHECK_STATE_HEADER;//主状态机的状态变成检查请求头
    return NO_REQUEST; 
}

//解析HTTP请求头
http_connect::HTTP_CODE http_connect::prase_headers(char *text)
{
    //遇到空行，表示头部字段解析完毕
    if(text[0]=='\0')
    {
        //如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体
        if(m_content_length!=0)
        {
            // 状态机转移到CHECK_STATE_CONTENT状态
            m_check_state=CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则说明我们已经得到一个完整的HTTP请求
        return GET_REQUEST;
    }
    else if(strncasecmp(text,"Connection:",11)==0)
    {
        //处理Connection头部字段 Connection:keep-alive
        text+=11;
        text+=strspn(text," \t");
        if(strcasecmp(text,"keep-alive")==0)
        {
            m_linger=true;
        }
    }
    else if(strncasecmp(text,"Content-Length:",15)==0)
    {
        //处理Content-Length头部
        text+=15;
        text+=strspn(text," \t");
        m_content_length=atol(text);
    }
    else if(strncasecmp(text,"Host:",5)==0)
    {
        //处理Host头部信息
        text+=5;
        text+=strspn(text," \t");
        m_host=text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_connect::HTTP_CODE http_connect::prase_content(char *text)
{
    if(m_read_idx>=(m_content_length+m_check_index))
    {
        text[m_content_length]='\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//从状态机
//解析一行数据，判断依据时\n
http_connect::LINE_STATUS http_connect::prase_line()
{
    char temp;
    for(;m_check_index<m_read_idx;++m_check_index)
    {
        temp=m_readbuf[m_check_index];
        if(temp=='\r')
        {
            if((m_check_index+1)==m_read_idx)
            {
                return LINE_OPEN;
            }
            else if(m_readbuf[m_check_index+1]=='\n')
            {
                m_readbuf[m_check_index++]='\0';
                m_readbuf[m_check_index++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if(temp=='\n')
        {
            if((m_check_index>1)&&(m_readbuf[m_check_index-1]=='\r'))
            {
                m_readbuf[m_check_index-1]='\0';
                m_readbuf[m_check_index++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } 
    }
    return LINE_OPEN;
}

//当得到一个完整的、正确的HTTP请求时，我们就分析目标文件的属性
//如果目标文件存在、对所有用户可读，并且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_connect::HTTP_CODE http_connect::do_request()
{
    // "/home/qinxin/WebServer/root"
    strcpy(m_real_file,doc_root);
    int len = strlen( doc_root );
    const char *p = strrchr(m_url, '/');
    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "','");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
    {
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    if (stat(m_real_file, &m_file_stat) < 0)
    {
        return NO_RESOURCE;
    }
    if (!(m_file_stat.st_mode & S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }
    if (S_ISDIR(m_file_stat.st_mode))
    {
        return BAD_REQUEST;
    }
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_connect::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

//写HTTP响应
 bool http_connect::write()
 {
    int temp = 0;
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp < 0 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_writebuf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0)
        {
            // 没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }

    } 
}

// 往写缓冲中写入待发送的数据
bool http_connect::add_response( const char* format, ... ) 
{
    if( m_write_idx >= WRITE_BUFFER_SIZE ) 
    {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_writebuf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) 
    {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );

    LOG_INFO("request:%s", m_writebuf);
    return true;
}

bool http_connect::add_status_line( int status, const char* title ) 
{
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_connect::add_headers(int content_len) 
{
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
    return true;
}

bool http_connect::add_content_length(int content_len) 
{
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_connect::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_connect::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_connect::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_connect::add_content_type() 
{
    return add_response("Content-Type:%s\r\n", "text/html");
}


// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_connect::process_write(HTTP_CODE ret) 
{
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);
            if (m_file_stat.st_size != 0)
            {
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_writebuf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else
            {
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
        default:
            return false;
    }
    m_iv[ 0 ].iov_base = m_writebuf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

//由线程池的工作线程调用，用来处理HTTP请求
void http_connect::process()
{
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST ) {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }
    
    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_connect();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}