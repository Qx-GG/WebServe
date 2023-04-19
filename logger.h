/*
    日志模块(单例模式,懒汉模式)
*/

#ifndef LOG_H
#define LOG_H
#include <iostream>
using namespace std;
#include <string.h>
#include "block_queue.h"
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
class logger
{
public:

    //创建一个返回单例的函数
    static logger* instance();

    //可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *m_filename, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    //异步写日志方法,调用私有方法async_write_log
    static void *flush_log_thread(void *args)
    {
        logger::instance()->async_write_log();
    }

    //将输出内容按标准格式整理
    void write_log(int level,const char * format,...);

    //强制刷新缓冲区
    void flush(void);

private:
    logger();
    virtual ~logger();

    //异步写日志方法
    void *async_write_log()
    {
        string single_log;
        //从阻塞队列中取出一条日志内容，写入文件
        while(m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }

    char dir_name[128];                         //路径名
    char log_name[128];                         //log文件名
    FILE *m_fp;                                 //打开log的文件指针
    static logger * m_instance;                 //创建一个实例(全局唯一)
    int m_today;                                //因为按天分类,记录当前时间是那一天
    char *m_buf;                                //日志缓冲区
    int m_log_buf_size;                         //日志缓冲区大小
    int m_split_lines;                          //日志最大行数
    long long m_count;                          //日志行数记录
    int max_queue_size;                         //最长日志条队列
    bool m_is_async;                            //是否同步标志位
    int m_close_log;                            //关闭日志
    locker m_mutex;                             //锁
    block_queue<string> *m_log_queue;           //阻塞队列
};

#define LOG_DEBUG(format, ...) if(0 == m_close_log) {logger::instance()->write_log(0, format, ##__VA_ARGS__); logger::instance()->flush();}
#define LOG_INFO(format, ...) if(0 == m_close_log) {logger::instance()->write_log(1, format, ##__VA_ARGS__); logger::instance()->flush();}
#define LOG_WARN(format, ...) if(0 == m_close_log) {logger::instance()->write_log(2, format, ##__VA_ARGS__); logger::instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {logger::instance()->write_log(3, format, ##__VA_ARGS__); logger::instance()->flush();}
#endif