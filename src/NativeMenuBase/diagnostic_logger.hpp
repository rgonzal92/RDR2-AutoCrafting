#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace diagnostic
{
	/** Structured data captured for one intercepted crafting operation. */
	struct Record
	{
		std::string_view event{};
		int frame{};
		std::uint32_t script_hash{};
		std::uintptr_t script_thread{};
		std::optional<std::int64_t> owner_id{};
		std::optional<std::uint32_t> subject_hash{};
		std::optional<int> quantity{};
		std::optional<std::uint32_t> slot_hash{};
		std::optional<std::uint32_t> reason_hash{};
		std::optional<int> before_count{};
		std::optional<int> after_count{};
		std::optional<int> result{};
		std::optional<int> prompt{};
		std::string_view text{};
	};

	/**
	 * Opens AutoCraft-diagnostic.log beside the loaded ASI.
	 *
	 * @return true when the log was opened and its header was written.
	 */
	bool Initialize();

	/** Writes and flushes one ordered diagnostic record. */
	void Write(const Record& record);

	/** Returns whether the current thread is executing a diagnostic count query. */
	bool IsInternalQuery();

	/** Prevents diagnostic count queries from recursively producing hook records. */
	class ScopedInternalQuery
	{
	public:
		ScopedInternalQuery();
		~ScopedInternalQuery();

		ScopedInternalQuery(const ScopedInternalQuery&) = delete;
		ScopedInternalQuery& operator=(const ScopedInternalQuery&) = delete;

	private:
		bool previous_state_;
	};
}
