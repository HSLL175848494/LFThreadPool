// Note: The current file contains an embedded third-party file: LightweightSemaphore.hpp
// For comments within the region marked between embedding start and embedding end, please refer to the relevant copyright statement

#ifndef HSLL_THREADPOOL
#define HSLL_THREADPOOL

#include <vector>
#include <atomic>
#include <thread>
#include <assert.h>
#include <condition_variable>

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

#define HSLL_ALLOW_THROW

#if defined(_WIN32)
#include <malloc.h>
#define ALIGNED_MALLOC(size, align) _aligned_malloc(size, align)
#define ALIGNED_FREE(ptr) _aligned_free(ptr)
#else
#include <stdlib.h>
#if !defined(_ISOC11_SOURCE) && !defined(__APPLE__)
#define ALIGNED_MALLOC(size, align) ({ \
    void* ptr = NULL; \
    if (posix_memalign(&ptr, align, size) != 0) ptr = NULL; \
    ptr; \
})
#else
#define ALIGNED_MALLOC(size, align) aligned_alloc(align, (size + align - 1) & ~(size_t)(align - 1))
#endif
#define ALIGNED_FREE(ptr) free(ptr)
#endif

namespace HSLL
{
	template <typename T>
	struct is_move_constructible
	{
	private:
		template <typename U, typename = decltype(U(std::declval<U&&>()))>
		static constexpr std::true_type test_move(int);

		template <typename>
		static constexpr std::false_type test_move(...);

	public:
		static constexpr bool value = decltype(test_move<T>(true))::value;
	};

	template <typename T>
	struct is_copy_constructible
	{
	private:
		template <typename U, typename = decltype(U{ std::declval<U&>() }) >
		static constexpr std::true_type test_copy(bool);

		template <typename>
		static constexpr std::false_type test_copy(...);

	public:
		static constexpr bool value = decltype(test_copy<T>(true))::value;
	};

	template <unsigned int TSIZE, unsigned int ALIGN>
	class TaskStack;

	template <class F, class... Args>
	class HeapCallable;

	template <class F, class... Args>
	struct TaskImpl;

	template <typename T>
	struct is_generic_hc : std::false_type
	{
	};

	template <class T, class... Args>
	struct is_generic_hc<HeapCallable<T, Args...>> : std::true_type
	{
	};

	template <typename T>
	struct is_generic_ti : std::false_type
	{
	};

	template <class T, class... Args>
	struct is_generic_ti<TaskImpl<T, Args...>> : std::true_type
	{
	};

	template <typename T>
	struct is_generic_ts : std::false_type
	{
	};

	template <unsigned int S, unsigned int A>
	struct is_generic_ts<TaskStack<S, A>> : std::true_type
	{
	};

	template <size_t... Is>
	struct index_sequence
	{
	};

	template <size_t N, size_t... Is>
	struct make_index_sequence_impl : make_index_sequence_impl<N - 1, N - 1, Is...>
	{
	};

	template <size_t... Is>
	struct make_index_sequence_impl<0, Is...>
	{
		using type = index_sequence<Is...>;
	};

	template <size_t N>
	struct make_index_sequence
	{
		using type = typename make_index_sequence_impl<N>::type;
	};

	template <bool...>
	struct bool_pack;

	template <bool... Bs>
	using all_true = std::is_same<bool_pack<true, Bs...>, bool_pack<Bs..., true>>;

	template <typename... Ts>
	using are_all_copy_constructible = all_true<is_copy_constructible<Ts>::value...>;

	template <typename Callable, typename... Ts>
	void tinvoke(Callable& callable, Ts &...args)
	{
		callable(args...);
	}

	template <typename Tuple, size_t... Is>
	void apply_impl(Tuple& tup, index_sequence<Is...>)
	{
		tinvoke(std::get<Is>(tup)...);
	}

	template <typename Tuple>
	void tuple_apply(Tuple& tup)
	{
		apply_impl(tup, typename make_index_sequence<std::tuple_size<Tuple>::value>::type{});
	}

	/**
	 * @brief Heap-allocated type-erased callable wrapper with shared ownership
	 * @tparam F Type of callable object
	 * @tparam Args Types of bound arguments
	 * @details
	 * - Stores decayed copies of function and arguments in a shared tuple
	 * - Supports copy/move operations through shared reference counting
	 * - Invocation always passes arguments as lvalues (same as TaskStack)
	 * - Safe for cross-thread usage through shared_ptr semantics
	 */
	template <class F, class... Args>
	class HeapCallable
	{
	private:
		using Package = std::tuple<typename std::decay<F>::type, typename std::decay<Args>::type...>;
		std::shared_ptr<Package> storage;

	public:

		void operator()() noexcept
		{
			if (storage)
				tuple_apply(*storage);
		}

		/**
		 * @brief Constructs callable with function and arguments
		 * @tparam F Forwarding reference to callable type
		 * @tparam Args Forwarding references to argument types
		 * @param func Callable target function
		 * @param args Arguments to bind to function call
		 * @note Disables overload when F is HeapCallable type (prevents nesting)
		 * @note Arguments are stored as decayed types (copy/move constructed)
		 */
		template <typename std::enable_if<!is_generic_hc<typename std::decay<F>::type>::value, int>::type = 0>
		HeapCallable(F&& func, Args &&...args) HSLL_ALLOW_THROW
			: storage(std::make_shared<Package>(std::forward<F>(func), std::forward<Args>(args)...)) {}
	};

	/**
	 * @brief Factory function to create HeapCallable objects
	 * @tparam F Type of callable object
	 * @tparam Args Types of arguments to bind
	 * @param func Callable target function
	 * @param args Arguments to bind to function call
	 * @return HeapCallable instance managing shared ownership of the callable
	 */
	template <typename F,
		typename std::enable_if<!is_generic_hc<typename std::decay<F>::type>::value, int>::type = 0,
		typename... Args>
	HeapCallable<F, Args...> make_callable(F&& func, Args &&...args) HSLL_ALLOW_THROW
	{
		return HeapCallable<F, Args...>(std::forward<F>(func), std::forward<Args>(args)...);
	}

	/**
	 * @brief Base interface for type-erased task objects
	 * @details Provides virtual methods for task execution and storage management
	 */
	struct TaskBase
	{
		virtual ~TaskBase() = default;
		virtual void execute() noexcept = 0;
		virtual void copyTo(void* memory) const noexcept = 0;
		virtual void moveTo(void* memory) noexcept = 0;
		virtual bool is_copyable() const noexcept = 0;
	};

	/**
	 * @brief Concrete task implementation storing function and arguments
	 * @details Stores decayed copies of function and arguments in a tuple
	 */
	template <class F, class... Args>
	struct TaskImpl : TaskBase
	{
		template <typename T, bool Copyable>
		struct CopyHelper;

		template <typename T>
		struct CopyHelper<T, true>
		{
			static void copyTo(const T* self, void* dst) noexcept
			{
				new (dst) T(*self);
			}
		};

		template <typename T>
		struct CopyHelper<T, false>
		{
			static void copyTo(const T*, void*) noexcept
			{
				printf("\nTaskImpl must be copy constructible for cloneTo()");
				std::abort();
			}
		};

		template <typename T, bool Movable>
		struct MoveHelper;

		template <typename T>
		struct MoveHelper<T, true>
		{
			static T&& apply(T& obj) noexcept
			{
				return std::move(obj);
			}
		};

		template <typename T>
		struct MoveHelper<T, false>
		{
			static T& apply(T& obj) noexcept
			{
				return obj;
			}
		};

		using Tuple = std::tuple<typename std::decay<F>::type, typename std::decay<Args>::type...>;
		Tuple storage;

		void tuple_move(void* dst)
		{
			move_impl(dst, typename make_index_sequence<std::tuple_size<Tuple>::value>::type{});
		}

		template <size_t... Is>
		void move_impl(void* dst, index_sequence<Is...>)
		{
			tmove(dst, std::get<Is>(storage)...);
		}

		template <typename... Ts>
		void tmove(void* dst, Ts &...args)
		{
			new (dst) TaskImpl(MoveHelper<Ts, is_move_constructible<Ts>::value>::apply(args)...);
		}

		template <class Func, class... Params,
			typename std::enable_if<!is_generic_ti<typename std::decay<Func>::type>::value, int>::type = 0>
		TaskImpl(Func&& func, Params &&...args)
			: storage(std::forward<Func>(func), std::forward<Params>(args)...) {}

		void execute() noexcept override
		{
			tuple_apply(storage);
		}

		void copyTo(void* dst) const noexcept override
		{
			CopyHelper<TaskImpl,
				are_all_copy_constructible<typename std::decay<F>::type,
				typename std::decay<Args>::type...>::value>::copyTo(this, dst);
		}

		void moveTo(void* dst) noexcept override
		{
			tuple_move(dst);
		}

		bool is_copyable() const noexcept override
		{
			return are_all_copy_constructible<
				typename std::decay<F>::type,
				typename std::decay<Args>::type...>::value;
		}
	};

	/**
	 * @brief Metafunction to compute the task implementation type and its size
	 * @tparam F Type of callable object
	 * @tparam Args Types of bound arguments
	 * @details Provides:
	 *   - `type`: Concrete TaskImpl type for given function and arguments
	 *   - `size`: Size in bytes of the TaskImpl type
	 */
	template <class F, class... Args>
	struct task_stack
	{
		using type = TaskImpl<F, Args...>;
		static constexpr unsigned int size = sizeof(type);
	};

#if __cplusplus >= 201402L
	template <class F, class... Args>
	static constexpr unsigned int task_stack_size = sizeof(task_stack<F, Args...>::size);
#endif

	/**
	 * @brief Stack-allocated task container with fixed-size storage
	 * @tparam TSIZE Size of internal storage buffer (default = 64)
	 * @tparam ALIGN Alignment requirement for storage (default = 8)
	 */
	template <unsigned int TSIZE = 64, unsigned int ALIGN = 8>
	class TaskStack
	{
		static_assert(TSIZE >= 24, "TSIZE must >= 24");
		static_assert(ALIGN >= alignof(void*), "Alignment must >= alignof(void*)");
		static_assert(TSIZE% ALIGN == 0, "TSIZE must be a multiple of ALIGN");
		alignas(ALIGN) char storage[TSIZE];

		/**
		 * @brief Helper template to conditionally create stack-allocated or heap-backed TaskStack
		 */
		template <bool Condition, typename F, typename... Args>
		struct Maker;

		template <typename F, typename... Args>
		struct Maker<true, F, Args...>
		{
			static TaskStack make(F&& func, Args &&...args)
			{
				return TaskStack(std::forward<F>(func), std::forward<Args>(args)...);
			}
		};

		template <typename F, typename... Args>
		struct Maker<false, F, Args...>
		{
			static TaskStack make(F&& func, Args &&...args)
			{
				return TaskStack(HeapCallable<F, Args...>(std::forward<F>(func), std::forward<Args>(args)...));
			}
		};

	public:
		/**
		 * @brief Metafunction to validate task compatibility with storage
		 * @tparam F Type of callable object
		 * @tparam Args Types of bound arguments
		 * @value true if task fits in storage and meets alignment requirements
		 */
		template <class F, class... Args>
		struct task_invalid
		{
			static constexpr bool value = sizeof(typename task_stack<F, Args...>::type) <= sizeof(TaskStack) &&
				alignof(typename task_stack<F, Args...>::type) <= ALIGN;
		};

#if __cplusplus >= 201402L
		template <class F, class... Args>
		static constexpr bool task_invalid_v = sizeof(typename task_stack<F, Args...>::type) <= TSIZE &&
			alignof(typename task_stack<F, Args...>::type) <= ALIGN;
#endif

		/**
		 * @brief Constructs task in internal storage
		 * @tparam F Type of callable object
		 * @tparam Args Types of bound arguments
		 * @param func Callable target function
		 * @param args Arguments to bind to function call
		 * @note Disables overload when F is a TaskStack (prevents nesting)
		 * @note Static assertion ensures storage size is sufficient
		 *
		 * Important usage note:
		 * - Argument value category (lvalue/rvalue) affects ONLY how
		 *   arguments are stored internally (copy vs move construction)
		 * - During execute(), arguments are ALWAYS passed as lvalues
		 * - Functions with rvalue reference parameters are NOT supported
		 *   Example: void bad_func(std::string&&) // Not allowed
		 */
		template <class F, class... Args,
			typename std::enable_if<!is_generic_ts<typename std::decay<F>::type>::value, int>::type = 0>
		TaskStack(F&& func, Args &&...args) HSLL_ALLOW_THROW
		{
			typedef typename task_stack<F, Args...>::type ImplType;
			static_assert(sizeof(ImplType) <= TSIZE, "TaskImpl size exceeds storage");
			static_assert(alignof(ImplType) <= ALIGN, "TaskImpl alignment exceeds storage alignment");
			new (storage) ImplType(std::forward<F>(func), std::forward<Args>(args)...);
		}

		/**
		 * @brief Factory method that automatically selects storage strategy
		 * @tparam F Type of callable object
		 * @tparam Args Types of bound arguments
		 * @param func Callable to store
		 * @param args Arguments to bind
		 * @return TaskStack with either:
		 *         - Directly stored task if it fits in stack buffer
		 *         - Heap-allocated fallback via HeapCallable otherwise
		 * @note Uses SFINAE to prevent nesting of HeapCallable objects
		 */
		template <class F, class... Args,
			typename std::enable_if<!is_generic_hc<typename std::decay<F>::type>::value, int>::type = 0>
		static TaskStack make_auto(F&& func, Args &&...args) HSLL_ALLOW_THROW
		{
			return Maker<task_invalid<F, Args...>::value, F, Args...>::make(
				std::forward<F>(func),
				std::forward<Args>(args)...);
		}

		/**
		 * @brief Factory method that forces heap-backed storage
		 * @tparam F Type of callable object
		 * @tparam Args Types of bound arguments
		 * @param func Callable to store
		 * @param args Arguments to bind
		 * @return TaskStack using HeapCallable storage
		 * @note Always uses heap allocation regardless of size
		 */
		template <class F, class... Args,
			typename std::enable_if<!is_generic_hc<typename std::decay<F>::type>::value, int>::type = 0>
		static TaskStack make_heap(F&& func, Args &&...args) HSLL_ALLOW_THROW
		{
			return Maker<false, F, Args...>::make(
				std::forward<F>(func),
				std::forward<Args>(args)...);
		}

		void execute() noexcept
		{
			getBase()->execute();
		}

		bool is_copyable() const noexcept
		{
			return getBase()->is_copyable();
		}

		TaskStack(const TaskStack& other) noexcept
		{
			other.getBase()->copyTo(storage);
		}

		TaskStack(TaskStack&& other) noexcept
		{
			other.getBase()->moveTo(storage);
		}

		~TaskStack() noexcept
		{
			getBase()->~TaskBase();
		}

		TaskStack& operator=(const TaskStack& other) = delete;
		TaskStack& operator=(TaskStack&& other) = delete;

	private:
		TaskBase* getBase() noexcept
		{
			return (TaskBase*)storage;
		}

		const TaskBase* getBase() const noexcept
		{
			return (const TaskBase*)storage;
		}
	};
}

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>

namespace HSLL
{
	class ReadWriteLock {
	public:
		ReadWriteLock() {
			InitializeSRWLock(&srwlock_);
		}

		~ReadWriteLock() = default;

		void lock_read() {
			AcquireSRWLockShared(&srwlock_);
		}

		void unlock_read() {
			ReleaseSRWLockShared(&srwlock_);
		}

		void lock_write() {
			AcquireSRWLockExclusive(&srwlock_);
		}

		void unlock_write() {
			ReleaseSRWLockExclusive(&srwlock_);
		}

		ReadWriteLock(const ReadWriteLock&) = delete;
		ReadWriteLock& operator=(const ReadWriteLock&) = delete;

	private:
		SRWLOCK srwlock_;
	};
}

#elif defined(__linux__) || defined(__unix__) || \
      defined(__APPLE__) || defined(__FreeBSD__) || \
      defined(__OpenBSD__) || defined(__NetBSD__)

#include <semaphore.h>
#include <pthread.h>

namespace HSLL
{
	class ReadWriteLock {
	public:
		ReadWriteLock() {
			pthread_rwlock_init(&rwlock_, nullptr);
		}

		~ReadWriteLock() {
			pthread_rwlock_destroy(&rwlock_);
		}

		void lock_read() {
			pthread_rwlock_rdlock(&rwlock_);
		}

		void unlock_read() {
			pthread_rwlock_unlock(&rwlock_);
		}

		void lock_write() {
			pthread_rwlock_wrlock(&rwlock_);
		}

		void unlock_write() {
			pthread_rwlock_unlock(&rwlock_);
		}

		ReadWriteLock(const ReadWriteLock&) = delete;
		ReadWriteLock& operator=(const ReadWriteLock&) = delete;

	private:
		pthread_rwlock_t rwlock_;
	};
}


#else
#error "Unsupported platform"
#endif

namespace HSLL
{
	class ReadLockGuard {
	public:
		explicit ReadLockGuard(ReadWriteLock& lock) : lock_(lock) {
			lock_.lock_read();
		}

		~ReadLockGuard() {
			lock_.unlock_read();
		}

		ReadLockGuard(const ReadLockGuard&) = delete;
		ReadLockGuard& operator=(const ReadLockGuard&) = delete;

	private:
		ReadWriteLock& lock_;
	};

	class WriteLockGuard {
	public:
		explicit WriteLockGuard(ReadWriteLock& lock) : lock_(lock) {
			lock_.lock_write();
		}

		~WriteLockGuard() {
			lock_.unlock_write();
		}

		WriteLockGuard(const WriteLockGuard&) = delete;
		WriteLockGuard& operator=(const WriteLockGuard&) = delete;

	private:
		ReadWriteLock& lock_;
	};
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////embedding start

// Provides an efficient implementation of a semaphore (LightweightSemaphore).
// This is an extension of Jeff Preshing's sempahore implementation (licensed 
// under the terms of its separate zlib license) that has been adapted and
// extended by Cameron Desrochers.
//
// Modified by [HSLL] on [2025/7/14]
// Modifications:
//   - Removed MOODYCAMEL_DELETE_FUNCTION macro and replaced with direct 
//     "= delete" syntax for copy constructors and assignment operators
//   - Utilizing modern C++11 delete semantics for clearer intent

#if defined(_WIN32)
// Avoid including windows.h in a header; we only need a handful of
// items, so we'll redeclare them here (this is relatively safe since
// the API generally has to remain stable between Windows versions).
// I know this is an ugly hack but it still beats polluting the global
// namespace with thousands of generic names or adding a .cpp for nothing.
extern "C" {
	struct _SECURITY_ATTRIBUTES;
	__declspec(dllimport) void* __stdcall CreateSemaphoreW(_SECURITY_ATTRIBUTES* lpSemaphoreAttributes, long lInitialCount, long lMaximumCount, const wchar_t* lpName);
	__declspec(dllimport) int __stdcall CloseHandle(void* hObject);
	__declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void* hHandle, unsigned long dwMilliseconds);
	__declspec(dllimport) int __stdcall ReleaseSemaphore(void* hSemaphore, long lReleaseCount, long* lpPreviousCount);
}
#elif defined(__MACH__)
#include <mach/mach.h>
#elif defined(__MVS__)
#include <zos-semaphore.h>
#elif defined(__unix__)
#include <semaphore.h>

#if defined(__GLIBC_PREREQ) && defined(_GNU_SOURCE)
#if __GLIBC_PREREQ(2,30)
#define MOODYCAMEL_LIGHTWEIGHTSEMAPHORE_MONOTONIC
#endif
#endif
#endif

namespace moodycamel
{
	namespace details
	{

		// Code in the mpmc_sema namespace below is an adaptation of Jeff Preshing's
		// portable + lightweight semaphore implementations, originally from
		// https://github.com/preshing/cpp11-on-multicore/blob/master/common/sema.h
		// LICENSE:
		// Copyright (c) 2015 Jeff Preshing
		//
		// This software is provided 'as-is', without any express or implied
		// warranty. In no event will the authors be held liable for any damages
		// arising from the use of this software.
		//
		// Permission is granted to anyone to use this software for any purpose,
		// including commercial applications, and to alter it and redistribute it
		// freely, subject to the following restrictions:
		//
		// 1. The origin of this software must not be misrepresented; you must not
		//	claim that you wrote the original software. If you use this software
		//	in a product, an acknowledgement in the product documentation would be
		//	appreciated but is not required.
		// 2. Altered source versions must be plainly marked as such, and must not be
		//	misrepresented as being the original software.
		// 3. This notice may not be removed or altered from any source distribution.
#if defined(_WIN32)
		class Semaphore
		{
		private:
			void* m_hSema;

			Semaphore(const Semaphore& other) = delete;
			Semaphore& operator=(const Semaphore& other) = delete;

		public:
			Semaphore(int initialCount = 0)
			{
				assert(initialCount >= 0);
				const long maxLong = 0x7fffffff;
				m_hSema = CreateSemaphoreW(nullptr, initialCount, maxLong, nullptr);
				assert(m_hSema);
			}

			~Semaphore()
			{
				CloseHandle(m_hSema);
			}

			bool wait()
			{
				const unsigned long infinite = 0xffffffff;
				return WaitForSingleObject(m_hSema, infinite) == 0;
			}

			bool try_wait()
			{
				return WaitForSingleObject(m_hSema, 0) == 0;
			}

			bool timed_wait(std::uint64_t usecs)
			{
				return WaitForSingleObject(m_hSema, (unsigned long)(usecs / 1000)) == 0;
			}

			void signal(int count = 1)
			{
				while (!ReleaseSemaphore(m_hSema, count, nullptr));
			}
		};
#elif defined(__MACH__)
//---------------------------------------------------------
// Semaphore (Apple iOS and OSX)
// Can't use POSIX semaphores due to http://lists.apple.com/archives/darwin-kernel/2009/Apr/msg00010.html
//---------------------------------------------------------
		class Semaphore
		{
		private:
			semaphore_t m_sema;

			Semaphore(const Semaphore& other) = delete;
			Semaphore& operator=(const Semaphore& other) = delete;

		public:
			Semaphore(int initialCount = 0)
			{
				assert(initialCount >= 0);
				kern_return_t rc = semaphore_create(mach_task_self(), &m_sema, SYNC_POLICY_FIFO, initialCount);
				assert(rc == KERN_SUCCESS);
				(void)rc;
			}

			~Semaphore()
			{
				semaphore_destroy(mach_task_self(), m_sema);
			}

			bool wait()
			{
				return semaphore_wait(m_sema) == KERN_SUCCESS;
			}

			bool try_wait()
			{
				return timed_wait(0);
			}

			bool timed_wait(std::uint64_t timeout_usecs)
			{
				mach_timespec_t ts;
				ts.tv_sec = static_cast<unsigned int>(timeout_usecs / 1000000);
				ts.tv_nsec = static_cast<int>((timeout_usecs % 1000000) * 1000);

				// added in OSX 10.10: https://developer.apple.com/library/prerelease/mac/documentation/General/Reference/APIDiffsMacOSX10_10SeedDiff/modules/Darwin.html
				kern_return_t rc = semaphore_timedwait(m_sema, ts);
				return rc == KERN_SUCCESS;
			}

			void signal()
			{
				while (semaphore_signal(m_sema) != KERN_SUCCESS);
			}

			void signal(int count)
			{
				while (count-- > 0)
				{
					while (semaphore_signal(m_sema) != KERN_SUCCESS);
				}
			}
		};
#elif defined(__unix__) || defined(__MVS__)
//---------------------------------------------------------
// Semaphore (POSIX, Linux, zOS)
//---------------------------------------------------------
		class Semaphore
		{
		private:
			sem_t m_sema;

			Semaphore(const Semaphore& other) = delete;
			Semaphore& operator=(const Semaphore& other) = delete;

		public:
			Semaphore(int initialCount = 0)
			{
				assert(initialCount >= 0);
				int rc = sem_init(&m_sema, 0, static_cast<unsigned int>(initialCount));
				assert(rc == 0);
				(void)rc;
			}

			~Semaphore()
			{
				sem_destroy(&m_sema);
			}

			bool wait()
			{
				// http://stackoverflow.com/questions/2013181/gdb-causes-sem-wait-to-fail-with-eintr-error
				int rc;
				do {
					rc = sem_wait(&m_sema);
				} while (rc == -1 && errno == EINTR);
				return rc == 0;
			}

			bool try_wait()
			{
				int rc;
				do {
					rc = sem_trywait(&m_sema);
				} while (rc == -1 && errno == EINTR);
				return rc == 0;
			}

			bool timed_wait(std::uint64_t usecs)
			{
				struct timespec ts;
				const int usecs_in_1_sec = 1000000;
				const int nsecs_in_1_sec = 1000000000;
#ifdef MOODYCAMEL_LIGHTWEIGHTSEMAPHORE_MONOTONIC
				clock_gettime(CLOCK_MONOTONIC, &ts);
#else
				clock_gettime(CLOCK_REALTIME, &ts);
#endif
				ts.tv_sec += (time_t)(usecs / usecs_in_1_sec);
				ts.tv_nsec += (long)(usecs % usecs_in_1_sec) * 1000;
				// sem_timedwait bombs if you have more than 1e9 in tv_nsec
				// so we have to clean things up before passing it in
				if (ts.tv_nsec >= nsecs_in_1_sec) {
					ts.tv_nsec -= nsecs_in_1_sec;
					++ts.tv_sec;
				}

				int rc;
				do {
#ifdef MOODYCAMEL_LIGHTWEIGHTSEMAPHORE_MONOTONIC
					rc = sem_clockwait(&m_sema, CLOCK_MONOTONIC, &ts);
#else
					rc = sem_timedwait(&m_sema, &ts);
#endif
				} while (rc == -1 && errno == EINTR);
				return rc == 0;
			}

			void signal()
			{
				while (sem_post(&m_sema) == -1);
			}

			void signal(int count)
			{
				while (count-- > 0)
				{
					while (sem_post(&m_sema) == -1);
				}
			}
		};
#else
#error Unsupported platform! (No semaphore wrapper available)
#endif

	}	// end namespace details


	//---------------------------------------------------------
	// LightweightSemaphore
	//---------------------------------------------------------
	class LightweightSemaphore
	{
	public:
		typedef std::make_signed<std::size_t>::type ssize_t;

	private:
		std::atomic<ssize_t> m_count;
		details::Semaphore m_sema;
		int m_maxSpins;

		bool waitWithPartialSpinning(std::int64_t timeout_usecs = -1)
		{
			ssize_t oldCount;
			int spin = m_maxSpins;
			while (--spin >= 0)
			{
				oldCount = m_count.load(std::memory_order_relaxed);
				if ((oldCount > 0) && m_count.compare_exchange_strong(oldCount, oldCount - 1, std::memory_order_acquire, std::memory_order_relaxed))
					return true;
				std::atomic_signal_fence(std::memory_order_acquire);	 // Prevent the compiler from collapsing the loop.
			}
			oldCount = m_count.fetch_sub(1, std::memory_order_acquire);
			if (oldCount > 0)
				return true;
			if (timeout_usecs < 0)
			{
				if (m_sema.wait())
					return true;
			}
			if (timeout_usecs > 0 && m_sema.timed_wait((std::uint64_t)timeout_usecs))
				return true;
			// At this point, we've timed out waiting for the semaphore, but the
			// count is still decremented indicating we may still be waiting on
			// it. So we have to re-adjust the count, but only if the semaphore
			// wasn't signaled enough times for us too since then. If it was, we
			// need to release the semaphore too.
			while (true)
			{
				oldCount = m_count.load(std::memory_order_acquire);
				if (oldCount >= 0 && m_sema.try_wait())
					return true;
				if (oldCount < 0 && m_count.compare_exchange_strong(oldCount, oldCount + 1, std::memory_order_relaxed, std::memory_order_relaxed))
					return false;
			}
		}

		ssize_t waitManyWithPartialSpinning(ssize_t max, std::int64_t timeout_usecs = -1)
		{
			assert(max > 0);
			ssize_t oldCount;
			int spin = m_maxSpins;
			while (--spin >= 0)
			{
				oldCount = m_count.load(std::memory_order_relaxed);
				if (oldCount > 0)
				{
					ssize_t newCount = oldCount > max ? oldCount - max : 0;
					if (m_count.compare_exchange_strong(oldCount, newCount, std::memory_order_acquire, std::memory_order_relaxed))
						return oldCount - newCount;
				}
				std::atomic_signal_fence(std::memory_order_acquire);
			}
			oldCount = m_count.fetch_sub(1, std::memory_order_acquire);
			if (oldCount <= 0)
			{
				if ((timeout_usecs == 0) || (timeout_usecs < 0 && !m_sema.wait()) || (timeout_usecs > 0 && !m_sema.timed_wait((std::uint64_t)timeout_usecs)))
				{
					while (true)
					{
						oldCount = m_count.load(std::memory_order_acquire);
						if (oldCount >= 0 && m_sema.try_wait())
							break;
						if (oldCount < 0 && m_count.compare_exchange_strong(oldCount, oldCount + 1, std::memory_order_relaxed, std::memory_order_relaxed))
							return 0;
					}
				}
			}
			if (max > 1)
				return 1 + tryWaitMany(max - 1);
			return 1;
		}

	public:
		LightweightSemaphore(ssize_t initialCount = 0, int maxSpins = 10000) : m_count(initialCount), m_maxSpins(maxSpins)
		{
			assert(initialCount >= 0);
			assert(maxSpins >= 0);
		}

		bool tryWait()
		{
			ssize_t oldCount = m_count.load(std::memory_order_relaxed);
			while (oldCount > 0)
			{
				if (m_count.compare_exchange_weak(oldCount, oldCount - 1, std::memory_order_acquire, std::memory_order_relaxed))
					return true;
			}
			return false;
		}

		bool wait()
		{
			return tryWait() || waitWithPartialSpinning();
		}

		bool wait(std::int64_t timeout_usecs)
		{
			return tryWait() || waitWithPartialSpinning(timeout_usecs);
		}

		// Acquires between 0 and (greedily) max, inclusive
		ssize_t tryWaitMany(ssize_t max)
		{
			assert(max >= 0);
			ssize_t oldCount = m_count.load(std::memory_order_relaxed);
			while (oldCount > 0)
			{
				ssize_t newCount = oldCount > max ? oldCount - max : 0;
				if (m_count.compare_exchange_weak(oldCount, newCount, std::memory_order_acquire, std::memory_order_relaxed))
					return oldCount - newCount;
			}
			return 0;
		}

		// Acquires at least one, and (greedily) at most max
		ssize_t waitMany(ssize_t max, std::int64_t timeout_usecs)
		{
			assert(max >= 0);
			ssize_t result = tryWaitMany(max);
			if (result == 0 && max > 0)
				result = waitManyWithPartialSpinning(max, timeout_usecs);
			return result;
		}

		ssize_t waitMany(ssize_t max)
		{
			ssize_t result = waitMany(max, -1);
			assert(result > 0);
			return result;
		}

		void signal(ssize_t count = 1)
		{
			assert(count >= 0);
			ssize_t oldCount = m_count.fetch_add(count, std::memory_order_release);
			ssize_t toRelease = -oldCount < count ? -oldCount : count;
			if (toRelease > 0)
			{
				m_sema.signal((int)toRelease);
			}
		}

		std::size_t availableApprox() const
		{
			ssize_t count = m_count.load(std::memory_order_relaxed);
			return count > 0 ? static_cast<std::size_t>(count) : 0;
		}
	};

}   // end namespace moodycamel


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////embedding end

namespace HSLL
{
	enum BULK_CMETHOD
	{
		COPY,
		MOVE
	};

	/**
	* @brief Helper template for bulk construction (copy/move)
	*/
	template <typename T, BULK_CMETHOD Method>
	struct BulkConstructHelper;

	template <typename T>
	struct BulkConstructHelper<T, COPY>
	{
		static void construct(T& dst, T& src)
		{
			new (&dst) T(src);
		}
	};

	template <typename T>
	struct BulkConstructHelper<T, MOVE>
	{
		static void construct(T& dst, T& src)
		{
			new (&dst) T(std::move(src));
		}
	};

	template <BULK_CMETHOD Method, typename T>
	void bulk_construct(T& dst, T& src)
	{
		BulkConstructHelper<T, Method>::construct(dst, src);
	}

	template<typename TYPE>
	class TPLFQueue
	{
		struct Slot
		{
			alignas(TYPE) unsigned char storage[sizeof(TYPE)];
			std::atomic<unsigned long long> sequence;
		};

		Slot* buffer;
		unsigned int capacity;
		std::atomic<unsigned long long> head;
		std::atomic<unsigned long long> tail;

		Slot* tryLockHead(unsigned long long& r_current_head, unsigned int count = 1)
		{
			Slot* slot;
			unsigned long long current_head = head.load(std::memory_order_relaxed);

			while (true)
			{
				unsigned long long required = current_head + count;
				slot = buffer + ((required - 1) % capacity);
				unsigned long long seq = slot->sequence.load(std::memory_order_acquire);
				long long diff = (long long)(seq - required);

				if (diff > 0)
				{
					current_head = head.load(std::memory_order_relaxed);
				}
				else if (!diff)
				{
					if (head.compare_exchange_weak(
						current_head, required,
						std::memory_order_relaxed, std::memory_order_relaxed
					)) break;
				}
				else
				{
					return nullptr;
				}
			}

			r_current_head = current_head;
			return slot;
		}

		unsigned long long tryLockHeadBulk(unsigned int& count)
		{
			unsigned long long current_head;
			while (count)
			{
				if (tryLockHead(current_head, count))
					return current_head;

				count >>= 1;
			}

			count = 0;
			return 0;
		}

		Slot* tryLockTail(unsigned long long& r_current_tail, unsigned int count = 1)
		{
			Slot* slot;
			unsigned long long current_tail = tail.load(std::memory_order_relaxed);

			while (true)
			{
				unsigned long long required = current_tail + count - 1;
				slot = buffer + (required % capacity);
				unsigned long long seq = slot->sequence.load(std::memory_order_acquire);
				long long diff = (long long)(seq - required);

				if (diff > 0)
				{
					current_tail = tail.load(std::memory_order_relaxed);
				}
				else if (!diff)
				{
					if (tail.compare_exchange_weak(
						current_tail, required + 1,
						std::memory_order_relaxed, std::memory_order_relaxed
					)) break;
				}
				else
				{
					return nullptr;
				}
			}
			r_current_tail = current_tail;
			return slot;
		}

		unsigned long long tryLockTailBulk(unsigned int& count)
		{
			unsigned long long current_Tail;
			while (count)
			{
				if (tryLockTail(current_Tail, count))
					return current_Tail;

				count >>= 1;
			}

			count = 0;
			return 0;
		}

		void waitReady(std::atomic<unsigned long long>& sequence, unsigned long long require)
		{
			while (sequence.load(std::memory_order_relaxed) != require);
		}

	public:

		TPLFQueue() : buffer(nullptr) {}

		bool init(unsigned int user_capacity)
		{
			if (buffer || user_capacity < 2)
				return false;

			capacity = user_capacity;
			buffer = (Slot*)(ALIGNED_MALLOC(sizeof(Slot) * capacity, std::max(alignof(TYPE), (size_t)64)));

			if (!buffer)
				return false;

			for (unsigned int i = 0; i < capacity; ++i)
			{
				new (&buffer[i]) Slot;
				buffer[i].sequence.store(i, std::memory_order_release);
			}

			head = 0;
			tail = 0;
			return true;
		}

		template <typename... Args>
		bool emplace(Args &&...args)
		{
			assert(buffer);
			Slot* slot;
			unsigned long long current_tail;

			if (LIKELY(slot = tryLockTail(current_tail)))
			{
				new (slot->storage) TYPE(std::forward<Args>(args)...);
				slot->sequence.store(current_tail + 1, std::memory_order_release);
				return true;
			}

			return false;
		}

		template <class T>
		bool push(T&& item)
		{
			assert(buffer);
			Slot* slot;
			unsigned long long current_tail;

			if (LIKELY(slot = tryLockTail(current_tail)))
			{
				new (slot->storage) TYPE(std::forward<T>(item));
				slot->sequence.store(current_tail + 1, std::memory_order_release);
				return true;
			}

			return false;
		}

		template <BULK_CMETHOD METHOD = COPY>
		unsigned int pushBulk(TYPE* elements, unsigned int count)
		{
			assert(buffer);
			assert(elements && count);
			count = std::min(count, capacity);
			unsigned num = count;
			unsigned long long current_tail = tryLockTailBulk(num);

			if (UNLIKELY(!num))
				return 0;

			for (unsigned int i = 0; i < num; i++)
			{
				unsigned long long index = current_tail + i;
				unsigned int slot_idx = index % capacity;
				Slot& slot = buffer[slot_idx];

				if (LIKELY(i != num - 1))
					waitReady(slot.sequence, index);

				TYPE* item = (TYPE*)(slot.storage);
				bulk_construct<METHOD>(*item, elements[i]);
				slot.sequence.store(index + 1, std::memory_order_release);
			}

			if (LIKELY(num < count))
				return num + pushBulk<METHOD>(elements + num, count - num);

			return count;
		}

		template <BULK_CMETHOD METHOD = COPY>
		unsigned int pushBulk(TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
		{
			assert(buffer);
			assert(part1 && part2 && count1);
			unsigned int total = std::min(count1 + count2, capacity);
			unsigned int num = total;
			unsigned long long current_tail = tryLockTailBulk(num);

			if (UNLIKELY(!num))
				return 0;

			for (unsigned int i = 0; i < num; i++)
			{
				unsigned long long index = current_tail + i;
				unsigned int slot_idx = index % capacity;
				Slot& slot = buffer[slot_idx];
				TYPE* item = reinterpret_cast<TYPE*>(slot.storage);

				if (LIKELY(i != num - 1))
					waitReady(slot.sequence, index);

				if (i < count1)
					bulk_construct<METHOD>(*item, part1[i]);
				else
					bulk_construct<METHOD>(*item, part2[i - count1]);

				slot.sequence.store(index + 1, std::memory_order_release);
			}

			if (LIKELY(num < total))
			{
				if (num < count1)
					return num + pushBulk<METHOD>(part1 + num, count1 - num, part2, count2);
				else
					return num + pushBulk<METHOD>(part2 + num - count1, total - num);
			}

			return total;
		}

		bool pop(TYPE& out)
		{
			assert(buffer);
			Slot* slot;
			unsigned long long current_head;

			if (LIKELY(slot = tryLockHead(current_head)))
			{
				TYPE* item = (TYPE*)(slot->storage);
				new (&out) TYPE(std::move(*item));
				item->~TYPE();
				slot->sequence.store(current_head + capacity, std::memory_order_release);
				return true;
			}

			return false;
		}

		unsigned int popBulk(TYPE* elements, unsigned int count)
		{
			assert(buffer);
			assert(elements && count);
			count = std::min(count, capacity);
			unsigned long long current_head = tryLockHeadBulk(count);

			if (UNLIKELY(!count))
				return 0;

			for (unsigned int i = 0; i < count; i++)
			{
				unsigned long long index = current_head + i;
				unsigned int slot_idx = index % capacity;
				Slot& slot = buffer[slot_idx];

				if (LIKELY(i != count - 1))
					waitReady(slot.sequence, index + 1);

				TYPE* item = (TYPE*)slot.storage;
				new (elements + i) TYPE(std::move(*item));
				item->~TYPE();
				slot.sequence.store(index + capacity, std::memory_order_release);
			}

			return count;
		}

		unsigned int get_size()
		{
			assert(buffer);
			long long h = (long long)head.load(std::memory_order_acquire);
			long long t = (long long)tail.load(std::memory_order_acquire);
			return t - h;
		}

		unsigned long long get_bsize()
		{
			assert(buffer);
			return sizeof(Slot) * capacity;
		}

		void release()
		{
			assert(buffer);
			unsigned long long current_head = head.load();
			unsigned long long current_tail = tail.load();

			while (current_head != current_tail)
			{
				Slot& slot = buffer[current_head % capacity];
				if (slot.sequence.load() == current_head + 1)
				{
					TYPE* free_ptr = (TYPE*)slot.storage;
					free_ptr->~TYPE();
				}
				current_head++;
			}

			ALIGNED_FREE(buffer);
			head = 0;
			tail = 0;
			buffer = nullptr;
		}

		~TPLFQueue()
		{
			if (buffer)
				release();
		}

		TPLFQueue(const TPLFQueue&) = delete;
		TPLFQueue& operator=(const TPLFQueue&) = delete;
	};

	template<typename TYPE>
	class alignas(64) TPBLFQueue
	{
		bool flag;
		TPLFQueue<TYPE> queue;
		std::atomic<bool> isStopped;
		moodycamel::LightweightSemaphore sem;

	public:

		TPBLFQueue() :flag(false) {}

		bool init(unsigned int capacity)
		{
			if (!queue.init(capacity))
				return false;

			isStopped.store(false, std::memory_order_release);
			flag = true;

			return true;
		}

		void stopWait()
		{
			assert(flag);
			return isStopped.store(true, std::memory_order_release);
		}

		bool is_Stopped()
		{
			assert(flag);
			return isStopped.load(std::memory_order_relaxed);
		}

		bool is_Stopped_Real()
		{
			assert(flag);
			return isStopped.load(std::memory_order_acquire);
		}

		unsigned int get_size()
		{
			assert(flag);
			return queue.get_size();
		}

		unsigned long long get_bsize()
		{
			assert(flag);
			return queue.get_bsize();
		}

		template <typename... Args>
		bool emplace(Args &&...args)
		{
			assert(flag);
			if (LIKELY(queue.emplace(std::forward<Args>(args)...)))
			{
				sem.signal();
				return true;
			}

			return false;
		}

		template <class T>
		bool push(T&& item)
		{
			assert(flag);
			if (LIKELY(queue.push(std::forward<T>(item))))
			{
				sem.signal();
				return true;
			}

			return false;
		}

		template <BULK_CMETHOD METHOD = COPY>
		unsigned int pushBulk(TYPE* elements, unsigned int count)
		{
			assert(flag);
			unsigned int num;
			if (LIKELY(num = queue.template pushBulk<METHOD>(elements, count)))
			{
				sem.signal(num);
				return num;
			}

			return 0;
		}

		template <BULK_CMETHOD METHOD = COPY>
		unsigned int pushBulk(TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
		{
			assert(flag);
			unsigned int num;
			if (LIKELY(num = queue.template pushBulk<METHOD>(part1, count1, part2, count2)))
			{
				sem.signal(num);
				return num;
			}

			return 0;
		}

		bool pop(TYPE& element)
		{
			assert(flag);
			if (LIKELY(sem.tryWait()))
			{
				while (UNLIKELY(!queue.pop(element)));
				return true;
			}

			return false;
		}

		bool wait_pop(TYPE& element, std::int64_t timeout_usecs)
		{
			assert(flag);
			if (LIKELY(sem.wait(timeout_usecs)))
			{
				while (UNLIKELY(!queue.pop(element)));
				return true;
			}

			return false;
		}

		unsigned int popBulk(TYPE* elements, unsigned int count)
		{
			assert(flag);
			unsigned int num;
			if (LIKELY(num = sem.tryWaitMany(count)))
			{
				unsigned int succeed = 0;

				while (UNLIKELY(succeed < num))
					succeed += queue.popBulk(elements + succeed, num - succeed);

				return num;
			}

			return 0;
		}

		unsigned int wait_popBulk(TYPE* elements, unsigned int count, std::int64_t timeout_usecs)
		{
			assert(flag);
			unsigned int num = 0;
			if (LIKELY(num = sem.waitMany(count, timeout_usecs)))
			{
				unsigned int succeed = 0;

				while (UNLIKELY(succeed < num))
					succeed += queue.popBulk(elements + succeed, num - succeed);

				return num;
			}

			return 0;
		}

		void release()
		{
			assert(flag);
			queue.release();
			sem.~LightweightSemaphore();
			new (&sem) moodycamel::LightweightSemaphore();
			flag = false;
		}

		~TPBLFQueue()
		{
			if (flag)
				release();
		}

		TPBLFQueue(const TPBLFQueue&) = delete;
		TPBLFQueue& operator=(const TPBLFQueue&) = delete;
	};
}

#define HSLL_THREADPOOL_TIMEOUT 5
#define HSLL_THREADPOOL_SHRINK_FACTOR 0.25
#define HSLL_THREADPOOL_EXPAND_FACTOR 0.75

static_assert(HSLL_THREADPOOL_TIMEOUT > 0, "Invalid timeout value.");
static_assert(HSLL_THREADPOOL_SHRINK_FACTOR < HSLL_THREADPOOL_EXPAND_FACTOR&& HSLL_THREADPOOL_EXPAND_FACTOR < 1.0
	&& HSLL_THREADPOOL_SHRINK_FACTOR>0.0, "Invalid factors.");

namespace HSLL
{
	template <class T>
	class SingleStealer
	{
		template <class TYPE>
		friend class ThreadPool;
	private:

		unsigned int index;
		unsigned int queueLength;
		unsigned int* threadNum;
		unsigned int threshold;

		ReadWriteLock* rwLock;
		TPBLFQueue<T>* queues;
		TPBLFQueue<T>* ignore;

		SingleStealer(ReadWriteLock* rwLock, TPBLFQueue<T>* queues, TPBLFQueue<T>* ignore,
			unsigned int queueLength, unsigned int* threadNum)
		{
			this->index = 0;
			this->queueLength = queueLength;
			this->threadNum = threadNum;
			this->threshold = std::min((unsigned int)2, queueLength);
			this->rwLock = rwLock;
			this->queues = queues;
			this->ignore = ignore;
		}

		unsigned int steal(T& element)
		{
			ReadLockGuard lock(*rwLock);
			unsigned int num = *threadNum;
			for (int i = 0; i < num; ++i)
			{
				unsigned int now = (index + i) % num;
				TPBLFQueue<T>* queue = queues + now;
				if (queue->get_size() >= threshold && queue != ignore)
				{
					if (queue->pop(element))
					{
						index = now;
						return 1;
					}
				}
			}
			return 0;
		}
	};

	template <class T>
	class BulkStealer
	{
		template <class TYPE>
		friend class ThreadPool;

	private:

		unsigned int index;
		unsigned int batchSize;
		unsigned int queueLength;
		unsigned int* threadNum;
		unsigned int threshold;

		ReadWriteLock* rwLock;
		TPBLFQueue<T>* queues;
		TPBLFQueue<T>* ignore;

		BulkStealer(ReadWriteLock* rwLock, TPBLFQueue<T>* queues, TPBLFQueue<T>* ignore, unsigned int queueLength,
			unsigned int* threadNum, unsigned int batchSize)
		{
			this->index = 0;
			this->batchSize = batchSize;
			this->queueLength = queueLength;
			this->threadNum = threadNum;
			this->threshold = std::min(2 * batchSize, queueLength);
			this->rwLock = rwLock;
			this->queues = queues;
			this->ignore = ignore;
		}

		unsigned int steal(T* elements)
		{
			ReadLockGuard lock(*rwLock);
			unsigned int num = *threadNum;
			for (int i = 0; i < num; ++i)
			{
				unsigned int now = (index + i) % num;
				TPBLFQueue<T>* queue = queues + now;
				if (queue->get_size() >= threshold && queue != ignore)
				{
					unsigned int count = queue->popBulk(elements, batchSize);
					if (count)
					{
						index = now;
						return count;
					}
				}
			}
			return 0;
		}
	};

	/**
	 * @brief Thread pool implementation with multiple queues for task distribution
	 * @tparam T Type of task objects to be processed, must implement execute() method
	 */
	template <class T = TaskStack<>>
	class ThreadPool
	{
		static_assert(is_generic_ts<T>::value, "TYPE must be a TaskStack type");

	private:

		bool shutdownPolicy;			  ///< Thread pool shutdown policy: true for graceful shutdown	
		unsigned int threadNum;			  ///< Number of worker threads/queues
		unsigned int minThreadNum;
		unsigned int maxThreadNum;
		unsigned int batchSize;
		unsigned int queueLength;		  ///< Capacity of each internal queue
		std::chrono::milliseconds adjustInterval;

		ReadWriteLock rwLock;
		std::atomic<bool> exitFlag;
		moodycamel::LightweightSemaphore exitSem;
		moodycamel::LightweightSemaphore* stopSem;
		moodycamel::LightweightSemaphore* startSem;

		std::thread monitor;
		TPBLFQueue<T>* queues;		  ///< Per-worker task queues
		std::vector<std::thread> workers; ///< Worker thread collection
		std::atomic<unsigned int> index;  ///< Atomic counter for round-robin task distribution to worker queues

	public:

		/**
		 * @brief Constructs an uninitialized thread pool
		 */
		ThreadPool() : queues(nullptr) {}

		/**
		* @brief Initializes thread pool resources
		* @param capacity Capacity of each internal queue (>2)
		* @param minThreadNum Minimum number of worker threads (!=0)
		* @param maxThreadNum Maximum number of worker threads (>=minThreadNum)
		* @param batchSize Maximum tasks to process per batch (min 1)
		* @param adjustInterval Time interval for checking the load and adjusting the number of active threads
		* @return true if initialization succeeded, false otherwise
		*/
		bool init(unsigned int capacity, unsigned int minThreadNum,
			unsigned int maxThreadNum, unsigned int batchSize = 1,
			std::chrono::milliseconds adjustInterval = std::chrono::milliseconds(3000)) noexcept
		{
			if (batchSize == 0 || minThreadNum == 0 || capacity < 2 || minThreadNum > maxThreadNum)
				return false;

			unsigned int succeed = 0;

			if (maxThreadNum > 1)
			{
				stopSem = new(std::nothrow) moodycamel::LightweightSemaphore[maxThreadNum];
				if (!stopSem)
					goto clean_1;

				startSem = new(std::nothrow) moodycamel::LightweightSemaphore[maxThreadNum];
				if (!startSem)
					goto clean_2;
			}

			queues = (TPBLFQueue<T>*)ALIGNED_MALLOC(maxThreadNum * sizeof(TPBLFQueue<T>), 64);
			if (!queues)
				goto clean_3;

			for (unsigned i = 0; i < maxThreadNum; ++i)
			{
				new (&queues[i]) TPBLFQueue<T>();

				if (!queues[i].init(capacity))
					goto clean_4;

				succeed++;
			}

			this->index = 0;
			this->exitFlag = false;
			this->shutdownPolicy = true;
			this->minThreadNum = minThreadNum;
			this->maxThreadNum = maxThreadNum;
			this->threadNum = maxThreadNum;
			this->batchSize = std::min(batchSize, capacity);
			this->queueLength = capacity;
			this->adjustInterval = adjustInterval;
			workers.reserve(maxThreadNum);

			for (unsigned i = 0; i < maxThreadNum; ++i)
				workers.emplace_back(&ThreadPool::worker, this, i);

			if (maxThreadNum > 1)
				monitor = std::thread(&ThreadPool::load_monitor, this);

			return true;

		clean_4:

			for (unsigned i = 0; i < succeed; ++i)
				queues[i].~TPBLFQueue<T>();

		clean_3:

			ALIGNED_FREE(queues);
			queues = nullptr;

		clean_2:

			if (maxThreadNum > 1)
			{
				delete[] stopSem;
				stopSem = nullptr;
			}

		clean_1:

			return false;
		}


		/**
		 * @brief Non-blocking task emplacement with perfect forwarding
		 * @tparam Args Types of arguments for task constructor
		 * @param args Arguments forwarded to task constructor
		 * @return true if task was enqueued, false if queue was full
		 * @details Constructs task in-place at selected position without blocking
		 */
		template <typename... Args>
		bool emplace(Args &&...args) noexcept
		{
			assert(queues);

			if (maxThreadNum == 1)
				return queues->emplace(std::forward<Args>(args)...);

			ReadLockGuard lock(rwLock);
			return select_queue().emplace(std::forward<Args>(args)...);
		}

		/**
		 * @brief Non-blocking push for preconstructed task
		 * @tparam U Deduced task type (supports perfect forwarding)
		 * @param task Task object to enqueue
		 * @return true if task was enqueued, false if queue was full
		 */
		template <class U>
		bool enqueue(U&& task) noexcept
		{
			assert(queues);

			if (maxThreadNum == 1)
				return queues->push(std::forward<U>(task));

			ReadLockGuard lock(rwLock);
			return select_queue().push(std::forward<U>(task));
		}

		/**
		 * @brief Non-blocking bulk push for multiple tasks
		 * @tparam METHOD Bulk construction method (COPY or MOVE, default COPY)
		 * @param tasks Array of tasks to enqueue
		 * @param count Number of tasks in array
		 * @return Actual number of tasks enqueued
		 */
		template <BULK_CMETHOD METHOD = COPY>
		unsigned int enqueue_bulk(T* tasks, unsigned int count) noexcept
		{
			assert(queues);

			if (maxThreadNum == 1)
				return queues->template pushBulk<METHOD>(tasks, count);

			ReadLockGuard lock(rwLock);
			return select_queue_for_bulk(std::max(1u, count / 2)).template pushBulk<METHOD>(tasks, count);
		}

		/**
		 * @brief Non-blocking bulk push for multiple tasks (dual-part version)
		 * @tparam METHOD Bulk construction method (COPY or MOVE, default COPY)
		 * @param part1 First array segment of tasks to enqueue
		 * @param count1 Number of tasks in first segment
		 * @param part2 Second array segment of tasks to enqueue
		 * @param count2 Number of tasks in second segment
		 * @return Actual number of tasks successfully enqueued (sum of both segments minus failures)
		 * @note Designed for ring buffers that benefit from batched two-part insertion.
		 */
		template <BULK_CMETHOD METHOD = COPY>
		unsigned int enqueue_bulk(T* part1, unsigned int count1, T* part2, unsigned int count2) noexcept
		{
			assert(queues);

			if (maxThreadNum == 1)
				return queues->template pushBulk<METHOD>(part1, count1, part2, count2);

			ReadLockGuard lock(rwLock);
			return select_queue_for_bulk(std::max(1u, (count1 + count2) / 2)).template pushBulk<METHOD>(part1, count1, part2, count2);
		}

		//Get the maximum occupied space of the thread pool.
		unsigned long long get_max_usage()
		{
			assert(queues);

			return  maxThreadNum * queues->get_bsize();
		}

		/**
		 * @brief Waits until all task queues are empty (all tasks have been taken from queues)
		 * @note This does NOT guarantee all tasks have completed execution - it only ensures:
		 *       1. All tasks have been dequeued by worker threads
		 *       2. May return while some tasks are still being processed by workers
		 * @details Continuously checks all active queues until they're empty.
		 *          Uses yield() between checks to avoid busy waiting.
		 */
		void join()
		{
			assert(queues);

			while (true)
			{
				bool flag = true;

				for (int i = 0; i < maxThreadNum; ++i)
				{
					if (queues[i].get_size()())
					{
						flag = false;
						break;
					}
				}

				if (flag)
					return;
				else
					std::this_thread::yield();
			}
		}

		/**
		 * @brief Waits until all task queues are empty (all tasks dequeued) or sleeps for specified intervals between checks.
		 * @note This does NOT guarantee all tasks have completed execution - it only ensures:
		 *       1. All tasks have been dequeued by worker threads
		 *       2. May return while some tasks are still being processed by workers
		 * @tparam Rep Arithmetic type representing tick count
		 * @tparam Period Type representing tick period
		 * @param interval Sleep duration between queue checks. Smaller values increase responsiveness
		 *                 but may use more CPU, larger values reduce CPU load but delay detection.
		 */
		template <class Rep, class Period>
		void join(const std::chrono::duration<Rep, Period>& interval)
		{
			assert(queues);

			while (true)
			{
				bool flag = true;
				for (int i = 0; i < maxThreadNum; ++i)
				{
					if (queues[i].get_size())
					{
						flag = false;
						break;
					}
				}

				if (flag)
					return;
				else
					std::this_thread::sleep_for(interval);
			}
		}

		/**
		 * @brief Stops all workers and releases resources
		 * @param shutdownPolicy true for graceful shutdown (waiting for tasks to complete), false for immediate shutdown
		 */
		void exit(bool shutdownPolicy = true) noexcept
		{
			assert(queues);

			if (maxThreadNum > 1)
			{
				exitFlag = true;
				exitSem.signal();
				monitor.join();

				for (unsigned i = 0; i < workers.size(); ++i)
					startSem[i].signal();
			}

			this->shutdownPolicy = shutdownPolicy;

			for (unsigned i = 0; i < workers.size(); ++i)
				queues[i].stopWait();

			for (auto& worker : workers)
				worker.join();

			workers.clear();
			workers.shrink_to_fit();

			if (maxThreadNum > 1)
			{
				delete[] stopSem;
				delete[] startSem;
			}

			for (unsigned i = 0; i < maxThreadNum; ++i)
				queues[i].~TPBLFQueue<T>();

			ALIGNED_FREE(queues);
			queues = nullptr;
		}

		~ThreadPool() noexcept
		{
			if (queues)
				exit(false);
		}

		ThreadPool(const ThreadPool&) = delete;
		ThreadPool& operator=(const ThreadPool&) = delete;
		ThreadPool(ThreadPool&&) = delete;
		ThreadPool& operator=(ThreadPool&&) = delete;

	private:

		unsigned int next_index() noexcept
		{
			return index.fetch_add(1, std::memory_order_relaxed) % threadNum;
		}

		TPBLFQueue<T>& select_queue() noexcept
		{
			unsigned int index = next_index();

			if (queues[index].get_size() < queueLength)
				return queues[index];

			unsigned int half = threadNum / 2;
			return queues[(index + half) % threadNum];
		}

		TPBLFQueue<T>& select_queue_for_bulk(unsigned required) noexcept
		{
			unsigned int index = next_index();

			if (queues[index].get_size() + required <= queueLength)
				return queues[index];

			unsigned int half = threadNum / 2;
			return queues[(index + half) % threadNum];
		}

		static inline void execute_tasks(T* tasks, unsigned int count)
		{
			for (unsigned int i = 0; i < count; ++i)
			{
				tasks[i].execute();
				tasks[i].~T();
			}
		}

		void load_monitor() noexcept
		{
			while (true)
			{
				if (exitSem.wait(adjustInterval.count() * 1000))
					return;

				unsigned int allSize = queueLength * threadNum;
				unsigned int totalSize = 0;

				for (int i = 0; i < threadNum; ++i)
					totalSize += queues[i].get_size();

				if (totalSize < allSize * HSLL_THREADPOOL_SHRINK_FACTOR && threadNum > minThreadNum)
				{
					rwLock.lock_write();
					threadNum--;
					rwLock.unlock_write();
					queues[threadNum].stopWait();
					while (!stopSem[threadNum].wait());
					queues[threadNum].release();
				}
				else if (totalSize > allSize * HSLL_THREADPOOL_EXPAND_FACTOR && threadNum < maxThreadNum)
				{
					unsigned int newThreads = std::max(1u, (maxThreadNum - threadNum) / 2);
					unsigned int succeed = 0;
					for (int i = threadNum; i < threadNum + newThreads; ++i)
					{
						if (!queues[i].init(queueLength))
							break;

						startSem[i].signal();
						succeed++;
					}

					if (succeed > 0)
					{
						rwLock.lock_write();
						threadNum += succeed;
						rwLock.unlock_write();
					}
				}
			}
		}

		void worker(unsigned int index) noexcept
		{
			if (batchSize == 1)
				process_single(queues + index, index);
			else
				process_bulk(queues + index, index, batchSize);
		}

		void process_single(TPBLFQueue<T>* queue, unsigned int index) noexcept
		{
			alignas(alignof(T)) char storage[sizeof(T)];
			T* task = (T*)(&storage);

			if (maxThreadNum == 1)
			{
				while (true)
				{
					if (queue->wait_pop(*task, HSLL_THREADPOOL_TIMEOUT * 1000))
					{
						task->execute();
						task->~T();
					}
					else
					{
						if (queue->is_Stopped_Real())
							break;
					}
				}

				while (shutdownPolicy && queue->pop(*task))
				{
					task->execute();
					task->~T();
				}
				return;
			}

			SingleStealer<T> stealer(&rwLock, queues, queue, queueLength, &threadNum);

			while (true)
			{
				while (true)
				{
					while (queue->pop(*task))
					{
						task->execute();
						task->~T();
					}

					if (stealer.steal(*task))
					{
						task->execute();
						task->~T();
					}
					else
					{
						if (queue->wait_pop(*task, HSLL_THREADPOOL_TIMEOUT * 1000))
						{
							task->execute();
							task->~T();
						}
						else
						{
							if (queue->is_Stopped_Real())
								break;
						}
					}

					if (queue->is_Stopped())
						break;
				}

				while (shutdownPolicy && queue->pop(*task))
				{
					task->execute();
					task->~T();
				}

				stopSem[index].signal();
				while (!startSem[index].wait());

				if (exitFlag)
					break;
			}
		}

		void process_bulk(TPBLFQueue<T>* queue, unsigned int index, unsigned batchSize) noexcept
		{
			T* tasks;
			unsigned int count;

			if (!(tasks = (T*)ALIGNED_MALLOC(sizeof(T) * batchSize, alignof(T))))
				std::abort();

			unsigned int size_threshold = batchSize;
			unsigned int round_threshold = batchSize / 2;

			if (maxThreadNum == 1)
			{
				while (true)
				{
					while (true)
					{
						unsigned int round = 1;
						unsigned int size = queue->get_size();

						while (size < size_threshold && round < round_threshold)
						{
							std::this_thread::yield();
							size = queue->get_size();
							round++;
							std::this_thread::yield();
						}

						if (size && (count = queue->popBulk(tasks, size_threshold)))
							execute_tasks(tasks, count);
						else
							break;
					}

					count = queue->wait_popBulk(tasks, batchSize, HSLL_THREADPOOL_TIMEOUT * 1000);

					if (count)
					{
						execute_tasks(tasks, count);
					}
					else
					{
						if (queue->is_Stopped_Real())
							break;
					}
				}

				while (shutdownPolicy && (count = queue->popBulk(tasks, batchSize)))
					execute_tasks(tasks, count);

				ALIGNED_FREE(tasks);
				return;
			}

			BulkStealer<T> stealer(&rwLock, queues, queue, queueLength, &threadNum, batchSize);

			while (true)
			{
				while (true)
				{
					while (true)
					{
						unsigned int round = 1;
						unsigned int size = queue->get_size();

						while (size < size_threshold && round < round_threshold)
						{
							std::this_thread::yield();
							size = queue->get_size();
							round++;
							std::this_thread::yield();
						}

						if (size && (count = queue->popBulk(tasks, size_threshold)))
							execute_tasks(tasks, count);
						else
							break;
					}

					count = stealer.steal(tasks);
					if (count)
					{
						execute_tasks(tasks, count);
					}
					else
					{
						count = queue->wait_popBulk(tasks, batchSize, HSLL_THREADPOOL_TIMEOUT * 1000);

						if (count)
						{
							execute_tasks(tasks, count);
						}
						else
						{
							if (queue->is_Stopped_Real())
								break;
						}
					}

					if (queue->is_Stopped())
						break;
				}

				while (shutdownPolicy && (count = queue->popBulk(tasks, batchSize)))
					execute_tasks(tasks, count);

				stopSem[index].signal();
				while (!startSem[index].wait());

				if (exitFlag)
					break;
			}

			ALIGNED_FREE(tasks);
		}
	};

	template <class T, unsigned int BATCH>
	class BatchSubmitter
	{
		static_assert(is_generic_ts<T>::value, "TYPE must be a TaskStack type");
		static_assert(BATCH > 0, "BATCH > 0");
		alignas(alignof(T)) unsigned char buf[BATCH * sizeof(T)];

		T* elements;
		unsigned int size;
		unsigned int index;
		ThreadPool<T>* pool;

		bool check_and_submit()
		{
			if (size == BATCH)
				return submit() == BATCH;

			return true;
		}

	public:
		/**
		* @brief Constructs a batch submitter associated with a thread pool
		* @param pool Pointer to the thread pool for batch task submission
		*/
		BatchSubmitter(ThreadPool<T>* pool) : size(0), index(0), elements((T*)buf), pool(pool) {
			assert(pool);
		}

		/**
		 * @brief Gets current number of buffered tasks
		 * @return Number of tasks currently held in the batch buffer
		 */
		unsigned int get_size() const noexcept
		{
			return size;
		}

		/**
		 * @brief Checks if batch buffer is empty
		 * @return true if no tasks are buffered, false otherwise
		 */
		bool empty() const noexcept
		{
			return size == 0;
		}

		/**
		 * @brief Checks if batch buffer is full
		 * @return true if batch buffer has reached maximum capacity (BATCH), false otherwise
		 */
		bool full() const noexcept
		{
			return size == BATCH;
		}

		/**
		 * @brief Constructs task in-place in batch buffer
		 * @tparam Args Types of arguments for task constructor
		 * @param args Arguments forwarded to task constructor
		 * @return true if task was added to buffer or submitted successfully,
		 *         false if submission failed due to full thread pool queues
		 * @details Automatically submits batch if buffer becomes full during emplace
		 */
		template <typename... Args>
		bool emplace(Args &&...args) noexcept
		{
			if (!check_and_submit())
				return false;

			new (elements + index) T(std::forward<Args>(args)...);
			index = (index + 1) % BATCH;
			size++;
			return true;
		}

		/**
		 * @brief Adds preconstructed task to batch buffer
		 * @tparam U Deduced task type
		 * @param task Task object to buffer
		 * @return true if task was added to buffer or submitted successfully,
		 *         false if submission failed due to full thread pool queues
		 * @details Automatically submits batch if buffer becomes full during add
		 */
		template <class U>
		bool add(U&& task) noexcept
		{
			if (!check_and_submit())
				return false;

			new (elements + index) T(std::forward<U>(task));
			index = (index + 1) % BATCH;
			size++;
			return true;
		}

		/**
		 * @brief Submits all buffered tasks to thread pool
		 * @return Number of tasks successfully submitted
		 * @details Moves buffered tasks to thread pool in bulk.
		 */
		unsigned int submit() noexcept
		{
			if (!size)
				return 0;

			unsigned int start = (index - size + BATCH) % BATCH;
			unsigned int len1 = (start + size <= BATCH) ? size : (BATCH - start);
			unsigned int len2 = size - len1;
			unsigned int submitted;

			if (!len2)
			{
				if (len1 == 1)
					submitted = pool->enqueue(std::move(*(elements + start))) ? 1 : 0;
				else
					submitted = pool->template enqueue_bulk<MOVE>(elements + start, len1);
			}
			else
			{
				submitted = pool->template enqueue_bulk<MOVE>(
					elements + start, len1,
					elements, len2
				);
			}

			if (submitted > 0)
			{
				if (submitted <= len1)
				{
					for (unsigned i = 0; i < submitted; ++i)
						elements[(start + i) % BATCH].~T();
				}
				else
				{
					for (unsigned i = 0; i < len1; ++i)
						elements[(start + i) % BATCH].~T();

					for (unsigned i = 0; i < submitted - len1; ++i)
						elements[i].~T();
				}

				size -= submitted;
			}

			return submitted;
		}

		~BatchSubmitter() noexcept
		{
			if (size > 0)
			{
				unsigned int start = (index - size + BATCH) % BATCH;
				unsigned int len1 = (start + size <= BATCH) ? size : (BATCH - start);
				unsigned int len2 = size - len1;

				for (unsigned int i = 0; i < len1; i++)
					elements[(start + i) % BATCH].~T();

				for (unsigned int i = 0; i < len2; i++)
					elements[i].~T();
			}
		}

		BatchSubmitter(const BatchSubmitter&) = delete;
		BatchSubmitter& operator=(const BatchSubmitter&) = delete;
		BatchSubmitter(BatchSubmitter&&) = delete;
		BatchSubmitter& operator=(BatchSubmitter&&) = delete;
	};
}

#endif