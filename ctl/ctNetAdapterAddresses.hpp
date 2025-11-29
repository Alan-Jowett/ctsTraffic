/**
 * @file ctNetAdapterAddresses.hpp
 * @brief Helper to enumerate and query network adapter addresses.
 *
 * Provides a lightweight wrapper around GetAdaptersAddresses to produce
 * an STL-style iterable container of `IP_ADAPTER_ADDRESSES` records. Also
 * includes a predicate functor to locate an adapter by a `ctSockaddr`.
 
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

// ReSharper disable CppInconsistentNaming
#pragma once

// cpp headers
#include <stdexcept>
#include <utility>
#include <vector>
#include <memory>
// os headers
// ReSharper disable once CppUnusedIncludeDirective
#include <WinSock2.h>
// ReSharper disable once CppUnusedIncludeDirective
#include <ws2ipdef.h>
#include <iphlpapi.h>
// ctl headers
#include "ctSockaddr.hpp"
// wil headers always included last
#include <wil/resource.h>

namespace ctl
{
class ctNetAdapterAddresses
{
public:
    /**
     * @brief Iterator type over the `IP_ADAPTER_ADDRESSES` linked list.
     *
     * The iterator wraps a shared buffer owning the GetAdaptersAddresses
     * output and provides STL-compatible forward-iterator semantics.
     */
    class iterator
    {
    public:
        iterator() = default;

        /**
         * @brief Construct an iterator that will enumerate the buffer.
         * @param[in] ipAdapter Shared buffer returned from GetAdaptersAddresses.
         */
        explicit iterator(std::shared_ptr<std::vector<BYTE>> ipAdapter) noexcept :
            m_buffer(std::move(ipAdapter))
        {
            if (m_buffer && !m_buffer->empty())
            {
                m_current = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(&m_buffer->at(0));
            }
        }

        /**
         * @brief Swap two iterators.
         * @param[in,out] rhs Iterator to swap with.
         */
        void swap(_Inout_ iterator& rhs) noexcept
        {
            using std::swap;
            swap(m_buffer, rhs.m_buffer);
            swap(m_current, rhs.m_current);
        }

        /**
         * @brief Dereference the iterator to access the current adapter entry.
         * @return Reference to the current `IP_ADAPTER_ADDRESSES` entry.
         * @throws std::out_of_range if the iterator is at `end()`.
         */
        IP_ADAPTER_ADDRESSES& operator*() const
        {
            if (!m_current)
            {
                throw std::out_of_range("out_of_range: ctNetAdapterAddresses::iterator::operator*");
            }
            return *m_current;
        }

        /**
         * @brief Access members of the current adapter entry.
         * @return Pointer to the current `IP_ADAPTER_ADDRESSES` entry.
         * @throws std::out_of_range if the iterator is at `end()`.
         */
        IP_ADAPTER_ADDRESSES* operator->() const
        {
            if (!m_current)
            {
                throw std::out_of_range("out_of_range: ctNetAdapterAddresses::iterator::operator->");
            }
            return m_current;
        }

        /**
         * @brief Compare two iterators for equality.
         * @param[in] iter The other iterator to compare with.
         * @return True if both iterators point to the same position.
         */
        bool operator==(const iterator& iter) const noexcept
        {
            // for comparison of 'end' iterators, just look at current
            if (!m_current)
            {
                return m_current == iter.m_current;
            }

            return m_buffer == iter.m_buffer &&
                   m_current == iter.m_current;
        }

        /**
         * @brief Compare two iterators for inequality.
         * @param[in] iter The other iterator to compare with.
         * @return True when the iterators do not point to the same position.
         */
        bool operator!=(const iterator& iter) const noexcept
        {
            return !(*this == iter);
        }

        /**
         * @brief Pre-increment the iterator to the next adapter entry.
         * @return Reference to the incremented iterator.
         * @throws std::out_of_range if incrementing past the end.
         */
        iterator& operator++()
        {
            if (!m_current)
            {
                throw std::out_of_range("out_of_range: ctNetAdapterAddresses::iterator::operator++");
            }
            // increment
            m_current = m_current->Next;
            return *this;
        }

        /**
         * @brief Advance the iterator by one and return the previous value.
         * @return Iterator state prior to increment.
         */
        /**
         * @brief Post-increment the iterator (advance but return prior state).
         * @return Iterator state prior to increment.
         */
        iterator operator++(int)
        {
            auto tempIterator(*this);
            ++*this;
            return tempIterator;
        }

        /**
         * @brief Advance the iterator by `inc` positions.
         * @param[in] inc Number of positions to advance.
         * @return Reference to the advanced iterator.
         * @throws std::out_of_range if the iterator advances past the end.
         */
        iterator& operator+=(uint32_t inc)
        {
            for (unsigned loop = 0; loop < inc && m_current != nullptr; ++loop)
            {
                m_current = m_current->Next;
            }
            if (!m_current)
            {
                throw std::out_of_range("out_of_range: ctNetAdapterAddresses::iterator::operator+=");
            }
            return *this;
        }

        // iterator_traits
        // - allows <algorithm> functions to be used
        using iterator_category = std::forward_iterator_tag;
        using value_type = IP_ADAPTER_ADDRESSES;
        using difference_type = int;
        using pointer = IP_ADAPTER_ADDRESSES*;
        using reference = IP_ADAPTER_ADDRESSES&;

    private:
        /**
         * @brief Shared ownership of the raw buffer returned by GetAdaptersAddresses.
         *
         * The buffer contains a linked list of `IP_ADAPTER_ADDRESSES` structures.
         */
        std::shared_ptr<std::vector<BYTE>> m_buffer{};

        /**
         * @brief Pointer to the current adapter address entry within `m_buffer`.
         */
        PIP_ADAPTER_ADDRESSES m_current = nullptr;
    };

    /**
     * @brief Construct and immediately refresh adapter addresses.
     * @param[in] family Address family filter (AF_UNSPEC/AF_INET/AF_INET6).
     * @param[in] gaaFlags Flags forwarded to GetAdaptersAddresses (GAA_FLAG_*).
     */
    explicit ctNetAdapterAddresses(unsigned family = AF_UNSPEC, DWORD gaaFlags = 0) :
        m_buffer(std::make_shared<std::vector<BYTE>>(16384))
    {
        this->refresh(family, gaaFlags);
    }

    /**
     * @brief Refresh the internal adapter list from the system.
     * @param[in] family Address family to request (AF_UNSPEC/AF_INET/AF_INET6).
     * @param[in] gaaFlags Flags forwarded to GetAdaptersAddresses.
     *
     * This call replaces the internal buffer; any existing iterators
     * referring to this instance will be invalidated.
     *
     * Exception guarantees: basic guarantee — on failure an exception is thrown
     * and prior internal information is lost.
     */
    void refresh(unsigned family = AF_UNSPEC, DWORD gaaFlags = 0) const
    {
        // get both v4 and v6 adapter info
        auto byteSize = static_cast<ULONG>(m_buffer->size());
        auto err = GetAdaptersAddresses(
            family, // Family
            gaaFlags, // Flags
            nullptr, // Reserved
            reinterpret_cast<PIP_ADAPTER_ADDRESSES>(&m_buffer->at(0)),
            &byteSize
        );
        if (err == ERROR_BUFFER_OVERFLOW)
        {
            m_buffer->resize(byteSize);
            err = GetAdaptersAddresses(
                family, // Family
                gaaFlags, // Flags
                nullptr, // Reserved
                reinterpret_cast<PIP_ADAPTER_ADDRESSES>(&m_buffer->at(0)),
                &byteSize
            );
        }
        if (err != NO_ERROR)
        {
            THROW_WIN32_MSG(err, "GetAdaptersAddresses");
        }
    }

    /**
     * @brief Begin iterator for the adapter list.
     * @return iterator pointing at the first adapter, or `end()` if none.
     */
    [[nodiscard]] iterator begin() const noexcept
    {
        return iterator(m_buffer);
    }

    /**
     * @brief End iterator for the adapter list.
     * @return Default-constructed iterator representing end.
     */
    // ReSharper disable once CppMemberFunctionMayBeStatic
    [[nodiscard]] iterator end() const noexcept // NOLINT(readability-convert-member-functions-to-static)
    {
        return {};
    }

private:
    /**
     * @brief Owning buffer that contains the adapter addresses list returned
     * by GetAdaptersAddresses.
     *
     * Stored as a shared_ptr so iterators may keep a reference while
     * allowing the ctNetAdapterAddresses instance to update/replace it.
     */
    std::shared_ptr<std::vector<BYTE>> m_buffer{};
};

/**
 * @brief Predicate functor to match a network adapter by a specific address.
 *
 * Designed to be used with STL algorithms and `ctNetAdapterAddresses::iterator`.
 */
struct ctNetAdapterMatchingAddrPredicate
{
    /**
     * @brief Construct the predicate with a target address.
     * @param[in] addr The `ctSockaddr` to match against adapter unicast addresses.
     */
    explicit ctNetAdapterMatchingAddrPredicate(ctSockaddr addr) noexcept :
        m_targetAddr(std::move(addr))
    {
    }

    /**
     * @brief Test whether the supplied adapter contains the target address.
     * @param[in] ipAddress The adapter record to inspect.
     * @return True if the adapter has a unicast address equal to the target.
     */
    bool operator ()(const IP_ADAPTER_ADDRESSES& ipAddress) const noexcept
    {
        for (const auto* unicastAddress = ipAddress.FirstUnicastAddress;
             unicastAddress != nullptr;
             unicastAddress = unicastAddress->Next)
        {
            if (ctSockaddr(&unicastAddress->Address) == m_targetAddr)
            {
                return true;
            }
        }
        return false;
    }

private:
    /**
     * @brief The target address to match against adapter unicast addresses.
     */
    const ctSockaddr m_targetAddr;
};
} // namespace ctl
