#pragma once

#include <cstdint>
#include <type_traits>
#include <vector>

/**
 * Lightweight wrapper for a process address found by a signature scan.
 *
 * A handle can be invalid when a scan does not match. Call IsValid before
 * dereferencing or resolving offsets from a handle.
 */
class Handle
{
public:
	/** Creates an invalid handle. */
	Handle() : m_ptr(nullptr) {}

	/**
	 * Wraps an already resolved process address.
	 *
	 * @param ptr Address inside the current process.
	 */
	explicit Handle(std::uintptr_t ptr) : m_ptr(reinterpret_cast<void*>(ptr)) {}

	/**
	 * Views the address as a pointer of the requested type.
	 *
	 * @tparam T Pointer type to produce.
	 * @return The address reinterpreted as T.
	 */
	template <typename T>
	std::enable_if_t<std::is_pointer_v<T>, T> As();

	/**
	 * Views the address as a reference of the requested type.
	 *
	 * @tparam T Lvalue-reference type to produce.
	 * @return A reference to the value at this address.
	 */
	template <typename T>
	std::enable_if_t<std::is_lvalue_reference_v<T>, T> As();

	/**
	 * Returns the address as an integer.
	 *
	 * @tparam T Must be std::uintptr_t.
	 * @return The raw process address.
	 */
	template <typename T>
	std::enable_if_t<std::is_same_v<T, std::uintptr_t>, T> As();

	/**
	 * Returns a new handle advanced by a byte offset.
	 *
	 * @param offset Number of bytes to add.
	 * @return A new handle at the adjusted address.
	 */
	template <typename T>
	Handle Add(T offset)
	{
		return Handle(As<std::uintptr_t>() + offset);
	}

	/**
	 * Resolves a RIP-relative 32-bit displacement at this address.
	 *
	 * @return A handle pointing to the displacement target.
	 */
	Handle Rip()
	{
		return Add(As<std::int32_t&>()).Add(4);
	}

	/** Returns whether this handle contains a resolved address. */
	[[nodiscard]] bool IsValid() const
	{
		return m_ptr != nullptr;
	}

private:
	void* m_ptr;
};

template <typename T>
inline std::enable_if_t<std::is_pointer_v<T>, T> Handle::As()
{
	return reinterpret_cast<T>(m_ptr);
}

template <typename T>
inline std::enable_if_t<std::is_lvalue_reference_v<T>, T> Handle::As()
{
	return *static_cast<std::add_pointer_t<std::remove_reference_t<T>>>(m_ptr);
}

template <typename T>
inline std::enable_if_t<std::is_same_v<T, std::uintptr_t>, T> Handle::As()
{
	return reinterpret_cast<std::uintptr_t>(m_ptr);
}

/** Scans a loaded module for a byte signature with `?` wildcard bytes. */
class scanner
{
	uintptr_t module_address;
	static std::vector<int> ConvPatternToByte(const char* pattern);
public:
	/**
	 * Scans the configured module for a signature.
	 *
	 * @param signature Space-separated hexadecimal bytes; `?` represents any byte.
	 * @return A matching address, or an invalid Handle when none is found.
	 */
	Handle scan(const char* signature);

	/**
	 * Selects the module that later scans will search.
	 *
	 * @param module_name Windows module name, or null for the main executable.
	 */
	scanner(const char* module_name);
};
