# How AutoCraft works

This document explains the mod in plain English. You do not need to know C++
to use it, but it also describes where the important behavior lives in the
source code.

## What the mod does

AutoCraft applies to **every recipe selected from the crafting menu**. One
craft action can perform up to 50 normal RDR2 crafts without waiting for each
repeated animation. Cooking and brewing at a campfire are automated rather
than batched (see "Cooking automation" below).

It does **not** multiply an item amount. Instead, RDR2 still performs each
individual transaction:

1. RDR2 checks the recipe's ingredients and available output capacity.
2. RDR2 removes the required ingredients.
3. RDR2 adds one normal output (ammunition or an inventory item).
4. AutoCraft counts that successful addition and, if the batch has not ended,
   lets RDR2 begin the next validated transaction immediately.

If there are ingredients for 7 crafts, the batch makes 7. If the ammo pouch or
satchel slot is nearly full, the batch stops when no more output fits.

## Terms used in the source

- **Native**: a function supplied by RDR2, such as "add ammo".
- **Hook**: a small wrapper that lets AutoCraft observe or adjust one native
  call before returning control to RDR2.
- **Transaction**: one complete game-validated craft: ingredient removal plus
  successful output addition.
- **Batch**: up to 50 transactions started by one player craft action.

## Startup

`src/AutoCraft/main.cpp` is the Windows DLL entry point. It initializes
MinHook and registers `ScriptMain` with ScriptHook.

`src/AutoCraft/script.cpp` contains the mod behavior. `ScriptMain` scans
the running game for two addresses:

1. the currently executing RDR2 script thread;
2. RDR2's native-function dispatcher.

If either address cannot be found, the mod logs an error and does not install
hooks. That is safer than guessing at an address after a game update.

## Normal ammunition-batch flow

The normal build only operates while RDR2 is running either `player_camp` or
`interactive_campfire`. This avoids changing inventory behavior elsewhere in
the game.

### Starting a batch

- Selecting an ammo recipe performs its first craft normally. When RDR2
  actually adds ammo, AutoCraft starts the remaining 49 possible crafts.
- Pressing **Craft Again** arms a 50-craft batch before its next craft begins.

The successful `_ADD_AMMO_TO_PED_BY_TYPE` call is the confirmation that a craft
really happened. AutoCraft does not decrement the remaining count merely
because a button was pressed or an animation event fired.

### Skipping repeated animation waits

RDR2 normally waits for a crafting animation event before committing a craft.
AutoCraft reports that event as ready while a batch remains. RDR2 then follows
its own existing recipe, ingredient, inventory, and ammo-capacity logic.

RDR2 also reports a `safetobreakout` event after each completed animation.
AutoCraft suppresses that exit only when the immediately preceding craft added
ammo successfully. If RDR2 cannot add ammo on the next attempt, its normal
exit is preserved.

There is also a 120-frame no-progress safeguard. If a forced craft neither
adds ammo nor reports its normal exit in that window, AutoCraft stops the batch
instead of keeping it active indefinitely.

### Notifications

RDR2 still performs individual inventory updates, which would normally request
one toast notification per craft. AutoCraft lets the first toast through and
hides later toasts in the same batch. Hiding a toast does not affect inventory
or ingredient changes.

## Item-batch flow (tonics, remedies, and other menu recipes)

Non-ammunition recipes never use the animation events above. The crafting
script instead waits out the full duration of one crafting animation, commits
exactly one validated transaction, and returns to a "craft again" prompt.
AutoCraft batches these differently:

- **Skipping the wait**: the script asks RDR2 how long the crafting animation
  is and uses that as its commit timer. AutoCraft answers that question with a
  tiny duration (only for that exact crafting animation), so each craft
  commits almost immediately.
- **Craft again, automatically**: after each item craft, AutoCraft presses the
  "craft again" prompt for the player.
- **The game keeps its veto**: after every item craft, RDR2 itself re-checks
  ingredients and output capacity and enables the craft-again prompt only if
  another craft is legal. AutoCraft watches that enable/disable signal and
  only presses while the game says crafting is allowed. This matters because
  the item path removes ingredients before granting output; pressing blindly
  at capacity would consume ingredients for nothing.
- **Success signal**: a craft counts only when RDR2 actually adds the item to
  the inventory, exactly like the ammunition path counts a real ammo addition.

The batch also ends if the craft-again opportunity does not reappear shortly
after the last granted output (for example, the player backs out mid-batch),
or if a forced craft makes no progress within the fail-safe window.

## Cooking automation

Cooking meat and brewing use a separate game flow: ingredients are taken when
cooking starts, doneness is a hold-the-button meter, and the cooked item is
granted partway through the cooking animation. The menu-batch mechanism does
not apply there, so AutoCraft does two things instead:

- **Automation**: the cook, stow, and cook-again prompts become fast
  self-completing holds — the mechanism the original upstream mod shipped —
  so one manual cook continues through the whole stack hands-free.
- **Batched output per animation**: when the game grants the cooked item, it
  has already validated and paid for exactly one cook. AutoCraft repeats that
  complete exchange up to 10 times inside the same animation: for each extra
  output it first confirms the satchel slot has room and every recorded
  ingredient of the set is still available, then removes one full ingredient
  set and re-runs the game's own grant, verifying each count change. The
  batch stops at the first shortage or full slot. This is the original mod's
  "3 per animation" idea with per-item verification, so ingredients can never
  be consumed without a matching output.

The mod never forces the "meat is done" animation event: the camp script polls
it continuously, so forcing it would grant items every frame and again when
the real event fires. The quantity exchange above is the safe equivalent.

If the satchel fills up, the game hides the stow prompt and only offers to eat
the food. AutoCraft never auto-eats, so automation simply stops there and
waits for the player. Brewing is unaffected by the exchange logic because it
never grants an inventory item.

## Crafting-menu refresh

RDR2 takes an ingredient-count snapshot when a recipe menu opens. A rapid
batch can finish after that snapshot, leaving the menu temporarily showing the
first craft's deduction instead of the full batch.

After a multi-craft batch, AutoCraft sets RDR2's existing recipe-refresh flag.
The game rebuilds its own recipe and ingredient data from live inventory, so
the displayed count matches the real count without closing the UI.

The flag is a game-script global verified for **RDR2 1.0.1491.50**. A future
RDR2 update may move that global. If an update changes menu behavior, first use
the Diagnostic build to revalidate it before changing the normal build.

The rebuild can briefly reset the selected recipe or variant. That is a game
side effect of asking it to refresh the recipe list.

## Why the mod does not multiply native quantities

Earlier versions multiplied amounts passed to inventory and ammo natives. That
could add many outputs even when RDR2 had validated only one craft, or consume
more inputs than were available. The current design avoids this: every output
is created only after RDR2's own transaction succeeds.

## Source map

| File | Purpose |
| --- | --- |
| `src/AutoCraft/main.cpp` | DLL attach/detach and ScriptHook registration. |
| `src/AutoCraft/script.cpp` | Batch state, hooks, UI refresh, and Diagnostic behavior. |
| `src/AutoCraft/hookhandler.hpp` | MinHook wrapper used to intercept RDR2 natives. |
| `src/AutoCraft/scanner.*` | Signature scanner used to find version-sensitive game addresses. |
| `src/AutoCraft/diagnostic_logger.*` | Tab-separated logging used by the Diagnostic build. |
| `docs/AMMO_DIAGNOSTICS.md` | Safe procedure for a logging-only investigation. |

## Diagnostic build

The `Diagnostic|x64` configuration produces `AutoCraftDiagnostic.asi`. It does
not batch crafts, modify native arguments, or alter native return values. It
only records normal RDR2 calls to `AutoCraft-diagnostic.log` beside the ASI.

Never load `AutoCraft.asi` and `AutoCraftDiagnostic.asi` together. See
[`AMMO_DIAGNOSTICS.md`](AMMO_DIAGNOSTICS.md) before using the Diagnostic build.

## Updating RDR2 or native declarations

`inc/natives.h` contains native names and C++ signatures. Updating it can help
with names and documentation, but it does not automatically fix crafting
behavior: AutoCraft's batching issue was in the camp scripts' state flow.

Before changing a native declaration or game-script global:

1. compare only the native hashes and signatures AutoCraft actually uses;
2. build both Release and Diagnostic configurations;
3. repeat the ingredient-limited and near-capacity tests;
4. revalidate the recipe-refresh global with the Diagnostic build after a game
   update.
