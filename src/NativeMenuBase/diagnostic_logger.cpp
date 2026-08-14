#include "diagnostic_logger.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>
#include <windows.h>

namespace
{
	std::ofstream log_stream;
	std::mutex log_mutex;
	std::uint64_t sequence_number = 0;
	thread_local bool internal_query_active = false;
	int module_anchor = 0;

	std::string Escape(std::string_view value)
	{
		std::string escaped;
		escaped.reserve(value.size());
		for (const char character : value) {
			switch (character) {
			case '\t':
				escaped += "\\t";
				break;
			case '\r':
				escaped += "\\r";
				break;
			case '\n':
				escaped += "\\n";
				break;
			default:
				escaped += character;
				break;
			}
		}
		return escaped;
	}

	template <typename T>
	void WriteOptional(std::ostream& output, const std::optional<T>& value)
	{
		if (value.has_value()) {
			output << *value;
		}
		else {
			output << '-';
		}
	}

	void WriteOptionalHash(std::ostream& output, const std::optional<std::uint32_t>& value)
	{
		if (value.has_value()) {
			output << "0x" << std::hex << std::uppercase << *value << std::dec;
		}
		else {
			output << '-';
		}
	}
}

bool diagnostic::Initialize()
{
	HMODULE module = nullptr;
	const auto anchor = reinterpret_cast<LPCWSTR>(&module_anchor);
	if (!GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		anchor,
		&module)) {
		return false;
	}

	std::array<wchar_t, 32768> module_path{};
	const DWORD path_length = GetModuleFileNameW(module, module_path.data(), static_cast<DWORD>(module_path.size()));
	if (path_length == 0 || path_length >= module_path.size()) {
		return false;
	}

	const auto log_path = std::filesystem::path(module_path.data()).parent_path() / "AutoCraft-diagnostic.log";
	log_stream.open(log_path, std::ios::out | std::ios::trunc);
	if (!log_stream.is_open()) {
		return false;
	}

	log_stream
		<< "sequence\tframe\tscript_hash\tscript_thread\tevent\towner_id\tsubject_hash"
		<< "\tquantity\tslot_hash\treason_hash\tbefore_count\tafter_count\tcapacity\tresult\tprompt\ttext\n";
	log_stream.flush();
	return true;
}

void diagnostic::Write(const Record& record)
{
	std::scoped_lock lock(log_mutex);
	if (!log_stream.is_open()) {
		return;
	}

	log_stream << ++sequence_number << '\t'
		<< record.frame << '\t'
		<< "0x" << std::hex << std::uppercase << record.script_hash << '\t'
		<< "0x" << record.script_thread << std::dec << '\t'
		<< record.event << '\t';
	WriteOptional(log_stream, record.owner_id);
	log_stream << '\t';
	WriteOptionalHash(log_stream, record.subject_hash);
	log_stream << '\t';
	WriteOptional(log_stream, record.quantity);
	log_stream << '\t';
	WriteOptionalHash(log_stream, record.slot_hash);
	log_stream << '\t';
	WriteOptionalHash(log_stream, record.reason_hash);
	log_stream << '\t';
	WriteOptional(log_stream, record.before_count);
	log_stream << '\t';
	WriteOptional(log_stream, record.after_count);
	log_stream << '\t';
	WriteOptional(log_stream, record.capacity);
	log_stream << '\t';
	WriteOptional(log_stream, record.result);
	log_stream << '\t';
	WriteOptional(log_stream, record.prompt);
	log_stream << '\t' << Escape(record.text) << '\n';
	log_stream.flush();
}

bool diagnostic::IsInternalQuery()
{
	return internal_query_active;
}

diagnostic::ScopedInternalQuery::ScopedInternalQuery()
	: previous_state_(internal_query_active)
{
	internal_query_active = true;
}

diagnostic::ScopedInternalQuery::~ScopedInternalQuery()
{
	internal_query_active = previous_state_;
}
