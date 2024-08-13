#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>

// Any类型：可以接收任意数据的类型
// 为什么不设计成员变量还有模版的参数呢？  因为模版参数， 必须加上template<typename T> ，使用时必须加<> ，
// 该方法 可以不加<> 直接使用 类方法，获取任意类型的变量值
// 相当于对模版类进行封装，基类指向模版类，获取结果
class Any
{
public:
	Any() = default;
	~Any() = default;
	Any(const Any&) = delete;
	Any& operator=(const Any&) = delete;
	Any(Any&&) = default;
	Any& operator=(Any&&) = default;

	// 这个构造函数可以让Any类型接收任意其它的数据
	template<typename T>  // T:int    Derive<int>
	// 
	Any(T data) : base_(std::make_unique<Derive<T>>(data))
	{}

	// 这个方法能把Any对象里面存储的data数据提取出来
	template<typename T>
	T cast_()
	{
		// 我们怎么从base_找到它所指向的Derive对象，从它里面取出data成员变量
		// 基类指针 =》 派生类指针   RTTI
		Derive<T>* pd = dynamic_cast<Derive<T>*>(base_.get());
		if (pd == nullptr)
		{
			throw "type is unmatch!";
		}
		return pd->data_;
	}
private:
	// 基类类型 虚函数
	class Base
	{
	public:
		virtual ~Base() = default;
	};

	// 派生类类型 模版
	template<typename T>
	class Derive : public Base
	{
	public:
		Derive(T data) : data_(data) 
		{}
		T data_;  // 保存了任意的其它类型
	};

private:
	// 定义一个基类的指针
	std::unique_ptr<Base> base_;
};

// 实现一个信号量类
// 加强版的互斥锁  用一个变量控制 resLimit_ 当他的取值为0,1时，就是互斥锁了
// 用于结果类，当任务还没执行完，结果类获取结果，阻塞
class Semaphore
{
public:
	Semaphore(int limit = 0) 
		:resLimit_(limit)
	{}
	~Semaphore() = default;

	// 获取一个信号量资源，如果有资源，不等待，
	void wait()
	{
		std::unique_lock<std::mutex> lock(mtx_);
		// 等待信号量有资源，没有资源的话，会阻塞当前线程
		cond_.wait(lock, [&]()->bool {return resLimit_ > 0; });
		resLimit_--;
	}

	// 增加一个信号量资源，通知等待的信号量，干活
	void post()
	{
		std::unique_lock<std::mutex> lock(mtx_);
		resLimit_++;
		// linux下condition_variable的析构函数什么也没做
		// 导致这里状态已经失效，无故阻塞
		cond_.notify_all();  // 等待状态，释放mutex锁 通知条件变量wait的地方，可以起来干活了
	}
private:
	int resLimit_;	// 结果资源
	std::mutex mtx_;				// resLimit_的互斥锁    其实Result 就一个，不可能访问冲突，主要就是利用 条件变量的wait功能
	std::condition_variable cond_;	// resLimit_的条件变量
};

// Task类型的前置声明
class Task;

// 实现接收提交到线程池的task任务执行完成后的返回值类型Result
class Result
{
public:
	Result(std::shared_ptr<Task> task, bool isValid = true);
	~Result() = default;

	// 问题一：setVal方法，获取任务执行完的返回值的 ，消费者线程执行完了，调用
	void setVal(Any any);

	// 问题二：get方法，用户调用这个方法获取task的返回值 ， 如果任务还没执行完，阻塞
	Any get();
private:
	Any any_;						// 存储任务的返回值
	Semaphore sem_;					// 线程通信信号量
	std::shared_ptr<Task> task_;    // 延长生命周期 //指向对应获取返回值的任务对象	// 为什么Result还需要拿到任务类？ 构造时，给每一个task类 加上result类，task就不需要设置  result了
	std::atomic_bool isValid_;	    // 返回值是否有效
};

// 任务抽象基类
// 用户继承该类，实现任务函数编写
class Task
{
public:
	Task();
	~Task() = default;
	void exec();				// 对不能定义的虚函数进行封装
	void setResult(Result* res);

	// 用户可以自定义任意任务类型，从Task继承，重写run方法，实现自定义任务处理
	virtual Any run() = 0;

private:
	Result* result_; // Result对象的声明周期 》
};

// 线程池支持的模式
enum class PoolMode
{
	MODE_FIXED,  // 固定数量的线程
	MODE_CACHED, // 线程数量可动态增长
};

// 线程类型
// 对C++的线程方法进行封装，传入1：执行函数，2：线程id
class Thread
{
public:
	// 线程函数对象类型
	using ThreadFunc = std::function<void(int)>;	// 线程池执行任务的类型

	// 线程构造
	Thread(ThreadFunc func);						// 相当于回调，调用ThreadPool的 private: 	void threadFunc(int threadid); 方法

	// 线程析构
	~Thread();
	// 启动线程
	void start();

	// 获取线程id
	int getId()const;
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_;  // 保存线程id
};

/*
example:
ThreadPool pool;
pool.start(4);

class MyTask : public Task
{
	public:
		void run() { // 线程代码... }
};

pool.submitTask(std::make_shared<MyTask>());
*/



class ThreadPool
{
public:
	// 线程池构造
	ThreadPool();

	// 线程池析构
	~ThreadPool();

	// 设置线程池的工作模式
	void setMode(PoolMode mode);

	// 设置task任务队列上线阈值
	void setTaskQueMaxThreshHold(int threshhold);

	// 设置线程池cached模式下线程阈值
	void setThreadSizeThreshHold(int threshhold);

	// 给线程池提交任务
	Result submitTask(std::shared_ptr<Task> sp);

	// 开启线程池
	void start(int initThreadSize = std::thread::hardware_concurrency());  //CPU当前的核心数量

	ThreadPool(const ThreadPool&) = delete;				// 线程池拷贝删除
	ThreadPool& operator=(const ThreadPool&) = delete;	// 线程池赋值删除

private:
	// 定义线程函数
	void threadFunc(int threadid);

	// 检查pool的运行状态
	bool checkRunningState() const;

private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;		// 线程列表(id,线程)

	int initThreadSize_;											// 初始的线程数量
	int threadSizeThreshHold_;										// 线程数量上限阈值，无限创建线程造成资源浪费
	std::atomic_int curThreadSize_;									// 记录当前线程池里面线程的总数量  threads_.size()
	std::atomic_int idleThreadSize_;								// 记录空闲线程的数量 用于 catch模式

	std::queue<std::shared_ptr<Task>> taskQue_;						// 任务队列  存储所有待处理的任务
	std::atomic_int taskSize_;										// 任务的数量	taskQue_.size()
	int taskQueMaxThreshHold_;									    // 任务队列数量上限阈值

	std::mutex taskQueMtx_;											// 保证任务队列的线程安全
	std::condition_variable notFull_;								// 表示任务队列不满		用于任务队列加任务	任务队列等待，  消费线程通知   这个控制添加任务的数量，一般的线程池不设置该值。
	std::condition_variable notEmpty_;								// 表示任务队列不空		消费线程取任务		消费线程通知，  任务队列等待	用于通知消费线程的，必须设置
	std::condition_variable exitCond_;								// 等到线程资源全部回收	线程池退出时使用

	PoolMode poolMode_;												// 当前线程池的工作模式
	std::atomic_bool isPoolRunning_;								// 表示当前线程池的启动状态	线程池退出时使用
};

#endif
