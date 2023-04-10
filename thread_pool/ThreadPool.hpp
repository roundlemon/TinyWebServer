#ifndef _THREAD_POOL_HPP_
#define _THREAD_POOL_HPP_
#include "TaskQueue.hpp"
#include "TaskQueue.cpp"

template <class T>
class ThreadPool
{
public:
    ThreadPool(int minn, int maxx);
    ~ThreadPool();
    void addtask(Task<T> t);
    int getbusynum();
    int getalivenum();

private:
    static void *manager(void *arg);
    static void *worker(void *arg);
    void threadexit();

private:
    TaskQueue<T> *m_taskQ;
    int minnum;
    int maxnum;
    int busynum;
    int exitnum;
    int alivenum;
    bool shutdown = false;
    pthread_cond_t notEmpty;
    pthread_mutex_t poolmutex;
    pthread_t managerID;
    pthread_t *workerIDs;
};

#endif