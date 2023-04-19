#ifndef THREADPOOL_H
#define THREADPOOL_H
#include <pthread.h>
#include <queue>
#include "locker.h"
#include <exception>
#include "sqlpool.h"

//线程池类，定义成模板类(为了线程池更加通用)，T是指任务类,里面包含了请求队列
template<typename T>
class threadpool
{
public:
    threadpool(connection_pool *connPool,int threadpool_num=8,int max_request=1000);
    ~threadpool();
    //添加任务
    bool append(T* request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void* worker(void *arg);//静态函数(只能操作静态成员)
    void run();
    
private:
    //线程数量
    int m_threadpool_num;

    //线程池大小，是一个数组(大小为m_threadpool_num)
    pthread_t *m_threads;

    //数据库
    connection_pool *m_connPool;  

    //请求队列中最多允许的等待处理的请求事件的数量
    int m_max_request;

    //请求队列
    queue<T *> m_workqueue;

    //互斥锁
    locker m_queuelocker;

    //信号量用来判断是否有事件需要处理
    sem m_queuestat;

    //是否结束线程
    bool m_stop;

};

template<typename T>
threadpool<T>::threadpool(connection_pool *connPool,int threadpool_num,int max_request):
    m_threadpool_num(threadpool_num),m_max_request(max_request),m_stop(false),m_threads(NULL),m_connPool(connPool)
    {
        if((threadpool_num<=0)||(max_request<=0))
        {
            throw std::exception();
        }

        //创建线程数组
        m_threads=new pthread_t[threadpool_num];
        if(!m_threads)
        {
            throw std::exception();
        }

        //创建theadpool_num个线程，并将它们设置为线程脱离(因为主线程的工作不包括释放工作线程的资源这个任务，所以需要系统自动取回收资源)
        for(int i=0;i<threadpool_num;i++)
        {
            cout<<"create the "<<i<<" thread"<<endl;
            if(pthread_create(m_threads+i,NULL,worker,this)!=0)
            {
                delete []m_threads;
                throw std::exception();
            }
            if(pthread_detach(m_threads[i]))
            {
                delete []m_threads;
                throw std::exception();
            }
        }
    }

template<typename T>
threadpool<T>::~threadpool()
{
    delete []m_threads;
    m_stop=true;
}

template<typename T>
bool threadpool<T>::append(T* request)
{
    m_queuelocker.lock();
    if(m_workqueue.size()>=m_max_request)
    {
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template<typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool=(threadpool *)arg;
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>::run()
{
    while(!m_stop)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if(m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }

        T* request=m_workqueue.front();
        m_workqueue.pop();
        m_queuelocker.unlock();

        if(!request)
        {
            continue;
        }
        connectionRAII mysqlcon(&request->mysql, m_connPool);
        request->process();
    }
}


#endif