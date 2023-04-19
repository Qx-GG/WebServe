#ifndef LOCKER_H
#define LOCKER_H
#include <pthread.h>
#include <exception>
#include <semaphore.h>
//线程同步机制封装类

//互斥锁类
class locker
{
public:
    locker()
    {
        //初始化互斥锁
        if(pthread_mutex_init(&m_mutex,NULL)!=0)
        {
            //异常
            throw std::exception();
        }
    }

    ~locker()
    {
        //销毁互斥锁
        pthread_mutex_destroy(&m_mutex);
    }

    //上锁
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex)==0;
    }

    //解锁
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex)==0;
    }

    pthread_mutex_t *get()
    {
        return &m_mutex;
    }
private:
    pthread_mutex_t m_mutex;
};


//条件变量
class cond
{
public:
    cond()
    {
        //初始化条件变量
        if(pthread_cond_init(&m_cond,NULL)!=0)
        {
            throw std::exception();
        }
    }
    ~cond()
    {
        //销毁条件变量
        pthread_cond_destroy(&m_cond);
    }

    //阻塞函数
    bool wait(pthread_mutex_t *mutex)
    {
        return pthread_cond_wait(&m_cond,mutex)==0;
    }

    //阻塞一段时间
    bool timedwait(pthread_mutex_t *mutex,struct timespec t)
    {
        return pthread_cond_timedwait(&m_cond,mutex,&t)==0;
    }

    //唤醒一个或者多个等待的线程
    bool signal(pthread_mutex_t *mutex)
    {
        return pthread_cond_signal(&m_cond)==0;
    }

    //唤醒全部的线程
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond)==0;
    }
private:
    pthread_cond_t m_cond;
};

//信号量类
class sem
{
public:
    //默认构造函数
    sem()
    {
        if(sem_init(&m_sem,0,0)!=0)
        {
            throw std::exception();
        }
    }

    //构造函数
    sem(int num)
    {
        if(sem_init(&m_sem,0,num)!=0)
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        //销毁信号量
        sem_destroy(&m_sem);
    }

    //等待信号量
    bool wait()
    {
        return sem_wait(&m_sem)==0;
    }

    //增加信号量
    bool post()
    {
        return sem_post(&m_sem)==0;
    }

private:
    sem_t m_sem;
};





#endif