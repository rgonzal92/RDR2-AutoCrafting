// Licensed under the MIT License.

#include "script.h"

#include "../../inc/main.h"
#include "../../inc/natives.h"
#include "console.h"
#include "diagnostic_logger.hpp"
#include "hookhandler.hpp"
#include "rage.hpp"
#include "scanner.hpp"

#include <optional>
#include <string_view>

#ifndef AUTOCRAFT_DIAGNOSTIC
#define AUTOCRAFT_DIAGNOSTIC 0
#endif

namespace
{
	constexpr int kCraftBatchLimit = 50;
	constexpr Hash kCraftCommitEvent = 0xFC4F2858;
	constexpr Hash kSafeBreakoutEvent = 0x238C32C3;
	constexpr Hash kCraftingScript = 0x38EB3D5B;
	constexpr Hash kCraftingMenuScript = 0xA64E7B5F;
	constexpr Hash kCraftingHudContext = 0xE3FEFB3D;

	Prompt tracked_crafting_prompt = 0;
	int skipped_frame = -1;
	int remaining_batch_crafts = 0;
	bool force_safe_breakout = false;
	bool batch_popup_shown = false;
	rage::scrThread** current_script_thread = nullptr;

	using GetNativeHandler = rage::scrNativeHandler(*)(rage::scrNativeHash hash);
	GetNativeHandler get_native_handler = nullptr;

	Hash CurrentScriptHash()
	{
		if (current_script_thread == nullptr || *current_script_thread == nullptr) {
			return 0;
		}
		return (*current_script_thread)->m_scriptHash;
	}

	bool IsCraftingScript()
	{
		const Hash script_hash = CurrentScriptHash();
		return script_hash == kCraftingScript || script_hash == kCraftingMenuScript;
	}

	bool IsCraftingPromptText(std::string_view text)
	{
		return text == "CAMP_REC_COOK_AGN"
			|| text == "STOW_ITEM"
			|| text == "CRAFT_FASTER"
			|| text == "CAMP_REC_MAKE_AGN"
			|| text == "CAMP_REC_MAKE";
	}

#if AUTOCRAFT_DIAGNOSTIC
	diagnostic::Record MakeRecord(std::string_view event)
	{
		diagnostic::Record record;
		record.event = event;
		record.frame = MISC::GET_FRAME_COUNT();
		record.script_hash = CurrentScriptHash();
		if (current_script_thread != nullptr && *current_script_thread != nullptr) {
			record.script_thread = reinterpret_cast<std::uintptr_t>(*current_script_thread);
		}
		return record;
	}

	int QueryInventoryCount(int inventory_id, Hash item)
	{
		diagnostic::ScopedInternalQuery query;
		return INVENTORY::_INVENTORY_GET_INVENTORY_ITEM_COUNT_WITH_ITEMID(inventory_id, item, false);
	}

	int QueryAmmoCount(Ped ped, Hash ammo_type)
	{
		diagnostic::ScopedInternalQuery query;
		return WEAPON::GET_PED_AMMO_BY_TYPE(ped, ammo_type);
	}

	std::optional<int> QueryAmmoCapacity(Ped ped, Hash ammo_type)
	{
		diagnostic::ScopedInternalQuery query;
		const Hash weapon = WEAPON::_GET_WEAPON_TYPE_FROM_AMMO_TYPE(ammo_type);
		int capacity = 0;
		if (weapon == 0 || !WEAPON::GET_MAX_AMMO(ped, &capacity, weapon)) {
			return std::nullopt;
		}
		return capacity;
	}

	void InitializeDiagnosticHooks()
	{
		NHOOK("_UI_PROMPT_SET_TEXT", 0x5DD02A8318420DD7, {
			if (!IsCraftingScript() || diagnostic::IsInternalQuery()) {
				CALL();
				return;
			}

			const Prompt prompt = ctx->get_arg<Prompt>(0);
			const char* prompt_text = ctx->get_arg<const char*>(1);
			const std::string_view text = prompt_text != nullptr ? prompt_text : "";
			const bool relevant_prompt = IsCraftingPromptText(text);
			if (relevant_prompt) {
				tracked_crafting_prompt = prompt;
			}
			CALL();
			if (relevant_prompt) {
				auto record = MakeRecord("PROMPT_SET_TEXT");
				record.prompt = prompt;
				record.text = text;
				diagnostic::Write(record);
			}
		});

		NHOOK("_UI_PROMPT_REGISTER_END", 0xF7AA2696A22AD8B9, {
			if (!IsCraftingScript() || diagnostic::IsInternalQuery()) {
				CALL();
				return;
			}

			const Prompt prompt = ctx->get_arg<Prompt>(0);
			CALL();
			if (prompt == tracked_crafting_prompt) {
				auto record = MakeRecord("PROMPT_REGISTER_END");
				record.prompt = prompt;
				diagnostic::Write(record);
			}
		});

		NHOOK("_UI_PROMPT_IS_JUST_PRESSED", 0x2787CC611D3FACC5, {
			if (!IsCraftingScript() || diagnostic::IsInternalQuery()) {
				CALL();
				return;
			}

			const Prompt prompt = ctx->get_arg<Prompt>(0);
			CALL();
			const BOOL pressed = *ctx->get_return_value<BOOL>();
			if (prompt == tracked_crafting_prompt && pressed) {
				auto record = MakeRecord("PROMPT_JUST_PRESSED");
				record.prompt = prompt;
				record.result = 1;
				diagnostic::Write(record);
			}
		});

		NHOOK("_UI_PROMPT_IS_PRESSED", 0x21E60E230086697F, {
			if (!IsCraftingScript() || diagnostic::IsInternalQuery()) {
				CALL();
				return;
			}

			const Prompt prompt = ctx->get_arg<Prompt>(0);
			CALL();
			const BOOL pressed = *ctx->get_return_value<BOOL>();
			if (prompt == tracked_crafting_prompt && pressed) {
				auto record = MakeRecord("PROMPT_PRESSED");
				record.prompt = prompt;
				record.result = 1;
				diagnostic::Write(record);
			}
		});

		NHOOK("_INVENTORY_ADD_ITEM_WITH_GUID", 0xCB5D11F9508A928D, {
			if (!IsCraftingScript() || diagnostic::IsInternalQuery()) {
				CALL();
				return;
			}

			const int inventory_id = ctx->get_arg<int>(0);
			const Hash item = ctx->get_arg<Hash>(3);
			const Hash slot = ctx->get_arg<Hash>(4);
			const int quantity = ctx->get_arg<int>(5);
			const Hash reason = ctx->get_arg<Hash>(6);
			const int before_count = QueryInventoryCount(inventory_id, item);
			CALL();
			const BOOL result = *ctx->get_return_value<BOOL>();
			const int after_count = QueryInventoryCount(inventory_id, item);

			auto record = MakeRecord("INVENTORY_ADD_ITEM");
			record.owner_id = inventory_id;
			record.subject_hash = item;
			record.quantity = quantity;
			record.slot_hash = slot;
			record.reason_hash = reason;
			record.before_count = before_count;
			record.after_count = after_count;
			record.result = result ? 1 : 0;
			diagnostic::Write(record);
		});

		NHOOK("_INVENTORY_REMOVE_INVENTORY_ITEM_WITH_ITEMID", 0xB4158C8C9A3B5DCE, {
			if (!IsCraftingScript() || diagnostic::IsInternalQuery()) {
				CALL();
				return;
			}

			const int inventory_id = ctx->get_arg<int>(0);
			const Hash item = ctx->get_arg<Hash>(1);
			const int quantity = ctx->get_arg<int>(2);
			const Hash reason = ctx->get_arg<Hash>(3);
			const int before_count = QueryInventoryCount(inventory_id, item);
			CALL();
			const BOOL result = *ctx->get_return_value<BOOL>();
			const int after_count = QueryInventoryCount(inventory_id, item);

			auto record = MakeRecord("INVENTORY_REMOVE_ITEM");
			record.owner_id = inventory_id;
			record.subject_hash = item;
			record.quantity = quantity;
			record.reason_hash = reason;
			record.before_count = before_count;
			record.after_count = after_count;
			record.result = result ? 1 : 0;
			diagnostic::Write(record);
		});

		NHOOK("_REMOVE_AMMO_FROM_PED_BY_TYPE", 0xB6CFEC32E3742779, {
			if (!IsCraftingScript() || diagnostic::IsInternalQuery()) {
				CALL();
				return;
			}

			const Ped ped = ctx->get_arg<Ped>(0);
			const Hash ammo_type = ctx->get_arg<Hash>(1);
			const int quantity = ctx->get_arg<int>(2);
			const Hash reason = ctx->get_arg<Hash>(3);
			const int before_count = QueryAmmoCount(ped, ammo_type);
			CALL();
			const int after_count = QueryAmmoCount(ped, ammo_type);

			auto record = MakeRecord("AMMO_REMOVE");
			record.owner_id = ped;
			record.subject_hash = ammo_type;
			record.quantity = quantity;
			record.reason_hash = reason;
			record.before_count = before_count;
			record.after_count = after_count;
			diagnostic::Write(record);
		});

		NHOOK("_ADD_AMMO_TO_PED_BY_TYPE", 0x106A811C6D3035F3, {
			if (!IsCraftingScript() || diagnostic::IsInternalQuery()) {
				CALL();
				return;
			}

			const Ped ped = ctx->get_arg<Ped>(0);
			const Hash ammo_type = ctx->get_arg<Hash>(1);
			const int quantity = ctx->get_arg<int>(2);
			const Hash reason = ctx->get_arg<Hash>(3);
			const int before_count = QueryAmmoCount(ped, ammo_type);
			CALL();
			const int after_count = QueryAmmoCount(ped, ammo_type);

			auto record = MakeRecord("AMMO_ADD");
			record.owner_id = ped;
			record.subject_hash = ammo_type;
			record.quantity = quantity;
			record.reason_hash = reason;
			record.before_count = before_count;
			record.after_count = after_count;
			record.capacity = QueryAmmoCapacity(ped, ammo_type);
			diagnostic::Write(record);
		});

		NHOOK("HAS_ANIM_EVENT_FIRED", 0x5851CC48405F4A07, {
			if (!IsCraftingScript() || diagnostic::IsInternalQuery()) {
				CALL();
				return;
			}

			const Entity entity = ctx->get_arg<Entity>(0);
			const Hash event_hash = ctx->get_arg<Hash>(1);
			CALL();
			const BOOL fired = *ctx->get_return_value<BOOL>();
			if (fired || event_hash == static_cast<Hash>(-61921192)) {
				auto record = MakeRecord("ANIM_EVENT");
				record.owner_id = entity;
				record.subject_hash = event_hash;
				record.result = fired ? 1 : 0;
				diagnostic::Write(record);
			}
		});

		NHOOK("HAS_ENTITY_EXITED_ANIM_SCENE", 0xB89FCFF19DAFFF28, {
			if (!IsCraftingScript() || diagnostic::IsInternalQuery()) {
				CALL();
				return;
			}

			const AnimScene scene = ctx->get_arg<AnimScene>(0);
			const char* entity_name = ctx->get_arg<const char*>(1);
			CALL();
			auto record = MakeRecord("ANIM_SCENE_EXIT");
			record.owner_id = scene;
			record.text = entity_name != nullptr ? entity_name : "";
			record.result = *ctx->get_return_value<BOOL>() ? 1 : 0;
			diagnostic::Write(record);
		});

		NHOOK("SET_ANIM_SCENE_RATE", 0x75820B801CFF262A, {
			if (IsCraftingScript() && !diagnostic::IsInternalQuery()) {
				auto record = MakeRecord("ANIM_SCENE_RATE");
				record.owner_id = ctx->get_arg<AnimScene>(0);
				record.quantity = static_cast<int>(ctx->get_arg<float>(1) * 1000.0f);
				diagnostic::Write(record);
			}
			CALL();
		});
	}
#else
	void InitializeNormalHooks()
	{
		NHOOK("_ENABLE_HUD_CONTEXT_THIS_FRAME", 0xC9CAEAEEC1256E54, {
			if (IsCraftingScript() && ctx->get_arg<Hash>(0) == kCraftingHudContext) {
				skipped_frame = MISC::GET_FRAME_COUNT();
			}
			CALL();
		});

		NHOOK("_UI_PROMPT_SET_TEXT", 0x5DD02A8318420DD7, {
			if (IsCraftingScript() && skipped_frame != MISC::GET_FRAME_COUNT()) {
				const char* prompt_text = ctx->get_arg<const char*>(1);
				if (prompt_text != nullptr && IsCraftingPromptText(prompt_text)) {
					tracked_crafting_prompt = ctx->get_arg<Prompt>(0);
				}
			}
			CALL();
		});

		NHOOK("_UI_PROMPT_IS_JUST_PRESSED", 0x2787CC611D3FACC5, {
			const Prompt prompt = ctx->get_arg<Prompt>(0);
			CALL();
			if (IsCraftingScript()
				&& tracked_crafting_prompt != 0
				&& prompt == tracked_crafting_prompt
				&& *ctx->get_return_value<BOOL>()) {
				remaining_batch_crafts = kCraftBatchLimit;
				force_safe_breakout = false;
				batch_popup_shown = false;
			}
		});

		NHOOK("HAS_ANIM_EVENT_FIRED", 0x5851CC48405F4A07, {
			const Hash event_hash = ctx->get_arg<Hash>(1);
			CALL();
			if (!IsCraftingScript()) {
				return;
			}

			if (event_hash == kCraftCommitEvent && remaining_batch_crafts > 0) {
				ctx->set_return_value<BOOL>(true);
			}
			else if (event_hash == kSafeBreakoutEvent && force_safe_breakout) {
				ctx->set_return_value<BOOL>(true);
				force_safe_breakout = false;
			}
		});

		NHOOK("_INVENTORY_ADD_ITEM_WITH_GUID", 0xCB5D11F9508A928D, {
			CALL();
			if (IsCraftingScript() && remaining_batch_crafts > 0) {
				remaining_batch_crafts = 0;
				force_safe_breakout = true;
			}
		});
		NHOOK("_ADD_AMMO_TO_PED_BY_TYPE", 0x106A811C6D3035F3, {
			if (!IsCraftingScript()) {
				CALL();
				return;
			}

			const Ped ped = ctx->get_arg<Ped>(0);
			const Hash ammo_type = ctx->get_arg<Hash>(1);
			const int before_count = WEAPON::GET_PED_AMMO_BY_TYPE(ped, ammo_type);
			CALL();
			const int after_count = WEAPON::GET_PED_AMMO_BY_TYPE(ped, ammo_type);
			if (after_count <= before_count) {
				return;
			}

			if (remaining_batch_crafts > 0) {
				--remaining_batch_crafts;
			}
			else {
				remaining_batch_crafts = kCraftBatchLimit - 1;
				batch_popup_shown = false;
			}

			if (remaining_batch_crafts == 0) {
				force_safe_breakout = true;
			}
		});

		NHOOK("_UI_FEED_POST_SAMPLE_TOAST_RIGHT", 0xB249EBCB30DD88E0, {
			if (IsCraftingScript() && (remaining_batch_crafts > 0 || force_safe_breakout)) {
				if (batch_popup_shown) {
					ctx->set_return_value<int>(0);
					return;
				}
				batch_popup_shown = true;
			}
			CALL();
		});
	}
#endif
}

void ScriptMain()
{
#if ALLOCATE_CONSOLE
	AllocateConsole("Debug");
#endif

#if AUTOCRAFT_DIAGNOSTIC
	if (!diagnostic::Initialize()) {
		PRINT_ERROR("Initialization failed: diagnostic log could not be opened.");
		return;
	}
#endif

	scanner sc(nullptr);
	auto script_thread_pattern = sc.scan("48 89 2D ? ? ? ? 48 89 2D ? ? ? ? 48 8B 04 F9");
	auto native_handler_pattern = sc.scan("E8 ? ? ? ? 42 8B 9C FE");
	if (!script_thread_pattern.IsValid() || !native_handler_pattern.IsValid()) {
		PRINT_ERROR("Initialization failed: required game signatures were not found.");
		return;
	}

	current_script_thread = script_thread_pattern.Add(3).Rip().As<rage::scrThread**>();
	get_native_handler = native_handler_pattern.Add(1).Rip().As<GetNativeHandler>();
	if (current_script_thread == nullptr || get_native_handler == nullptr) {
		PRINT_ERROR("Initialization failed: required game functions could not be resolved.");
		return;
	}

#if AUTOCRAFT_DIAGNOSTIC
	InitializeDiagnosticHooks();
#else
	InitializeNormalHooks();
#endif

	for (;;) {
		WAIT(0);
	}
}
