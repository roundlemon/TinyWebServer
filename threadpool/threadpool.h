#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include <unistd.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000, int max_thread_num = 20);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

    static void *manager(void *arg);

private:
    int m_thread_number;  // 线程池中的线程数
    int m_max_requests;   // 请求队列中允许的最大请求数
    pthread_t *m_threads; // 描述线程池的数组，其大小为m_thread_number

    pthread_t *m_manager;    // 管理者线程
    int m_max_thread_number; // 最大线程数
    int m_busynum;
    int m_alivenum; // 现存线程数
    int m_exitnum;

    std::list<T *> m_workqueue;  // 请求队列
    locker m_queuelocker;        // 保护请求队列的互斥锁
    sem m_queuestat;             // 是否有任务需要处理
    connection_pool *m_connPool; // 数据库
    int m_actor_model;           // 模型切换
};

template <class T>
void *threadpool<T>::manager(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    const int NUMBER = 2;
    while (true)
    {
        sleep(3);
        pool->m_queuelocker.lock();
        int busyn = pool->m_busynum;
        int quesz = pool->m_workqueue.size();
        int aliven = pool->m_alivenum;
        pool->m_queuelocker.unlock();
        int counter = 0;
        if (aliven - busyn < quesz && aliven < pool->m_max_thread_number)
        {
            cout << "add thread!" << endl;
            pool->m_queuelocker.lock();
            int counter = 0;
            for (int i = 0; i < pool->m_max_thread_number && pool->m_alivenum < pool->m_max_thread_number && counter < NUMBER; i++)
            {
                if (pool->m_threads[i] == 0)
                {
                    pthread_create(&pool->m_threads[i], nullptr, worker, pool);
                    counter++;
                    pool->m_alivenum++;
                    cout << "添加线程：" << to_string(pthread_self()) << " 现在存活线程数：" << pool->m_alivenum << endl;
                }
            }
            pool->m_queuelocker.unlock();
        }
        // 销毁
        if (busyn * 2 < aliven && aliven > pool->m_thread_number)
        {
            cout << "destory thread!" << endl;
            pool->m_queuelocker.lock();

            pool->m_exitnum = NUMBER;
            pool->m_queuelocker.unlock();


            for (int i = 0; i < NUMBER; i++)
            {
                pool->m_queuestat.post();
            }
        }
    }
}

template <typename T>
threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number, int max_request, int max_thread_num)
{
    m_actor_model = actor_model;
    m_connPool = connPool;
    m_thread_number = thread_number;
    m_busynum = 0;
    m_alivenum = m_thread_number;
    m_max_requests = max_request;
    m_max_thread_number = max_thread_num;
    m_exitnum = 0;

    m_threads = new pthread_t[m_max_thread_number];
    memset(m_threads, 0, sizeof(m_threads));
    for (int i = 0; i < m_thread_number; i++)
    {
        pthread_create(&m_threads[i], nullptr, worker, this);
        pthread_detach(m_threads[i]);
    }
    pthread_create(m_manager, nullptr, manager, this);
    pthread_detach(*m_manager);
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}
template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}
template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if(m_exitnum>0)
        {
            m_exitnum--;
            int pid = pthread_self();
            for(int i = 0;i<m_max_thread_number;i++)
            {
                if(m_threads[i]==pid)
                {
                    m_threads[i] = 0;
                    break;
                }
            }
            m_alivenum--;
            m_queuelocker.unlock();
            pthread_exit(nullptr);
        }
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;

        // 改变busynum数量
        m_queuelocker.lock();
        m_busynum++;
        m_queuelocker.unlock();

        if (1 == m_actor_model)
        {
            if (0 == request->m_state)
            {
                if (request->read_once())
                {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            else
            {
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
        // 改变busynum数量
        m_queuelocker.lock();
        m_busynum--;
        m_queuelocker.unlock();
    }
}

#endif