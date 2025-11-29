/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#pragma once

/**
 * @file ctsIOPatternBufferPolicy.hpp
 * @brief Buffer allocation and verification policy templates for IO patterns.
 *
 * This header defines the `ctsIOPatternBufferPolicy` template and a set of
 * specializations for different allocation and buffer types (static vs dynamic,
 * heap vs registered I/O). The policies provide helpers to obtain a buffer of
 * the requested size and to verify buffers when applicable.
 */

namespace ctsTraffic
{
/**
 * @brief Tag type for static allocation policy.
 */
using ctsIOPatternAllocationTypeStatic = struct ctsIOPatternAllocationTypeStatic_t;

/**
 * @brief Tag type for dynamic allocation policy.
 */
using ctsIOPatternAllocationtypeDynamic = struct ctsIOPatternAllocationtypeDynamic_t;

/**
 * @brief Tag type for heap-based buffers.
 */
using ctsIOPatternBufferTypeHeap = struct ctsIOPatternBufferTypeHeap_t;

/**
 * @brief Tag type for Registered I/O (RIO) buffers.
 */
using ctsIOPatternBufferTypeRegisteredIo = struct ctsIOPatternBufferTypeRegisteredIo_t;


template <typename AllocationType, typename BufferType>
class ctsIOPatternBufferPolicy
{
public:
    /**
     * @brief Acquire a buffer for use by an IO operation.
     *
     * @param size [in] The number of bytes requested for the buffer.
     * @return Pointer to the allocated buffer, or nullptr on failure.
     */
    char* get_buffer(size_t size) noexcept;

    /**
     * @brief Verify that a buffer is valid for use with this policy.
     *
     * @param buffer [in] Pointer to the buffer to verify.
     * @return `true` if the buffer is valid, `false` otherwise.
     */
    bool verify_buffer(const char* buffer) noexcept;
};


//
// Static heap buffers
// - won't be verified
//
template <>
class ctsIOPatternBufferPolicy<ctsIOPatternAllocationTypeStatic, ctsIOPatternBufferTypeHeap>
{
    /**
     * @brief Policy specialization for static heap buffers.
     *
     * Static heap buffers are pre-allocated and therefore typically do not
     * require verification.
     */
};

//
// Static RIO buffers
// - won't be verified
//
template <>
class ctsIOPatternBufferPolicy<ctsIOPatternAllocationTypeStatic, ctsIOPatternBufferTypeRegisteredIo>
{
    /**
     * @brief Policy specialization for static Registered I/O (RIO) buffers.
     */
};

//
// Dynamic heap buffers
// - won't be verified
//
template <>
class ctsIOPatternBufferPolicy<ctsIOPatternAllocationtypeDynamic, ctsIOPatternBufferTypeHeap>
{
    /**
     * @brief Policy specialization for dynamic heap buffers.
     *
     * Dynamic heap buffers are allocated on demand and may require different
     * lifetime semantics compared to static buffers.
     */
};

//
// Static RIO buffers
// - won't be verified
//
template <>
class ctsIOPatternBufferPolicy<ctsIOPatternAllocationtypeDynamic, ctsIOPatternBufferTypeRegisteredIo>
{
    /**
     * @brief Policy specialization for dynamic Registered I/O (RIO) buffers.
     */
};
}
