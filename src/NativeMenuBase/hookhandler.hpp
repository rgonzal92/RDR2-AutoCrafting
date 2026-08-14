#pragma once

#include "console.h"
#include "rage.hpp"

#include "../../inc/MinHook.h"

namespace hooks
{
	/** Installs and enables one native hook, leaving the original handler null on failure. */
	inline void InstallNativeHook(
		[[maybe_unused]] const char* name,
		rage::scrNativeHandler target,
		rage::scrNativeHandler detour,
		rage::scrNativeHandler* original)
	{
		if (target == nullptr || detour == nullptr || original == nullptr) {
			PRINT_ERROR("Hooking| Invalid handler for: ", name);
			return;
		}

		*original = nullptr;
		auto status = MH_CreateHook(
			reinterpret_cast<void*>(target),
			reinterpret_cast<void*>(detour),
			reinterpret_cast<void**>(original));
		if (status != MH_OK) {
			PRINT_ERROR("Hooking| CreateHook failed for: ", name, " Error: ", MH_StatusToString(status));
			return;
		}

		status = MH_EnableHook(reinterpret_cast<void*>(target));
		if (status != MH_OK) {
			PRINT_ERROR("Hooking| EnableHook failed for: ", name, " Error: ", MH_StatusToString(status));
			MH_RemoveHook(reinterpret_cast<void*>(target));
			*original = nullptr;
			return;
		}

		PRINT_INFO("Hooking| Enabled: ", name);
	}
}

#define CALL() do { \
	if (original != nullptr) { \
		original(ctx); \
	} \
} while (false)

#define NHOOK(name, hash, callback) do { \
	static rage::scrNativeHandler original = nullptr; \
	hooks::InstallNativeHook(name, get_native_handler(hash), [](rage::scrNativeCallContext* ctx)callback, &original); \
} while (false)
