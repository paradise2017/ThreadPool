﻿#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

const int TASK_MAX_THRESHHOLD = INT32_MAX;  // INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;		// 单位：秒

/**
	@item threadpool
	@brief C++新特性的线程池
**/

// 线程池支持的模式
enum class PoolMode
{
	MODE_FIXED,  // 固定数量的线程
	MODE_CACHED, // 线程数量可动态增长
};

// 线程类型
class Thread
{
public:
	// 线程函数对象类型，线程执行的任务函数（包含该线程id）
	using ThreadFunc = std::function<void(int)>;

	// 线程构造
	Thread(ThreadFunc func)
		: func_(func)
		, threadId_(generateId_++)
	{}
	// 线程析构
	~Thread() = default;

	// 启动线程
	void start()
	{
		// 创建一个线程来执行一个线程函数 pthread_create
		std::thread t(func_, threadId_);  // C++11来说 线程对象t  和线程函数func_
		t.detach(); // 设置分离线程   pthread_detach  pthread_t设置成分离线程
	}

	// 获取线程id
	int getId()const
	{
		return threadId_;
	}
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_;  // 保存线程id
};

int Thread::generateId_ = 0;	// 静态成员变量，全类共享，不占用对象内存，且只在程序的全局数据区分配一次内存空间

// 线程池类型
class ThreadPool
{
public:
	// 线程池构造
	ThreadPool()
		: initThreadSize_(0)
		, taskSize_(0)
		, idleThreadSize_(0)
		, curThreadSize_(0)
		, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
		, threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
		, poolMode_(PoolMode::MODE_FIXED)
		, isPoolRunning_(false)
	{}

	// 线程池析构
	~ThreadPool()
	{
		isPoolRunning_ = false;

		// 等待线程池里面所有的线程返回  有两种状态：阻塞 & 正在执行任务中
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
	}

	// 设置线程池的工作模式
	void setMode(PoolMode mode)
	{
		if (checkRunningState())
			return;
		poolMode_ = mode;
	}

	// 设置task任务队列上线阈值
	void setTaskQueMaxThreshHold(int threshhold)
	{
		if (checkRunningState())
			return;
		taskQueMaxThreshHold_ = threshhold;
	}

	// 设置线程池cached模式下线程阈值
	void setThreadSizeThreshHold(int threshhold)
	{
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
		{
			threadSizeThreshHold_ = threshhold;
		}
	}

	// 给线程池提交任务
	// 使用可变参模板编程，让submitTask可以接收任意任务函数和任意数量的参数
	// pool.submitTask(sum1, 10, 20); 
	// 返回值future<>  Func 函数对象指针  Args 参数列表
	template<typename Func, typename... Args>		// -> 指定返回值类型，给auto提示
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>	// 推导函数调用之后的返回值（函数，参数）
	{
		// 打包任务，放入任务队列里面 RType是类型   函数，参数， 返回类型 = RType
		using RType = decltype(func(args...));
		//task	指针，指向packaged_task<RType()包		 // void()
		// auto = std::shared_ptr<std::packaged_task<RType()>>    packaged_task 需要函数返回值，参数列表，bind 绑定只需包含返回类型就好了
		auto task = std::make_shared<std::packaged_task<RType()>>(
		std::bind(std::forward<Func>(func), std::forward<Args>(args)...));		// 把任意参数的绑定到无参函数对象

		std::future<RType> result = task->get_future();	// 提前绑定结果，  该任务在任务队列 用std::function<void()> 执行
		// 获取锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		// 用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool { return taskQue_.size() < (size_t)taskQueMaxThreshHold_; }))
		{
			// 表示notFull_等待1s种，条件依然没有满足
			std::cerr << "task queue is full, submit task fail." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType { return RType(); });
			// packaged_task是类，使用要解引用
			(*task)(); 
			return task->get_future();
		}

		// 如果有空余，把任务放入任务队列中
		// using Task = std::function<void()>;	//function执行pack包的任务
		// task的智能指针计数+1
		// 前面的 task 可能是 int() test(), 或者 double()  test() 任务。
		// function 对task进行封装， 全部封装成void()
		taskQue_.emplace([task]() {(*task)();}); //task是智能指针 延长生命周期  函数对象延长了生命周期 //不能按引用，引用出作用域就没了
		taskSize_++;

		// 因为新放了任务，任务队列肯定不空了，在notEmpty_上进行通知，赶快分配线程执行任务
		notEmpty_.notify_all();

		// cached模式 任务处理比较紧急 场景：小而快的任务 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ < threadSizeThreshHold_)
		{
			std::cout << ">>> create new thread..." << std::endl;

			// 创建新的线程对象
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			// 启动线程
			threads_[threadId]->start();
			// 修改线程个数相关的变量
			curThreadSize_++;
			idleThreadSize_++;
		}

		// 返回任务的Result对象
		return result;
	}

	// 开启线程池
	void start(int initThreadSize = std::thread::hardware_concurrency())
	{
		// 设置线程池的运行状态
		isPoolRunning_ = true;

		// 记录初始线程个数
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		// 创建线程对象
		for (int i = 0; i < initThreadSize_; i++)
		{
			// 创建thread线程对象的时候，把线程函数给到thread线程对象
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			// threads_.emplace_back(std::move(ptr));
		}

		// 启动所有线程  std::vector<Thread*> threads_;
		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start(); // 需要去执行一个线程函数
			idleThreadSize_++;    // 记录初始空闲线程的数量
		}
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	// 定义线程函数
	void threadFunc(int threadid) // 线程池执行该任务，传入该线程的id
	{
		auto lastTime = std::chrono::high_resolution_clock().now();

		// 所有任务必须执行完成，线程池才可以回收所有线程资源
		while(true)
		{
			Task task;  // std::function<void()> 函数对象
			{
				// 先获取锁
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid:" << std::this_thread::get_id()
					<< "尝试获取任务..." << std::endl;

				// cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s，应该把多余的线程
				// 结束回收掉（超过initThreadSize_数量的线程要进行回收）
				// 当前时间 - 上一次线程执行的时间 > 60s

				// 每一秒中返回一次   怎么区分：超时返回？还是有任务待执行返回
				// 锁 + 双重判断
				while (taskQue_.size() == 0)
				{
					// 线程池要结束，回收线程资源
					if (!isPoolRunning_)
					{
						threads_.erase(threadid); // std::this_thread::getid()
						std::cout << "threadid:" << std::this_thread::get_id() << " exit!"
							<< std::endl;
						exitCond_.notify_all();
						return; // 线程函数结束，线程结束
					}

					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						// 条件变量，超时返回了
						if (std::cv_status::timeout ==
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME
								&& curThreadSize_ > initThreadSize_)
							{
								// 开始回收当前线程
								// 记录线程数量的相关变量的值修改
								// 把线程对象从线程列表容器中删除   没有办法 threadFunc《=》thread对象
								// threadid => thread对象 => 删除
								threads_.erase(threadid); // std::this_thread::getid()
								curThreadSize_--;
								idleThreadSize_--;

								std::cout << "threadid:" << std::this_thread::get_id() << " exit!"
									<< std::endl;
								return;
							}
						}
					}
					else
					{
						// 等待notEmpty条件
						notEmpty_.wait(lock);
					}
				}

				idleThreadSize_--;

				std::cout << "tid:" << std::this_thread::get_id()
					<< "获取任务成功..." << std::endl;

				// 从任务队列中取一个任务出来
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				// 如果依然有剩余任务，继续通知其它得线程执行任务
				if (taskQue_.size() > 0)
				{
					notEmpty_.notify_all();
				}

				// 取出一个任务，进行通知，通知可以继续提交生产任务
				notFull_.notify_all();
			} // 就应该把锁释放掉

			// 当前线程负责执行这个任务 task函数对象，  函数指针， 指向nullptr
			if (task != nullptr)
			{
				task(); // 执行function<void()> 
			}

			idleThreadSize_++;
			lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间
		}
	}

	// 检查pool的运行状态
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}

private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_; // 线程列表

	int initThreadSize_;  // 初始的线程数量
	int threadSizeThreshHold_; // 线程数量上限阈值
	std::atomic_int curThreadSize_;	// 记录当前线程池里面线程的总数量
	std::atomic_int idleThreadSize_; // 记录空闲线程的数量

	// Task任务 =》 函数对象  中间层，不能确定任务的返回值，用；lambda执行
	using Task = std::function<void()>;
	std::queue<Task> taskQue_; // 任务队列  线程池安装，不会释放掉
	std::atomic_int taskSize_; // 任务的数量
	int taskQueMaxThreshHold_;  // 任务队列数量上限阈值

	std::mutex taskQueMtx_; // 保证任务队列的线程安全
	std::condition_variable notFull_; // 表示任务队列不满
	std::condition_variable notEmpty_; // 表示任务队列不空
	std::condition_variable exitCond_; // 等到线程资源全部回收

	PoolMode poolMode_; // 当前线程池的工作模式
	std::atomic_bool isPoolRunning_; // 表示当前线程池的启动状态
};

#endif
