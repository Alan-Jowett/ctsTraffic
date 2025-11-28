#pragma once
/**
 * @file
 * @brief A small thread-pool backed queue helper that can schedule work and
 *        provide waitable results for call sites that need a result.
 *
 * This header exposes `ctThreadpoolQueue` which wraps a Windows thread pool
 * and provides an ordered queue of work items. Work items can be simple
 * fire-and-forget `std::function<void()>` objects or `ctThreadpoolQueueWaitableResult`
 * objects that allow the submitter to wait for completion and read a result.
 */

// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include <deque>
#include <functional>
#include <memory>
#include <variant>
#include <Windows.h>

#include <wil/resource.h>

namespace ctl
{
	/**
	 * @brief Policy that controls how the queue submits work to the threadpool.
	 *
	 * - `Growable`: always submit work to the threadpool (the pool may create
	 *   threads to handle load).
	 * - `Flat`: only submit to the threadpool when the queue transitions from
	 *   empty to non-empty (keeps work on the single worker thread when possible).
	 */
	enum class ctThreadpoolGrowthPolicy : std::uint8_t
	{
		Growable,
		Flat
	};

	template <ctThreadpoolGrowthPolicy GrowthPolicy>
	class ctThreadpoolQueue;

	/**
	 * @brief Abstract base used for waitable work items.
	 *
	 * Implementations expose `run()` which will be called on the threadpool
	 * worker when the work is scheduled, and `abort()` which is invoked when
	 * the queue is being canceled and the work will not be executed.
	 */
	class ctThreadpoolQueueWaitableResultInterface
	{
	public:
		ctThreadpoolQueueWaitableResultInterface() = default;
		virtual ~ctThreadpoolQueueWaitableResultInterface() = default;

		ctThreadpoolQueueWaitableResultInterface(ctThreadpoolQueueWaitableResultInterface&&) noexcept = delete;
		ctThreadpoolQueueWaitableResultInterface& operator=(ctThreadpoolQueueWaitableResultInterface&&) noexcept = delete;
		ctThreadpoolQueueWaitableResultInterface(const ctThreadpoolQueueWaitableResultInterface&) = delete;
		ctThreadpoolQueueWaitableResultInterface& operator=(const ctThreadpoolQueueWaitableResultInterface&) = delete;

	private:
		// limit who can run() and abort()
		template <ctThreadpoolGrowthPolicy GrowthPolicy>
		friend class ctThreadpoolQueue;

		/**
		 * @brief Execute the encapsulated work. Called by the managed threadpool thread.
		 */
		virtual void run() noexcept = 0;

		/**
		 * @brief Cancel the pending work and wake any waiters.
		 */
		virtual void abort() noexcept = 0;
	};

	/**
	 * @brief A waitable wrapper around a functor that produces a result.
	 *
	 * The submitter receives a `std::shared_ptr<ctThreadpoolQueueWaitableResult<TReturn>>`
	 * which can be used to wait for completion and read the resulting value.
	 */
	template <typename TReturn>
	class ctThreadpoolQueueWaitableResult final : public ctThreadpoolQueueWaitableResultInterface
	{
	public:
		/**
		 * @brief Construct wrapper from a callable that returns `TReturn`.
		 *
		 * @tparam FunctorType Callable type.
		 * @param functor [in] Callable to invoke on the TP thread.
		 * @throws wil::ResultException or `std::bad_alloc` if allocation fails.
		 */
		template <typename FunctorType>
		explicit ctThreadpoolQueueWaitableResult(FunctorType&& functor) : m_function(std::forward<FunctorType>(functor)) {}

		~ctThreadpoolQueueWaitableResult() override = default;

		// returns ERROR_SUCCESS if the callback ran to completion
		// returns ERROR_TIMEOUT if this wait timed out
		// - this can be called multiple times if needing to probe
		// any other error code resulted from attempting to run the callback
		// - meaning it did *not* run to completion
		/**
		 * @brief Wait for the work to complete.
		 *
		 * @param timeout [in] Timeout in milliseconds.
		 * @return DWORD Win32 error code. `ERROR_SUCCESS` on success, `ERROR_TIMEOUT` if timed out.
		 */
		DWORD wait(DWORD timeout) const noexcept
		{
			if (!m_completionSignal.wait(timeout))
			{
				// not setting m_internalError to timeout
				// since the caller is allowed to try to wait() again later
				return ERROR_TIMEOUT;
			}
			const auto lock = m_lock.lock();
			return m_internalError;
		}

		// waitable event handle, signaled when the callback has run to completion (or failed)
		/**
		 * @brief Get the event handle that will be signaled when the work completes.
		 *
		 * @return HANDLE Waitable event.
		 */
		HANDLE notification_event() const noexcept
		{
			return m_completionSignal.get();
		}

		/**
		 * @brief Read the result of the callable after successful completion.
		 *
		 * @return const TReturn& Reference to the stored result.
		 */
		const TReturn& read_result() const noexcept
		{
			return m_result;
		}

		// move the result out of the object for move-only types
		/**
		 * @brief Move the result out for move-only result types.
		 *
		 * @return TReturn Moved result.
		 */
		TReturn move_result() noexcept
		{
			TReturn moveOut(std::move(m_result));
			return moveOut;
		}

		// non-copyable
		ctThreadpoolQueueWaitableResult(const ctThreadpoolQueueWaitableResult&) = delete;
		ctThreadpoolQueueWaitableResult& operator=(const ctThreadpoolQueueWaitableResult&) = delete;
		ctThreadpoolQueueWaitableResult(ctThreadpoolQueueWaitableResult&&) noexcept = delete;
		ctThreadpoolQueueWaitableResult& operator=(ctThreadpoolQueueWaitableResult&&) noexcept = delete;

	private:
		void run() noexcept override
		{
			// we are now running in the TP callback
			{
				const auto lock = m_lock.lock();
				if (m_runStatus != RunStatus::NotYetRun)
				{
					// return early - the caller has already canceled this
					return;
				}
				m_runStatus = RunStatus::Running;
			}

			DWORD error = NO_ERROR;
			try
			{
				m_result = std::move(m_function());
			}
			catch (...)
			{
				const HRESULT hr = wil::ResultFromCaughtException();
				// HRESULT_TO_WIN32
				error = HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : hr;
			}

			const auto lock = m_lock.lock();
			WI_ASSERT(m_runStatus == RunStatus::Running);
			m_runStatus = RunStatus::RanToCompletion;
			m_internalError = error;
			m_completionSignal.SetEvent();
		}

		void abort() noexcept override
		{
			const auto lock = m_lock.lock();
			// only override the error if we know we haven't started running their functor
			if (m_runStatus == RunStatus::NotYetRun)
			{
				m_runStatus = RunStatus::Canceled;
				m_internalError = ERROR_CANCELLED;
				m_completionSignal.SetEvent();
			}
		}

		std::function<TReturn()> m_function;
		wil::unique_event m_completionSignal{ wil::EventOptions::ManualReset };
		mutable wil::critical_section m_lock{ 200 };
		TReturn m_result{};
		DWORD m_internalError = NO_ERROR;

		enum class RunStatus : std::uint8_t
		{
			NotYetRun,
			Running,
			RanToCompletion,
			Canceled
		} m_runStatus{ RunStatus::NotYetRun };
	};


	/**
	 * @brief A simple ordered work queue backed by a Windows threadpool.
	 *
	 * @tparam GrowthPolicy Controls whether the queue is `Growable` or `Flat`.
	 * - Growable: submits each work item to the TP immediately.
	 * - Flat: attempts to keep work on a single TP thread and only submits when
	 *   the queue transitions from empty to non-empty.
	 */
	template <ctThreadpoolGrowthPolicy GrowthPolicy>
	class ctThreadpoolQueue
	{
	public:
		/**
		 * @brief Construct a `ctThreadpoolQueue`.
		 *
		 * Creates an internal threadpool environment and a single worker.
		 */
		explicit ctThreadpoolQueue() : m_tpEnvironment(0, 1)
		{
			// create a single-threaded threadpool
			m_tpHandle = m_tpEnvironment.create_tp(WorkCallback, this);
		}

		/**
		 * @brief Submit a callable to the queue and receive a waitable result.
		 *
		 * @tparam TReturn Result type returned by the callable.
		 * @tparam FunctorType Callable type.
		 * @param functor [in] Callable to schedule.
		 * @return std::shared_ptr<ctThreadpoolQueueWaitableResult<TReturn>> Shared pointer
		 *         to a waitable result wrapper; `nullptr` if submission failed.
		 */
		template <typename TReturn, typename FunctorType>
		std::shared_ptr<ctThreadpoolQueueWaitableResult<TReturn>> submit_with_results(FunctorType&& functor) noexcept try
		{
			FAIL_FAST_IF(m_tpHandle.get() == nullptr);

			std::shared_ptr<ctThreadpoolQueueWaitableResult<TReturn>> returnResult{ std::make_shared<ctThreadpoolQueueWaitableResult<TReturn>>(std::forward<FunctorType>(functor)) };
			auto shouldSubmit = false;

			// scope to the queue lock
			{
				const auto queueLock = m_lock.lock();
				shouldSubmit = ShouldSubmitThreadpoolWork();
				m_workItems.emplace_back(returnResult);
			}

			if (shouldSubmit)
			{
				SubmitThreadpoolWork(m_tpHandle.get());
			}
			return returnResult;
		}
		catch (...)
		{
			LOG_CAUGHT_EXCEPTION();
			return nullptr;
		}

		/**
		 * @brief Submit a fire-and-forget callable to the queue.
		 *
		 * @tparam FunctorType Callable type convertible to `std::function<void()>`.
		 * @param functor [in] Callable to schedule.
		 */
		template <typename FunctorType>
		void submit(FunctorType&& functor) noexcept try
		{
			FAIL_FAST_IF(m_tpHandle.get() == nullptr);

			auto shouldSubmit = false;

			// scope to the queue lock
			{
				const auto queueLock = m_lock.lock();
				shouldSubmit = ShouldSubmitThreadpoolWork();
				m_workItems.emplace_back(std::forward<SimpleFunctionT>(functor));
			}

			if (shouldSubmit)
			{
				SubmitThreadpoolWork(m_tpHandle.get());
			}
		}
		CATCH_LOG()

			// functors must return type HRESULT
			/**
			 * @brief Submit a callable and wait for it to complete. Callable must
			 *        return an `HRESULT`.
			 *
			 * @tparam FunctorType Callable type.
			 * @param functor [in] Callable to run synchronously on the TP worker.
			 * @return HRESULT Result of the callable or an error code.
			 */
			template <typename FunctorType>
			HRESULT submit_and_wait(FunctorType&& functor) noexcept try
		{
			if constexpr (GrowthPolicy == ctThreadpoolGrowthPolicy::Growable)
			{
				HRESULT hr = HRESULT_FROM_WIN32(ERROR_OUTOFMEMORY);
				if (const auto waitableResult = submit_with_results<HRESULT>(std::forward<FunctorType>(functor)))
				{
					hr = HRESULT_FROM_WIN32(waitableResult->wait(INFINITE));
					if (SUCCEEDED(hr))
					{
						hr = waitableResult->read_result();
					}
				}
				return hr;
			}

			// this is not applicable for flat queues
			FAIL_FAST_MSG("submit_and_wait only supported with Growable queues");
		}
		CATCH_RETURN()

			// cancels anything queued to the TP - this ctThreadpoolQueue instance can no longer be used
			/**
			 * @brief Cancel all queued work and shutdown the internal threadpool.
			 *
			 * After calling `cancel()` the `ctThreadpoolQueue` instance may no
			 * longer be used.
			 */
			void cancel() noexcept try
		{
			if (m_tpHandle)
			{
				// immediately release anyone waiting for these work-items not yet run
				{
					const auto queueLock = m_lock.lock();

					for (const auto& work : m_workItems)
					{
						// signal that these are canceled before we shut down the TP which they could be scheduled
						if (const auto* pWaitableWorkItem = std::get_if<WaitableFunctionT>(&work))
						{
							(*pWaitableWorkItem)->abort();
						}
					}

					m_workItems.clear();
				}
			}

			// force the m_tpHandle to wait and close the TP
			m_tpHandle.reset();
			m_tpEnvironment.reset();
		}
		CATCH_LOG()

			/**
			 * @brief Determine if the current thread is the internal threadpool worker.
			 *
			 * @return bool `true` if running on the queue worker thread.
			 */
			bool IsRunningInQueue() const noexcept
		{
			const auto currentThreadId = GetThreadId(GetCurrentThread());
			return currentThreadId == static_cast<DWORD>(InterlockedCompareExchange64(&m_threadpoolThreadId, 0ll, 0ll));
		}

		/**
		 * @brief Destructor: cancels queued work and releases the threadpool.
		 */
		~ctThreadpoolQueue() noexcept
		{
			cancel();
		}

		ctThreadpoolQueue(const ctThreadpoolQueue&) = delete;
		ctThreadpoolQueue& operator=(const ctThreadpoolQueue&) = delete;
		ctThreadpoolQueue(ctThreadpoolQueue&&) = delete;
		ctThreadpoolQueue& operator=(ctThreadpoolQueue&&) = delete;

	private:
		struct TpEnvironment
		{
			using unique_tp_pool = wil::unique_any<PTP_POOL, decltype(&CloseThreadpool), CloseThreadpool>;
			unique_tp_pool m_threadPool;

			using unique_tp_env = wil::unique_struct<TP_CALLBACK_ENVIRON, decltype(&DestroyThreadpoolEnvironment), DestroyThreadpoolEnvironment>;
			unique_tp_env m_tpEnvironment;

			TpEnvironment(DWORD countMinThread, DWORD countMaxThread)
			{
				InitializeThreadpoolEnvironment(&m_tpEnvironment);

				m_threadPool.reset(CreateThreadpool(nullptr));
				THROW_LAST_ERROR_IF_NULL(m_threadPool.get());

				// Set min and max thread counts for custom thread pool
				THROW_LAST_ERROR_IF(!SetThreadpoolThreadMinimum(m_threadPool.get(), countMinThread));
				SetThreadpoolThreadMaximum(m_threadPool.get(), countMaxThread);
				SetThreadpoolCallbackPool(&m_tpEnvironment, m_threadPool.get());
			}

			wil::unique_threadpool_work create_tp(PTP_WORK_CALLBACK callback, void* pv)
			{
				wil::unique_threadpool_work newThreadpool(CreateThreadpoolWork(callback, pv, m_threadPool ? &m_tpEnvironment : nullptr));
				THROW_LAST_ERROR_IF_NULL(newThreadpool.get());
				return newThreadpool;
			}

			void reset()
			{
				m_threadPool.reset();
				m_tpEnvironment.reset();
			}
		};

		using SimpleFunctionT = std::function<void()>;
		using WaitableFunctionT = std::shared_ptr<ctThreadpoolQueueWaitableResultInterface>;
		using FunctionVariantT = std::variant<SimpleFunctionT, WaitableFunctionT>;

		// the lock must be destroyed *after* the TP object (thus must be declared first)
		// since the lock is used in the TP callback
		// the lock is mutable to allow us to acquire the lock in const methods
		mutable wil::critical_section m_lock{ 200 };
		TpEnvironment m_tpEnvironment;
		wil::unique_threadpool_work m_tpHandle;
		std::deque<FunctionVariantT> m_workItems;
		mutable LONG64 m_threadpoolThreadId{ 0 }; // useful for callers to assert they are running within the queue

		// ReSharper disable once CppNotAllPathsReturnValue
		bool ShouldSubmitThreadpoolWork() noexcept
		{
			if constexpr (GrowthPolicy == ctThreadpoolGrowthPolicy::Flat)
			{
				// return true to call SubmitThreadpoolWork if it's empty
				// else we already called SubmitThreadpoolWork for existing the item in the queue (that we're about to erase)
				const auto returnValue = m_workItems.empty();
				m_workItems.clear();
				return returnValue;
			}

			if constexpr (GrowthPolicy == ctThreadpoolGrowthPolicy::Growable)
			{
				return true;
			}
		}

		static void CALLBACK WorkCallback(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept try
		{
			auto* pThis = static_cast<ctThreadpoolQueue*>(context);

			FunctionVariantT work;
			{
				const auto queueLock = pThis->m_lock.lock();

				if (pThis->m_workItems.empty())
				{
					// pThis object is being destroyed and the queue was cleared
					return;
				}

				std::swap(work, pThis->m_workItems.front());
				pThis->m_workItems.pop_front();

				InterlockedExchange64(&pThis->m_threadpoolThreadId, GetThreadId(GetCurrentThread()));
			}

			// run the tasks outside the ctThreadpoolQueue lock
			const auto resetThreadIdOnExit = wil::scope_exit([pThis] { InterlockedExchange64(&pThis->m_threadpoolThreadId, 0ll); });
			if (work.index() == 0)
			{
				const auto& workItem = std::get<SimpleFunctionT>(work);
				workItem();
			}
			else
			{
				const auto& waitableWorkItem = std::get<WaitableFunctionT>(work);
				waitableWorkItem->run();
			}
		}
		CATCH_LOG()
	};
} // namespace
