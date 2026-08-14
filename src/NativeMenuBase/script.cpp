// Licensed under the MIT License.

#include "script.h"

#include "../../inc/main.h"
#include "../../inc/natives.h"
#include "console.h"
#include "hookhandler.hpp"
#include "rage.hpp"
#include "scanner.hpp"

#include <limits>
#include <string_view>

namespace
{
	constexpr int kCraftMultiplier = 10;
	constexpr Hash kCraftingScript = 954940763;
	constexpr Hash kCraftingMenuScript = 2790161247;
	constexpr Hash kCraftingHudContext = 0xE3FEFB3D;

	Prompt tracked_crafting_prompt = 0;
	int skipped_frame = -1;
	rage::scrThread** current_script_thread = nullptr;

	using GetNativeHandler = rage::scrNativeHandler(*)(rage::scrNativeHash hash);
	GetNativeHandler get_native_handler = nullptr;

	bool IsCraftingScript()
	{
		if (current_script_thread == nullptr || *current_script_thread == nullptr) {
			return false;
		}

		const auto script_hash = (*current_script_thread)->m_scriptHash;
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

	bool HasCompletedAutoCraftPrompt(rage::scrNativeCallContext* ctx)
	{
		const auto prompt = ctx->get_arg<Prompt>(0);
		return tracked_crafting_prompt != 0
			&& prompt == tracked_crafting_prompt
			&& HUD::_UI_PROMPT_HAS_HOLD_MODE_COMPLETED(prompt);
	}

	constexpr int ScaleCraftAmount(int amount) noexcept
	{
		if (amount <= 0 || amount > std::numeric_limits<int>::max() / kCraftMultiplier) {
			return amount;
		}

		return amount * kCraftMultiplier;
	}

	void InitializeHooks()
	{
		PRINT_INFO("Adding Hooks...");

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

		NHOOK("_UI_PROMPT_REGISTER_END", 0xF7AA2696A22AD8B9, {
			const auto prompt = ctx->get_arg<Prompt>(0);
			if (IsCraftingScript() && tracked_crafting_prompt != 0 && prompt == tracked_crafting_prompt) {
				HUD::_UI_PROMPT_SET_HOLD_AUTO_FILL_MODE(prompt, 3800, 1000);
			}

			CALL();
		});

		NHOOK("_UI_PROMPT_IS_JUST_PRESSED", 0x2787CC611D3FACC5, {
			if (IsCraftingScript() && HasCompletedAutoCraftPrompt(ctx)) {
				ctx->set_return_value<bool>(true);
				return;
			}

			CALL();
		});

		NHOOK("_UI_PROMPT_IS_PRESSED", 0x21E60E230086697F, {
			if (IsCraftingScript() && HasCompletedAutoCraftPrompt(ctx)) {
				ctx->set_return_value<bool>(true);
				return;
			}

			CALL();
		});

		NHOOK("_INVENTORY_ADD_ITEM_WITH_GUID", 0xCB5D11F9508A928D, {
			if (IsCraftingScript()) {
				ctx->set_arg<int>(5, ScaleCraftAmount(ctx->get_arg<int>(5)));
			}

			CALL();
		});

		NHOOK("_INVENTORY_REMOVE_INVENTORY_ITEM_WITH_ITEMID", 0xB4158C8C9A3B5DCE, {
			if (IsCraftingScript()) {
				ctx->set_arg<int>(2, ScaleCraftAmount(ctx->get_arg<int>(2)));
			}

			CALL();
		});

		NHOOK("_REMOVE_AMMO_FROM_PED_BY_TYPE", 0xB6CFEC32E3742779, {
			if (IsCraftingScript()) {
				ctx->set_arg<int>(2, ScaleCraftAmount(ctx->get_arg<int>(2)));
			}

			CALL();
		});

		NHOOK("_ADD_AMMO_TO_PED_BY_TYPE", 0x106A811C6D3035F3, {
			if (IsCraftingScript()) {
				ctx->set_arg<int>(2, ScaleCraftAmount(ctx->get_arg<int>(2)));
			}

			CALL();
		});
	}
}

void ScriptMain()
{
#if ALLOCATE_CONSOLE
	AllocateConsole("Debug");
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

	InitializeHooks();

	for (;;) {
		WAIT(0);
	}
}
