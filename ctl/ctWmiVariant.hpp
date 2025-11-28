// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <vector>
#include <string>

#include <Windows.h>
#include <objbase.h>

#include <wil/stl.h>
#include <wil/com.h>
#include <wil/resource.h>

/**
 * @file ctWmiVariant.hpp
 * @brief Helpers to create and read VARIANTs for use with WMI.
 *
 * The `ctWmiMakeVariant` and `ctWmiReadFromVariant` family of functions
 * provide type-safe construction and extraction helpers for `VARIANT` values
 * passed to and received from WMI APIs. These helpers apply the WMI marshalling
 * conventions (for example representing 64-bit integers as BSTRs) and perform
 * validation to ensure correct VARIANT types are used.
 */

// ctWmiMakeVariant(const ) functions are specializations designed to help callers
// who want a way to construct a VARIANT that is safe for passing into WMI
// since WMI has limitations on what VARIANT types it accepts
namespace ctl
{
/**
 * @brief Test whether a VARIANT is empty or null.
 * @param[in] variant Pointer to the VARIANT to test.
 * @return True when the VARIANT type is `VT_EMPTY` or `VT_NULL`.
 */
inline bool IsVariantEmptyOrNull(_In_ const VARIANT* variant) noexcept
{
    return V_VT(variant) == VT_EMPTY || V_VT(variant) == VT_NULL;
}

/**
 * @brief Construct a VARIANT representing a boolean value suitable for WMI.
 * @param[in] value Boolean value to store.
 * @return A `wil::unique_variant` containing `VT_BOOL`.
 */
inline wil::unique_variant ctWmiMakeVariant(const bool value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_BOOL;
    V_BOOL(localVariant.addressof()) = value ? TRUE : FALSE;
    return localVariant;
}

/**
 * @brief Read a boolean out of a VARIANT.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives the boolean value if present.
 * @return True when the VARIANT contained a boolean, false if empty/null.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ bool* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_BOOL);
    *value = V_BOOL(variant) ? true : false;
    return true;
}

/**
 * @brief Construct a VARIANT representing a single-byte integer (`VT_UI1`).
 * @param[in] value Value to store.
 * @return A `wil::unique_variant` with `VT_UI1`.
 */
inline wil::unique_variant ctWmiMakeVariant(const char value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_UI1;
    V_UI1(localVariant.addressof()) = value;
    return localVariant;
}

/**
 * @brief Read a `char` value from a VARIANT expecting `VT_UI1`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives the `char` value if present.
 * @return True when the VARIANT contained a `VT_UI1` value.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ char* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_UI1);
    *value = V_UI1(variant);
    return true;
}

/**
 * @brief Construct a VARIANT representing an unsigned byte (`VT_UI1`).
 * @param[in] value Value to store.
 * @return A `wil::unique_variant` with `VT_UI1`.
 */
inline wil::unique_variant ctWmiMakeVariant(const unsigned char value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_UI1;
    V_UI1(localVariant.addressof()) = value;
    return localVariant;
}

/**
 * @brief Read an `unsigned char` from a VARIANT expecting `VT_UI1`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives the unsigned byte if present.
 * @return True when the VARIANT contained a `VT_UI1` value.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ unsigned char* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_UI1);
    *value = V_UI1(variant);
    return true;
}

/**
 * @brief Construct a VARIANT representing a signed 16-bit integer (`VT_I2`).
 * @param[in] value Value to store.
 * @return A `wil::unique_variant` with `VT_I2`.
 */
inline wil::unique_variant ctWmiMakeVariant(const short value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_I2;
    V_I2(localVariant.addressof()) = value;
    return localVariant;
}

/**
 * @brief Read a signed 16-bit integer from a VARIANT expecting `VT_I2`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives the short value if present.
 * @return True when the VARIANT contained `VT_I2`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ short* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_I2);
    *value = V_I2(variant);
    return true;
}

/**
 * @brief Construct a VARIANT representing an unsigned 16-bit integer.
 * @param[in] value Value to store.
 * @return A `wil::unique_variant` containing the value as `VT_I2`.
 */
inline wil::unique_variant ctWmiMakeVariant(const unsigned short value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_I2;
    V_I2(localVariant.addressof()) = static_cast<short>(value);
    return localVariant;
}

/**
 * @brief Read an unsigned 16-bit integer from a VARIANT stored as `VT_I2`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives the unsigned short value if present.
 * @return True when the VARIANT contained a signed 16-bit integer.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ unsigned short* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_I2);
    *value = V_I2(variant);
    return true;
}

/**
 * @brief Construct a VARIANT representing a 32-bit integer (`VT_I4`).
 * @param[in] value Value to store.
 * @return A `wil::unique_variant` with `VT_I4`.
 */
inline wil::unique_variant ctWmiMakeVariant(const long value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_I4;
    V_I4(localVariant.addressof()) = value;
    return localVariant;
}

/**
 * @brief Read a 32-bit integer from a VARIANT expecting `VT_I4`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives the long value if present.
 * @return True when the VARIANT contained `VT_I4`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ long* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_I4);
    *value = V_I4(variant);
    return true;
}

/**
 * @brief Construct a VARIANT representing an unsigned 32-bit integer.
 * @param[in] value Value to store.
 * @return A `wil::unique_variant` containing the value as `VT_I4`.
 */
inline wil::unique_variant ctWmiMakeVariant(const unsigned long value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_I4;
    V_I4(localVariant.addressof()) = static_cast<long>(value);
    return localVariant;
}

/**
 * @brief Read an unsigned 32-bit integer from a VARIANT stored as `VT_I4`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives the unsigned long value if present.
 * @return True when the VARIANT contained `VT_I4`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ unsigned long* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_I4);
    *value = V_I4(variant);
    return true;
}

/**
 * @brief Construct a VARIANT representing a 32-bit signed integer (`VT_I4`).
 * @param[in] value Value to store.
 * @return A `wil::unique_variant` with `VT_I4`.
 */
inline wil::unique_variant ctWmiMakeVariant(const int value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_I4;
    V_I4(localVariant.addressof()) = value;
    return localVariant;
}

/**
 * @brief Read a 32-bit signed integer from a VARIANT expecting `VT_I4`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives the int value if present.
 * @return True when the VARIANT contained `VT_I4`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ int* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_I4);
    *value = V_I4(variant);
    return true;
}

/**
 * @brief Construct a VARIANT representing an unsigned 32-bit integer.
 * @param[in] value Value to store.
 * @return A `wil::unique_variant` containing the value as `VT_I4`.
 */
inline wil::unique_variant ctWmiMakeVariant(const unsigned int value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_I4;
    V_I4(localVariant.addressof()) = static_cast<long>(value);
    return localVariant;
}

/**
 * @brief Read an unsigned 32-bit integer from a VARIANT stored as `VT_I4`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives the unsigned int value if present.
 * @return True when the VARIANT contained `VT_I4`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ unsigned int* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_I4);
    *value = V_I4(variant);
    return true;
}

/**
 * @brief Construct a VARIANT representing a 32-bit float (`VT_R4`).
 * @param[in] value Value to store.
 * @return A `wil::unique_variant` with `VT_R4`.
 */
inline wil::unique_variant ctWmiMakeVariant(const float value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_R4;
    V_R4(localVariant.addressof()) = value;
    return localVariant;
}

/**
 * @brief Read a 32-bit float from a VARIANT expecting `VT_R4`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives the float value if present.
 * @return True when the VARIANT contained `VT_R4`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ float* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_R4);
    *value = V_R4(variant);
    return true;
}

/**
 * @brief Construct a VARIANT representing a 64-bit double (`VT_R8`).
 * @param[in] value Value to store.
 * @return A `wil::unique_variant` with `VT_R8`.
 */
inline wil::unique_variant ctWmiMakeVariant(const double value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_R8;
    V_R8(localVariant.addressof()) = value;
    return localVariant;
}

/**
 * @brief Read a 64-bit double from a VARIANT expecting `VT_R8`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives the double value if present.
 * @return True when the VARIANT contained `VT_R8`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ double* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_R8);
    *value = V_R8(variant);
    return true;
}

/**
 * @brief Construct a VARIANT representing `SYSTEMTIME` as `VT_DATE`.
 * @param[in] value The SYSTEMTIME to convert.
 * @return A `wil::unique_variant` with `VT_DATE` containing the converted time.
 */
inline wil::unique_variant ctWmiMakeVariant(SYSTEMTIME value)
{
    wil::unique_variant localVariant;
    DOUBLE time{};
    THROW_HR_IF(E_INVALIDARG, !::SystemTimeToVariantTime(&value, &time));
    V_VT(localVariant.addressof()) = VT_DATE;
    V_DATE(localVariant.addressof()) = time;
    return localVariant;
}

/**
 * @brief Read a `SYSTEMTIME` from a VARIANT stored as `VT_DATE`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives the SYSTEMTIME on success.
 * @return True when the VARIANT contained `VT_DATE`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ SYSTEMTIME* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_DATE);
    THROW_HR_IF(E_INVALIDARG, !::VariantTimeToSystemTime(V_DATE(variant), value));
    return true;
}

/**
 * @brief Construct a VARIANT containing a `BSTR`.
 * @param[in] value The BSTR to copy into the VARIANT.
 * @return A `wil::unique_variant` with `VT_BSTR`.
 */
inline wil::unique_variant ctWmiMakeVariant(_In_ const BSTR value) // NOLINT(misc-misplaced-const)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_BSTR;
    V_BSTR(localVariant.addressof()) = SysAllocString(value);
    THROW_IF_NULL_ALLOC(V_BSTR(localVariant.addressof()));
    return localVariant;
}

/**
 * @brief Read a `BSTR` from a VARIANT stored as `VT_BSTR`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives a newly allocated BSTR on success.
 * @return True when the VARIANT contained `VT_BSTR`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ BSTR* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_BSTR);
    *value = SysAllocString(V_BSTR(variant));
    THROW_IF_NULL_ALLOC(*value);
    return true;
}

/**
 * @brief Construct a VARIANT from a wide string by allocating a `BSTR`.
 * @param[in] value Null-terminated wide string to copy.
 * @return A `wil::unique_variant` with `VT_BSTR`.
 */
inline wil::unique_variant ctWmiMakeVariant(_In_ PCWSTR value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_BSTR;
    V_BSTR(localVariant.addressof()) = SysAllocString(value);
    THROW_IF_NULL_ALLOC(V_BSTR(localVariant.addressof()));
    return localVariant;
}

/**
 * @brief Read a `std::wstring` from a VARIANT containing `VT_BSTR`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[in,out] value Receives the string on success; the string is cleared first.
 * @return True when the VARIANT contained `VT_BSTR`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Inout_ std::wstring* value)
{
    value->clear();
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_BSTR);
    value->assign(V_BSTR(variant));
    return true;
}

// Even though VARIANT's support 64-bit integers, WMI passes them around as BSTRs
/**
 * @brief Construct a VARIANT representing a 64-bit unsigned integer.
 *
 * WMI expects 64-bit integers to be represented as `BSTR`s, so this helper
 * converts the value to a wide string and stores it as `VT_BSTR`.
 * @param[in] value The 64-bit unsigned value to store.
 * @return A `wil::unique_variant` with `VT_BSTR` containing the decimal string.
 */
inline wil::unique_variant ctWmiMakeVariant(const unsigned long long value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_BSTR;
    V_BSTR(localVariant.addressof()) = SysAllocString(std::to_wstring(value).c_str());
    THROW_IF_NULL_ALLOC(V_BSTR(localVariant.addressof()));
    return localVariant;
}

/**
 * @brief Read a 64-bit unsigned integer from a VARIANT stored as `VT_BSTR`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives the parsed unsigned long long on success.
 * @return True when the VARIANT contained `VT_BSTR`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ unsigned long long* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_BSTR);
    *value = _wcstoui64(V_BSTR(variant), nullptr, 10);
    return true;
}

// Even though VARIANT's support 64-bit integers, WMI passes them around as BSTRs
/**
 * @brief Construct a VARIANT representing a 64-bit signed integer (stored as `BSTR`).
 * @param[in] value The 64-bit signed value to store.
 * @return A `wil::unique_variant` with `VT_BSTR` containing the decimal string.
 */
inline wil::unique_variant ctWmiMakeVariant(_In_ const long long value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_BSTR;
    V_BSTR(localVariant.addressof()) = SysAllocString(std::to_wstring(value).c_str());
    THROW_IF_NULL_ALLOC(V_BSTR(localVariant.addressof()));
    return localVariant;
}

/**
 * @brief Read a 64-bit signed integer from a VARIANT stored as `VT_BSTR`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[out] value Receives the parsed long long on success.
 * @return True when the VARIANT contained `VT_BSTR`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Out_ long long* value)
{
    *value = {};
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_BSTR);
    *value = _wcstoi64(V_BSTR(variant), nullptr, 10);
    return true;
}

/**
 * @brief Construct a VARIANT containing a COM object pointer (`VT_UNKNOWN`).
 * @tparam T COM interface type.
 * @param[in] value COM pointer to store in the VARIANT (AddRef is performed).
 * @return A `wil::unique_variant` with `VT_UNKNOWN`.
 */
template <typename T>
wil::unique_variant ctWmiMakeVariant(const wil::com_ptr<T>& value) noexcept
{
    wil::unique_variant variant;
    V_VT(&variant) = VT_UNKNOWN;
    V_UNKNOWN(variant.addressof()) = value.get();
    // Must deliberately AddRef the raw pointer assigned to punkVal in the variant
    V_UNKNOWN(variant.addressof())->AddRef();
    return variant;
}

/**
 * @brief Read a COM interface pointer from a VARIANT containing `VT_UNKNOWN`.
 * @tparam T The COM interface type to query for.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[in,out] value Receives the queried interface pointer on success.
 * @return True when the VARIANT contained `VT_UNKNOWN`.
 */
template <typename T>
bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Inout_ wil::com_ptr<T>* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_UNKNOWN);
    THROW_IF_FAILED(V_UNKNOWN(variant)->QueryInterface(__uuidof(T), reinterpret_cast<void**>(value->put())));
    return true;
}

/**
 * @brief Read an array of COM pointers from a SAFEARRAY stored in a VARIANT.
 * @tparam T The COM interface type to query each element for.
 * @param[in] variant Pointer to the VARIANT to read (must be VT_UNKNOWN|VT_ARRAY).
 * @param[in,out] value Receives a vector of `wil::com_ptr<T>` on success.
 * @return True when the VARIANT contained `VT_UNKNOWN | VT_ARRAY`.
 */
template <typename T>
bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Inout_ std::vector<wil::com_ptr<T>>* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != (VT_UNKNOWN | VT_ARRAY));

    IUnknown** iUnknownArray{};
    THROW_IF_FAILED(::SafeArrayAccessData(variant->parray, reinterpret_cast<void**>(&iUnknownArray)));
    const auto unAccessArray = wil::scope_exit([&]() noexcept { SafeArrayUnaccessData(variant->parray); });

    std::vector<wil::com_ptr<T>> tempData;
    for (auto loop = 0ul; loop < variant->parray->rgsabound[0].cElements; ++loop)
    {
        wil::com_ptr<T> tempPtr;
        THROW_IF_FAILED(iUnknownArray[loop]->QueryInterface(__uuidof(T), reinterpret_cast<void**>(tempPtr.put())));
        tempData.push_back(tempPtr);
    }
    value->swap(tempData);
    return true;
}

/**
 * @brief Construct a VARIANT containing an array of wide strings (`VT_BSTR|VT_ARRAY`).
 * @param[in] data Vector of wide strings to store in the SAFEARRAY.
 * @return A `wil::unique_variant` containing the SAFEARRAY and `VT_BSTR | VT_ARRAY`.
 */
inline wil::unique_variant ctWmiMakeVariant(const std::vector<std::wstring>& data)
{
    auto* const tempSafeArray = SafeArrayCreateVector(VT_BSTR, 0, static_cast<ULONG>(data.size()));
    THROW_IF_NULL_ALLOC(tempSafeArray);
    auto guardArray = wil::scope_exit([&]() noexcept { SafeArrayDestroy(tempSafeArray); });

    for (size_t loop = 0; loop < data.size(); ++loop)
    {
        // SafeArrayPutElement requires an array of indexes for each dimension of the array
        // - in this case, we have a 1-dimensional array, thus an array of 1 LONG - assigned to the loop variable
        long index[1]{static_cast<long>(loop)};

        auto* const bstr = SysAllocString(data[loop].c_str());
        THROW_IF_NULL_ALLOC(bstr);
        THROW_IF_FAILED(::SafeArrayPutElement(tempSafeArray, index, bstr));
    }

    wil::unique_variant variant;
    variant.parray = tempSafeArray;
    variant.vt = VT_BSTR | VT_ARRAY;

    // don't free the SAFEARRAY on success - its lifetime is transferred to variant
    guardArray.release();
    return variant;
}

/**
 * @brief Read an array of wide strings from a VARIANT containing `VT_BSTR|VT_ARRAY`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[in,out] value Receives the vector of strings on success.
 * @return True when the VARIANT contained `VT_BSTR | VT_ARRAY`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Inout_ std::vector<std::wstring>* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != (VT_BSTR | VT_ARRAY));

    BSTR* stringArray{};
    THROW_IF_FAILED(::SafeArrayAccessData(variant->parray, reinterpret_cast<void**>(&stringArray)));
    const auto unAccessArray = wil::scope_exit([&]() noexcept { SafeArrayUnaccessData(variant->parray); });

    std::vector<std::wstring> tempData;
    for (auto loop = 0ul; loop < variant->parray->rgsabound[0].cElements; ++loop)
    {
        tempData.emplace_back(stringArray[loop]);
    }
    value->swap(tempData);
    return true;
}

/**
 * @brief Construct a VARIANT containing an array of 32-bit unsigned integers.
 * @param[in] data Vector of `uint32_t` values to store.
 * @return A `wil::unique_variant` with `VT_UI4 | VT_ARRAY`.
 */
inline wil::unique_variant ctWmiMakeVariant(const std::vector<uint32_t>& data)
{
    auto* const tempSafeArray = SafeArrayCreateVector(VT_UI4, 0, static_cast<ULONG>(data.size()));
    THROW_IF_NULL_ALLOC(tempSafeArray);
    auto guardArray = wil::scope_exit([&]() noexcept { SafeArrayDestroy(tempSafeArray); });

    for (size_t loop = 0; loop < data.size(); ++loop)
    {
        // SafeArrayPutElement requires an array of indexes for each dimension of the array
        // - in this case, we have a 1-dimensional array, thus an array of 1 LONG - assigned to the loop variable
        long index[1]{static_cast<long>(loop)};

        uint32_t value = data[loop];
        THROW_IF_FAILED(::SafeArrayPutElement(tempSafeArray, index, &value));
    }

    wil::unique_variant variant;
    variant.parray = tempSafeArray;
    variant.vt = VT_UI4 | VT_ARRAY;

    // don't free the SAFEARRAY on success - its lifetime is transferred to variant
    guardArray.release();
    return variant;
}

/**
 * @brief Read an array of 32-bit unsigned integers from a VARIANT containing `VT_UI4|VT_ARRAY`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[in,out] value Receives the vector of integers on success.
 * @return True when the VARIANT contained `VT_UI4 | VT_ARRAY`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Inout_ std::vector<uint32_t>* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != (VT_UI4 | VT_ARRAY));

    uint32_t* intArray{};
    THROW_IF_FAILED(::SafeArrayAccessData(variant->parray, reinterpret_cast<void**>(&intArray)));
    const auto unAccessArray = wil::scope_exit([&]() noexcept { SafeArrayUnaccessData(variant->parray); });

    std::vector<uint32_t> tempData;
    for (auto loop = 0ul; loop < variant->parray->rgsabound[0].cElements; ++loop)
    {
        tempData.push_back(intArray[loop]);
    }
    value->swap(tempData);
    return true;
}

/**
 * @brief Construct a VARIANT containing an array of unsigned shorts.
 *
 * WMI prefers arrays of 32-bit integers for certain types; this helper
 * stores `unsigned short` arrays as `VT_I4 | VT_ARRAY` to satisfy the WMI marshaller.
 * @param[in] data Vector of `unsigned short` values to store.
 * @return A `wil::unique_variant` with `VT_I4 | VT_ARRAY`.
 */
inline wil::unique_variant ctWmiMakeVariant(const std::vector<unsigned short>& data)
{
    // WMI marshaller complains type mismatch using VT_UI2 | VT_ARRAY, and VT_I4 | VT_ARRAY works fine.
    auto* const tempSafeArray = SafeArrayCreateVector(VT_I4, 0, static_cast<ULONG>(data.size()));
    THROW_IF_NULL_ALLOC(tempSafeArray);
    auto guardArray = wil::scope_exit([&]() noexcept { SafeArrayDestroy(tempSafeArray); });

    for (size_t loop = 0; loop < data.size(); ++loop)
    {
        // SafeArrayPutElement requires an array of indexes for each dimension of the array
        // - in this case, we have a 1-dimensional array, thus an array of 1 LONG - assigned to the loop variable
        long index[1]{static_cast<long>(loop)};

        // Expand unsigned short to long because the SAFEARRAY created assumes VT_I4 elements
        long value = data[loop];
        THROW_IF_FAILED(::SafeArrayPutElement(tempSafeArray, index, &value));
    }

    wil::unique_variant variant;
    variant.parray = tempSafeArray;
    variant.vt = VT_I4 | VT_ARRAY;

    // don't free the SAFEARRAY on success - its lifetime is transferred to variant
    guardArray.release();
    return variant;
}

/**
 * @brief Read an array of unsigned shorts from a VARIANT stored as `VT_I4|VT_ARRAY`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[in,out] value Receives the vector of unsigned shorts on success.
 * @return True when the VARIANT contained `VT_I4 | VT_ARRAY`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Inout_ std::vector<unsigned short>* value)
{
    // WMI marshaller complains type mismatch using VT_UI2 | VT_ARRAY, and VT_I4 | VT_ARRAY works fine.
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != (VT_I4 | VT_ARRAY));

    long* intArray{};
    THROW_IF_FAILED(::SafeArrayAccessData(variant->parray, reinterpret_cast<void**>(&intArray)));
    const auto unAccessArray = wil::scope_exit([&]() noexcept { SafeArrayUnaccessData(variant->parray); });

    std::vector<unsigned short> tempData;
    for (auto loop = 0ul; loop < variant->parray->rgsabound[0].cElements; ++loop)
    {
        THROW_HR_IF(E_INVALIDARG, intArray[loop] > MAXUINT16);
        tempData.push_back(static_cast<unsigned short>(intArray[loop]));
    }
    value->swap(tempData);
    return true;
}

/**
 * @brief Construct a VARIANT containing an array of bytes (`VT_UI1 | VT_ARRAY`).
 * @param[in] data Vector of bytes to store.
 * @return A `wil::unique_variant` with `VT_UI1 | VT_ARRAY`.
 */
inline wil::unique_variant ctWmiMakeVariant(const std::vector<unsigned char>& data)
{
    auto* const tempSafeArray = SafeArrayCreateVector(VT_UI1, 0, static_cast<ULONG>(data.size()));
    THROW_IF_NULL_ALLOC(tempSafeArray);
    auto guardArray = wil::scope_exit([&]() noexcept { SafeArrayDestroy(tempSafeArray); });

    for (size_t loop = 0; loop < data.size(); ++loop)
    {
        // SafeArrayPutElement requires an array of indexes for each dimension of the array
        // - in this case, we have a 1-dimensional array, thus an array of 1 LONG - assigned to the loop variable
        long index[1]{static_cast<long>(loop)};

        unsigned char value = data[loop];
        THROW_IF_FAILED(::SafeArrayPutElement(tempSafeArray, index, &value));
    }

    wil::unique_variant variant;
    variant.parray = tempSafeArray;
    variant.vt = VT_UI1 | VT_ARRAY;

    // don't free the SAFEARRAY on success - its lifetime is transferred to variant
    guardArray.release();
    return variant;
}

/**
 * @brief Read an array of bytes from a VARIANT containing `VT_UI1|VT_ARRAY`.
 * @param[in] variant Pointer to the VARIANT to read.
 * @param[in,out] value Receives the vector of bytes on success.
 * @return True when the VARIANT contained `VT_UI1 | VT_ARRAY`.
 */
inline bool ctWmiReadFromVariant(_In_ const VARIANT* variant, _Inout_ std::vector<unsigned char>* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != (VT_UI1 | VT_ARRAY));

    unsigned char* charArray{};
    THROW_IF_FAILED(::SafeArrayAccessData(variant->parray, reinterpret_cast<void**>(&charArray)));
    const auto unAccessArray = wil::scope_exit([&]() noexcept { SafeArrayUnaccessData(variant->parray); });

    std::vector<unsigned char> tempData;
    for (auto loop = 0ul; loop < variant->parray->rgsabound[0].cElements; ++loop)
    {
        tempData.push_back(charArray[loop]);
    }
    value->swap(tempData);
    return true;
}
}
