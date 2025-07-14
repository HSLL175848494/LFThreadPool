#ifndef HSLL_TBLOCKFREEQUEUE
#define HSLL_TBLOCKFREEQUEUE

#include "lightweightsemaphore.h"
#include <algorithm>

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

#if defined(_WIN32)
#include <malloc.h>
#define ALIGNED_MALLOC(size, align) _aligned_malloc(size, align)
#define ALIGNED_FREE(ptr) _aligned_free(ptr)
#else
#define ALIGNED_MALLOC(size, align) aligned_alloc(align, (size + align - 1) & ~(align - 1))
#define ALIGNED_FREE(ptr) free(ptr)
#endif


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
						std::memory_order_release, std::memory_order_relaxed
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

		unsigned long long tryLockHeadBulk(unsigned int& count)//pop
		{
			unsigned long long current_head;

			while (count)
			{
				if (tryLockHead(current_head, count))
					return current_head;

				count >>= 1;
			}

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
						std::memory_order_release, std::memory_order_relaxed
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

		unsigned long long tryLockTailBulk(unsigned int& count)//push
		{
			unsigned long long current_Tail;

			while (count)
			{
				if (tryLockTail(current_Tail, count))
					return current_Tail;

				count >>= 1;
			}

			return 0;
		}

		void waitReady(std::atomic<unsigned long long>& sequence, unsigned long long required)
		{
			while (sequence.load(std::memory_order_acquire) != required)
				std::this_thread::yield();
		}

	public:

		TPLFQueue() : buffer(nullptr) {}

		bool init(unsigned int capacity)
		{
			if (buffer || capacity < 2)
				return false;

			this->capacity = capacity;
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
			unsigned int num = count;
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
			assert(part1 && count1);

			if (count2 == 0 || part2 == nullptr)
				return pushBulk<METHOD>(part1, count1);

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
			unsigned int num = count;
			unsigned long long current_head = tryLockHeadBulk(num);

			if (UNLIKELY(!num))
				return 0;

			for (unsigned int i = 0; i < num; i++)
			{
				unsigned long long index = current_head + i;
				unsigned int slot_idx = index % capacity;
				Slot& slot = buffer[slot_idx];

				if (LIKELY(i != num - 1))
					waitReady(slot.sequence, index + 1);

				TYPE* item = (TYPE*)slot.storage;
				new (elements + i) TYPE(std::move(*item));
				item->~TYPE();
				slot.sequence.store(index + capacity, std::memory_order_release);
			}

			if (LIKELY(num < count))
				return num + popBulk(elements + num, count - num);

			return count;
		}

		unsigned int get_size()
		{
			assert(buffer);
			long long h = (long long)head.load(std::memory_order_acquire);
			long long t = (long long)tail.load(std::memory_order_acquire);
			return std::min(capacity, (unsigned int)(t - h));
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

#endif // HSLL_TBLOCKFREEQUEUEK