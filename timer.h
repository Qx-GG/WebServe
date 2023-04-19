#ifndef TIMER
#define TIMER

#include <iostream>
using namespace std;
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
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "logger.h"
#include "http_connect.h"

//连接资源结构体成员需要用到定时器类,需要前向声明
class util_timer;

//连接资源
struct client_data
{
    //客户端socket地址
    sockaddr_in address;

    //socket文件描述符
    int socketfd;

    //定时器
    util_timer *timer;
};

class util_timer
{
public:

    util_timer()
    {
        prev=NULL;
        next=NULL;
    }
    //超时时间
    time_t expire;

    //回调函数
    void (*cb_func)(client_data*);

    //连接资源
    client_data* user_data;

    //前向定时器
    util_timer* prev;

    //后缀定时器
    util_timer* next;

};

//定时器容器
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    //添加定时器，内部调用私有成员add_timer
    void add_timer(util_timer *timer);

    //调整定时器，任务发生变化时，调整定时器在链表中的位置
    void adjust_timer(util_timer *timer);

    //删除定时器
    void del_timer(util_timer *timer);

    //定时任务处理函数
    void tick();

private:

    //私有成员，被公有成员add_timer和adjust_time调用,主要用于调整链表内部结点
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;
    util_timer *tail;
};

void cb_func(client_data* user_data);

#endif