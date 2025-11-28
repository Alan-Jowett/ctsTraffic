// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <stdexcept>

#include <Windows.h>
#include <WbemIdl.h>

#include <wil/stl.h>
#include <wil/com.h>
#include <wil/resource.h>

/**
 * @file ctWmiService.hpp
 * @brief Lightweight WMI service wrapper and class property enumeration helpers.
 *
 * `ctWmiService` encapsulates connecting to a WMI namespace and provides
 * convenience helpers to invoke IWbemServices methods. `ctWmiEnumerateClassProperties`
 * exposes an iterator-based interface to enumerate class properties for a
 * given WMI class object.
 */

namespace ctl
{
	/**
	 * @brief RAII wrapper for an IWbemServices connection.
	 *
	 * The constructor connects to the specified WMI namespace and configures
	 * the COM proxy security blanket. Callers are responsible for calling
	 * `CoInitialize`/`CoInitializeEx` and configuring process-wide security
	 * before creating `ctWmiService` if required.
	 */
	class ctWmiService
	{
	public:
		/**
		 * @brief Construct and connect to the WMI namespace at `path`.
		 * @param[in] path WMI namespace path (for example `ROOT\\CIMV2`).
		 *
		 * Note: This constructor throws on failure.
		 */
		explicit ctWmiService(_In_ PCWSTR path)
		{
			m_wbemLocator = wil::CoCreateInstance<WbemLocator, IWbemLocator>();

			THROW_IF_FAILED(m_wbemLocator->ConnectServer(
				wil::make_bstr(path).get(), // Object path of WMI namespace
				nullptr, // Username. NULL = current user
				nullptr, // User password. NULL = current
				nullptr, // Locale. NULL indicates current
				0, // Security flags.
				nullptr, // Authority (e.g. Kerberos)
				nullptr, // Context object 
				m_wbemServices.put())); // receive pointer to IWbemServices proxy

			THROW_IF_FAILED(CoSetProxyBlanket(
				m_wbemServices.get(), // Indicates the proxy to set
				RPC_C_AUTHN_WINNT, // RPC_C_AUTHN_xxx
				RPC_C_AUTHZ_NONE, // RPC_C_AUTHZ_xxx
				nullptr, // Server principal name 
				RPC_C_AUTHN_LEVEL_CALL, // RPC_C_AUTHN_LEVEL_xxx 
				RPC_C_IMP_LEVEL_IMPERSONATE, // RPC_C_IMP_LEVEL_xxx
				nullptr, // client identity
				EOAC_NONE)); // proxy capabilities 
		}

		~ctWmiService() = default;
		ctWmiService(const ctWmiService& service) noexcept = default;
		ctWmiService& operator=(const ctWmiService& service) noexcept = default;
		ctWmiService(ctWmiService&& rhs) noexcept = default;
		ctWmiService& operator=(ctWmiService&& rhs) noexcept = default;

		/**
		 * @brief Indirection to the underlying `IWbemServices` pointer.
		 * @return Raw `IWbemServices*` pointer.
		 */
		IWbemServices* operator->() noexcept
		{
			return m_wbemServices.get();
		}

		/**
		 * @brief Const indirection to the underlying `IWbemServices` pointer.
		 * @return Raw `IWbemServices*` pointer.
		 */
		const IWbemServices* operator->() const noexcept
		{
			return m_wbemServices.get();
		}

		/**
		 * @brief Compare two `ctWmiService` instances for equality.
		 * @param[in] service Other instance to compare.
		 * @return True when both instances reference the same underlying COM objects.
		 */
		bool operator ==(const ctWmiService& service) const noexcept
		{
			return m_wbemLocator == service.m_wbemLocator &&
				m_wbemServices == service.m_wbemServices;
		}

		bool operator !=(const ctWmiService& service) const noexcept
		{
			return !(*this == service);
		}

		/**
		 * @brief Retrieve the underlying `IWbemServices` pointer.
		 * @return Raw pointer to `IWbemServices`.
		 */
		IWbemServices* get() noexcept
		{
			return m_wbemServices.get();
		}

		/**
		 * @brief Retrieve the underlying `IWbemServices` pointer (const).
		 * @return Raw pointer to `IWbemServices`.
		 */
		[[nodiscard]] const IWbemServices* get() const noexcept
		{
			return m_wbemServices.get();
		}

		/**
		 * @brief Delete a WMI object given its object path asynchronously.
		 * @param[in] objPath Object path string identifying the WMI instance to delete.
		 * @param[in] context Optional IWbemContext pointer for the call.
		 *
		* Performs the delete as an asynchronous `DeleteInstance` call and waits
		 * for the call result. Throws on failure.
		 */
		void delete_path(_In_ PCWSTR objPath, const wil::com_ptr<IWbemContext>& context) const
		{
			wil::com_ptr<IWbemCallResult> result;
			THROW_IF_FAILED(m_wbemServices->DeleteInstance(
				wil::make_bstr(objPath).get(),
				WBEM_FLAG_RETURN_IMMEDIATELY,
				context.get(),
				result.addressof()));
			// wait for the call to complete
			HRESULT status{};
			THROW_IF_FAILED(result->GetCallStatus(WBEM_INFINITE, &status));
			THROW_IF_FAILED(status);
		}

		/**
		 * @brief Delete a WMI instance specified by object path synchronously.
		 * @param[in] objPath Object path string identifying the WMI instance to delete.
		 */
		void delete_path(_In_ PCWSTR objPath) const
		{
			const wil::com_ptr<IWbemContext> nullContext;
			delete_path(objPath, nullContext.get());
		}

	private:
		wil::com_ptr<IWbemLocator> m_wbemLocator{};
		wil::com_ptr<IWbemServices> m_wbemServices{};
	};

	/**
	 * @brief Exposes enumerating properties of a WMI class object through an iterator.
	 *
	 * Construct with either an already-resolved `IWbemClassObject` or a class name
	 * and a `ctWmiService` to fetch the class object. The iterator returns property
	 * names and supports querying the CIM type via `type()`.
	 */
	class ctWmiEnumerateClassProperties
	{
	public:
		class iterator;

		ctWmiEnumerateClassProperties(ctWmiService service, wil::com_ptr<IWbemClassObject> classObject) noexcept :
			m_wbemServices(std::move(service)),
			m_wbemClass(std::move(classObject))
		{
		}

		ctWmiEnumerateClassProperties(ctWmiService service, _In_ PCWSTR className) :
			m_wbemServices(std::move(service))
		{
			THROW_IF_FAILED(m_wbemServices->GetObjectW(
				wil::make_bstr(className).get(),
				0,
				nullptr,
				m_wbemClass.put(),
				nullptr));
		}

		ctWmiEnumerateClassProperties(ctWmiService service, _In_ BSTR className) :
			m_wbemServices(std::move(service))
		{
			THROW_IF_FAILED(m_wbemServices->GetObjectW(
				className,
				0,
				nullptr,
				m_wbemClass.put(),
				nullptr));
		}

		/**
		 * @brief Get an iterator to the first property name.
		 * @param[in] nonSystemPropertiesOnly When true, only non-system properties are returned.
		 * @return An `iterator` positioned at the first property or `end()` if none.
		 */
		[[nodiscard]] iterator begin(const bool nonSystemPropertiesOnly = true) const
		{
			return { m_wbemClass, nonSystemPropertiesOnly };
		}

		/**
		 * @brief End sentinel for property enumeration.
		 * @return Default-constructed `iterator` representing the end.
		 */
		[[nodiscard]] static iterator end() noexcept
		{
			return {};
		}

		// A forward iterator to enable forward-traversing instances of the queried WMI provider

		class iterator
		{
			const uint32_t m_endIteratorIndex = ULONG_MAX;

			wil::com_ptr<IWbemClassObject> m_wbemClassObject{};
			wil::shared_bstr m_propertyName{};
			CIMTYPE m_propertyType = 0;
			uint32_t m_index = m_endIteratorIndex;

		public:
			// Iterator requires the caller's IWbemServices interface and class name
			iterator() noexcept = default;

			iterator(wil::com_ptr<IWbemClassObject> classObject, bool nonSystemPropertiesOnly) :
				m_wbemClassObject(std::move(classObject)), m_index(0)
			{
				THROW_IF_FAILED(m_wbemClassObject->BeginEnumeration(nonSystemPropertiesOnly ? WBEM_FLAG_NONSYSTEM_ONLY : 0));
				increment();
			}

			~iterator() noexcept = default;
			iterator(const iterator&) noexcept = default;
			iterator(iterator&&) noexcept = default;

			iterator& operator =(const iterator&) noexcept = delete;
			iterator& operator =(iterator&&) noexcept = delete;

			void swap(_Inout_ iterator& rhs) noexcept
			{
				using std::swap;
				swap(m_index, rhs.m_index);
				swap(m_wbemClassObject, rhs.m_wbemClassObject);
				swap(m_propertyName, rhs.m_propertyName);
				swap(m_propertyType, rhs.m_propertyType);
			}

			/**
			 * @brief Retrieve the current property name.
			 * @return The property name as a `wil::shared_bstr`.
			 * @throws std::out_of_range if the iterator is at the end.
			 */
			wil::shared_bstr operator*() const
			{
				if (m_index == m_endIteratorIndex)
				{
					throw std::out_of_range("ctWmiProperties::iterator::operator - invalid subscript");
				}
				return m_propertyName;
			}

			/**
			 * @brief Pointer-like access to the current property name.
			 * @return Pointer to the current property name BSTR.
			 * @throws std::out_of_range if the iterator is at the end.
			 */
			const wil::shared_bstr* operator->() const
			{
				if (m_index == m_endIteratorIndex)
				{
					throw std::out_of_range("ctWmiProperties::iterator::operator-> - invalid subscript");
				}
				return &m_propertyName;
			}

			/**
			 * @brief Retrieve the CIMTYPE of the current property.
			 * @return The CIMTYPE value for the current property.
			 * @throws std::out_of_range if the iterator is at the end.
			 */
			[[nodiscard]] CIMTYPE type() const
			{
				if (m_index == m_endIteratorIndex)
				{
					throw std::out_of_range("ctWmiProperties::iterator::type - invalid subscript");
				}
				return m_propertyType;
			}

			bool operator==(const iterator& iter) const noexcept
			{
				if (m_index != m_endIteratorIndex)
				{
					return m_index == iter.m_index &&
						m_wbemClassObject == iter.m_wbemClassObject;
				}
				return m_index == iter.m_index;
			}

			bool operator!=(const iterator& iter) const noexcept
			{
				return !(*this == iter);
			}

			iterator& operator++()
			{
				increment();
				return *this;
			}

			iterator operator++(int)
			{
				iterator temp(*this);
				increment();
				return temp;
			}

			iterator& operator+=(uint32_t _inc)
			{
				for (auto loop = 0ul; loop < _inc; ++loop)
				{
					increment();
					if (m_index == m_endIteratorIndex)
					{
						throw std::out_of_range("ctWmiProperties::iterator::operator+= - invalid subscript");
					}
				}
				return *this;
			}

			// iterator_traits (allows <algorithm> functions to be used)
			using iterator_category = std::forward_iterator_tag;
			using value_type = wil::shared_bstr;
			using difference_type = int;
			using pointer = BSTR;
			using reference = wil::shared_bstr&;

		private:
			void increment()
			{
				if (m_index == m_endIteratorIndex)
				{
					throw std::out_of_range("ctWmiProperties::iterator - cannot increment: at the end");
				}

				CIMTYPE nextCimType{};
				wil::shared_bstr nextName;
				const auto hr = m_wbemClassObject->Next(
					0,
					nextName.addressof(),
					nullptr,
					&nextCimType,
					nullptr);
				switch (hr)
				{
				case WBEM_S_NO_ERROR:
				{
					// update the instance members
					++m_index;
					using std::swap;
					swap(m_propertyName, nextName);
					swap(m_propertyType, nextCimType);
					break;
				}

				case WBEM_S_NO_MORE_DATA:
				{
					// at the end...
					m_index = m_endIteratorIndex;
					m_propertyName.reset();
					m_propertyType = 0;
					break;
				}

				default: THROW_IF_FAILED(hr);
				}
			}
		};

	private:
		ctWmiService m_wbemServices;
		wil::com_ptr<IWbemClassObject> m_wbemClass{};
	};
} // namespace ctl
