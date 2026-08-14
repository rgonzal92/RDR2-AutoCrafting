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

#ifndef AUTOCRAFT_TRACE
#define AUTOCRAFT_TRACE 0
#endif

namespace
{
	constexpr int kCraftBatchLimit = 50;
	constexpr int kBatchNoProgressFrameLimit = 120;
	constexpr Hash kCraftCommitEvent = 0xFC4F2858;
	constexpr Hash kSafeBreakoutEvent = 0x238C32C3;
	constexpr Hash kCraftingScript = 0x38EB3D5B;
	constexpr Hash kCraftingMenuScript = 0xA64E7B5F;
	constexpr Hash kCraftingHudContext = 0xE3FEFB3D;
	constexpr int kCraftRecipeCountGlobal = 1913490;
	constexpr int kCraftPreselectedRecipeGlobal = 1913491;
	constexpr int kCraftMenuRefreshGlobal = 1913494;

	Prompt tracked_crafting_prompt = 0;
	Prompt recipe_menu_prompt = 0;
	int skipped_frame = -1;
	int remaining_batch_crafts = 0;
	int completed_batch_crafts = 0;
	bool pending_menu_refresh = false;
	bool force_safe_breakout = false;
	int forced_commit_frame = -1;
	bool awaiting_batch_ammo_add = false;
	// RDR2 reports safetobreakout after every successful craft animation. This
	// distinguishes that normal per-craft signal from a failed craft attempt.
	bool craft_succeeded_since_safe_breakout = false;
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

#if AUTOCRAFT_DIAGNOSTIC || AUTOCRAFT_TRACE
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
#endif

#if AUTOCRAFT_TRACE
	void TraceBatchState(
		std::string_view event,
		std::optional<Hash> subject_hash = std::nullopt,
		std::optional<int> result = std::nullopt,
		std::optional<Prompt> prompt = std::nullopt,
		std::string_view text = {})
	{
		auto record = MakeRecord(event);
		record.subject_hash = subject_hash;
		record.before_count = remaining_batch_crafts;
		record.after_count = completed_batch_crafts;
		record.capacity = force_safe_breakout ? 1 : 0;
		record.quantity = pending_menu_refresh ? 1 : 0;
		record.result = result;
		record.prompt = prompt;
		record.text = text;
		diagnostic::Write(record);
	}

	void TraceAmmoNative(Ped ped, Hash ammo_type, int quantity, int before_count, int after_count)
	{
		auto record = MakeRecord("BATCH_AMMO_NATIVE");
		record.owner_id = ped;
		record.subject_hash = ammo_type;
		record.quantity = quantity;
		record.before_count = before_count;
		record.after_count = after_count;
		diagnostic::Write(record);
	}

	#define TRACE_BATCH(...) TraceBatchState(__VA_ARGS__)
	#define TRACE_AMMO_NATIVE(...) TraceAmmoNative(__VA_ARGS__)
#else
	#define TRACE_BATCH(...) ((void)0)
	#define TRACE_AMMO_NATIVE(...) ((void)0)
#endif

	void FinishBatch()
	{
		TRACE_BATCH("BATCH_FINISH_BEGIN");
		remaining_batch_crafts = 0;
		force_safe_breakout = false;
		forced_commit_frame = -1;
		awaiting_batch_ammo_add = false;
		craft_succeeded_since_safe_breakout = false;

		// A single craft already refreshes the recipe menu. Request a rebuild only
		// when RDR2 committed additional transactions inside the same menu action.
		if (completed_batch_crafts > 1) {
			pending_menu_refresh = true;
		}
		completed_batch_crafts = 0;
		TRACE_BATCH("BATCH_FINISH_END");
	}

	void RefreshCraftingMenu()
	{
		if (!pending_menu_refresh) {
			return;
		}

		if (auto refresh_flag = getGlobalPtr(kCraftMenuRefreshGlobal); refresh_flag != nullptr) {
			*refresh_flag = 1;
			pending_menu_refresh = false;
			TRACE_BATCH("MENU_REFRESH_WRITE", std::nullopt, 1);
		}
	}

#if AUTOCRAFT_DIAGNOSTIC
	int last_global_trace_frame = -60;

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

			const int frame = MISC::GET_FRAME_COUNT();
			if (frame - last_global_trace_frame >= 60) {
				const auto recipe_count = getGlobalPtr(kCraftRecipeCountGlobal);
				const auto selected_recipe = getGlobalPtr(kCraftPreselectedRecipeGlobal);
				const auto refresh_flag = getGlobalPtr(kCraftMenuRefreshGlobal);
				if (recipe_count != nullptr && selected_recipe != nullptr && refresh_flag != nullptr) {
					auto record = MakeRecord("CRAFT_GLOBALS");
					record.before_count = static_cast<int>(*recipe_count);
					record.subject_hash = static_cast<Hash>(*selected_recipe);
					record.result = *refresh_flag != 0 ? 1 : 0;
					diagnostic::Write(record);
					last_global_trace_frame = frame;
				}
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
					if (std::string_view(prompt_text) == "CAMP_REC_MAKE") {
						recipe_menu_prompt = tracked_crafting_prompt;
					}
					TRACE_BATCH("BATCH_PROMPT_TEXT", std::nullopt, std::nullopt,
						tracked_crafting_prompt, prompt_text);
				}
			}
			CALL();
		});

		NHOOK("_UI_PROMPT_IS_JUST_PRESSED", 0x2787CC611D3FACC5, {
			const Prompt prompt = ctx->get_arg<Prompt>(0);
			CALL();
			if (!IsCraftingScript()) {
				return;
			}

			const bool pressed = *ctx->get_return_value<BOOL>() != 0;
			if (pressed || prompt == recipe_menu_prompt) {
				TRACE_BATCH("BATCH_PROMPT_QUERY", std::nullopt, pressed ? 1 : 0, prompt);
			}
			if (prompt == recipe_menu_prompt && !pressed) {
				RefreshCraftingMenu();
			}
			if (tracked_crafting_prompt != 0
				&& prompt == tracked_crafting_prompt
				&& pressed) {
				remaining_batch_crafts = kCraftBatchLimit;
				completed_batch_crafts = 0;
				force_safe_breakout = false;
				forced_commit_frame = -1;
				awaiting_batch_ammo_add = false;
				craft_succeeded_since_safe_breakout = false;
				batch_popup_shown = false;
				TRACE_BATCH("BATCH_ARM", std::nullopt, 1, prompt);
			}
		});

		NHOOK("HAS_ANIM_EVENT_FIRED", 0x5851CC48405F4A07, {
			const Hash event_hash = ctx->get_arg<Hash>(1);
			CALL();
			if (!IsCraftingScript()) {
				return;
			}
			if (event_hash == kCraftCommitEvent || event_hash == kSafeBreakoutEvent) {
				TRACE_BATCH("BATCH_ANIM_EVENT", event_hash,
					*ctx->get_return_value<BOOL>() != 0 ? 1 : 0);
			}

			if (event_hash == kCraftCommitEvent && remaining_batch_crafts > 0) {
				if (!awaiting_batch_ammo_add) {
					forced_commit_frame = MISC::GET_FRAME_COUNT();
					awaiting_batch_ammo_add = true;
				}
				ctx->set_return_value<BOOL>(true);
			}
			else if (event_hash == kSafeBreakoutEvent) {
				const bool game_wants_to_exit = *ctx->get_return_value<BOOL>() != 0;
				if (force_safe_breakout) {
					ctx->set_return_value<BOOL>(true);
					FinishBatch();
				}
				else if (remaining_batch_crafts > 0 && craft_succeeded_since_safe_breakout) {
					// Keep the loop alive after a real craft; the next forced commit is
					// still validated by RDR2 before it can add any ammo.
					ctx->set_return_value<BOOL>(false);
					craft_succeeded_since_safe_breakout = false;
					TRACE_BATCH("BATCH_BREAKOUT_SUPPRESSED", event_hash, 0);
				}
				else if (remaining_batch_crafts > 0 && game_wants_to_exit) {
					// RDR2 declined the next transaction because of ingredients or
					// capacity, so preserve its normal partial-batch exit.
					FinishBatch();
					TRACE_BATCH("BATCH_BREAKOUT_ON_FAILURE", event_hash, 1);
				}
				else if (remaining_batch_crafts > 0
					&& awaiting_batch_ammo_add
					&& MISC::GET_FRAME_COUNT() - forced_commit_frame >= kBatchNoProgressFrameLimit) {
					// Do not keep forcing an unacknowledged transaction forever if a
					// script variant neither adds ammo nor emits its normal exit event.
					ctx->set_return_value<BOOL>(true);
					FinishBatch();
					TRACE_BATCH("BATCH_BREAKOUT_ON_TIMEOUT", event_hash, 1);
				}
			}
		});

		NHOOK("_INVENTORY_ADD_ITEM_WITH_GUID", 0xCB5D11F9508A928D, {
			CALL();
			if (IsCraftingScript() && remaining_batch_crafts > 0) {
				TRACE_BATCH("BATCH_CANCEL_ITEM_ADD");
				remaining_batch_crafts = 0;
				completed_batch_crafts = 0;
				forced_commit_frame = -1;
				awaiting_batch_ammo_add = false;
				craft_succeeded_since_safe_breakout = false;
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
			TRACE_AMMO_NATIVE(ped, ammo_type, ctx->get_arg<int>(2), before_count, after_count);
			if (after_count <= before_count) {
				return;
			}

			const bool starts_new_batch = remaining_batch_crafts == 0;
			if (!starts_new_batch && !awaiting_batch_ammo_add) {
				return;
			}

			forced_commit_frame = -1;
			awaiting_batch_ammo_add = false;
			++completed_batch_crafts;
			craft_succeeded_since_safe_breakout = true;

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
			TRACE_BATCH("BATCH_AMMO_STATE", ammo_type, after_count > before_count ? 1 : 0);
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

#if AUTOCRAFT_DIAGNOSTIC || AUTOCRAFT_TRACE
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
