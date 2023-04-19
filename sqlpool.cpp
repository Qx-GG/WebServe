#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include "sqlpool.h"
connection_pool::connection_pool()
{
    this->m_CurCoon=0;
    this->m_FreeCoon=0;
}

connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

//RAII机制销毁连接池
connection_pool::~connection_pool()
{
    DestroyPool();
}

//构造初始化
void connection_pool::init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log)
{
    //初始化数据库信息
    this->m_url=url;
    this->m_User=User;
    this->m_PassWord=PassWord;
    this->m_DatabaseName=DataBaseName;
    this->m_Port=Port;

    //创建Maxconn条连接
    for(int i=0;i<MaxConn;i++)
    {
        MYSQL *connect=NULL;
        connect=mysql_init(connect);

        if(connect==NULL)
        {
            cout<<"Error:"<<mysql_errno(connect);
            exit(-1);
        }
        connect=mysql_real_connect(connect,url.c_str(),User.c_str(),PassWord.c_str(),DataBaseName.c_str(),Port,NULL,0);

        if(connect==NULL)
        {
            cout<<"Error:"<<mysql_errno(connect);
            exit(-1);
        }

        //更新连接池和空闲连接数
        connList.push_back(connect);
        m_FreeCoon++;
    }

    //将信号量初始化为最大连接数
    reserve=sem(m_FreeCoon);
    this->m_MaxCoon=m_FreeCoon;
}

//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
    MYSQL* connect=NULL;
    //数据库池里面没有可以用的连接
    if(connList.size()==0)
    {
        return NULL;
    }

    //信号量-1，如果信号量为0则阻塞等待
    reserve.wait();

    //加锁
    lock.lock();

    //取出连接
    connect=connList.front();
    connList.pop_front();

    //已连接数+1，可用连接数-1
    m_CurCoon++;
    m_FreeCoon--;

    //解锁
    lock.unlock();

    return connect;
}

//释放数据库连接
bool connection_pool::ReleaseConnection(MYSQL *connect)
{
    if(connect==NULL)
    {
        return false;
    }

    lock.lock();

    //数据库池添加可用连接
    connList.push_back(connect);

    //已连接数-1，可用连接数+1
    m_CurCoon--;
    m_FreeCoon++;

    lock.unlock();

    //信号量+1
    reserve.post();
    return true;
}

//销毁连接池，通过迭代器遍历连接池链表，关闭对应数据库连接，清空链表并重置空闲连接和现有连接数量

void connection_pool::DestroyPool()
{
    lock.lock();

    if(connList.size()>0)
    {
        //通过迭代器遍历连接池链表，关闭数据库连接
        for(auto it=connList.begin();it!=connList.end();it++)
        {
            MYSQL* connect=*it;
            mysql_close(connect);
        }

        //重置空闲连接和现有连接数量
        m_CurCoon=0;
        m_FreeCoon=0;

        //清空链表
        connList.clear();
    }

    lock.unlock();
}

//不直接调用获取和释放连接的接口，将其封装起来，通过RAII机制进行获取和释放
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool)
{
    *SQL=connPool->GetConnection();

    connectRAII=*SQL;
    poolRAII=connPool;
}

connectionRAII::~connectionRAII()
{
    poolRAII->ReleaseConnection(connectRAII);
}