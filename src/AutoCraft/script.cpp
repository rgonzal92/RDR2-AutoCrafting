// Licensed under the MIT License.

// AutoCraft accelerates ammunition crafting by repeating RDR2's own validated
// craft transaction. It never changes an inventory or ammunition quantity.

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

// The Trace build is the normal build plus a decision log: identical hooks and
// behavior, with every batching decision written to AutoCraft-diagnostic.log
// so a play session produces evidence instead of anecdotes.
#ifndef AUTOCRAFT_TRACE
#define AUTOCRAFT_TRACE 0
#endif

namespace
{
	// The maximum number of successful game-validated ammo crafts per action.
	constexpr int kCraftBatchLimit = 50;
	// A forced craft normally commits in the same frame. This is only a fail-safe
	// for an unexpected script state that otherwise would keep the batch active.
	constexpr int kBatchNoProgressFrameLimit = 120;
	// Animation events and script hashes observed in player_camp and
	// interactive_campfire on RDR2 1.0.1491.50.
	constexpr Hash kCraftCommitEvent = 0xFC4F2858;
	constexpr Hash kSafeBreakoutEvent = 0x238C32C3;
	constexpr Hash kCraftingScript = 0x38EB3D5B;
	constexpr Hash kCraftingMenuScript = 0xA64E7B5F;
	constexpr Hash kCraftingHudContext = 0xE3FEFB3D;
	// These globals are diagnostic-only probes. They must be revalidated after a
	// game update because ScriptHook global indexes are build-specific.
	constexpr int kCraftRecipeCountGlobal = 1913490;
	constexpr int kCraftPreselectedRecipeGlobal = 1913491;
	// The crafting scripts consume this flag by rebuilding their recipe data.
	// Its index was verified on RDR2 1.0.1491.50.
	constexpr int kCraftMenuRefreshGlobal = 1913494;

	// Prompt handles identify the game controls that begin a craft or signal that
	// the recipe menu is active again.
	Prompt tracked_crafting_prompt = 0;
	rage::scrThread** current_script_thread = nullptr;

#if !AUTOCRAFT_DIAGNOSTIC
	// Non-ammo menu recipes commit on a script timer seeded from this exact
	// animation's duration; returning a tiny duration collapses that wait.
	// The strings match the script's own literals byte-for-byte.
	constexpr std::string_view kItemCraftAnimDict = "MECH_INVENTORY@CRAFTING@FALLBACKS@IN_HAND@MALE_A";
	constexpr std::string_view kItemCraftAnimClip = "craft_trans_stow";
	constexpr float kItemCraftShortDuration = 0.05f;
	// Auto-fill/hold times applied to the cooking-flow prompts. These are the
	// original upstream mod's shipped values, proven not to outrun the cook
	// animation that grants the cooked item.
	constexpr int kCookAutoFillMs = 3800;
	constexpr int kCookHoldMs = 1000;

	// The crafting script relabels one shared prompt per recipe type. Only a
	// prompt currently labeled CAMP_REC_MAKE may arm or drive an item batch;
	// cook/brew labels route to the cooking flow instead.
	bool tracked_prompt_is_make = false;
	// Last enabled state the game assigned to the tracked MAKE prompt. The
	// crafting script disables the prompt while a craft is in flight and
	// re-enables it only after re-validating ingredients and output capacity,
	// so this mirrors the game's own "may craft again" verdict. Forced presses
	// are gated on it because the item commit path removes ingredients before
	// granting output.
	bool make_prompt_enabled = false;
	int skipped_frame = -1;
	// These counters track real, successful game transactions—not requested
	// quantities. A failed transaction never decrements the remaining count.
	int remaining_batch_crafts = 0;
	int completed_batch_crafts = 0;
	bool pending_menu_refresh = false;
	bool force_safe_breakout = false;
	int forced_commit_frame = -1;
	bool awaiting_batch_output = false;
	// RDR2 reports safetobreakout after every successful craft animation. This
	// distinguishes that normal per-craft signal from a failed craft attempt.
	bool craft_succeeded_since_safe_breakout = false;
	bool batch_popup_shown = false;
	// True while the active batch repeats item (non-ammo) crafts via forced
	// "craft again" presses instead of forced animation events.
	bool item_batch = false;
	// An item batch may only continue seamlessly: if the next "craft again"
	// opportunity does not appear shortly after the last granted output (for
	// example the player backed out to the recipe list), the batch ends
	// instead of pressing a prompt in an unexpected menu state.
	constexpr int kBatchContinuityFrameLimit = 30;
	int last_craft_success_frame = -1;
	// Cooking-flow prompts converted to fast auto-fill holds (the original
	// upstream mechanism). A small ring buffer: several coexist while cooking.
	// The enabled flag mirrors the game's _UI_PROMPT_SET_ENABLED calls: leaving
	// a prompt disabled is the cooking flow's ONLY ingredient/capacity guard
	// (the press-reading script function does not check it, and a cook started
	// without ingredients both grants a free item and can wedge the player in
	// a promptless state), so automation must never act on a disabled prompt.
	// Automation presses a cooking prompt only after it has been continuously
	// enabled for this long AND the player ped is not mid scenario transition.
	// Do not shorten this: a 1000ms dwell wedged the cooking flow twice even
	// with the transition gate — the fatal condition is starting the next cook
	// while the ped is still IN the previous cooking pose, and no reliable
	// native probe exists for "fully settled". 3800ms is the original mod's
	// cadence and survived a full multi-stack session in traces. Pacing uses
	// the game clock because _UI_PROMPT_HAS_HOLD_MODE_COMPLETED reported
	// completion within two frames of a prompt's creation.
	constexpr int kCookPressDelayMs = 3800;
	constexpr int kMaxCookPrompts = 4;
	struct CookPrompt
	{
		Prompt handle;
		bool enabled;
		int enabled_since_ms;
	};
	CookPrompt cook_prompts[kMaxCookPrompts] = {};
	int cook_prompt_cursor = 0;

	// One cooking cycle removes its ingredient set when the cook starts and
	// grants the cooked item later from an animation event. The removals are
	// recorded so the grant hook can repeat the complete, verified exchange
	// for extra outputs inside the same animation.
	constexpr int kCookBatchSize = kCraftBatchLimit;
	constexpr int kMaxCookIngredients = 4;
	// Fallback stack limit when no slot query yields a sane answer. Kept well
	// below the standard 99 provision cap so the batch can never push an item
	// into the refused-add boundary that wedges the cooking flow.
	constexpr int kCookAssumedStackLimit = 90;
	// Windows are wall-clock milliseconds from GET_GAME_TIMER. Frame counts
	// scale with the player's frame rate and proved too short in traces: a
	// paced cook-and-stow cycle outlived a frame-based window at high fps and
	// the exchange silently skipped.
	// A grant older than this cannot belong to the recorded removals anymore.
	constexpr int kCookCycleWindowMs = 60000;
	// Removals separated by more than this belong to a new cooking cycle.
	constexpr int kCookIngredientGapMs = 5000;
	struct CookIngredient
	{
		Hash item;
		int inventory_id;
		int quantity;
		Hash reason;
	};
	CookIngredient cook_cycle_ingredients[kMaxCookIngredients] = {};
	int cook_cycle_ingredient_count = 0;
	int cook_cycle_last_ms = 0;
	// Set while AutoCraft performs its own verified inventory operations so
	// the hooks below do not record them as game activity.
	bool internal_inventory_op = false;
#endif

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

#if AUTOCRAFT_DIAGNOSTIC
	bool IsCraftingPromptText(std::string_view text)
	{
		return text == "CAMP_REC_COOK_AGN"
			|| text == "STOW_ITEM"
			|| text == "CRAFT_FASTER"
			|| text == "CAMP_REC_MAKE_AGN"
			|| text == "CAMP_REC_MAKE";
	}

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

#if AUTOCRAFT_DIAGNOSTIC
	// The diagnostic build observes native calls but never alters arguments or
	// return values, so one manual craft remains an unmodified RDR2 transaction.
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

		NHOOK("_UI_PROMPT_SET_ENABLED", 0x8A0FB4D03A630D21, {
			if (!IsCraftingScript() || diagnostic::IsInternalQuery()) {
				CALL();
				return;
			}

			const Prompt prompt = ctx->get_arg<Prompt>(0);
			const BOOL enabled = ctx->get_arg<BOOL>(1);
			CALL();
			if (prompt == tracked_crafting_prompt) {
				auto record = MakeRecord("PROMPT_SET_ENABLED");
				record.prompt = prompt;
				record.result = enabled ? 1 : 0;
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
			if (fired || event_hash == kCraftCommitEvent) {
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
#if AUTOCRAFT_TRACE
	/** Writes one mod-decision record to the trace log (Trace builds only). */
	void Trace(std::string_view event,
		std::optional<std::int64_t> owner = std::nullopt,
		std::optional<std::uint32_t> subject = std::nullopt,
		std::optional<int> quantity = std::nullopt,
		std::optional<int> result = std::nullopt,
		std::string_view text = {})
	{
		diagnostic::Record record;
		record.event = event;
		record.frame = MISC::GET_FRAME_COUNT();
		record.script_hash = CurrentScriptHash();
		record.owner_id = owner;
		record.subject_hash = subject;
		record.quantity = quantity;
		record.result = result;
		record.text = text;
		diagnostic::Write(record);
	}
#else
	template <typename... Args>
	void Trace(Args&&...) {}
#endif

	/** Returns whether a prompt label belongs to the cooking/fire-crafting flow. */
	bool IsCookFlowPromptText(std::string_view text)
	{
		return text == "CRAFT_FASTER"
			|| text == "STOW_ITEM"
			|| text == "CAMP_REC_COOK_AGN"
			|| text == "CAMP_REC_MAKE_AGN";
	}

	CookPrompt* FindCookPrompt(Prompt prompt)
	{
		if (prompt == 0) {
			return nullptr;
		}
		for (CookPrompt& tracked : cook_prompts) {
			if (tracked.handle == prompt) {
				return &tracked;
			}
		}
		return nullptr;
	}

	void TrackCookPrompt(Prompt prompt)
	{
		if (FindCookPrompt(prompt) != nullptr) {
			return;
		}
		// The game enables a prompt as part of registering it; assume enabled
		// until a _UI_PROMPT_SET_ENABLED call says otherwise.
		cook_prompts[cook_prompt_cursor] =
			CookPrompt{prompt, true, MISC::GET_GAME_TIMER()};
		cook_prompt_cursor = (cook_prompt_cursor + 1) % kMaxCookPrompts;
	}

	void ForgetCookPrompt(Prompt prompt)
	{
		for (CookPrompt& tracked : cook_prompts) {
			if (tracked.handle == prompt) {
				tracked = CookPrompt{};
			}
		}
	}

	/**
	 * Returns whether automation may press this cooking prompt right now: the
	 * game must have it enabled, it must have stayed enabled for the dwell
	 * time, and the player must not be mid scenario transition — pressing
	 * during a transition is what wedged the cooking state machine.
	 * Pressing resets the dwell so a prompt is never pressed twice in a burst.
	 */
	bool TryConsumeCookPress(Prompt prompt)
	{
		CookPrompt* tracked = FindCookPrompt(prompt);
		if (tracked == nullptr || !tracked->enabled) {
			return false;
		}
		const int now_ms = MISC::GET_GAME_TIMER();
		if (now_ms - tracked->enabled_since_ms < kCookPressDelayMs) {
			return false;
		}
		if (PED::_IS_PED_DOING_SCENARIO_TRANSITION(PLAYER::PLAYER_PED_ID())) {
			// Not a consumed press: the dwell stays satisfied and the press
			// fires on the first frame the transition completes.
			return false;
		}
		tracked->enabled_since_ms = now_ms;
		return true;
	}

	/** Resets transient batch state after RDR2 completes or rejects a craft. */
	void FinishBatch()
	{
		Trace("BATCH_FINISH", std::nullopt, std::nullopt, completed_batch_crafts,
			item_batch ? 1 : 0);
		remaining_batch_crafts = 0;
		force_safe_breakout = false;
		forced_commit_frame = -1;
		awaiting_batch_output = false;
		craft_succeeded_since_safe_breakout = false;
		item_batch = false;
		last_craft_success_frame = -1;

		// A single craft already refreshes the recipe menu. Request a rebuild only
		// when RDR2 committed additional transactions inside the same menu action.
		if (completed_batch_crafts > 1) {
			pending_menu_refresh = true;
		}
		completed_batch_crafts = 0;
	}

	/** Records one successful, game-validated craft and advances the batch. */
	void CountBatchCraft(bool is_item_craft)
	{
		Trace("CRAFT_COUNTED", std::nullopt, std::nullopt, remaining_batch_crafts,
			is_item_craft ? 1 : 0);
		forced_commit_frame = -1;
		awaiting_batch_output = false;
		item_batch = is_item_craft;
		last_craft_success_frame = MISC::GET_FRAME_COUNT();
		++completed_batch_crafts;
		if (!is_item_craft) {
			craft_succeeded_since_safe_breakout = true;
		}

		if (remaining_batch_crafts > 0) {
			--remaining_batch_crafts;
		}
		else {
			// The first craft of a menu selection arms the rest of the batch the
			// moment its output is granted.
			remaining_batch_crafts = kCraftBatchLimit - 1;
			batch_popup_shown = false;
		}

		if (remaining_batch_crafts == 0) {
			if (is_item_craft) {
				FinishBatch();
			}
			else {
				force_safe_breakout = true;
			}
		}
	}

	// Result codes for one attempted cooking exchange, written to the trace.
	constexpr int kCookExchangeOk = 1;
	constexpr int kCookExchangeNoHandler = 2;
	constexpr int kCookExchangeAtCapacity = 3;
	constexpr int kCookExchangeIngredientShort = 4;
	constexpr int kCookExchangeRemoveMismatch = 5;
	constexpr int kCookExchangeGrantMismatch = 6;
	constexpr int kCookExchangeNoLimitData = 7;

	/**
	 * Performs one additional cooking exchange inside the current grant call:
	 * verifies ingredient availability, re-runs the game's own grant with its
	 * original arguments, and — only after the game actually added the
	 * output — removes one full recorded ingredient set.
	 *
	 * All checks and the exchange run inside one native call on the script
	 * thread, so a verified count cannot change before it is used.
	 *
	 * @return kCookExchangeOk when one extra output was granted; any other
	 *         code stops the batch and names the reason in the trace.
	 */
	int TryCookExtraExchange(
		rage::scrNativeCallContext* ctx,
		rage::scrNativeHandler grant,
		int output_inventory_id,
		Hash output_item,
		int output_quantity,
		int output_limit)
	{
		if (grant == nullptr) {
			return kCookExchangeNoHandler;
		}
		// The game's scripts always pre-check capacity and never call the add
		// native when a slot is full; a refused add is an untested game path
		// and it wedged the cooking flow in traces. The batch must therefore
		// stop BEFORE the boundary: one grant of margin below the verified
		// limit, so the game's own pre-checked flow handles the last unit.
		// Without a trustworthy limit there is no safe way to batch at all.
		if (output_limit < 0) {
			return kCookExchangeNoLimitData;
		}
		const int output_count_now = INVENTORY::_INVENTORY_GET_INVENTORY_ITEM_COUNT_WITH_ITEMID(
			output_inventory_id, output_item, false);
		if (output_count_now + 2 * output_quantity > output_limit) {
			return kCookExchangeAtCapacity;
		}

		// Every ingredient must be available BEFORE granting: the payment
		// below must never be able to fail once the output has been granted.
		// Draining the stack to zero is safe: the game re-evaluates its
		// cook-again prompt's enabled state from live inventory after every
		// cook, and automation only ever presses enabled prompts.
		for (int i = 0; i < cook_cycle_ingredient_count; ++i) {
			const CookIngredient& ingredient = cook_cycle_ingredients[i];
			if (INVENTORY::_INVENTORY_GET_INVENTORY_ITEM_COUNT_WITH_ITEMID(
					ingredient.inventory_id, ingredient.item, false) < ingredient.quantity) {
				return kCookExchangeIngredientShort;
			}
		}

		// Grant first, pay second. If the game's own add refuses or no-ops,
		// nothing has been consumed and the batch simply stops — no capacity
		// prediction needed. The previous grant's return value overwrote the
		// first argument slot; restore it so the re-run sees its original
		// arguments.
		const int before_output = INVENTORY::_INVENTORY_GET_INVENTORY_ITEM_COUNT_WITH_ITEMID(
			output_inventory_id, output_item, false);
		ctx->get_arg<int>(0) = output_inventory_id;
		grant(ctx);
		const int after_output = INVENTORY::_INVENTORY_GET_INVENTORY_ITEM_COUNT_WITH_ITEMID(
			output_inventory_id, output_item, false);
		if (after_output - before_output != output_quantity) {
			return kCookExchangeGrantMismatch;
		}

		for (int i = 0; i < cook_cycle_ingredient_count; ++i) {
			const CookIngredient& ingredient = cook_cycle_ingredients[i];
			const int before = INVENTORY::_INVENTORY_GET_INVENTORY_ITEM_COUNT_WITH_ITEMID(
				ingredient.inventory_id, ingredient.item, false);
			INVENTORY::_INVENTORY_REMOVE_INVENTORY_ITEM_WITH_ITEMID(
				ingredient.inventory_id, ingredient.item, ingredient.quantity, ingredient.reason);
			const int after = INVENTORY::_INVENTORY_GET_INVENTORY_ITEM_COUNT_WITH_ITEMID(
				ingredient.inventory_id, ingredient.item, false);
			if (before - after != ingredient.quantity) {
				// Unreachable given the availability check above; traced loudly
				// if it ever happens so the exchange can be re-examined.
				return kCookExchangeRemoveMismatch;
			}
		}
		return kCookExchangeOk;
	}

	/** Requests RDR2's own recipe-menu rebuild after a multi-craft batch. */
	void RefreshCraftingMenu()
	{
		if (!pending_menu_refresh) {
			return;
		}

		if (auto refresh_flag = getGlobalPtr(kCraftMenuRefreshGlobal); refresh_flag != nullptr) {
			*refresh_flag = 1;
			pending_menu_refresh = false;
		}
	}

	/** Installs the release hooks that batch validated crafting transactions. */
	void InitializeNormalHooks()
	{
		// RDR2 sets this HUD context for an unrelated prompt path. Ignore prompt
		// text from that path so it cannot arm a crafting batch.
		NHOOK("_ENABLE_HUD_CONTEXT_THIS_FRAME", 0xC9CAEAEEC1256E54, {
			if (IsCraftingScript() && ctx->get_arg<Hash>(0) == kCraftingHudContext) {
				skipped_frame = MISC::GET_FRAME_COUNT();
			}
			CALL();
		});

		NHOOK("_UI_PROMPT_SET_TEXT", 0x5DD02A8318420DD7, {
			bool convert_cook_prompt = false;
			Prompt prompt = 0;
			if (IsCraftingScript() && skipped_frame != MISC::GET_FRAME_COUNT()) {
				const char* prompt_text = ctx->get_arg<const char*>(1);
				if (prompt_text != nullptr) {
					prompt = ctx->get_arg<Prompt>(0);
					const std::string_view text(prompt_text);
					if (text == "CAMP_REC_MAKE") {
						tracked_crafting_prompt = prompt;
						tracked_prompt_is_make = true;
						ForgetCookPrompt(prompt);
						// Entering menu context invalidates any recorded cooking
						// cycle, so a menu craft's grant can never be mistaken
						// for a cooking grant.
						cook_cycle_ingredient_count = 0;
						Trace("TRACK_MAKE", prompt, std::nullopt, std::nullopt, std::nullopt, text);
					}
					else if (text == "CAMP_REC_COOK" || text == "CAMP_REC_BREW") {
						// Cook/brew recipes relabel the same menu prompt and route
						// to the cooking flow; they must not arm an item batch and
						// must not auto-fill from the recipe menu.
						if (prompt == tracked_crafting_prompt) {
							tracked_prompt_is_make = false;
						}
						ForgetCookPrompt(prompt);
					}
					else if (IsCookFlowPromptText(text)) {
						TrackCookPrompt(prompt);
						// Cooking-flow prompts only exist while the cooking flow
						// runs, so seeing one means the recipe menu is gone. The
						// menu latch must clear even though the cook prompts use
						// different handles than the menu's MAKE prompt: a stale
						// latch misroutes cooking grants into the item-batch
						// path and disables the cook exchange.
						tracked_prompt_is_make = false;
						convert_cook_prompt = true;
						Trace("TRACK_COOK", prompt, std::nullopt, std::nullopt, std::nullopt, text);
					}
					else {
						// The game deletes and recreates prompts freely, so a
						// recycled handle relabeled to anything unrelated must
						// drop out of the cooking set.
						ForgetCookPrompt(prompt);
					}
				}
			}
			CALL();
			if (convert_cook_prompt) {
				// Covers relabels of prompts that were registered earlier. A fresh
				// registration applies its own mode after this text call, and the
				// _UI_PROMPT_REGISTER_END hook converts it again.
				HUD::_UI_PROMPT_SET_HOLD_AUTO_FILL_MODE(prompt, kCookAutoFillMs, kCookHoldMs);
			}
		});

		// Cooking-flow prompts become fast self-filling holds, so one manual
		// cook continues through cook/stow/cook-again without further input.
		// This is the original upstream mod's mechanism with shorter times.
		NHOOK("_UI_PROMPT_REGISTER_END", 0xF7AA2696A22AD8B9, {
			const Prompt prompt = ctx->get_arg<Prompt>(0);
			CALL();
			if (IsCraftingScript() && FindCookPrompt(prompt) != nullptr) {
				HUD::_UI_PROMPT_SET_HOLD_AUTO_FILL_MODE(prompt, kCookAutoFillMs, kCookHoldMs);
			}
		});

		NHOOK("_UI_PROMPT_SET_ENABLED", 0x8A0FB4D03A630D21, {
			if (IsCraftingScript()) {
				const Prompt prompt = ctx->get_arg<Prompt>(0);
				const bool enabled = ctx->get_arg<BOOL>(1) != 0;
				if (prompt == tracked_crafting_prompt) {
					Trace("MAKE_ENABLED", prompt, std::nullopt, std::nullopt, enabled ? 1 : 0);
					make_prompt_enabled = enabled;
					// After each item craft the game re-enables the MAKE prompt only
					// when another craft passed its ingredient and capacity checks. A
					// final "disabled" verdict with no craft in flight ends the batch.
					if (item_batch && !awaiting_batch_output && !make_prompt_enabled
						&& remaining_batch_crafts > 0) {
						FinishBatch();
					}
				}
				if (CookPrompt* cook_prompt = FindCookPrompt(prompt); cook_prompt != nullptr) {
					Trace("COOK_ENABLED", prompt, std::nullopt, std::nullopt, enabled ? 1 : 0);
					if (enabled && !cook_prompt->enabled) {
						cook_prompt->enabled_since_ms = MISC::GET_GAME_TIMER();
					}
					cook_prompt->enabled = enabled;
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

			// Cooking flow: press the prompt for the player after its dwell time
			// so the script advances through cook, stow, and cook-again on its
			// own. Only for prompts the game currently has enabled: a disabled
			// cook-again prompt is the game's sole out-of-ingredients guard, and
			// pressing through it starts an unpayable cook that grants a free
			// item and can wedge the player in a promptless cooking state.
			if (!pressed && TryConsumeCookPress(prompt)) {
				Trace("COOK_PRESS", prompt);
				ctx->set_return_value<BOOL>(true);
				return;
			}

			if (prompt != tracked_crafting_prompt || !tracked_prompt_is_make) {
				return;
			}

			if (!pressed) {
				// The script is idle on its MAKE prompt, which is the only safe
				// point to request RDR2's build-specific UI refresh flag.
				RefreshCraftingMenu();

				// Item batches repeat by pressing "craft again" for the player,
				// but only while the game itself re-enabled the prompt—its own
				// signal that ingredients and capacity allow another craft. The
				// continuity window keeps a leftover batch from pressing prompts
				// in a menu state it did not start in.
				if (item_batch && remaining_batch_crafts > 0 && !awaiting_batch_output) {
					if (MISC::GET_FRAME_COUNT() - last_craft_success_frame
						> kBatchContinuityFrameLimit) {
						FinishBatch();
					}
					else if (make_prompt_enabled) {
						Trace("FORCE_PRESS", prompt, std::nullopt, remaining_batch_crafts);
						ctx->set_return_value<BOOL>(true);
						awaiting_batch_output = true;
						forced_commit_frame = MISC::GET_FRAME_COUNT();
					}
				}
				return;
			}

			// A real press arms a full batch. The first craft selected from the
			// recipe list arms itself when its first output add succeeds below.
			Trace("BATCH_ARMED", prompt);
			remaining_batch_crafts = kCraftBatchLimit;
			completed_batch_crafts = 0;
			force_safe_breakout = false;
			forced_commit_frame = -1;
			awaiting_batch_output = false;
			craft_succeeded_since_safe_breakout = false;
			batch_popup_shown = false;
			item_batch = false;
			last_craft_success_frame = MISC::GET_FRAME_COUNT();
		});

		NHOOK("HAS_ANIM_EVENT_FIRED", 0x5851CC48405F4A07, {
			const Hash event_hash = ctx->get_arg<Hash>(1);
			CALL();
			if (!IsCraftingScript()) {
				return;
			}

			// The camp scripts poll animation events every frame, which makes
			// this a reliable place for the item-batch no-progress fail-safe.
			if (item_batch && awaiting_batch_output
				&& MISC::GET_FRAME_COUNT() - forced_commit_frame >= kBatchNoProgressFrameLimit) {
				FinishBatch();
			}

			if (item_batch) {
				// Item crafts never consult the commit or safetobreakout events;
				// leave them untouched while an item batch runs.
				return;
			}

			if (event_hash == kCraftCommitEvent && remaining_batch_crafts > 0) {
				// Let the game reach its existing validation and inventory path without
				// waiting for another visible craft-animation event.
				if (!awaiting_batch_output) {
					forced_commit_frame = MISC::GET_FRAME_COUNT();
					awaiting_batch_output = true;
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
				}
				else if (remaining_batch_crafts > 0 && game_wants_to_exit) {
					// RDR2 declined the next transaction because of ingredients or
					// capacity, so preserve its normal partial-batch exit.
					FinishBatch();
				}
				else if (remaining_batch_crafts > 0
					&& awaiting_batch_output
					&& MISC::GET_FRAME_COUNT() - forced_commit_frame >= kBatchNoProgressFrameLimit) {
					// Do not keep forcing an unacknowledged transaction forever if a
					// script variant neither adds ammo nor emits its normal exit event.
					ctx->set_return_value<BOOL>(true);
					FinishBatch();
				}
			}
		});

		// Cooking removes its ingredients when the cook starts, long before the
		// cooked item is granted. Record those removals so the grant below can
		// repeat the complete exchange. Menu recipes (CAMP_REC_MAKE) manage
		// their own ingredient flow and are never recorded.
		NHOOK("_INVENTORY_REMOVE_INVENTORY_ITEM_WITH_ITEMID", 0xB4158C8C9A3B5DCE, {
			if (IsCraftingScript() && !internal_inventory_op && !tracked_prompt_is_make) {
				const int now_ms = MISC::GET_GAME_TIMER();
				if (cook_cycle_ingredient_count > 0
					&& now_ms - cook_cycle_last_ms > kCookIngredientGapMs) {
					cook_cycle_ingredient_count = 0;
				}
				if (cook_cycle_ingredient_count < kMaxCookIngredients) {
					CookIngredient& ingredient = cook_cycle_ingredients[cook_cycle_ingredient_count];
					ingredient.inventory_id = ctx->get_arg<int>(0);
					ingredient.item = ctx->get_arg<Hash>(1);
					ingredient.quantity = ctx->get_arg<int>(2);
					ingredient.reason = ctx->get_arg<Hash>(3);
					++cook_cycle_ingredient_count;
					cook_cycle_last_ms = now_ms;
					Trace("COOK_INGREDIENT", ingredient.inventory_id, ingredient.item,
						ingredient.quantity);
				}
			}
			CALL();
		});

		NHOOK("_INVENTORY_ADD_ITEM_WITH_GUID", 0xCB5D11F9508A928D, {
			if (!IsCraftingScript() || internal_inventory_op) {
				CALL();
				return;
			}

			const int inventory_id = ctx->get_arg<int>(0);
			const Hash item = ctx->get_arg<Hash>(3);
			const Hash slot = ctx->get_arg<Hash>(4);
			const int before_count =
				INVENTORY::_INVENTORY_GET_INVENTORY_ITEM_COUNT_WITH_ITEMID(inventory_id, item, false);
			CALL();
			const BOOL result = *ctx->get_return_value<BOOL>();
			const int after_count =
				INVENTORY::_INVENTORY_GET_INVENTORY_ITEM_COUNT_WITH_ITEMID(inventory_id, item, false);
			if (!result || after_count <= before_count) {
				// Includes the game's silent at-capacity add failure; traced so a
				// missing grant is visible in the log.
				Trace("GRANT_FAILED", inventory_id, item, std::nullopt, result ? 1 : 0);
				return;
			}

			// An active cooking cycle is the strongest context signal: it only
			// exists while an actual cook is in flight (menu entry clears it),
			// so it outranks the menu latch when routing this grant.
			if (cook_cycle_ingredient_count > 0
				&& MISC::GET_GAME_TIMER() - cook_cycle_last_ms > kCookCycleWindowMs) {
				Trace("COOK_STALE", inventory_id, item);
				cook_cycle_ingredient_count = 0;
			}
			if (cook_cycle_ingredient_count == 0) {
				// A confirmed grant is the success signal for menu-recipe item
				// crafts, mirroring the ammo path below.
				if (tracked_prompt_is_make) {
					CountBatchCraft(true);
				}
				else {
					Trace("COOK_NO_CYCLE", inventory_id, item);
				}
				return;
			}

			// Cooking grants land here: the game validated and granted one cooked
			// item for the ingredient set it removed when the cook started. Repeat
			// that complete exchange for extra outputs inside the same animation,
			// one verified set at a time, stopping at the first shortage. Brewing
			// never grants an item, so it can never reach this path.

			const int granted_per_cook = after_count - before_count;
			Trace("COOK_GRANT", inventory_id, item, granted_per_cook);

			// Derive a trustworthy stack limit for the cooked item, preferring
			// the game's own upgrade-aware lookup: the crafting scripts resolve
			// stack limits through _GET_ITEM_ROLE_MAX_LEVEL_COUNT for items
			// with upgradeable stacks (satchel upgrades), and only fall back to
			// _GET_ITEM_SLOT_MAX_COUNT, which reports the base size (the
			// mysterious "5" for meat capped at 99 on an upgraded satchel).
			// Each answer must pass the sanity test: a real limit can never be
			// smaller than the count already held. In the trace: owner = the
			// queried limit, subject = the slot/item hash, quantity = count.
			const int role_limit =
				INVENTORY::_GET_ITEM_ROLE_MAX_LEVEL_COUNT(inventory_id, item);
			const int grant_slot_limit = INVENTORY::_GET_ITEM_SLOT_MAX_COUNT(item, slot);
			Trace("COOK_ROLE_LIMIT", role_limit, item, after_count);
			Trace("COOK_SLOT_INFO", grant_slot_limit, slot, after_count);
			int output_limit = -1;
			if (role_limit > 0 && role_limit >= after_count) {
				output_limit = role_limit;
			}
			else if (grant_slot_limit > 0 && grant_slot_limit >= after_count) {
				output_limit = grant_slot_limit;
			}
			else {
				// No sane answer from any query: assume a limit safely below
				// the standard 99 provision cap rather than disabling batching.
				output_limit = kCookAssumedStackLimit;
			}

			internal_inventory_op = true;
			for (int extra = 1; extra < kCookBatchSize; ++extra) {
				const int status = TryCookExtraExchange(
					ctx, original, inventory_id, item, granted_per_cook, output_limit);
				Trace("COOK_EXTRA", std::nullopt, item, extra, status);
				if (status != kCookExchangeOk) {
					break;
				}
			}
			internal_inventory_op = false;
			cook_cycle_ingredient_count = 0;
			// The extra grants overwrote the return slot; the script must see its
			// own call's result.
			ctx->set_return_value(result);
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

			// A positive count change is the definitive success signal. This is why
			// each batch remains limited by the player's real ingredients and capacity.
			const bool starts_new_batch = remaining_batch_crafts == 0;
			if (!starts_new_batch && !awaiting_batch_output) {
				return;
			}

			CountBatchCraft(false);
		});

		// Non-ammo recipes wait out this exact animation's duration before each
		// commit; a tiny duration makes the crafting script commit immediately.
		// Ammo recipes never read it, and no other call site exists in the two
		// crafting scripts.
		NHOOK("GET_ANIM_DURATION", 0x9FFAF4940A54CC09, {
			// The return value overwrites the first argument slot, so the string
			// arguments must be read before the original runs.
			bool shorten = false;
			if (IsCraftingScript()) {
				const char* anim_dict = ctx->get_arg<const char*>(0);
				const char* anim_clip = ctx->get_arg<const char*>(1);
				shorten = anim_dict != nullptr && anim_clip != nullptr
					&& kItemCraftAnimDict == anim_dict && kItemCraftAnimClip == anim_clip;
			}
			CALL();
			if (shorten) {
				Trace("ANIM_SHORTENED");
				ctx->set_return_value(kItemCraftShortDuration);
			}
		});

		NHOOK("_UI_PROMPT_IS_PRESSED", 0x21E60E230086697F, {
			const Prompt prompt = ctx->get_arg<Prompt>(0);
			CALL();
			if (IsCraftingScript() && *ctx->get_return_value<BOOL>() == 0
				&& TryConsumeCookPress(prompt)) {
				Trace("COOK_PRESS", prompt);
				ctx->set_return_value<BOOL>(true);
			}
		});

		NHOOK("_UI_FEED_POST_SAMPLE_TOAST_RIGHT", 0xB249EBCB30DD88E0, {
			if (IsCraftingScript() && (remaining_batch_crafts > 0 || force_safe_breakout)) {
				// Inventory changes still occur individually; only repeated UI toasts
				// are hidden after the first one in the same batch.
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
