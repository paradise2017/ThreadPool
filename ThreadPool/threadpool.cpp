#include "threadpool.h"
#include <functional>
#include <thread>
#include <iostream>

const int TASK_MAX_THRESHHOLD = INT32_MAX;			// 任务上限阈值
const int THREAD_MAX_THRESHHOLD = 1024;				// 线程的最大数量	
const int THREAD_MAX_IDLE_TIME = 60; // 单位：秒		// 线程等待时间

// 线程池构造
ThreadPool::ThreadPool()
	: initThreadSize_(0)							// 初始线程池大小
	, taskSize_(0)									// 任务队列数量
	, idleThreadSize_(0)							// 空闲线程数量
	, curThreadSize_(0)								// 当前线程数量
	, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)	// 任务上限阈值
	, threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)  // 线程数量上限阈值
	, poolMode_(PoolMode::MODE_FIXED)				// 默认固定模式
	, isPoolRunning_(false)							// 线程暂停标志
{}

// 线程池析构
ThreadPool::~ThreadPool()
{
	isPoolRunning_ = false;
	
	// 等待线程池里面所有的线程返回  有两种状态：阻塞 & 正在执行任务中 
	std::unique_lock<std::mutex> lock(taskQueMtx_);
	notEmpty_.notify_all();	// 可能没有通知完全，有可能线程执行完了，又去wait了 // 如果线程任务先获取锁 wait了，这里通知
	exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; }); // 或者是curThreadSize_为空
}

// 设置线程池的工作模式
// 线程启动之后不能设置，在启动前设置。
void ThreadPool::setMode(PoolMode mode)
{
	if (checkRunningState())
		return;
	poolMode_ = mode;
}

// 设置task任务队列上线阈值
// 线程启动之后不能设置，在启动前设置。
void ThreadPool::setTaskQueMaxThreshHold(int threshhold)
{
	if (checkRunningState())
		return;
	taskQueMaxThreshHold_ = threshhold;
}

// 设置线程池cached模式下线程阈值
// 线程启动之后不能设置，在启动前设置。FIX模式没有， Catch模式支持
void ThreadPool::setThreadSizeThreshHold(int threshhold)
{
	if (checkRunningState())
		return;
	if (poolMode_ == PoolMode::MODE_CACHED)
	{
		threadSizeThreshHold_ = threshhold;
	}
}

// 给线程池提交任务    用户调用该接口，传入任务对象，生产任务
// Result 生命周期 大于 Task
Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
	// 获取任务队列锁
	std::unique_lock<std::mutex> lock(taskQueMtx_);

	// 线程的通信  等待任务队列有空余   wait   wait_for （等待最多1s）  wait_until  （设置一个时间，等待到了直接返回）
	// 用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回
	if (!notFull_.wait_for(lock, std::chrono::seconds(1),
		[&]()->bool { return taskQue_.size() < (size_t)taskQueMaxThreshHold_; }))
	{
		// 表示notFull_等待1s种，条件依然没有满足
		std::cerr << "task queue is full, submit task fail." << std::endl;
		// return task->getResult();  // Task  Result   线程执行完task，task对象就被析构掉了
		
		return Result(sp, false);
	}

	// 如果有空余，把任务放入任务队列中
	taskQue_.emplace(sp);
	taskSize_++;

	// 因为新放了任务，任务队列肯定不空了，在notEmpty_上进行通知，赶快分配线程执行任务
	notEmpty_.notify_all();

	// cached模式 任务处理比较紧急 场景：小而快的任务 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来
	if (poolMode_ == PoolMode::MODE_CACHED			// cached模型
		&& taskSize_ > idleThreadSize_				// 任务数量大于空闲线程
		&& curThreadSize_ < threadSizeThreshHold_)	// 当前线程数量小于阈值
	{
		std::cout << ">>> create new thread..." << std::endl;

		// 创建新的线程对象， 创建线程就有了自定义的线程id
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
		int threadId = ptr->getId();
		threads_.emplace(threadId, std::move(ptr));		// 线程池增加线程
		// 启动线程
		threads_[threadId]->start();					// 当前线程启动
		// 修改线程个数相关的变量
		curThreadSize_++;								// 当前线程数量++
		idleThreadSize_++;								// 空闲线程数量++
	}

	// 返回任务的Result对象
	return Result(sp);
	// return task->getResult();  // 任务可能执行完了，  用户才去调用，task声明周期结束
}

// 开启线程池
void ThreadPool::start(int initThreadSize)
{
	// 设置线程池的运行状态
	isPoolRunning_ = true;

	// 记录初始线程个数
	initThreadSize_ = initThreadSize;	// 初始线程数量
	curThreadSize_ = initThreadSize;	// 当前线程数量

	// 创建线程对象
	for (int i = 0; i < initThreadSize_; i++)
	{
		// 创建thread线程对象的时候，把线程函数给到thread线程对象
		// threadFunc ThreadPool 的成员方法，（无限循环，执行任务）
		// 创建线程的时候 Thread 自动分配了ID了 
		// 调用 ptr时需要传参数
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
		int threadId = ptr->getId();
		// 使用move 是因为ptr 是unique 指针
		threads_.emplace(threadId, std::move(ptr));
	}

	// 启动所有线程 
	for (int i = 0; i < initThreadSize_; i++)
	{
		threads_[i]->start(); // 需要去执行一个线程函数
		idleThreadSize_++;    // 记录初始空闲线程的数量
	}
}

// 定义线程函数   线程池的所有线程从任务队列里面消费任务
void ThreadPool::threadFunc(int threadid)  // 线程函数返回，相应的线程也就结束了
{
	// 记录第一次执行函数时间。
	// 如果两次执行之间相差60秒，当前线程析构
	auto lastTime = std::chrono::high_resolution_clock().now();

	// 所有任务必须执行完成，线程池才可以回收所有线程资源
	while (true)
	{
		std::shared_ptr<Task> task;	//需要处理的任务
		{
			// 多个线程访问共享数据，必须获取锁
			std::unique_lock<std::mutex> lock(taskQueMtx_);

			std::cout << "tid:" << std::this_thread::get_id()
				<< "尝试获取任务..." << std::endl;

			// 锁 + 双重判断
			// 当任务队列为空时，等待。，不为空时，执行任务。
			// 如果任务队列没任务，cached 超过60s，线程删除
			// fix模式 wait
			while (taskQue_.size() == 0)
			{
				// 线程池要结束，回收线程资源
				// 析构时，catch模式未超过 60s  和 fix模式 都从这里退出
				if (!isPoolRunning_)
				{
					std::cout << "threadid:" << std::this_thread::get_id() << " exit!"
						<< std::endl;
					threads_.erase(threadid); // std::this_thread::getid()
					exitCond_.notify_all();
					return; // 线程函数结束，线程结束
				}

				// 每一秒钟返回一次   怎么区分：超时返回？还是有任务待执行返回
				// cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s，应该把多余的线程
				// 结束回收掉（超过initThreadSize_数量的线程要进行回收）
				// 当前时间 - 上一次线程执行的时间 > 60s
				if (poolMode_ == PoolMode::MODE_CACHED)
				{
					// 条件变量，超时返回了，判断是否超过60s
					if (std::cv_status::timeout ==
						notEmpty_.wait_for(lock, std::chrono::seconds(1)))
					{
						auto now = std::chrono::high_resolution_clock().now();
						auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
						// 时间超过60s，&& 当前线程数量超过初始的线程数量（额外增加的线程删除）
						if (dur.count() >= THREAD_MAX_IDLE_TIME
							&& curThreadSize_ > initThreadSize_)
						{
							// 开始回收当前线程
							// 记录线程数量的相关变量的值修改
							// 把线程对象从线程列表容器中删除   没有办法 threadFunc《=》thread对象
							// threadid => thread对象 => 删除
							std::cout << "threadid:" << std::this_thread::get_id() << " exit!"
								<< std::endl;
							threads_.erase(threadid); // std::this_thread::getid()
							curThreadSize_--;
							idleThreadSize_--;
							return;
						}
					}
				}
				else
				{
					// 固定模式
					notEmpty_.wait(lock);
				}
			}

			// 获取任务成功的条件， 任务队列不为空


			std::cout << "tid:" << std::this_thread::get_id()
				<< "获取任务成功..." << std::endl;

			idleThreadSize_--;
			// 从任务队列种取一个任务出来
			task = taskQue_.front();
			taskQue_.pop();
			taskSize_--;

			// 取出一个任务，进行通知，通知可以继续提交生产任务
			notFull_.notify_all();
		} // 拿到任务，就应该把锁释放掉，执行任务不需要锁。
		
		// 当前线程负责执行这个任务
		if (task != nullptr)
		{
			// task->run(); // 执行任务；把任务的返回值setVal方法给到Result
			task->exec();
		}
		
		idleThreadSize_++;									   // 任务执行完了，空闲的线程数+1
		lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间,用于空闲线程判断
	}
}

bool ThreadPool::checkRunningState() const
{
	return isPoolRunning_;
}

////////////////  线程方法实现
int Thread::generateId_ = 0;

// 线程构造
Thread::Thread(ThreadFunc func)
	: func_(func)
	, threadId_(generateId_++)
{}

// 线程析构
Thread::~Thread() {}

// 启动线程
// 分离线程
void Thread::start()
{
	// 创建一个线程来执行一个线程函数 pthread_create
	std::thread t(func_, threadId_);  // C++11来说 线程对象t  和线程函数func_
	//func_ = auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
	t.detach(); // 设置分离线程   pthread_detach  pthread_t设置成分离线程
}

int Thread::getId()const
{
	return threadId_;
}


/////////////////  Task方法实现
Task::Task()
	: result_(nullptr)
{}

void Task::exec()
{
	if (result_ != nullptr)
	{
		result_->setVal(run()); // 这里发生多态调用
	}
}

void Task::setResult(Result* res)
{
	result_ = res;
}

/////////////////   Result方法的实现
Result::Result(std::shared_ptr<Task> task, bool isValid)
	: isValid_(isValid)
	, task_(task)
{
	task_->setResult(this);		// 延长生命周期		// task 和 result 产生关系。 一个任务带一个result   task 和 result相互依赖
								// 这里用户也可以调用，采用成员方法，便于更好的封装
}

Any Result::get() // 用户调用的
{
	if (!isValid_)
	{
		return "";
	}
	sem_.wait(); // task任务如果没有执行完，这里会阻塞用户的线程
	return std::move(any_);
}

// 消费者任务执行Reuslt的任务执行完了，  获取结果，通知 Result 结果类取消阻塞。
void Result::setVal(Any any)  // 谁调用的呢？？？
{
	// 存储task的返回值
	this->any_ = std::move(any);
	sem_.post(); // 已经获取的任务的返回值，增加信号量资源  ，用户获取值就不需要等待了
}