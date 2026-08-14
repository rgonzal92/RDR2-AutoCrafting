#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>

#include "../../inc/types.h"

namespace rage
{
	class scrNativeCallContext
	{
	public:
		constexpr void reset()
		{
			m_arg_count = 0;
			m_data_count = 0;
		}

		template <typename T>
		constexpr void push_arg(T&& value)
		{
			static_assert(sizeof(T) <= sizeof(std::uint64_t));
			*reinterpret_cast<std::remove_cv_t<std::remove_reference_t<T>>*>(reinterpret_cast<std::uint64_t*>(m_args) + (m_arg_count++)) = std::forward<T>(value);
		}

		template <typename T>
		constexpr void push_arg(T& value)
		{
			static_assert(sizeof(T) <= sizeof(std::uint64_t));
			*reinterpret_cast<std::remove_cv_t<std::remove_reference_t<T>>*>(reinterpret_cast<std::uint64_t*>(m_args) + (m_arg_count++)) = std::forward<T>(value);
		}

		template <typename T>
		constexpr T& get_arg(std::size_t index)
		{
			static_assert(sizeof(T) <= sizeof(std::uint64_t));
			return *reinterpret_cast<T*>(reinterpret_cast<std::uint64_t*>(m_args) + index);
		}

		template <typename T>
		constexpr void set_arg(std::size_t index, T&& value)
		{
			static_assert(sizeof(T) <= sizeof(std::uint64_t));
			*reinterpret_cast<std::remove_cv_t<std::remove_reference_t<T>>*>(reinterpret_cast<std::uint64_t*>(m_args) + index) = std::forward<T>(value);
		}

		template <typename T>
		constexpr T* get_return_value()
		{
			return reinterpret_cast<T*>(m_return_value);
		}

		template <typename T>
		constexpr void set_return_value(T&& value)
		{
			*reinterpret_cast<std::remove_cv_t<std::remove_reference_t<T>>*>(m_return_value) = std::forward<T>(value);
		}

		template <typename T>
		constexpr void set_return_value(T& value)
		{
			*reinterpret_cast<std::remove_cv_t<std::remove_reference_t<T>>*>(m_return_value) = std::forward<T>(value);
		}

		constexpr std::uint32_t get_arg_count() {
			return m_arg_count & 0x0F;
		}
	protected:
		void* m_return_value;
		std::uint32_t m_arg_count;
		void* m_args;
		std::int32_t m_data_count;
		std::uint32_t m_data[48];
	};
	static_assert(sizeof(scrNativeCallContext) == 0xE0);

	using scrNativeHash = std::int64_t;
	using scrNativePair = std::pair<scrNativeHash, scrNativeHash>;
	using scrNativeHandler = void(__fastcall*)(scrNativeCallContext*);

	#pragma pack(push, 8)
	template <typename T>
	class atArray
	{
	public:
		atArray()
		{
			m_data = nullptr;
			m_count = 0;
			m_size = 0;
		}

		T* begin() const
		{
			return &m_data[0];
		}

		T* end() const
		{
			return &m_data[m_size];
		}

		T* data() const
		{
			return m_data;
		}

		std::uint16_t size() const
		{
			return m_size;
		}

		std::uint16_t count() const
		{
			return m_count;
		}

		T& operator[](std::uint16_t index) const
		{
			return m_data[index];
		}

	private:
		T* m_data;
		std::uint16_t m_size;
		std::uint16_t m_count;
	};
	static_assert(sizeof(rage::atArray<uint32_t>) == 0x10, "rage::atArray is not properly sized");
	#pragma pack(pop)

	enum eThreadState
	{
		idle,
		running,
		killed,
		unk3,
		unk4,
	};

	class scrThreadContext
	{
	public:
		uint32_t m_threadId;                  // 0x0000
		uint32_t m_scriptHash;                // 0x0004
		eThreadState m_threadState;           // 0x0008
		uint32_t m_instructionPtr;            // 0x000C
		uint32_t m_framePtr;                  // 0x0010
		uint32_t m_stackPointer;              // 0x0014
		uint32_t m_timerA;                    // 0x0018
		uint32_t m_timerB;                    // 0x001C
		uint32_t m_timerC;                    // 0x0020
		uint32_t m_unk1;                      // 0x0024
		uint32_t m_unk2;                      // 0x0028 
		char pad_002C[0x24];                  // 0x002C  
		uint32_t m_stackSize;                 // 0x0050
		char pad_0054[0x65C];                 // 0x0054
	};

	class scrThread
	{
	public:
		virtual ~scrThread() = default;
		virtual eThreadState Reset(uint32_t programId, void const* args, int argsCount) = 0;
		virtual eThreadState Run(int opsCount) = 0;
		virtual eThreadState Update(int opsCount) = 0;
		virtual void Kill() = 0;

		scrThreadContext m_context;                // 0x0008
		uintptr_t m_stack;                         // 0x06B8
		char pad_06C0[0x4];                        // 0x06C0
		uint32_t m_opsCount;                       // 0x06C4
		char pad_06C8[0x8];                        // 0x06C8
		char* m_exitMessage;                       // 0x06D0
		uint32_t m_scriptHash;                     // 0x06D8
		char pad_06DC[0x4];                        // 0x06DC
	};
	static_assert(sizeof(scrThread) == 0x6E0, "rage::scrThread is not properly sized");
}

namespace menu {
	class CallContext : public rage::scrNativeCallContext
	{
	public:
		constexpr CallContext() {
			m_return_value = &m_return_stack;
			m_args = &m_arg_stack;
		}

		CallContext(rage::scrNativeCallContext* ctx) {
			for (std::uint32_t i = 0; i < ctx->get_arg_count(); ++i) {
				this->push_arg<Any>(ctx->get_arg<Any>(i));
			}
			this->set_return_value<Any>(*ctx->get_return_value<Any>());
		}
	protected:
		uint64_t m_return_stack[10] = {};
		uint64_t m_arg_stack[40] = {};
	};
}
