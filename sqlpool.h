#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_
#include <iostream>
using namespace std;
#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "locker.h"

//用单例模式创建数据库池(懒汉模式)
class connection_pool
{
public:
    static connection_pool *GetInstance();//局部静态单例

    //构造初始化
    void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log);

    //当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
    MYSQL *GetConnection();

    //释放数据库连接
    bool ReleaseConnection(MYSQL *connect);
    
    //销毁连接池
     void DestroyPool();

private:
connection_pool();
~connection_pool();

public:
    int m_MaxCoon;                //最大连接数
    int m_CurCoon;                //当前已连接的连接数
    int m_FreeCoon;               //当前已经释放的连接数
    locker lock;                
    list<MYSQL *> connList;       //连接池
    sem reserve;                  //信号量

public:
    string m_url;			    //主机地址
	string m_Port;		        //数据库端口号
	string m_User;		        //登陆数据库用户名
	string m_PassWord;	        //登陆数据库密码
	string m_DatabaseName;      //使用数据库名
	int m_close_log;	        //日志开关
};

//将数据库连接的获取与释放通过RAII机制封装,避免手动释放
class connectionRAII
{
public:
    //数据库连接本身是指针类型，所以参数需要通过双指针才能对其进行修改
    connectionRAII(MYSQL **connect, connection_pool *connPool);
    ~connectionRAII();
private:
    MYSQL* connectRAII;
    connection_pool *poolRAII;
};

#endif