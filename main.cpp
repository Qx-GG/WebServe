#include <iostream>
using namespace std;
#include "locker.h"
#include "threadpool.h"
#include <stdlib.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <error.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include <string.h>
#include "http_connect.h"
#include "logger.h"
#include "timer.h"
#include "sqlpool.h"
#define MAX_FD 65535                //最多能接收多少个客户端的连接
#define MAX_EVENT_NUM 1000          //一次监听的最大的文件描述符的数量
#define TIMESLOT  5               //最小超时单位

//定时器相关
static int pipefd[2];
static sort_timer_lst timer_lst;
client_data *users_timer=new client_data[MAX_FD];
bool timeout = false;
bool stop_server = false;

void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

//添加信号捕捉
void addsig( int sig )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}


//将文件描述符设置为非阻塞
int nonblocking(int fd)
{
    int old_flag=fcntl(fd,F_GETFL);
    int new_flag=old_flag|O_NONBLOCK;
    fcntl(fd,F_SETFL,new_flag);
    return old_flag;
}

void timer_handler()
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

void show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}


//将文件描述符添加到epoll
extern void addfd(int epollfd,int fd,bool one_shot);

//从epoll中删除文件描述符
extern void delfd(int epollfd,int fd);

//修改epoll的文件描述符信息
extern void modfd(int epollfd,int fd,int ev);

//日志开关
int m_close_log=0;

//日志系统
void log_write()
{
    logger::instance()->init("log", m_close_log, 2000);
}

//数据库相关
connection_pool *m_connPool;
string m_user;         //登陆数据库用户名
string m_passWord;     //登陆数据库密码
string m_databaseName; //使用数据库名
int m_sql_num=8;


int main(int argc,char *argv[])
{
    //数据库信息,登录名,密码,库名
    string m_user = "root";
    string m_passWord = "997955";
    string m_databaseName = "yourdb";


    if(argc<=1)
    {
        LOG_ERROR("输入错误");
        cout<<"按照如下格式运行："<<argv[0]<<endl;
        exit(-1);
    }

    //获取端口号
    int port=atoi(argv[1]);

    int ret=0;
    //对SIGPIPE信号进行处理
    //addsig(SIGPIPE,SIG_IGN);

    log_write();

    //创建一个数组用来保存所有的客户端信息
    http_connect *users=new http_connect[MAX_FD];

    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //初始化数据库读取表
    users->initmysql_result(m_connPool);

    //创建线程池
    int m_thread_num=8;
    threadpool<http_connect> *pool=NULL;
    try
    {
        pool=new threadpool<http_connect>(m_connPool, m_thread_num);
    }
    catch(...)
    {
        exit(-1);
    }

    //创建一个监听的socket
    int listenfd=socket(PF_INET,SOCK_STREAM,0);

    //设置端口复用
    int optval=1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEPORT,&optval,sizeof(optval));
    

    //绑定地址
    struct sockaddr_in listenaddr;
    listenaddr.sin_family=AF_INET;
    char ip1[]="192.168.149.129";
    int num1;
    inet_pton(AF_INET,ip1,&num1);
    listenaddr.sin_addr.s_addr=num1;
    listenaddr.sin_port=htons(port);
    ret=bind(listenfd,(sockaddr *)&listenaddr,sizeof(listenaddr));
    assert( ret != -1 );

    //监听
    ret=listen(listenfd,5);
    assert( ret != -1 );


    LOG_DEBUG("创建监听成功！");
    //设置epoll
    int epollfd=epoll_create(5);
    if(epollfd==-1)
    {
        perror("epoll");
        exit(-1);
    }

    //创建epoll对象，事件数组
    epoll_event events[MAX_EVENT_NUM];

    //讲监听的socket加入epoll
    addfd(epollfd,listenfd,false);
    http_connect::m_epollfd=epollfd;

    //创建一个管道用来传送信号
    ret=socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);

    //将管道接收端的文件描述符加入epoll
    nonblocking(pipefd[1]);
    addfd(epollfd,pipefd[0],false);
    addsig(SIGALRM);
    addsig(SIGTERM);
    alarm(TIMESLOT);

    //主线程不断循环检测事件的发生
    while(!stop_server)
    {
        int num=epoll_wait(epollfd,events,MAX_EVENT_NUM,-1);
        if(num==-1)
        {
            if(errno!=EINTR)
            {
                LOG_ERROR("%s", "epoll failure");
                break;
            }
        }

        //循环遍历数组
        for(int i=0;i<num;i++)
        {
            int sockfd=events[i].data.fd;

            //有客户端连接进来
            if(sockfd==listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlen=sizeof(client_address);
                int clientfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlen);
                if ( clientfd < 0 ) 
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                //目前的连接数满了
                if(http_connect::m_usernum>=MAX_FD)
                {
                    show_error(clientfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");

                    //回复“服务器满了”
                    close(clientfd);
                    continue;
                }
            
                LOG_INFO("ip地址为:%s的客户端连接进来了!",inet_ntoa(client_address.sin_addr));

                //将新的客户的数据初始化，然后放入用户数组
                users[clientfd].init(clientfd,client_address,m_close_log,m_user, m_passWord, m_databaseName);

                //初始化client_data数据,创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[clientfd].address = client_address;
                users_timer[clientfd].socketfd = clientfd;
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[clientfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[clientfd].timer = timer;
                timer_lst.add_timer(timer);

            }

            //对方异常断开或者错误等事件
            else if(events[i].events&(EPOLLRDHUP|EPOLLERR|EPOLLHUP))
            {
                //关闭连接 
                LOG_ERROR("ip为%s的客户端出现错误!",inet_ntoa(users[sockfd].m_address.sin_addr));

                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);
                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
                LOG_INFO("close fd %d", users_timer[sockfd].socketfd);
                users[sockfd].close_connect();
            }

            //处理信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                LOG_INFO("超时了！");
                bool flag=true;
                int ret = 0;
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    return false;
                }
                else if (ret == 0)
                {
                    return false;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                            case SIGALRM:
                            {
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                                break;
                            }
                        }
                    }
                }
                if (false == flag)
                {
                    LOG_ERROR("%s", "dealclientdata failure");
                }
            }
            else if(events[i].events&EPOLLIN)
            {
                util_timer *timer = users_timer[sockfd].timer;
                //一次性将所有的数据读取出来
                if(users[sockfd].read())
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].m_address.sin_addr));
                    pool->append(users+sockfd);
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);

                        LOG_INFO("%s", "adjust timer once");
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                    LOG_INFO("close fd %d", users_timer[sockfd].socketfd);
                    users[sockfd].close_connect();
                }
            }
            else if(events[i].events&EPOLLOUT)
            {
                util_timer *timer = users_timer[sockfd].timer;
                //一次性写完所有数据
                if(users[sockfd].write())
                {  
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].m_address.sin_addr));
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                        LOG_INFO("%s", "adjust timer once");
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                    LOG_INFO("close fd %d", users_timer[sockfd].socketfd);
                    users[sockfd].close_connect();
                }
            }
        }
        if (timeout)
        {
            timer_handler();
            LOG_INFO("%s", "timer tick"); 
            timeout = false;
        }
    }

    close(epollfd);
    close(listenfd);
    delete []users;
    delete pool;
}