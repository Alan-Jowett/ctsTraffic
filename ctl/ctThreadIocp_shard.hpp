/**
 * @file ctThreadIocp_shard.hpp
 * @brief IOCP-backed worker shard that implements `ctThreadIocp_base`.
 *
 * This class creates an IO completion port, associates a HANDLE or SOCKET
 * with it, and spawns worker threads that service completions with
 * `GetQueuedCompletionStatusEx`. Threads can be affinitized using
 * `GroupAffinity` entries supplied by the caller.
 *
 * @copyright Copyright (c) Microsoft Corporation
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABILITY OR NON-INFRINGEMENT.
 *
 * See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.
 */

#pragma once

#include <atomic>
#include <thread>
#include <vector>
#include <memory>

// reuse the callback info/type from the original header
#include "ctThreadIocp.hpp"
#include "ctThreadIocp_base.hpp"
#include "ctCpuAffinity.hpp"

#include <wil/resource.h>

/**
 * @file
 * @brief IOCP-backed worker shard that implements `ctThreadIocp_base`.
 *
 * This class creates an IO completion port, associates a HANDLE or SOCKET
 * with it, and spawns worker threads that service completions with
 * `GetQueuedCompletionStatusEx`. Threads can be affinitized using
 * `GroupAffinity` entries supplied by the caller.
 */

namespace ctl
{
/**
 * @brief IOCP-backed replacement for `ctThreadIocp` using std::thread workers.
 *
 * Behavior summary:
 * - Creates an IO Completion Port and associates the provided HANDLE/SOCKET.
 * - Spawns `numThreads` worker threads which call `GetQueuedCompletionStatusEx`.
 * - When an `OVERLAPPED*` (returned from `new_request`) completes, the stored
 *   callback is invoked on a worker thread and the callback info object is
 *   deleted.
 * - Optionally affinitizes threads by providing a list of `GroupAffinity`.
 *
 * @note The class inherits `ctThreadIocp_base` and implements its contract.
 */
class ctThreadIocp_shard : public ctThreadIocp_base
{
public:
	/**
	 * @brief Construct a shard and associate the provided HANDLE with the IOCP.
	 *
	 * @param _handle [in] Handle to associate with the IOCP (file/socket handle).
	 * @param numThreads [in] Number of worker threads to spawn. If zero the
	 *        implementation will choose a sensible default based on hardware.
	 * @param groupAffinities [in] Optional vector of `GroupAffinity` entries
	 *        used to affinitize threads. Each worker uses
	 *        `groupAffinities[threadIndex % groupAffinities.size()]` when
	 *        present.
	 * @param batchSize [in] Number of completions to dequeue in a single call
	 *        to `GetQueuedCompletionStatusEx`. Must be >= 1.
	 */
	explicit ctThreadIocp_shard(HANDLE _handle, size_t numThreads = 0, const std::vector<GroupAffinity>& groupAffinities = {}, size_t batchSize = 1)
		: m_shutdown(false), m_groupAffinities(groupAffinities), m_batchSize(batchSize ? batchSize : 1)
	{
		Init(_handle, numThreads);
	}

	/**
	 * @brief SOCKET overload forwards to the HANDLE constructor.
	 *
	 * @param _socket [in] Socket handle to associate with the IOCP.
	 * @param numThreads [in] Number of worker threads to spawn.
	 * @param groupAffinities [in] Optional affinity vector for workers.
	 * @param batchSize [in] Batch size for dequeuing completions.
	 */
	explicit ctThreadIocp_shard(SOCKET _socket, size_t numThreads = 0, const std::vector<GroupAffinity>& groupAffinities = {}, size_t batchSize = 1)
		: ctThreadIocp_shard(reinterpret_cast<HANDLE>(_socket), numThreads, groupAffinities, batchSize) // NOLINT(performance-no-int-to-ptr)
	{
	}

	/**
	 * @brief Destructor that shuts down worker threads and joins them.
	 */
	~ctThreadIocp_shard() noexcept override
	{
		ShutdownAndJoin();
	}

	ctThreadIocp_shard(const ctThreadIocp_shard&) = delete;
	ctThreadIocp_shard& operator=(const ctThreadIocp_shard&) = delete;
	ctThreadIocp_shard(ctThreadIocp_shard&&) = delete;
	ctThreadIocp_shard& operator=(ctThreadIocp_shard&&) = delete;

	/**
	 * @brief Allocate a callback info object and return the associated OVERLAPPED*.
	 *
	 * @param _callback [in] Callback invoked when the operation completes; the
	 *        callback receives the `OVERLAPPED*` originally returned by this
	 *        function.
	 * @return OVERLAPPED* [out] Pointer to an `OVERLAPPED` owned by the
	 *         callback info object. The caller must not delete this pointer.
	 */
	OVERLAPPED* new_request(std::function<void(OVERLAPPED*)> _callback) const override
	{
		auto* new_callback = new ctThreadIocpCallbackInfo(std::move(_callback));
		ZeroMemory(&new_callback->ov, sizeof OVERLAPPED);
		return &new_callback->ov;
	}

	/**
	 * @brief Notify that a previously allocated OVERLAPPED* will not be used.
	 *
	 * @param pOverlapped [in] The `OVERLAPPED*` returned from `new_request`
	 *        that will not be consumed due to a synchronous failure.
	 */
	void cancel_request(const OVERLAPPED* pOverlapped) const noexcept override
	{
		const auto* const old_request = reinterpret_cast<const ctThreadIocpCallbackInfo*>(pOverlapped);
		delete old_request;
	}

	/**
	 * @brief Post a custom completion packet to the IOCP (useful for testing).
	 *
	 * @param key [in] Completion key to post.
	 * @param bytes [in] Number of bytes to report with the completion.
	 * @param ov [in] Optional OVERLAPPED pointer to post.
	 * @return bool True on success, false on failure.
	 */
	bool post_completion(ULONG_PTR key = 0, DWORD bytes = 0, OVERLAPPED* ov = nullptr) const noexcept
	{
		return !!PostQueuedCompletionStatus(m_iocp.get(), bytes, key, ov);
	}

private:
	wil::unique_handle m_iocp;
	std::vector<std::thread> m_workers;
	std::atomic<bool> m_shutdown;
	std::vector<GroupAffinity> m_groupAffinities;
	const size_t m_batchSize;

	/**
	 * @brief Initialize the IOCP and spawn worker threads.
	 *
	 * @param _handle [in] Handle to associate with the IOCP.
	 * @param numThreads [in] Number of worker threads to create.
	 */
	void Init(HANDLE _handle, size_t numThreads)
	{
		// Create the IOCP into a local RAII handle first so it will be closed
		// automatically if association or thread creation fails.
		wil::unique_handle iocp(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0));
		THROW_LAST_ERROR_IF_NULL(iocp.get());

		// associate the user handle/socket with our IOCP
		const auto* const assoc = CreateIoCompletionPort(_handle, iocp.get(), 0, 0);
		THROW_LAST_ERROR_IF_NULL(assoc);

		m_iocp = std::move(iocp);

		if (numThreads == 0)
		{
			numThreads = std::max<size_t>(1, std::thread::hardware_concurrency());
		}
		m_workers.reserve(numThreads);
		for (size_t threadCount = 0; threadCount < numThreads; ++threadCount)
		{
			m_workers.emplace_back(&ctThreadIocp_shard::WorkerLoop, this, threadCount);
		}
	}

	/**
	 * @brief Worker loop executed by each spawned thread.
	 *
	 * The worker dequeues completions using `GetQueuedCompletionStatusEx` and
	 * invokes callbacks for completed `OVERLAPPED*` entries.
	 *
	 * @param index [in] Zero-based index of the worker thread.
	 */
	void WorkerLoop(size_t index) const noexcept
	{
		// if group affinities were provided, use SetThreadGroupAffinity which respects groups
		if (!m_groupAffinities.empty())
		{
			const GroupAffinity& ga = m_groupAffinities[index % m_groupAffinities.size()];
			GROUP_AFFINITY gaff{};
			gaff.Group = ga.Group;
			gaff.Mask = ga.Mask;
			// ignore return value; affinity is best-effort
			SetThreadGroupAffinity(GetCurrentThread(), &gaff, nullptr);
			// TODO: leaving in the printf for now until I find a better way to
			// communicate how threads are being created and affinitized
			wprintf(L"Worker thread %lu : index %zu set to Group %u, Mask 0x%llx\n",
				GetCurrentThreadId(),
				index,
				static_cast<unsigned>(gaff.Group),
				static_cast<unsigned long long>(gaff.Mask));
		}

		try
		{
			std::vector<OVERLAPPED_ENTRY> entries(m_batchSize);

			while (!m_shutdown.load(std::memory_order_acquire))
			{
				ULONG numRemoved = 0;
				const BOOL ok = GetQueuedCompletionStatusEx(m_iocp.get(), entries.data(), static_cast<ULONG>(entries.size()), &numRemoved, INFINITE, FALSE);
				LOG_LAST_ERROR_IF(!ok);

				for (ULONG i = 0; i < numRemoved; ++i)
				{
					const OVERLAPPED_ENTRY& e = entries[i];
					if (e.lpOverlapped)
					{
						const auto* request = reinterpret_cast<ctThreadIocpCallbackInfo*>(e.lpOverlapped);
						request->callback(e.lpOverlapped);
						delete request;
					}
					else
					{
						if (m_shutdown.load(std::memory_order_acquire))
						{
							break;
						}
					}
				}
			}
		}
		catch (...)
		{
			LOG_CAUGHT_EXCEPTION();
		}

	}

	/**
	 * @brief Signal worker threads to exit and join them.
	 */
	void ShutdownAndJoin() noexcept
	{
		bool expected = false;
		if (!m_shutdown.compare_exchange_strong(expected, true))
		{
			// already shutting down
			return;
		}

		// wake up all workers by posting null OVERLAPPEDs
		for (size_t i = 0; i < m_workers.size(); ++i)
		{
			PostQueuedCompletionStatus(m_iocp.get(), 0, 0, nullptr);
		}

		for (auto& t : m_workers)
		{
			if (t.joinable())
			{
				t.join();
			}
		}

		// m_iocp will be closed automatically by wil::unique_handle's destructor
	}
};

} // namespace ctl
