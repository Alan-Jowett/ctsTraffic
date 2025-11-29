/**
 * @file ctPerformanceCounter.hpp
 * @brief WMI-based performance counter collection utilities.
 *
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 *
 * This header implements helpers to enumerate, sample, and aggregate
 * performance counters exposed via WMI. It provides data accessors,
 * per-counter storage and aggregation, and a manager that schedules
 * periodic refreshes.
 *
 * Concepts for this file:
 * - WMI Classes expose performance counters through hi-performance WMI interfaces
 * - ctPerformanceCounterCounter exposes one counter within one WMI performance counter class
 * - Every performance counter object contains a 'Name' key field, uniquely identifying a 'set' of data points for that counter
 * - Counters are 'snapped' every one second, with the timeslot tracked with the data
 *
 * Factory and implementation notes:
 * - Factory functions (ctShared<class>PerfCounter) create per-counter instances.
 * - They accept the WMI class name and the counter name to sample.
 * - Internally, a templated ctPerformanceCounterCounterImpl handles enumeration and data access.
 *
 * Detailed concepts and notes:
 * - WMI classes expose performance counters via high-performance WMI interfaces.
 * - `ctPerformanceCounterCounter` represents one counter within a WMI performance class.
 * - Each performance counter object contains a `Name` key field which uniquely identifies
 *   the set of data points for that counter.
 * - Counters are sampled ("snapped") on a periodic interval (typically once per second),
 *   and each sample is stored as a time-slice.
 *
 * Factory usage and template parameters:
 * - Users obtain counter objects through per-counter factory functions (named like
 *   `ctShared<...>PerfCounter`). These factories accept the WMI class name and the
 *   counter/property name to sample and return a typed counter object.
 * - Internally, factories instantiate `ctPerformanceCounterCounterImpl` with three
 *   template arguments:
 *   1. The enumeration interface type (either `IWbemHiPerfEnum` or `IWbemClassObject`).
 *   2. The accessor interface type (either `IWbemObjectAccess` or `IWbemClassObject`).
 *   3. The data type used to represent the sampled counter values.
 * - The implementation receives the WMI class name and the counter name as constructor
 *   arguments and registers with the shared WMI refresher.
 *
 * Publicly exposed functionality from `ctPerformanceCounterCounter`:
 * - `add_filter(counterName, value)`: restricts captured instances to those matching
 *   the provided property/value pair.
 * - `reference_range(instanceName)`: returns a pair of iterators (begin,end) referencing
 *   the collected time-slices for the specified instance name; a `nullptr` instance name
 *   matches all instances (used for static classes).
 *
 * Data collection behavior:
 * - `ctPerformanceCounterCounter` updates its data by invoking a (pure-virtual)
 *   `update_counter_data()` implementation once per sample interval. Derived classes
 *   refresh their WMI accessor and call `add_instance()` for each returned instance.
 * - `add_instance()` inspects the instance (using the accessor type) and either creates
 *   a new `ctPerformanceCounterCounterData<T>` for previously unseen instances or appends
 *   the value to an existing instance's data.
 * - Caller responsibilities: callers must ensure thread-safety for WMI connections and
 *   that COM is initialized on threads that interact with these classes.
 *
 * Accessor types and behavior:
 * - Two accessor/enumeration combinations are supported:
 *   - `IWbemHiPerfEnum` + `IWbemObjectAccess`: enumerates multiple instances and
 *     provides `ReadDWORD`/`ReadQWORD` accessors for numeric and string properties.
 *   - `IWbemClassObject` + `IWbemClassObject`: used for single-instance (static)
 *     providers where values are read through `IWbemClassObject::Get`.
 * - `ctPerformanceCounterCounterData<T>` encapsulates per-instance storage and exposes
 *   `add()`/`match()`/iterator access. Collection types supported include detailed
 *   (store all samples), mean-only (maintain count/min/max/mean), and first-last.
 */

// ReSharper disable CppInconsistentNaming
#pragma once

#include <iterator>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <string>
#include <algorithm>

#include <Windows.h>
#include <objbase.h>
#include <oleauto.h>

#include "ctWmiInstance.hpp"
#include "ctWmiService.hpp"
#include "ctWmiVariant.hpp"

#include <wil/stl.h>
#include <wil/resource.h>
#include <wil/win32_helpers.h>

namespace ctl
{
	/**
	 * @enum ctPerformanceCounterCollectionType
	 * @brief Types of collection strategies for recorded counter values.
	 *
	 * - Detailed: store each sampled value.
	 * - MeanOnly: maintain count/min/max/mean.
	 * - FirstLast: store only the first and last values along with a count.
	 */
	enum class ctPerformanceCounterCollectionType : std::uint8_t
	{
		Detailed,
		MeanOnly,
		FirstLast
	};

	/**
	 * @brief Compare two VARIANT-like values for equality.
	 *
	 * Supports integer, string (BSTR), date and boolean comparisons. Floating
	 * point comparisons are intentionally not supported and will fail fast.
	 *
	 * @param[in] rhs Right-hand value.
	 * @param[in] lhs Left-hand value.
	 * @return true if values are considered equal, false otherwise.
	 */
	inline bool operator==(const wil::unique_variant &rhs, const wil::unique_variant &lhs) noexcept
	{
		if (rhs.vt == VT_NULL || lhs.vt == VT_NULL)
		{
			return rhs.vt == VT_NULL && lhs.vt == VT_NULL;
		}

		if (rhs.vt == VT_EMPTY || lhs.vt == VT_EMPTY)
		{
			return rhs.vt == VT_EMPTY && lhs.vt == VT_EMPTY;
		}

		if (rhs.vt == VT_BSTR || lhs.vt == VT_BSTR)
		{
			if (rhs.vt == VT_BSTR && lhs.vt == VT_BSTR)
			{
				return 0 == _wcsicmp(rhs.bstrVal, lhs.bstrVal);
			}
			return false;
		}

		if (rhs.vt == VT_DATE || lhs.vt == VT_DATE)
		{
			if (rhs.vt == VT_DATE && lhs.vt == VT_DATE)
			{
				return rhs.date == lhs.date; // NOLINT(clang-diagnostic-float-equal)
			}
			return false;
		}

		if (rhs.vt == VT_BOOL || lhs.vt == VT_BOOL)
		{
			if (rhs.vt == VT_BOOL && lhs.vt == VT_BOOL)
			{
				return rhs.boolVal == lhs.boolVal;
			}
			return false;
		}

		//
		// intentionally not supporting comparing floating-point types
		// - it's not going to provide a correct value
		// - the proper comparison should be < or  >
		//
		if (rhs.vt == VT_R4 || lhs.vt == VT_R4 ||
			rhs.vt == VT_R8 || lhs.vt == VT_R8)
		{
			FAIL_FAST_MSG("Not making equality comparisons on floating-point numbers");
		}

		//
		// Comparing integer types - not tightly enforcing type by default
		//
		uint32_t rhsInteger = 0;
		switch (rhs.vt)
		{
		case VT_I1:
			rhsInteger += rhs.cVal;
			break;
		case VT_UI1:
			rhsInteger += rhs.bVal;
			break;
		case VT_I2:
			rhsInteger += rhs.iVal;
			break;
		case VT_UI2:
			rhsInteger += rhs.uiVal;
			break;
		case VT_I4:
			rhsInteger += rhs.lVal;
			break;
		case VT_UI4:
			rhsInteger += rhs.ulVal;
			break;
		case VT_INT:
			rhsInteger += rhs.intVal;
			break;
		case VT_UINT:
			rhsInteger += rhs.uintVal;
			break;
		default:
			return false;
		}
		uint32_t lhsInteger = 0;
		switch (lhs.vt)
		{
		case VT_I1:
			lhsInteger += lhs.cVal;
			break;
		case VT_UI1:
			lhsInteger += lhs.bVal;
			break;
		case VT_I2:
			lhsInteger += lhs.iVal;
			break;
		case VT_UI2:
			lhsInteger += lhs.uiVal;
			break;
		case VT_I4:
			lhsInteger += lhs.lVal;
			break;
		case VT_UI4:
			lhsInteger += lhs.ulVal;
			break;
		case VT_INT:
			lhsInteger += lhs.intVal;
			break;
		case VT_UINT:
			lhsInteger += lhs.uintVal;
			break;
		default:
			return false;
		}

		return lhsInteger == rhsInteger;
	}

	/**
	 * @brief Inequality comparison for two VARIANT-like values.
	 *
	 * Performs the negation of the equality comparison. See `operator==` for
	 * the supported VARIANT types and comparison semantics.
	 *
	 * @param[in] rhs Right-hand value.
	 * @param[in] lhs Left-hand value.
	 * @return true if the values are not equal, false otherwise.
	 */
	inline bool operator!=(const wil::unique_variant &rhs, const wil::unique_variant &lhs) noexcept
	{
		return !(rhs == lhs);
	}

	namespace details
	{
		/**
		 * @brief Read a named counter property from an IWbemObjectAccess instance.
		 *
		 * This helper queries the property handle and reads the value using the
		 * appropriate IWbemObjectAccess methods, returning the result as a
		 * `wil::unique_variant`.
		 *
		 * @param[in] instance IWbemObjectAccess instance to read from.
		 * @param[in] counterName Unicode property name to read.
		 * @return Variant containing the value read from WMI.
		 * @throws wil::ResultException on WMI failures.
		 */
		inline wil::unique_variant ReadCounterFromWbemObjectAccess(
			_In_ IWbemObjectAccess *instance,
			_In_ PCWSTR counterName)
		{
			LONG propertyHandle{};
			CIMTYPE propertyType{};
			THROW_IF_FAILED(instance->GetPropertyHandle(counterName, &propertyType, &propertyHandle));

			wil::unique_variant currentValue;
			switch (propertyType)
			{
			case CIM_SINT32:
			case CIM_UINT32:
			{
				ULONG value{};
				THROW_IF_FAILED(instance->ReadDWORD(propertyHandle, &value));
				currentValue = ctWmiMakeVariant(value);
				break;
			}

			case CIM_SINT64:
			case CIM_UINT64:
			{
				ULONGLONG value{};
				THROW_IF_FAILED(instance->ReadQWORD(propertyHandle, &value));
				currentValue = ctWmiMakeVariant(value);
				break;
			}

			case CIM_STRING:
			{
				constexpr long cimStringDefaultSize = 64;
				std::wstring value(cimStringDefaultSize, L'\0');
				long valueSize = cimStringDefaultSize * sizeof(WCHAR);
				long returnedSize{};
				auto hr = instance->ReadPropertyValue(
					propertyHandle,
					valueSize,
					&returnedSize,
					reinterpret_cast<BYTE *>(value.data()));
				if (WBEM_E_BUFFER_TOO_SMALL == hr)
				{
					valueSize = returnedSize;
					value.resize(valueSize / sizeof(WCHAR));
					hr = instance->ReadPropertyValue(
						propertyHandle,
						valueSize,
						&returnedSize,
						reinterpret_cast<BYTE *>(value.data()));
				}
				THROW_IF_FAILED(hr);
				currentValue = ctWmiMakeVariant(value.c_str());
				break;
			}

			default:
				THROW_HR_MSG(
					HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
					"ctPerformanceCounter only supports data of type INT32, INT64, and BSTR: counter %ws is of type %u",
					counterName, static_cast<unsigned>(propertyType));
			}

			return currentValue;
		}

		/**
		 * @class ctPerformanceCounterDataAccessor
		 * @brief Internal helper that binds a WMI refresher enumeration/object to a C++ iterator.
		 *
		 * The template adapts two WMI access patterns so callers may uniformly iterate
		 * over the current set of accessor objects for a provider class. The concrete
		 * template specializations implement refresh and clear semantics for the
		 * corresponding WMI types.
		 *
		 * @tparam TEnum Enumeration/refresher COM type (e.g., `IWbemHiPerfEnum` or `IWbemClassObject`).
		 * @tparam TAccess Accessor COM type used to read instance properties.
		 *
		 * @note This class does not perform external synchronization; callers must
		 *       ensure threading guarantees for WMI usage and COM initialization.
		 */
		template <typename TEnum, typename TAccess>
		class ctPerformanceCounterDataAccessor
		{
		public:
			using ctAccessIterator = typename std::vector<TAccess *>::const_iterator;

			/**
			 * @brief Construct an accessor for the specified WMI class refresher.
			 *
			 * @param[in] wmi Initialized `ctWmiService` instance used to bind the refresher.
			 * @param[in] config `IWbemConfigureRefresher` used to add enumerations or objects.
			 * @param[in] classname Unicode WMI class name to observe (for example, `L"Win32_PerfFormattedData_..."`).
			 */
			ctPerformanceCounterDataAccessor(
				ctWmiService wmi,
				const wil::com_ptr<IWbemConfigureRefresher> &config,
				_In_ PCWSTR classname);

			~ctPerformanceCounterDataAccessor() noexcept
			{
				clear();
			}

			// refreshes internal data with the latest performance data
			/**
			 * @brief Refresh the internal collection of accessor objects.
			 *
			 * After calling this method the object can be iterated to obtain the
			 * latest set of WMI accessor objects for the target class.
			 */
			void refresh();

			[[nodiscard]] ctAccessIterator begin() const noexcept
			{
				return m_accessorObjects.cbegin();
			}

			[[nodiscard]] ctAccessIterator end() const noexcept
			{
				return m_accessorObjects.cend();
			}

			// non-copyable
			ctPerformanceCounterDataAccessor(const ctPerformanceCounterDataAccessor &) = delete;
			ctPerformanceCounterDataAccessor &operator=(const ctPerformanceCounterDataAccessor &) = delete;

			// movable
			ctPerformanceCounterDataAccessor(ctPerformanceCounterDataAccessor &&rhs) noexcept : m_enumerationObject(std::move(rhs.m_enumerationObject)),
																								m_accessorObjects(std::move(rhs.m_accessorObjects)),
																								m_currentIterator(std::move(rhs.m_currentIterator))
			{
				// since accessor_objects is storing raw pointers, manually clear out the rhs object,
				// so they won't be double-deleted
				rhs.m_accessorObjects.clear();
				rhs.m_currentIterator = rhs.m_accessorObjects.end();
			}

			ctPerformanceCounterDataAccessor &operator=(ctPerformanceCounterDataAccessor &&rhs) noexcept
			{
				m_enumerationObject = std::move(rhs.m_enumerationObject);
				m_accessorObjects = std::move(rhs.m_accessorObjects);
				m_currentIterator = std::move(rhs.m_currentIterator);
				// since accessor_objects is storing raw pointers, manually clear out the rhs object,
				// so they won't be double-deleted
				rhs.m_accessorObjects.clear();
				rhs.m_currentIterator = rhs.m_accessorObjects.end();
				return *this;
			}

		private:
			wil::com_ptr<TEnum> m_enumerationObject;
			// TAccess pointers are returned through enumeration_object::GetObjects, reusing the same vector for each refresh call
			std::vector<TAccess *> m_accessorObjects;
			ctAccessIterator m_currentIterator;

			/**
			 * @brief Release any cached accessor objects and reset internal state.
			 */
			void clear() noexcept;
		};

		inline ctPerformanceCounterDataAccessor<IWbemHiPerfEnum, IWbemObjectAccess>::ctPerformanceCounterDataAccessor(
			ctWmiService wmi, const wil::com_ptr<IWbemConfigureRefresher> &config, _In_ PCWSTR classname) : m_currentIterator(m_accessorObjects.end())
		{
			LONG lid{};
			THROW_IF_FAILED(config->AddEnum(wmi.get(), classname, 0, nullptr, m_enumerationObject.addressof(), &lid));
		}

		inline ctPerformanceCounterDataAccessor<IWbemClassObject, IWbemClassObject>::ctPerformanceCounterDataAccessor(
			ctWmiService wmi, const wil::com_ptr<IWbemConfigureRefresher> &config, _In_ PCWSTR classname) : m_currentIterator(m_accessorObjects.end())
		{
			ctWmiEnumerateInstance enumInstances(wmi);
			enumInstances.query(wil::str_printf<std::wstring>(L"SELECT * FROM %ws", classname).c_str());
			if (enumInstances.begin() == enumInstances.end())
			{
				THROW_HR_MSG(
					HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
					"Failed to refresh a static instances of the WMI class %ws", classname);
			}

			const auto instance = *enumInstances.begin();
			LONG lid{};
			THROW_IF_FAILED(
				config->AddObjectByTemplate(
					wmi.get(),
					instance.get_instance().get(),
					0,
					nullptr,
					m_enumerationObject.addressof(),
					&lid));

			// setting the raw pointer in the access vector to behave with the iterator
			m_accessorObjects.push_back(m_enumerationObject.get());
		}

		template <>
		inline void ctPerformanceCounterDataAccessor<IWbemHiPerfEnum, IWbemObjectAccess>::refresh()
		{
			clear();

			ULONG objectsReturned = 0;
			auto hr = m_enumerationObject->GetObjects(
				0,
				static_cast<ULONG>(m_accessorObjects.size()),
				m_accessorObjects.empty() ? nullptr : m_accessorObjects.data(),
				&objectsReturned);

			if (WBEM_E_BUFFER_TOO_SMALL == hr)
			{
				m_accessorObjects.resize(objectsReturned);
				hr = m_enumerationObject->GetObjects(
					0,
					static_cast<ULONG>(m_accessorObjects.size()),
					m_accessorObjects.data(),
					&objectsReturned);
			}
			THROW_IF_FAILED(hr);

			m_accessorObjects.resize(objectsReturned);
			m_currentIterator = m_accessorObjects.begin();
		}

		template <>
		inline void ctPerformanceCounterDataAccessor<IWbemClassObject, IWbemClassObject>::refresh()
		{
			// the underlying IWbemClassObject is already refreshed
			// accessor_objects will only ever have a singe instance
			FAIL_FAST_IF_MSG(
				m_accessorObjects.size() != 1,
				"ctPerformanceCounterDataAccessor<IWbemClassObject, IWbemClassObject>: for IWbemClassObject performance classes there can only ever have the single instance being tracked - instead has %Iu",
				m_accessorObjects.size());

			m_currentIterator = m_accessorObjects.begin();
		}

		template <>
		inline void ctPerformanceCounterDataAccessor<IWbemHiPerfEnum, IWbemObjectAccess>::clear() noexcept
		{
			for (IWbemObjectAccess *object : m_accessorObjects)
			{
				object->Release();
			}
			m_accessorObjects.clear();
			m_currentIterator = m_accessorObjects.end();
		}

		template <>
		inline void ctPerformanceCounterDataAccessor<IWbemClassObject, IWbemClassObject>::clear() noexcept
		{
			m_currentIterator = m_accessorObjects.end();
		}

		/**
		 * @class ctPerformanceCounterCounterData
		 * @brief Thread-safe per-instance storage for sampled counter values.
		 *
		 * Depending on the configured `ctPerformanceCounterCollectionType` this
		 * class either stores each sample (Detailed), maintains aggregate
		 * statistics (MeanOnly), or records only the first/last sample (FirstLast).
		 *
		 * @tparam T Numeric type used for storing counter samples (for example `ULONG`).
		 */
		template <typename T>
		class ctPerformanceCounterCounterData
		{
		private:
			mutable wil::critical_section m_guardData{500};
			const ctPerformanceCounterCollectionType m_collectionType = ctPerformanceCounterCollectionType::Detailed;
			const std::wstring m_instanceName;
			const std::wstring m_counterName;
			std::vector<T> m_counterData;
			uint64_t m_counterSum = 0;

			/**
			 * @brief Append a sample according to the collection strategy.
			 *
			 * @param[in] instanceData Sample value to add.
			 */
			void add_data(const T &instanceData)
			{
				const auto lock = m_guardData.lock();
				switch (m_collectionType)
				{
				case ctPerformanceCounterCollectionType::Detailed:
					m_counterData.push_back(instanceData);
					break;

				case ctPerformanceCounterCollectionType::MeanOnly:
					// vector is formatted as:
					// [0] == count
					// [1] == min
					// [2] == max
					// [3] == mean
					if (m_counterData.empty())
					{
						m_counterData.push_back(1);
						m_counterData.push_back(instanceData);
						m_counterData.push_back(instanceData);
						m_counterData.push_back(0);
					}
					else
					{
						++m_counterData[0];
						if (instanceData < m_counterData[1])
						{
							m_counterData[1] = instanceData;
						}
						if (instanceData > m_counterData[2])
						{
							m_counterData[2] = instanceData;
						}
					}

					m_counterSum += instanceData;
					break;

				case ctPerformanceCounterCollectionType::FirstLast:
					// the first data point write both min and max
					// [0] == count
					// [1] == first
					// [2] == last
					if (m_counterData.empty())
					{
						m_counterData.push_back(1);
						m_counterData.push_back(instanceData);
						m_counterData.push_back(instanceData);
					}
					else
					{
						++m_counterData[0];
						m_counterData[2] = instanceData;
					}
					break;

				default:
					FAIL_FAST_MSG(
						"Unknown ctPerformanceCounterCollectionType (%d)", m_collectionType);
				}
			}

			/**
			 * @brief Return iterator to the beginning of stored samples.
			 */
			typename std::vector<T>::const_iterator access_begin() noexcept
			{
				const auto lock = m_guardData.lock();
				// when accessing data, calculate the mean
				if (ctPerformanceCounterCollectionType::MeanOnly == m_collectionType)
				{
					m_counterData[3] = static_cast<T>(m_counterSum / m_counterData[0]);
				}
				return m_counterData.cbegin();
			}

			/**
			 * @brief Return iterator to the end of stored samples.
			 */
			typename std::vector<T>::const_iterator access_end() const noexcept
			{
				const auto lock = m_guardData.lock();
				return m_counterData.cend();
			}

		public:
			/**
			 * @brief Construct per-instance data from an IWbemObjectAccess accessor.
			 *
			 * @param[in] collectionType Collection strategy to use.
			 * @param[in] instance IWbemObjectAccess instance to query the `Name` key from.
			 * @param[in] counter Counter/property name being tracked.
			 */
			ctPerformanceCounterCounterData(
				const ctPerformanceCounterCollectionType collectionType,
				_In_ IWbemObjectAccess *instance,
				_In_ PCWSTR counter) : m_collectionType(collectionType),
									   m_instanceName(V_BSTR(ReadCounterFromWbemObjectAccess(instance, L"Name").addressof())),
									   m_counterName(counter)
			{
			}

			/**
			 * @brief Construct per-instance data from an IWbemClassObject (static class case).
			 *
			 * The static class is expected to have a NULL `Name` key; an exception
			 * is thrown if the value is unexpectedly present.
			 *
			 * @param[in] collectionType Collection strategy to use.
			 * @param[in] instance IWbemClassObject representing the single instance.
			 * @param[in] counter Counter/property name being tracked.
			 */
			ctPerformanceCounterCounterData(
				const ctPerformanceCounterCollectionType collectionType,
				_In_ IWbemClassObject *instance,
				_In_ PCWSTR counter) : m_collectionType(collectionType),
									   m_counterName(counter)
			{
				wil::unique_variant value;
				THROW_IF_FAILED(instance->Get(L"Name", 0, value.addressof(), nullptr, nullptr));

				// Name is expected to be NULL in this case
				// - since IWbemClassObject is expected to be a single instance
				THROW_HR_IF_MSG(
					HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
					value.vt != VT_NULL,
					"ctPerformanceCounterCounterData was given an IWbemClassObject to track that had a non-null 'Name' key field ['%ws']. Expected to be a NULL key field as to only support single-instances",
					V_BSTR(value.addressof()));
			}

			~ctPerformanceCounterCounterData() noexcept = default;

			// instanceName == nullptr means match everything
			/**
			 * @brief Determine whether this stored instance matches the requested name.
			 *
			 * @param[in,opt] instanceName Name to match, or nullptr to match all.
			 * @return true when the stored instance matches the provided name.
			 */
			bool match(_In_opt_ PCWSTR instanceName) const
			{
				if (!instanceName)
				{
					return true;
				}
				if (m_instanceName.empty())
				{
					return nullptr == instanceName;
				}
				return wil::compare_string_ordinal(instanceName, m_instanceName, true) == wistd::weak_ordering::equivalent;
			}

			/**
			 * @brief Read the counter value from an IWbemObjectAccess and add a sample.
			 *
			 * @param[in] instance IWbemObjectAccess instance to read the property from.
			 */
			void add(_In_ IWbemObjectAccess *instance)
			{
				T instanceData{};
				if (ctWmiReadFromVariant(
						ReadCounterFromWbemObjectAccess(instance, m_counterName.c_str()).addressof(),
						&instanceData))
				{
					add_data(instanceData);
				}
			}

			/**
			 * @brief Read the counter value from an IWbemClassObject and add a sample.
			 *
			 * @param[in] instance IWbemClassObject instance to read the property from.
			 */
			void add(_In_ IWbemClassObject *instance)
			{
				wil::unique_variant value;
				THROW_IF_FAILED(instance->Get(m_counterName.c_str(), 0, value.addressof(), nullptr, nullptr));
				// the instance could return null if there's no data
				if (value.vt != VT_NULL)
				{
					T instanceData;
					if (ctWmiReadFromVariant(value.addressof(), &instanceData))
					{
						add_data(instanceData);
					}
				}
			}

			/**
			 * @brief Iterator to the first stored sample.
			 *
			 * @return const_iterator pointing at the first sample.
			 */
			typename std::vector<T>::const_iterator begin() noexcept
			{
				const auto lock = m_guardData.lock();
				return access_begin();
			}

			typename std::vector<T>::const_iterator end() const noexcept
			{
				const auto lock = m_guardData.lock();
				return access_end();
			}

			/**
			 * @brief Number of samples currently stored.
			 *
			 * @return sample count.
			 */
			size_t count() noexcept
			{
				const auto lock = m_guardData.lock();
				return access_end() - access_begin();
			}

			/**
			 * @brief Clear all stored samples and reset aggregation state.
			 */
			void clear() noexcept
			{
				const auto lock = m_guardData.lock();
				m_counterData.clear();
				m_counterSum = 0;
			}

			// non-copyable
			ctPerformanceCounterCounterData(const ctPerformanceCounterCounterData &) = delete;
			ctPerformanceCounterCounterData &operator=(const ctPerformanceCounterCounterData &) = delete;

			// not movable
			ctPerformanceCounterCounterData(ctPerformanceCounterCounterData &&) = delete;
			ctPerformanceCounterCounterData &operator=(ctPerformanceCounterCounterData &&) = delete;
		};

		/**
		 * @brief Query the `Name` property for an IWbemObjectAccess instance.
		 *
		 * @param[in] instance IWbemObjectAccess accessor to query.
		 * @return Variant containing the `Name` property.
		 */
		inline wil::unique_variant ctQueryInstanceName(_In_ IWbemObjectAccess *instance)
		{
			return ReadCounterFromWbemObjectAccess(instance, L"Name");
		}

		/**
		 * @brief Query the `Name` property for an IWbemClassObject instance.
		 *
		 * @param[in] instance IWbemClassObject accessor to query.
		 * @return Variant containing the `Name` property.
		 */
		inline wil::unique_variant ctQueryInstanceName(_In_ IWbemClassObject *instance)
		{
			wil::unique_variant value;
			THROW_IF_FAILED(instance->Get(L"Name", 0, value.addressof(), nullptr, nullptr));
			return value;
		}

		/**
		 * @enum CallbackAction
		 * @brief Actions passed to registered counter callbacks.
		 */
		enum class CallbackAction : std::uint8_t
		{
			Start,
			Stop,
			Update,
			Clear
		};

		using ctPerformanceCounterCallback = std::function<void(CallbackAction)>;
	} // unnamed namespace

	// forward-declaration to reference ctPerformanceCounter
	class ctPerformanceCounter;

	// class ctPerformanceCounterCounter
	// - The abstract base class contains the WMI-specific code which all templated instances will derive from
	// - Using public inheritance + protected members over composition as we need a common type which we can pass to
	//   ctPerformanceCounter
	// - Exposes the iterator class for users to traverse the data points gathered
	//
	// Note: callers *MUST* guarantee connections with the WMI service stay connected
	//       for the lifetime of this object [e.g. guaranteed ctWmiService is instantiated]
	// Note: callers *MUST* guarantee that COM is CoInitialized on this thread before calling
	// Note: the ctPerformanceCounter class *will* retain WMI service instance
	//       it's recommended to guarantee it stays alive
	/**
	 * @class ctPerformanceCounterCounter
	 * @brief Abstract base type representing an aggregator for one named counter property.
	 *
	 * Concrete implementations perform WMI-specific refreshes and call into
	 * the base class to add per-instance samples. Users may register filters
	 * and obtain iterator ranges for a tracked instance using `reference_range`.
	 *
	 * @tparam T Sample storage type (for example `ULONG` or `ULONGLONG`).
	 */
	template <typename T>
	class ctPerformanceCounterCounter
	{
	public:
		/**
		 * @class iterator
		 * @brief Lightweight forward iterator over stored time-slices.
		 *
		 * This iterator wraps a `std::vector<T>::const_iterator` and provides
		 * basic checked traversal operations used by `reference_range`.
		 */
		class iterator
		{
		public:
			// iterator_traits - allows <algorithm> functions to be used
			using iterator_category = std::forward_iterator_tag;
			using value_type = T;
			using difference_type = size_t;
			using pointer = T *;
			using reference = T &;

			explicit iterator(typename std::vector<T>::const_iterator &&instance) noexcept : m_current(std::move(instance)), m_isEmpty(false)
			{
			}

			iterator() = default;
			~iterator() noexcept = default;

			iterator(iterator &&i) noexcept : m_current(std::move(i.m_current)),
											  m_isEmpty(i.m_isEmpty)
			{
			}

			iterator &operator=(iterator &&i) noexcept
			{
				m_current = std::move(i.m_current);
				m_isEmpty = i.m_isEmpty;
				return *this;
			}

			iterator(const iterator &i) noexcept : m_current(i.m_current),
												   m_isEmpty(i.m_isEmpty)
			{
			}

			iterator &operator=(const iterator &i) noexcept // NOLINT(cert-oop54-cpp)
			{
				iterator localCopy(i);
				*this = std::move(localCopy);
				return *this;
			}

			const T &operator*() const
			{
				if (m_isEmpty)
				{
					throw std::runtime_error(
						"ctPerformanceCounterCounter::iterator : dereferencing an iterator referencing an empty container");
				}
				return *m_current;
			}

			bool operator==(const iterator &iter) const noexcept
			{
				if (m_isEmpty || iter.m_isEmpty)
				{
					return m_isEmpty == iter.m_isEmpty;
				}
				return m_current == iter.m_current;
			}

			bool operator!=(const iterator &iter) const noexcept
			{
				return !(*this == iter);
			}

			// pre-increment
			iterator &operator++()
			{
				if (m_isEmpty)
				{
					throw std::runtime_error(
						"ctPerformanceCounterCounter::iterator : pre-incrementing an iterator referencing an empty container");
				}
				++m_current;
				return *this;
			}

			// post-increment
			iterator operator++(int)
			{
				if (m_isEmpty)
				{
					throw std::runtime_error(
						"ctPerformanceCounterCounter::iterator : post-incrementing an iterator referencing an empty container");
				}
				iterator temp(*this);
				++m_current;
				return temp;
			}

			// increment by integer
			iterator &operator+=(size_t inc)
			{
				if (m_isEmpty)
				{
					throw std::runtime_error(
						"ctPerformanceCounterCounter::iterator : post-incrementing an iterator referencing an empty container");
				}
				for (size_t loop = 0; loop < inc; ++loop)
				{
					++m_current;
				}
				return *this;
			}

		private:
			typename std::vector<T>::const_iterator m_current;
			bool m_isEmpty = true;
		};

		/**
		 * @brief Construct an aggregator for a named counter property.
		 *
		 * @param[in] counterName Wide string property name to sample from WMI objects.
		 * @param[in] collectionType Storage strategy for samples.
		 */
		ctPerformanceCounterCounter(_In_ PCWSTR counterName, const ctPerformanceCounterCollectionType collectionType) : m_collectionType(collectionType),
																														m_counterName(counterName)
		{
			m_refresher = wil::CoCreateInstance<WbemRefresher, IWbemRefresher>();
			m_configRefresher = m_refresher.query<IWbemConfigureRefresher>();
		}

		virtual ~ctPerformanceCounterCounter() noexcept = default;

		ctPerformanceCounterCounter(const ctPerformanceCounterCounter &) = delete;
		ctPerformanceCounterCounter &operator=(const ctPerformanceCounterCounter &) = delete;
		ctPerformanceCounterCounter(ctPerformanceCounterCounter &&) = delete;
		ctPerformanceCounterCounter &operator=(ctPerformanceCounterCounter &&) = delete;

		//
		// *not* thread-safe: caller must guarantee sequential access to add_filter()
		//
		/**
		 * @brief Add an instance filter limiting which instances are tracked.
		 *
		 * The filter restricts collection to instances whose `counterName` property
		 * equals `propertyValue`. Callers must ensure counters are stopped when
		 * invoking this method.
		 *
		 * @tparam V Variant-compatible value type for comparison.
		 * @param[in] counterName Property name to compare on each instance.
		 * @param[in] propertyValue Expected property value to match.
		 */
		template <typename V>
		void add_filter(_In_ PCWSTR counterName, V propertyValue)
		{
			FAIL_FAST_IF_MSG(
				!m_dataStopped,
				"ctPerformanceCounterCounter: must call stop_all_counters on the ctPerformanceCounter class containing this counter");
			m_instanceFilter.emplace_back(counterName, std::move(ctWmiMakeVariant(propertyValue)));
		}

		//
		// returns a pair<begin,end> of iterators that exposes data for each time-slice
		// - static classes will have a null instance name
		//
		/**
		 * @brief Obtain iterators referencing the sampled time-slices for an instance.
		 *
		 * @param[in,opt] instanceName Wide-string instance name to retrieve samples for,
		 * or `nullptr` to match the first stored instance (used for static classes).
		 * @return Pair of `iterator` (begin,end) for the stored samples. Empty pair
		 * indicates no matching instance.
		 */
		std::pair<iterator, iterator> reference_range(_In_opt_ PCWSTR instanceName = nullptr)
		{
			FAIL_FAST_IF_MSG(
				!m_dataStopped,
				"ctPerformanceCounterCounter: must call stop_all_counters on the ctPerformanceCounter class containing this counter");

			const auto lock = m_guardCounterData.lock();
			const auto foundInstance = std::ranges::find_if(
				m_counterData,
				[&](const auto &instance)
				{
					return instance->match(instanceName);
				});
			if (std::end(m_counterData) == foundInstance)
			{
				// nothing matching that instance name
				// return the end iterator (default constructor == end)
				return std::pair<iterator, iterator>(iterator(), iterator());
			}

			const std::unique_ptr<details::ctPerformanceCounterCounterData<T>> &instanceReference = *foundInstance;
			return std::pair<iterator, iterator>(instanceReference->begin(), instanceReference->end());
		}

	private:
		//
		// private structure to track the 'filter' which instances to track
		//
		/**
		 * @struct ctPerformanceCounterInstanceFilter
		 * @brief Encapsulates a property/value filter for selecting which instances to track.
		 *
		 * The filter compares the named property on an accessor instance against the
		 * stored `m_propertyValue`. Comparison operators are provided for both
		 * `IWbemObjectAccess` and `IWbemClassObject` accessor types.
		 */
		struct ctPerformanceCounterInstanceFilter
		{
			const std::wstring m_counterName;
			const wil::unique_variant m_propertyValue;

			ctPerformanceCounterInstanceFilter(_In_ PCWSTR counterName, wil::unique_variant &&propertyValue) : m_counterName(counterName),
																											   m_propertyValue(std::move(propertyValue))
			{
			}

			~ctPerformanceCounterInstanceFilter() = default;

			ctPerformanceCounterInstanceFilter(const ctPerformanceCounterInstanceFilter &rhs) = delete;
			ctPerformanceCounterInstanceFilter &operator=(const ctPerformanceCounterInstanceFilter &rhs) = delete;

			ctPerformanceCounterInstanceFilter(ctPerformanceCounterInstanceFilter &&rhs) noexcept : m_counterName(std::move(*const_cast<std::wstring *>(&rhs.m_counterName))),
																									m_propertyValue(std::move(*const_cast<wil::unique_variant *>(&rhs.m_propertyValue)))
			{
			}

			ctPerformanceCounterInstanceFilter &operator=(ctPerformanceCounterInstanceFilter &&) noexcept = delete;

			/**
			 * @brief Compare filter against an `IWbemObjectAccess` instance.
			 *
			 * @param[in] instance IWbemObjectAccess to compare against.
			 * @return true when the instance's property equals the stored value.
			 */
			bool operator==(_In_ IWbemObjectAccess *instance) const
			{
				return m_propertyValue == details::ReadCounterFromWbemObjectAccess(instance, m_counterName.c_str());
			}

			bool operator!=(_In_ IWbemObjectAccess *instance) const
			{
				return !(*this == instance);
			}

			/**
			 * @brief Compare filter against an `IWbemClassObject` instance.
			 *
			 * @param[in] instance IWbemClassObject to compare against.
			 * @return true when the instance's property equals the stored value.
			 */
			bool operator==(_In_ IWbemClassObject *instance) const
			{
				wil::unique_variant value;
				THROW_IF_FAILED(instance->Get(m_counterName.c_str(), 0, value.addressof(), nullptr, nullptr));
				if (value.vt == VT_NULL)
				{
					return false;
				}

				FAIL_FAST_IF_MSG(
					value.vt != m_propertyValue.vt,
					"VARIANT types do not match to make a comparison : Counter name '%ws', retrieved type '%u', expected type '%u'",
					m_counterName.c_str(), value.vt, m_propertyValue.vt);

				return m_propertyValue == value;
			}

			bool operator!=(_In_ IWbemClassObject *instance) const
			{
				return !(*this == instance);
			}
		};

		const ctPerformanceCounterCollectionType m_collectionType;
		const std::wstring m_counterName;
		wil::com_ptr<IWbemRefresher> m_refresher;
		wil::com_ptr<IWbemConfigureRefresher> m_configRefresher;
		std::vector<ctPerformanceCounterInstanceFilter> m_instanceFilter;
		// Must lock access to counter_data
		mutable wil::critical_section m_guardCounterData{500};
		std::vector<std::unique_ptr<details::ctPerformanceCounterCounterData<T>>> m_counterData;
		bool m_dataStopped = true;

	protected:
		virtual void update_counter_data() = 0;

		// ctPerformanceCounter needs private access to invoke register_callback in the derived type
		friend class ctPerformanceCounter;

		/**
		 * @brief Register a callback used by `ctPerformanceCounter` to drive
		 * sampling lifecycle events.
		 *
		 * The returned callback accepts `CallbackAction` values and will invoke
		 * the appropriate internal behavior (start/stop/update/clear).
		 *
		 * @return Function to be invoked by the manager.
		 */
		details::ctPerformanceCounterCallback register_callback()
		{
			// the callback function must be no-except - it can't leak an exception to the caller
			// as it shouldn't break calling all other callbacks if one happens to fail an update
			return [this](const details::CallbackAction updateData) noexcept
			{
				try
				{
					switch (updateData)
					{
					case details::CallbackAction::Start:
						update_counter_data();
						m_dataStopped = false;
						break;

					case details::CallbackAction::Stop:
						m_dataStopped = true;
						break;

					case details::CallbackAction::Update:
						// only the derived class has appropriate the accessor class to update the data
						update_counter_data();
						break;

					case details::CallbackAction::Clear:
					{
						FAIL_FAST_IF_MSG(
							!m_dataStopped,
							"ctPerformanceCounterCounter: must call stop_all_counters on the ctPerformanceCounter class containing this counter");

						const auto lock = m_guardCounterData.lock();
						for (auto &counterData : m_counterData)
						{
							counterData->clear();
						}
						break;
					}
					}
				}
				CATCH_LOG()
				// if failed to update the counter data this pass
				// will try again the next timer callback
			};
		}

		//
		// the derived classes need to use the same refresher object
		//
		wil::com_ptr<IWbemConfigureRefresher> access_refresher() const noexcept
		{
			return m_configRefresher;
		}

		//
		// this function is how one looks to see if the data machines requires knowing how to access the data from that specific WMI class
		// - it's also how to access the data is captured with the TAccess template type
		//
		void add_instance(_In_ IWbemObjectAccess *instance)
		{
			bool fAddData = m_instanceFilter.empty();
			if (!fAddData)
			{
				fAddData = std::any_of(
					std::cbegin(m_instanceFilter),
					std::cend(m_instanceFilter),
					[&](const auto &filter)
					{
						return filter == instance;
					});
			}

			// add the counter data for this instance if:
			// - have no filters [not filtering instances at all]
			// - matches at least one filter
			if (fAddData)
			{
				wil::unique_variant instanceName = details::ctQueryInstanceName(instance);
				// this would be odd but technically possible
				// - if this isn't a static class and this instance currently doesn't have a name
				if (instanceName.vt == VT_NULL)
				{
					return;
				}

				const auto lock = m_guardCounterData.lock();
				const auto trackedInstance = std::ranges::find_if(
					m_counterData,
					[&](const auto &counterData)
					{
						return counterData->match(instanceName.bstrVal);
					});

				// if this instance of this counter is new [new unique instance for this counter]
				// - we must add a new ctPerformanceCounterCounterData to the parent's counter_data vector
				// else
				// - we just add this counter value to the already-tracked instance
				if (trackedInstance == std::end(m_counterData))
				{
					m_counterData.push_back(
						std::make_unique<details::ctPerformanceCounterCounterData<T>>(
							m_collectionType, instance, m_counterName.c_str()));
					(*m_counterData.rbegin())->add(instance);
				}
				else
				{
					(*trackedInstance)->add(instance);
				}
			}
		}

		void add_instance(_In_ IWbemClassObject *instance)
		{
			// static WMI objects have only one instance
			const auto lock = m_guardCounterData.lock();
			if (m_counterData.empty())
			{
				m_counterData.push_back(
					std::make_unique<details::ctPerformanceCounterCounterData<T>>(
						m_collectionType, instance, m_counterName.c_str()));
			}

			m_counterData[0]->add(instance);
		}
	};

	// ctPerformanceCounterCounterImpl
	// - derived from the pure-virtual ctPerformanceCounterCounter class
	//   shares the same data type template typename with the parent class
	//
	// Template typename details:
	//
	// - TEnum: the IWbem* type to refresh the performance data
	// - TAccess: the IWbem* type used to access the performance data once refreshed
	// - TData: the data type of the counter of the class specified in the c'tor
	//
	// Only 2 combinations currently supported:
	// : ctPerformanceCounterCounter<IWbemHiPerfEnum, IWbemObjectAccess, TData>
	//   - refreshes N of instances of a counter
	// : ctPerformanceCounterCounter<IWbemClassObject, IWbemClassObject, TData>
	//   - refreshes a single instance of a counter
	template <typename TEnum, typename TAccess, typename TData>
	class ctPerformanceCounterCounterImpl final : public ctPerformanceCounterCounter<TData>
	{
	public:
		ctPerformanceCounterCounterImpl(
			const ctWmiService &wmi,
			_In_ PCWSTR className,
			_In_ PCWSTR counterName,
			const ctPerformanceCounterCollectionType collectionType) : ctPerformanceCounterCounter<TData>(counterName, collectionType),
																	   m_accessor(wmi, this->access_refresher(), className)
		// must qualify 'this' name lookup to access access_refresher since it's in the base class
		{
		}

		~ctPerformanceCounterCounterImpl() override = default;

		// non-copyable
		ctPerformanceCounterCounterImpl(const ctPerformanceCounterCounterImpl &) = delete;
		ctPerformanceCounterCounterImpl &operator=(const ctPerformanceCounterCounterImpl &) = delete;
		ctPerformanceCounterCounterImpl(ctPerformanceCounterCounterImpl &&) = delete;
		ctPerformanceCounterCounterImpl &operator=(ctPerformanceCounterCounterImpl &&) = delete;

	private:
		//
		// this concrete template class serves to capture the Enum and Access template types
		// - so can instantiate the appropriate accessor object
		details::ctPerformanceCounterDataAccessor<TEnum, TAccess> m_accessor;

		//
		// invoked from the parent class to add data matching any/all filters
		//
		// private function required to be implemented from the abstract base class
		// - concrete class must pass back a function callback for adding data points for the specified counter
		//
		/**
		 * @brief Refresh the WMI accessor and add samples for returned instances.
		 *
		 * This implementation initializes COM on the calling thread, refreshes
		 * the accessor, then iterates instances invoking the base-class
		 * `add_instance` helper.
		 */
		void update_counter_data() override
		{
			const auto coInit = wil::CoInitializeEx();

			// refresh this hi-perf object to get the current values
			// requires the invoker serializes all calls
			m_accessor.refresh();

			// the accessor object exposes begin() / end() to allow access to instances
			// - of the specified hi-performance counter
			for (const auto &instance : m_accessor)
			{
				// must qualify this name lookup to access add_instance since it's in the base class
				this->add_instance(instance);
			}
		}
	};

	// ctPerformanceCounter
	//
	// class to register for and collect performance counters
	// - captures counter data into the ctPerformanceCounterCounter objects passed through add()
	//
	// CAUTION:
	// - do not access the ctPerformanceCounterCounter instances while between calling start() and stop()
	// - any iterators returned can be invalidated when more data is added on the next cycle
	/**
	 * @brief Manager that schedules periodic sampling of registered counters.
	 *
	 * `ctPerformanceCounter` holds a WMI refresher and a list of registered
	 * counter callbacks. It drives periodic `Refresh` calls on the refresher
	 * and notifies each registered counter to update or clear collected data.
	 */
	class ctPerformanceCounter final
	{
	public:
		/**
		 * @brief Create a manager bound to a WMI service used to register counters.
		 *
		 * @param[in] wmiService Initialized `ctWmiService` representing the WMI namespace.
		 */
		explicit ctPerformanceCounter(ctWmiService wmiService) : m_wmiService(std::move(wmiService))
		{
			m_lockedData = std::make_unique<LockedData>();
			m_refresher = wil::CoCreateInstance<WbemRefresher, IWbemRefresher>();
			m_configRefresher = m_refresher.query<IWbemConfigureRefresher>();
		}

		~ctPerformanceCounter() noexcept
		{
			// when being destroyed, don't invoke callbacks
			// the counters might be destroyed, and we don't hold a ref on them
			if (m_lockedData)
			{
				auto lock = m_lockedData->m_lock.lock();
				m_lockedData->m_countersStarted = false;
			}
			m_timer.reset();
		}

		/**
		 * @brief Register a counter aggregator with the manager.
		 *
		 * The manager stores a callback for the counter and adds its refresher to
		 * the global config refresher.
		 *
		 * @tparam T Sample type of the counter object.
		 * @param[in] perfCounterObject Shared pointer to the aggregator to register.
		 */
		template <typename T>
		void add_counter(const std::shared_ptr<ctPerformanceCounterCounter<T>> &perfCounterObject)
		{
			m_callbacks.push_back(perfCounterObject->register_callback());
			auto revertCallback = wil::scope_exit([&]() noexcept
												  { m_callbacks.pop_back(); });

			THROW_IF_FAILED(m_configRefresher->AddRefresher(perfCounterObject->m_refresher.get(), 0, nullptr));
			// dismiss scope-guard - successfully added refresher
			revertCallback.release();
		}

		/**
		 * @brief Start periodic sampling for all registered counters.
		 *
		 * @param[in] interval Sampling interval in milliseconds.
		 */
		void start_all_counters(uint32_t interval)
		{
			if (!m_timer)
			{
				m_timer.reset(CreateThreadpoolTimer(TimerCallback, this, nullptr));
				THROW_LAST_ERROR_IF(!m_timer);
			}

			for (auto &callback : m_callbacks)
			{
				callback(details::CallbackAction::Start);
			}

			{
				auto lock = m_lockedData->m_lock.lock();
				m_lockedData->m_countersStarted = true;
				m_timerIntervalMs = interval;
				FILETIME relativeTimeout = wil::filetime::from_int64(
					-1 * wil::filetime_duration::one_millisecond * m_timerIntervalMs);
				SetThreadpoolTimer(m_timer.get(), &relativeTimeout, 0, 0);
			}
		}

		// no-throw / no-fail
		/**
		 * @brief Stop sampling and cancel any scheduled timers.
		 *
		 * This method is noexcept and will not throw on failure.
		 */
		void stop_all_counters() noexcept
		{
			{
				auto lock = m_lockedData->m_lock.lock();
				m_lockedData->m_countersStarted = false;
			}

			m_timer.reset();

			for (auto &callback : m_callbacks)
			{
				callback(details::CallbackAction::Stop);
			}
		}

		// no-throw / no-fail
		/**
		 * @brief Clear all stored counter data from registered aggregators.
		 *
		 * This is a best-effort, noexcept operation.
		 */
		void clear_counter_data() const noexcept
		{
			for (const auto &callback : m_callbacks)
			{
				callback(details::CallbackAction::Clear);
			}
		}

		/**
		 * @brief Remove all registered counters and reset the internal refresher.
		 */
		void reset_counters()
		{
			m_callbacks.clear();

			// release this Refresher and ConfigRefresher, so future counters will be added cleanly
			m_refresher = wil::CoCreateInstance<WbemRefresher, IWbemRefresher>();
			m_configRefresher = m_refresher.query<IWbemConfigureRefresher>();
		}

		// non-copyable
		ctPerformanceCounter(const ctPerformanceCounter &) = delete;
		ctPerformanceCounter &operator=(const ctPerformanceCounter &) = delete;
		ctPerformanceCounter &operator=(ctPerformanceCounter &&rhs) noexcept = delete;

		// movable
		ctPerformanceCounter(ctPerformanceCounter &&rhs) noexcept = default;

	private:
		ctWmiService m_wmiService;
		wil::com_ptr<IWbemRefresher> m_refresher;
		wil::com_ptr<IWbemConfigureRefresher> m_configRefresher;
		// for each interval, callback each of the registered aggregators
		std::vector<details::ctPerformanceCounterCallback> m_callbacks;
		uint32_t m_timerIntervalMs{};
		// timer to fire to indicate when to Refresh the data
		// declare last to guarantee will be destroyed first
		wil::unique_threadpool_timer m_timer;

		// must dynamically allocate this as the critical_section isn't movable
		// and the ctPerformanceCounter objects must be movable
		struct LockedData
		{
			wil::critical_section m_lock{500};
			bool m_countersStarted = false;
		};

		std::unique_ptr<LockedData> m_lockedData;

		static void NTAPI TimerCallback(PTP_CALLBACK_INSTANCE, PVOID pContext, PTP_TIMER) noexcept
		{
			const auto *pThis = static_cast<ctPerformanceCounter *>(pContext);
			try
			{
				// must guarantee COM is initialized on this thread
				const auto com = wil::CoInitializeEx();
				pThis->m_refresher->Refresh(0);
				for (const auto &callback : pThis->m_callbacks)
				{
					callback(details::CallbackAction::Update);
				}
			}
			catch (...)
			{
				// best-effort to update the caller with the data from this time-slice
			}

			auto lock = pThis->m_lockedData->m_lock.lock();
			if (pThis->m_lockedData->m_countersStarted)
			{
				FILETIME relativeTimeout = wil::filetime::from_int64(
					-1 * wil::filetime_duration::one_millisecond * pThis->m_timerIntervalMs);
				SetThreadpoolTimer(pThis->m_timer.get(), &relativeTimeout, 0, 0);
			}
		}
	};

	/**
	 * @enum ctWmiEnumClassType
	 * @brief Indicates whether a WMI performance class is static or instance-based.
	 *
	 * - `Static`: the WMI class exposes a single (static) instance accessed via `IWbemClassObject`.
	 * - `Instance`: the WMI class exposes multiple instances and is enumerated via `IWbemHiPerfEnum`.
	 */
	enum class ctWmiEnumClassType : std::uint8_t
	{
		Uninitialized,
		// created with ctMakeStaticPerfCounter
		Static,
		// created with ctMakeInstancePerfCounter
		Instance
	};

	/**
	 * @enum ctWmiEnumClassName
	 * @brief Well-known identifiers for supported WMI performance classes.
	 *
	 * These symbolic names map to specific WMI provider class names stored
	 * in the `ctPerformanceCounterDetails::c_performanceCounterPropertiesArray`.
	 */
	enum class ctWmiEnumClassName : std::uint8_t
	{
		Uninitialized,
		Process,
		Processor,
		Memory,
		NetworkAdapter,
		NetworkInterface,
		TcpipDiagnostics,
		TcpipIpv4,
		TcpipIpv6,
		TcpipTcpv4,
		TcpipTcpv6,
		TcpipUdpv4,
		TcpipUdpv6,
		WinsockBsp,
		WfpFilter,
		WfpFilterCount,
	};

	/**
	 * @struct ctPerformanceCounterCounterProperties
	 * @brief Compile-time descriptor of a WMI performance counter class.
	 *
	 * Instances of this structure describe the provider class name, whether the
	 * class is static or instance-backed, and lists of property names grouped by
	 * their expected storage type (`ULONG`, `ULONGLONG`, and string types).
	 */
	struct ctPerformanceCounterCounterProperties
	{
		/** @brief Whether the provider exposes a single static instance or multiple instances. */
		const ctWmiEnumClassType m_classType = ctWmiEnumClassType::Uninitialized;
		/** @brief Enumerated identifier for the well-known class. */
		const ctWmiEnumClassName m_className = ctWmiEnumClassName::Uninitialized;
		/** @brief The WMI provider class name (for example, L"Win32_PerfFormattedData_..."). */
		const wchar_t *m_providerName = nullptr;

		/** @brief Number of `ULONG` property names in `m_ulongFieldNames`. */
		const uint32_t m_ulongFieldNameCount = 0;
		/** @brief Array of `ULONG` property names. */
		const wchar_t **m_ulongFieldNames = nullptr;

		/** @brief Number of `ULONGLONG` property names in `m_ulonglongFieldNames`. */
		const uint32_t m_ulonglongFieldNameCount = 0;
		/** @brief Array of `ULONGLONG` property names. */
		const wchar_t **m_ulonglongFieldNames = nullptr;

		/** @brief Number of string property names in `m_stringFieldNames`. */
		const uint32_t m_stringFieldNameCount = 0;
		/** @brief Array of string property names. */
		const wchar_t **m_stringFieldNames = nullptr;

		/**
		 * @brief Determine whether a property name exists for the requested storage type.
		 *
		 * @tparam T The storage type to check for (for example `ULONG`, `ULONGLONG`, or `std::wstring`).
		 * @param[in] name Property name to look up.
		 * @return true if the property name is known for the requested type; false otherwise.
		 */
		template <typename T>
		bool PropertyNameExists(_In_ PCWSTR name) const noexcept;
	};

	template <>
	inline bool ctPerformanceCounterCounterProperties::PropertyNameExists<ULONG>(_In_ PCWSTR name) const noexcept
	// NOLINT(bugprone-exception-escape)
	{
		for (auto counter = 0ul; counter < m_ulongFieldNameCount; ++counter)
		{
			if (wil::compare_string_ordinal(name, m_ulongFieldNames[counter], true) == wistd::weak_ordering::equivalent)
			{
				return true;
			}
		}

		return false;
	}

	template <>
	inline bool ctPerformanceCounterCounterProperties::PropertyNameExists<ULONGLONG>(_In_ PCWSTR name) const noexcept
	// NOLINT(bugprone-exception-escape)
	{
		for (auto counter = 0ul; counter < m_ulonglongFieldNameCount; ++counter)
		{
			if (wil::compare_string_ordinal(name, m_ulonglongFieldNames[counter], true) == wistd::weak_ordering::equivalent)
			{
				return true;
			}
		}

		return false;
	}

	template <>
	inline bool ctPerformanceCounterCounterProperties::PropertyNameExists<std::wstring>(_In_ PCWSTR name) const noexcept
	// NOLINT(bugprone-exception-escape)
	{
		for (auto counter = 0ul; counter < m_stringFieldNameCount; ++counter)
		{
			if (wil::compare_string_ordinal(name, m_stringFieldNames[counter], true) == wistd::weak_ordering::equivalent)
			{
				return true;
			}
		}

		return false;
	}

	template <>
	inline bool ctPerformanceCounterCounterProperties::PropertyNameExists<wil::unique_bstr>(_In_ PCWSTR name) const noexcept
	// NOLINT(bugprone-exception-escape)
	{
		for (auto counter = 0ul; counter < m_stringFieldNameCount; ++counter)
		{
			if (wil::compare_string_ordinal(name, m_stringFieldNames[counter], true) == wistd::weak_ordering::equivalent)
			{
				return true;
			}
		}

		return false;
	}

	/**
	 * @namespace ctPerformanceCounterDetails
	 * @brief Internal compile-time lists and provider names for supported counters.
	 *
	 * This namespace contains provider class name constants and arrays of
	 * property names used by `ctCreatePerfCounter` to validate requested
	 * counters at compile-time.
	 */
	namespace ctPerformanceCounterDetails
	{
		// ReSharper disable StringLiteralTypo
		inline const wchar_t *g_commonStringPropertyNames[]{
			L"Caption",
			L"Description",
			L"Name"};

		inline const wchar_t *g_memoryCounter = L"Win32_PerfFormattedData_PerfOS_Memory";
		inline const wchar_t *g_memoryUlongCounterNames[]{
			L"CacheFaultsPerSec",
			L"DemandZeroFaultsPerSec",
			L"FreeSystemPageTableEntries",
			L"PageFaultsPerSec",
			L"PageReadsPerSec",
			L"PagesInputPerSec",
			L"PagesOutputPerSec",
			L"PagesPerSec",
			L"PageWritesPerSec",
			L"PercentCommittedBytesInUse",
			L"PoolNonpagedAllocs",
			L"PoolPagedAllocs",
			L"TransitionFaultsPerSec",
			L"WriteCopiesPerSec"};
		inline const wchar_t *g_memoryUlonglongCounterNames[]{
			L"AvailableBytes",
			L"AvailableKBytes",
			L"AvailableMBytes",
			L"CacheBytes",
			L"CacheBytesPeak",
			L"CommitLimit",
			L"CommittedBytes",
			L"Frequency_Object",
			L"Frequency_PerfTime",
			L"Frequency_Sys100NS",
			L"PoolNonpagedBytes",
			L"PoolPagedBytes",
			L"PoolPagedResidentBytes",
			L"SystemCacheResidentBytes",
			L"SystemCodeResidentBytes",
			L"SystemCodeTotalBytes",
			L"SystemDriverResidentBytes",
			L"SystemDriverTotalBytes",
			L"Timestamp_Object",
			L"Timestamp_PerfTime",
			L"Timestamp_Sys100NS"};

		inline const wchar_t *g_processorInformationCounter = L"Win32_PerfFormattedData_Counters_ProcessorInformation";
		inline const wchar_t *g_processorInformationUlongCounterNames[]{
			L"ClockInterruptsPersec",
			L"DPCRate",
			L"DPCsQueuedPersec",
			L"InterruptsPersec",
			L"ParkingStatus",
			L"PercentofMaximumFrequency",
			L"PercentPerformanceLimit",
			L"PerformanceLimitFlags",
			L"ProcessorFrequency",
			L"ProcessorStateFlags"};
		inline const wchar_t *g_processorInformationUlonglongCounterNames[]{
			L"AverageIdleTime",
			L"C1TransitionsPerSec",
			L"C2TransitionsPerSec",
			L"C3TransitionsPerSec",
			L"IdleBreakEventsPersec",
			L"PercentC1Time",
			L"PercentC2Time",
			L"PercentC3Time",
			L"PercentDPCTime",
			L"PercentIdleTime",
			L"PercentInterruptTime",
			L"PercentPriorityTime",
			L"PercentPrivilegedTime",
			L"PercentPrivilegedUtility",
			L"PercentProcessorPerformance",
			L"PercentProcessorTime",
			L"PercentProcessorUtility",
			L"PercentUserTime",
			L"Timestamp_Object",
			L"Timestamp_PerfTime",
			L"Timestamp_Sys100NS"};

		inline const wchar_t *g_perfProcProcessCounter = L"Win32_PerfFormattedData_PerfProc_Process";
		inline const wchar_t *g_perfProcProcessUlongCounterNames[]{
			L"CreatingProcessID",
			L"HandleCount",
			L"IDProcess",
			L"PageFaultsPerSec",
			L"PoolNonpagedBytes",
			L"PoolPagedBytes",
			L"PriorityBase",
			L"ThreadCount"};
		inline const wchar_t *g_perfProcProcessUlonglongCounterNames[]{
			L"ElapsedTime",
			L"Frequency_Object",
			L"Frequency_PerfTime",
			L"Frequency_Sys100NS",
			L"IODataBytesPerSec",
			L"IODataOperationsPerSec",
			L"IOOtherBytesPerSec",
			L"IOOtherOperationsPerSec",
			L"IOReadBytesPerSec",
			L"IOReadOperationsPerSec",
			L"IOWriteBytesPerSec",
			L"IOWriteOperationsPerSec",
			L"PageFileBytes",
			L"PageFileBytesPeak",
			L"PercentPrivilegedTime",
			L"PercentProcessorTime",
			L"PercentUserTime",
			L"PrivateBytes",
			L"Timestamp_Object",
			L"Timestamp_PerfTime",
			L"Timestamp_Sys100NS",
			L"VirtualBytes",
			L"VirtualBytesPeak",
			L"WorkingSet",
			L"WorkingSetPeak"};

		inline const wchar_t *g_tcpipNetworkAdapterCounter = L"Win32_PerfFormattedData_Tcpip_NetworkAdapter";
		inline const wchar_t *g_tcpipNetworkAdapterULongLongCounterNames[]{
			L"BytesReceivedPersec",
			L"BytesSentPersec",
			L"BytesTotalPersec",
			L"CurrentBandwidth",
			L"OffloadedConnections",
			L"OutputQueueLength",
			L"PacketsOutboundDiscarded",
			L"PacketsOutboundErrors",
			L"PacketsReceivedDiscarded",
			L"PacketsReceivedErrors",
			L"PacketsReceivedNonUnicastPersec",
			L"PacketsReceivedUnicastPersec",
			L"PacketsReceivedUnknown",
			L"PacketsReceivedPersec",
			L"PacketsSentNonUnicastPersec",
			L"PacketsSentUnicastPersec",
			L"PacketsSentPersec",
			L"PacketsPersec",
			L"TCPActiveRSCConnections",
			L"TCPRSCAveragePacketSize",
			L"TCPRSCCoalescedPacketsPersec",
			L"TCPRSCExceptionsPersec",
			L"Timestamp_Object",
			L"Timestamp_PerfTime",
			L"Timestamp_Sys100NS"};

		inline const wchar_t *g_tcpipNetworkInterfaceCounter = L"Win32_PerfFormattedData_Tcpip_NetworkInterface";
		inline const wchar_t *g_tcpipNetworkInterfaceULongLongCounterNames[]{
			L"BytesReceivedPerSec",
			L"BytesSentPerSec",
			L"BytesTotalPerSec",
			L"CurrentBandwidth",
			L"Frequency_Object",
			L"Frequency_PerfTime",
			L"Frequency_Sys100NS",
			L"OffloadedConnections",
			L"OutputQueueLength",
			L"PacketsOutboundDiscarded",
			L"PacketsOutboundErrors",
			L"PacketsPerSec",
			L"PacketsReceivedDiscarded",
			L"PacketsReceivedErrors",
			L"PacketsReceivedNonUnicastPerSec",
			L"PacketsReceivedPerSec",
			L"PacketsReceivedUnicastPerSec",
			L"PacketsReceivedUnknown",
			L"PacketsSentNonUnicastPerSec",
			L"PacketsSentPerSec",
			L"PacketsSentUnicastPerSec",
			L"TCPActiveRSCConnections",
			L"TCPRSCAveragePacketSize",
			L"TCPRSCCoalescedPacketsPersec",
			L"TCPRSCExceptionsPersec",
			L"Timestamp_Object",
			L"Timestamp_PerfTime",
			L"Timestamp_Sys100NS"};

		inline const wchar_t *g_tcpipIpv4Counter = L"Win32_PerfFormattedData_Tcpip_IPv4";
		inline const wchar_t *g_tcpipIpv6Counter = L"Win32_PerfFormattedData_Tcpip_IPv6";
		inline const wchar_t *g_tcpipIpULongCounterNames[]{
			L"DatagramsForwardedPersec",
			L"DatagramsOutboundDiscarded",
			L"DatagramsOutboundNoRoute",
			L"DatagramsReceivedAddressErrors",
			L"DatagramsReceivedDeliveredPersec",
			L"DatagramsReceivedDiscarded",
			L"DatagramsReceivedHeaderErrors",
			L"DatagramsReceivedUnknownProtocol",
			L"DatagramsReceivedPersec",
			L"DatagramsSentPersec",
			L"DatagramsPersec",
			L"FragmentReassemblyFailures",
			L"FragmentationFailures",
			L"FragmentedDatagramsPersec",
			L"FragmentsCreatedPersec",
			L"FragmentsReassembledPersec",
			L"FragmentsReceivedPersec"};

		inline const wchar_t *g_tcpipTcpv4Counter = L"Win32_PerfFormattedData_Tcpip_TCPv4";
		inline const wchar_t *g_tcpipTcpv6Counter = L"Win32_PerfFormattedData_Tcpip_TCPv6";
		inline const wchar_t *g_tcpipTcpULongCounterNames[]{
			L"ConnectionFailures",
			L"ConnectionsActive",
			L"ConnectionsEstablished",
			L"ConnectionsPassive",
			L"ConnectionsReset",
			L"SegmentsReceivedPersec",
			L"SegmentsRetransmittedPersec",
			L"SegmentsSentPersec",
			L"SegmentsPersec"};

		inline const wchar_t *g_tcpipUdpv4Counter = L"Win32_PerfFormattedData_Tcpip_UDPv4";
		inline const wchar_t *g_tcpipUdpv6Counter = L"Win32_PerfFormattedData_Tcpip_UDPv6";
		inline const wchar_t *g_tcpipUdpULongCounterNames[]{
			L"DatagramsNoPortPersec",
			L"DatagramsReceivedErrors",
			L"DatagramsReceivedPersec",
			L"DatagramsSentPersec",
			L"DatagramsPersec"};

		inline const wchar_t *g_tcpipPerformanceDiagnosticsCounter =
			L"Win32_PerfFormattedData_TCPIPCounters_TCPIPPerformanceDiagnostics";
		inline const wchar_t *g_tcpipPerformanceDiagnosticsULongCounterNames[]{
			L"Deniedconnectorsendrequestsinlowpowermode",
			L"IPv4NBLsindicatedwithlowresourceflag",
			L"IPv4NBLsindicatedwithoutprevalidation",
			L"IPv4NBLstreatedasnonprevalidated",
			L"IPv4NBLsPersecindicatedwithlowresourceflag",
			L"IPv4NBLsPersecindicatedwithoutprevalidation",
			L"IPv4NBLsPersectreatedasnonprevalidated",
			L"IPv4outboundNBLsnotprocessedviafastpath",
			L"IPv4outboundNBLsPersecnotprocessedviafastpath",
			L"IPv6NBLsindicatedwithlowresourceflag",
			L"IPv6NBLsindicatedwithoutprevalidation",
			L"IPv6NBLstreatedasnonprevalidated",
			L"IPv6NBLsPersecindicatedwithlowresourceflag",
			L"IPv6NBLsPersecindicatedwithoutprevalidation",
			L"IPv6NBLsPersectreatedasnonprevalidated",
			L"IPv6outboundNBLsnotprocessedviafastpath",
			L"IPv6outboundNBLsPersecnotprocessedviafastpath",
			L"TCPconnectrequestsfallenoffloopbackfastpath",
			L"TCPconnectrequestsPersecfallenoffloopbackfastpath",
			L"TCPinboundsegmentsnotprocessedviafastpath",
			L"TCPinboundsegmentsPersecnotprocessedviafastpath"};

		inline const wchar_t *g_microsoftWinsockBspCounter = L"Win32_PerfFormattedData_AFDCounters_MicrosoftWinsockBSP";
		inline const wchar_t *g_microsoftWinsockBspULongCounterNames[]{
			L"DroppedDatagrams",
			L"DroppedDatagramsPersec",
			L"RejectedConnections",
			L"RejectedConnectionsPersec"};

		inline const wchar_t *g_wfpFilterCounter = L"Win32_PerfFormattedData_Counters_WFP";
		inline const wchar_t *g_wfpFilterULongCounterNames[]{
			L"ProviderCount"};

		inline const wchar_t *g_wfpFilterCountCounter = L"Win32_PerfFormattedData_Counters_WFPFilterCount";
		// ReSharper disable once IdentifierTypo
		inline const wchar_t *g_wfpFilterCountULongLongCounterNames[]{
			L"FWPM_LAYER_INBOUND_IPPACKET_V4",
			L"FWPM_LAYER_INBOUND_IPPACKET_V4_DISCARD",
			L"FWPM_LAYER_INBOUND_IPPACKET_V6",
			L"FWPM_LAYER_INBOUND_IPPACKET_V6_DISCARD",
			L"FWPM_LAYER_OUTBOUND_IPPACKET_V4",
			L"FWPM_LAYER_OUTBOUND_IPPACKET_V4_DISCARD",
			L"FWPM_LAYER_OUTBOUND_IPPACKET_V6",
			L"FWPM_LAYER_OUTBOUND_IPPACKET_V6_DISCARD",
			L"FWPM_LAYER_IPFORWARD_V4",
			L"FWPM_LAYER_IPFORWARD_V4_DISCARD",
			L"FWPM_LAYER_IPFORWARD_V6",
			L"FWPM_LAYER_IPFORWARD_V6_DISCARD",
			L"FWPM_LAYER_INBOUND_TRANSPORT_V4",
			L"FWPM_LAYER_INBOUND_TRANSPORT_V4_DISCARD",
			L"FWPM_LAYER_INBOUND_TRANSPORT_V6",
			L"FWPM_LAYER_INBOUND_TRANSPORT_V6_DISCARD",
			L"FWPM_LAYER_OUTBOUND_TRANSPORT_V4",
			L"FWPM_LAYER_OUTBOUND_TRANSPORT_V4_DISCARD",
			L"FWPM_LAYER_OUTBOUND_TRANSPORT_V6",
			L"FWPM_LAYER_OUTBOUND_TRANSPORT_V6_DISCARD",
			L"FWPM_LAYER_STREAM_V4",
			L"FWPM_LAYER_STREAM_V4_DISCARD",
			L"FWPM_LAYER_STREAM_V6",
			L"FWPM_LAYER_STREAM_V6_DISCARD",
			L"FWPM_LAYER_DATAGRAM_DATA_V4",
			L"FWPM_LAYER_DATAGRAM_DATA_V4_DISCARD",
			L"FWPM_LAYER_DATAGRAM_DATA_V6",
			L"FWPM_LAYER_DATAGRAM_DATA_V6_DISCARD",
			L"FWPM_LAYER_INBOUND_ICMP_ERROR_V4",
			L"FWPM_LAYER_INBOUND_ICMP_ERROR_V4_DISCARD",
			L"FWPM_LAYER_INBOUND_ICMP_ERROR_V6",
			L"FWPM_LAYER_INBOUND_ICMP_ERROR_V6_DISCARD",
			L"FWPM_LAYER_OUTBOUND_ICMP_ERROR_V4",
			L"FWPM_LAYER_OUTBOUND_ICMP_ERROR_V4_DISCARD",
			L"FWPM_LAYER_OUTBOUND_ICMP_ERROR_V6",
			L"FWPM_LAYER_OUTBOUND_ICMP_ERROR_V6_DISCARD",
			L"FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V4",
			L"FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V4_DISCARD",
			L"FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V6",
			L"FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V6_DISCARD",
			L"FWPM_LAYER_ALE_AUTH_LISTEN_V4",
			L"FWPM_LAYER_ALE_AUTH_LISTEN_V4_DISCARD",
			L"FWPM_LAYER_ALE_AUTH_LISTEN_V6",
			L"FWPM_LAYER_ALE_AUTH_LISTEN_V6_DISCARD",
			L"FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4",
			L"FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4_DISCARD",
			L"FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6",
			L"FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6_DISCARD",
			L"FWPM_LAYER_ALE_AUTH_CONNECT_V4",
			L"FWPM_LAYER_ALE_AUTH_CONNECT_V4_DISCARD",
			L"FWPM_LAYER_ALE_AUTH_CONNECT_V6",
			L"FWPM_LAYER_ALE_AUTH_CONNECT_V6_DISCARD",
			L"FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4",
			L"FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4_DISCARD",
			L"FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6",
			L"FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6_DISCARD",
			L"FWPM_LAYER_INBOUND_MAC_FRAME_ETHERNET",
			L"FWPM_LAYER_OUTBOUND_MAC_FRAME_ETHERNET",
			L"FWPM_LAYER_INBOUND_MAC_FRAME_NATIVE",
			L"FWPM_LAYER_OUTBOUND_MAC_FRAME_NATIVE",
			L"FWPM_LAYER_NAME_RESOLUTION_CACHE_V4",
			L"FWPM_LAYER_NAME_RESOLUTION_CACHE_V6",
			L"FWPM_LAYER_ALE_RESOURCE_RELEASE_V4",
			L"FWPM_LAYER_ALE_RESOURCE_RELEASE_V6",
			L"FWPM_LAYER_ALE_ENDPOINT_CLOSURE_V4",
			L"FWPM_LAYER_ALE_ENDPOINT_CLOSURE_V6",
			L"FWPM_LAYER_ALE_CONNECT_REDIRECT_V4",
			L"FWPM_LAYER_ALE_CONNECT_REDIRECT_V6",
			L"FWPM_LAYER_ALE_BIND_REDIRECT_V4",
			L"FWPM_LAYER_ALE_BIND_REDIRECT_V6",
			L"FWPM_LAYER_STREAM_PACKET_V4",
			L"FWPM_LAYER_STREAM_PACKET_V6",
			L"FWPM_LAYER_INGRESS_VSWITCH_ETHERNET",
			L"FWPM_LAYER_EGRESS_VSWITCH_ETHERNET",
			L"FWPM_LAYER_INGRESS_VSWITCH_TRANSPORT_V4",
			L"FWPM_LAYER_INGRESS_VSWITCH_TRANSPORT_V6",
			L"FWPM_LAYER_EGRESS_VSWITCH_TRANSPORT_V4",
			L"FWPM_LAYER_EGRESS_VSWITCH_TRANSPORT_V6",
			L"FWPM_LAYER_INBOUND_TRANSPORT_FAST",
			L"FWPM_LAYER_OUTBOUND_TRANSPORT_FAST",
			L"FWPM_LAYER_INBOUND_MAC_FRAME_NATIVE_FAST",
			L"FWPM_LAYER_OUTBOUND_MAC_FRAME_NATIVE_FAST",
			L"FWPM_LAYER_INBOUND_RESERVED2",
			L"FWPM_LAYER_ALE_ACCEPT_REDIRECT_V4",
			L"FWPM_LAYER_ALE_ACCEPT_REDIRECT_V6",
			L"FWPM_LAYER_OUTBOUND_NETWORK_CONNECTION_POLICY_V4",
			L"FWPM_LAYER_OUTBOUND_NETWORK_CONNECTION_POLICY_V6",
			L"FWPM_LAYER_IPSEC_KM_DEMUX_V4",
			L"FWPM_LAYER_IPSEC_KM_DEMUX_V6",
			L"FWPM_LAYER_IPSEC_V4",
			L"FWPM_LAYER_IPSEC_V6",
			L"FWPM_LAYER_IKEEXT_V4",
			L"FWPM_LAYER_IKEEXT_V6",
			L"FWPM_LAYER_RPC_UM",
			L"FWPM_LAYER_RPC_EPMAP",
			L"FWPM_LAYER_RPC_EP_ADD",
			L"FWPM_LAYER_RPC_PROXY_CONN",
			L"FWPM_LAYER_RPC_PROXY_IF",
			L"FWPM_LAYER_KM_AUTHORIZATION",
			L"Total",
		};
		// ReSharper restore StringLiteralTypo

		// this patterns (const array of wchar_t* pointers)
		// allows for compile-time construction of the array of properties
		inline const ctPerformanceCounterCounterProperties c_performanceCounterPropertiesArray[]{

			{.m_classType = ctWmiEnumClassType::Static,
			 .m_className = ctWmiEnumClassName::Memory,
			 .m_providerName = g_memoryCounter,
			 .m_ulongFieldNameCount = ARRAYSIZE(g_memoryUlongCounterNames),
			 .m_ulongFieldNames = g_memoryUlongCounterNames,
			 .m_ulonglongFieldNameCount = ARRAYSIZE(g_memoryUlonglongCounterNames),
			 .m_ulonglongFieldNames = g_memoryUlonglongCounterNames,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},

			{.m_classType = ctWmiEnumClassType::Instance,
			 .m_className = ctWmiEnumClassName::Processor,
			 .m_providerName = g_processorInformationCounter,
			 .m_ulongFieldNameCount = ARRAYSIZE(g_processorInformationUlongCounterNames),
			 .m_ulongFieldNames = g_processorInformationUlongCounterNames,
			 .m_ulonglongFieldNameCount = ARRAYSIZE(g_processorInformationUlonglongCounterNames),
			 .m_ulonglongFieldNames = g_processorInformationUlonglongCounterNames,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},

			{.m_classType = ctWmiEnumClassType::Instance,
			 .m_className = ctWmiEnumClassName::Process,
			 .m_providerName = g_perfProcProcessCounter,
			 .m_ulongFieldNameCount = ARRAYSIZE(g_perfProcProcessUlongCounterNames),
			 .m_ulongFieldNames = g_perfProcProcessUlongCounterNames,
			 .m_ulonglongFieldNameCount = ARRAYSIZE(g_perfProcProcessUlonglongCounterNames),
			 .m_ulonglongFieldNames = g_perfProcProcessUlonglongCounterNames,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},

			{.m_classType = ctWmiEnumClassType::Instance,
			 .m_className = ctWmiEnumClassName::NetworkAdapter,
			 .m_providerName = g_tcpipNetworkAdapterCounter,
			 .m_ulongFieldNameCount = 0,
			 .m_ulongFieldNames = nullptr,
			 .m_ulonglongFieldNameCount = ARRAYSIZE(g_tcpipNetworkAdapterULongLongCounterNames),
			 .m_ulonglongFieldNames = g_tcpipNetworkAdapterULongLongCounterNames,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},

			{.m_classType = ctWmiEnumClassType::Instance,
			 .m_className = ctWmiEnumClassName::NetworkInterface,
			 .m_providerName = g_tcpipNetworkInterfaceCounter,
			 .m_ulongFieldNameCount = 0,
			 .m_ulongFieldNames = nullptr,
			 .m_ulonglongFieldNameCount = ARRAYSIZE(g_tcpipNetworkInterfaceULongLongCounterNames),
			 .m_ulonglongFieldNames = g_tcpipNetworkInterfaceULongLongCounterNames,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},

			{.m_classType = ctWmiEnumClassType::Static,
			 .m_className = ctWmiEnumClassName::TcpipIpv4,
			 .m_providerName = g_tcpipIpv4Counter,
			 .m_ulongFieldNameCount = ARRAYSIZE(g_tcpipIpULongCounterNames),
			 .m_ulongFieldNames = g_tcpipIpULongCounterNames,
			 .m_ulonglongFieldNameCount = 0,
			 .m_ulonglongFieldNames = nullptr,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},

			{.m_classType = ctWmiEnumClassType::Static,
			 .m_className = ctWmiEnumClassName::TcpipIpv6,
			 .m_providerName = g_tcpipIpv6Counter,
			 .m_ulongFieldNameCount = ARRAYSIZE(g_tcpipIpULongCounterNames),
			 .m_ulongFieldNames = g_tcpipIpULongCounterNames,
			 .m_ulonglongFieldNameCount = 0,
			 .m_ulonglongFieldNames = nullptr,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},

			{.m_classType = ctWmiEnumClassType::Static,
			 .m_className = ctWmiEnumClassName::TcpipTcpv4,
			 .m_providerName = g_tcpipTcpv4Counter,
			 .m_ulongFieldNameCount = ARRAYSIZE(g_tcpipTcpULongCounterNames),
			 .m_ulongFieldNames = g_tcpipTcpULongCounterNames,
			 .m_ulonglongFieldNameCount = 0,
			 .m_ulonglongFieldNames = nullptr,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},

			{.m_classType = ctWmiEnumClassType::Static,
			 .m_className = ctWmiEnumClassName::TcpipTcpv6,
			 .m_providerName = g_tcpipTcpv6Counter,
			 .m_ulongFieldNameCount = ARRAYSIZE(g_tcpipTcpULongCounterNames),
			 .m_ulongFieldNames = g_tcpipTcpULongCounterNames,
			 .m_ulonglongFieldNameCount = 0,
			 .m_ulonglongFieldNames = nullptr,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},

			{.m_classType = ctWmiEnumClassType::Static,
			 .m_className = ctWmiEnumClassName::TcpipUdpv4,
			 .m_providerName = g_tcpipUdpv4Counter,
			 .m_ulongFieldNameCount = ARRAYSIZE(g_tcpipUdpULongCounterNames),
			 .m_ulongFieldNames = g_tcpipUdpULongCounterNames,
			 .m_ulonglongFieldNameCount = 0,
			 .m_ulonglongFieldNames = nullptr,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},

			{.m_classType = ctWmiEnumClassType::Static,
			 .m_className = ctWmiEnumClassName::TcpipUdpv6,
			 .m_providerName = g_tcpipUdpv6Counter,
			 .m_ulongFieldNameCount = ARRAYSIZE(g_tcpipUdpULongCounterNames),
			 .m_ulongFieldNames = g_tcpipUdpULongCounterNames,
			 .m_ulonglongFieldNameCount = 0,
			 .m_ulonglongFieldNames = nullptr,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},

			{.m_classType = ctWmiEnumClassType::Static,
			 .m_className = ctWmiEnumClassName::TcpipDiagnostics,
			 .m_providerName = g_tcpipPerformanceDiagnosticsCounter,
			 .m_ulongFieldNameCount = ARRAYSIZE(g_tcpipPerformanceDiagnosticsULongCounterNames),
			 .m_ulongFieldNames = g_tcpipPerformanceDiagnosticsULongCounterNames,
			 .m_ulonglongFieldNameCount = 0,
			 .m_ulonglongFieldNames = nullptr,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},

			{.m_classType = ctWmiEnumClassType::Static,
			 .m_className = ctWmiEnumClassName::WinsockBsp,
			 .m_providerName = g_microsoftWinsockBspCounter,
			 .m_ulongFieldNameCount = ARRAYSIZE(g_microsoftWinsockBspULongCounterNames),
			 .m_ulongFieldNames = g_microsoftWinsockBspULongCounterNames,
			 .m_ulonglongFieldNameCount = 0,
			 .m_ulonglongFieldNames = nullptr,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},

			{.m_classType = ctWmiEnumClassType::Static,
			 .m_className = ctWmiEnumClassName::WfpFilter,
			 .m_providerName = g_wfpFilterCounter,
			 .m_ulongFieldNameCount = ARRAYSIZE(g_wfpFilterULongCounterNames),
			 .m_ulongFieldNames = g_wfpFilterULongCounterNames,
			 .m_ulonglongFieldNameCount = 0,
			 .m_ulonglongFieldNames = nullptr,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},

			{.m_classType = ctWmiEnumClassType::Static,
			 .m_className = ctWmiEnumClassName::WfpFilterCount,
			 .m_providerName = g_wfpFilterCountCounter,
			 .m_ulongFieldNameCount = 0,
			 .m_ulongFieldNames = nullptr,
			 .m_ulonglongFieldNameCount = ARRAYSIZE(g_wfpFilterCountULongLongCounterNames),
			 .m_ulonglongFieldNames = g_wfpFilterCountULongLongCounterNames,
			 .m_stringFieldNameCount = ARRAYSIZE(g_commonStringPropertyNames),
			 .m_stringFieldNames = g_commonStringPropertyNames},
		};
	}

	template <typename T>
	std::shared_ptr<ctPerformanceCounterCounter<T>> ctMakeStaticPerfCounter(
		const ctWmiService &wmi,
		_In_ PCWSTR className,
		_In_ PCWSTR counterName,
		ctPerformanceCounterCollectionType collectionType = ctPerformanceCounterCollectionType::Detailed)
	{
		// 'static' WMI PerfCounters enumerate via IWbemClassObject and accessed/refreshed via IWbemClassObject
		return std::make_shared<ctPerformanceCounterCounterImpl<IWbemClassObject, IWbemClassObject, T>>(
			wmi, className, counterName, collectionType);
	}

	template <typename T>
	std::shared_ptr<ctPerformanceCounterCounter<T>> ctMakeStaticPerfCounter(
		PCWSTR className,
		PCWSTR counterName,
		const ctPerformanceCounterCollectionType collectionType = ctPerformanceCounterCollectionType::Detailed)
	{
		const ctWmiService wmi(L"root\\cimv2");
		return ctMakeStaticPerfCounter<T>(wmi, className, counterName, collectionType);
	}

	/**
	 * @brief Create a counter for a static (single-instance) WMI performance class.
	 *
	 * Convenience overload that binds to the default `root\\cimv2` namespace.
	 *
	 * @tparam T Storage type for sampled values.
	 * @param[in] className WMI provider class name.
	 * @param[in] counterName Property name to sample.
	 * @param[in] collectionType Collection strategy to use.
	 * @return Shared pointer to the created counter.
	 */

	template <typename T>
	std::shared_ptr<ctPerformanceCounterCounter<T>> ctMakeInstancePerfCounter(
		const ctWmiService &wmi,
		_In_ PCWSTR className,
		_In_ PCWSTR counterName,
		ctPerformanceCounterCollectionType collectionType = ctPerformanceCounterCollectionType::Detailed)
	{
		// 'instance' WMI perf objects are enumerated through the IWbemHiPerfEnum interface and accessed/refreshed through the IWbemObjectAccess interface
		return std::make_shared<ctPerformanceCounterCounterImpl<IWbemHiPerfEnum, IWbemObjectAccess, T>>(
			wmi, className, counterName, collectionType);
	}

	template <typename T>
	std::shared_ptr<ctPerformanceCounterCounter<T>> ctMakeInstancePerfCounter(
		_In_ PCWSTR className,
		_In_ PCWSTR counterName,
		ctPerformanceCounterCollectionType collectionType = ctPerformanceCounterCollectionType::Detailed)
	{
		const ctWmiService wmi(L"root\\cimv2");
		return ctMakeInstancePerfCounter<T>(wmi, className, counterName, collectionType);
	}

	/**
	 * @brief Create a counter for an instance-backed WMI performance class.
	 *
	 * Convenience overload that binds to the default `root\\cimv2` namespace.
	 *
	 * @tparam T Storage type for sampled values.
	 * @param[in] className WMI provider class name.
	 * @param[in] counterName Property name to sample.
	 * @param[in] collectionType Collection strategy to use.
	 * @return Shared pointer to the created counter.
	 */

	/**
	 * @brief Create a performance counter based on a known enum class name.
	 *
	 * This helper validates the requested counter name against the
	 * compile-time `c_performanceCounterPropertiesArray` and constructs the
	 * appropriate static or instance-backed aggregator.
	 *
	 * @tparam T Type used to store counter samples (e.g., `ULONG`, `ULONGLONG`).
	 * @param[in] wmi Initialized WMI service to use for creating the counter.
	 * @param[in] className Enum indicating the well-known counter class.
	 * @param[in] counterName Property name to sample.
	 * @param[in] collectionType Collection strategy to use (default: Detailed).
	 * @return Shared pointer to the created counter aggregator.
	 * @throws wil::ResultException / wil::FailureException on validation or WMI errors.
	 */

	template <typename T>
	std::shared_ptr<ctPerformanceCounterCounter<T>> ctCreatePerfCounter(
		const ctWmiService &wmi,
		ctWmiEnumClassName className,
		_In_ PCWSTR counterName,
		ctPerformanceCounterCollectionType collectionType = ctPerformanceCounterCollectionType::Detailed)
	{
		const ctPerformanceCounterCounterProperties *foundProperty = nullptr;
		for (const auto &counterProperty : ctPerformanceCounterDetails::c_performanceCounterPropertiesArray)
		{
			if (className == counterProperty.m_className)
			{
				foundProperty = &counterProperty;
				break;
			}
		}

		THROW_HR_IF_MSG(
			HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
			!foundProperty,
			"Unknown WMI Performance Counter Class");

		THROW_HR_IF_MSG(
			HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
			!foundProperty->PropertyNameExists<T>(counterName),
			"CounterName (%ws) does not exist in the requested class (%u)",
			counterName, static_cast<unsigned>(className));

		if (foundProperty->m_classType == ctWmiEnumClassType::Static)
		{
			return ctMakeStaticPerfCounter<T>(wmi, foundProperty->m_providerName, counterName, collectionType);
		}

		FAIL_FAST_IF(foundProperty->m_classType != ctWmiEnumClassType::Instance);
		return ctMakeInstancePerfCounter<T>(wmi, foundProperty->m_providerName, counterName, collectionType);
	}

	template <typename T>
	std::shared_ptr<ctPerformanceCounterCounter<T>> ctCreatePerfCounter(
		ctWmiEnumClassName className,
		_In_ PCWSTR counterName,
		ctPerformanceCounterCollectionType collectionType = ctPerformanceCounterCollectionType::Detailed)
	{
		const ctWmiService wmi(L"root\\cimv2");
		return ctCreatePerfCounter<T>(wmi, className, counterName, collectionType);
	}

	/**
	 * @brief Create a performance counter for a well-known enum class by id.
	 *
	 * Convenience overload that binds to the default `root\\cimv2` namespace.
	 *
	 * @tparam T Storage type for sampled values.
	 * @param[in] className Well-known enum class identifier.
	 * @param[in] counterName Property name to sample within the class.
	 * @param[in] collectionType Collection strategy to use.
	 * @return Shared pointer to the created counter.
	 */
} // namespace ctl
