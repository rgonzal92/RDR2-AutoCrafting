# RDR2 AutoCrafting Mod

One craft action can run up to 50 of RDR2's normal crafting transactions
without waiting for each repeated animation. This covers ammunition and every
other recipe selected from the crafting menu (tonics, remedies, and so on).
Every transaction is validated by the game, consumes its required ingredients,
and stops when ingredients run out or the output reaches capacity.

Cooking at a campfire is automated and batched: after one manual cook, the
cook, stow, and cook-again prompts complete on their own, and each cooking
animation produces up to 50 cooked items — every extra one paid for with a
verified full ingredient set and stopped by satchel capacity.

AutoCraft is always enabled and has no custom menu or keyboard shortcut. One
inventory notification is shown for each batch.

## Documentation

- [How AutoCraft works](docs/ARCHITECTURE.md) explains batching, safety limits,
  menu refreshes, and the active source files in plain English.
- [Ammo crafting diagnostic](docs/AMMO_DIAGNOSTICS.md) describes the separate
  logging-only build used when a game update needs investigation.

## Build

Run the **Build AutoCraft** workflow from the repository's Actions page. The
Windows Release x64 build is uploaded as the `AutoCraft-ammo-50` artifact and
contains `AutoCraft.asi`.

## Original mod

https://www.nexusmods.com/reddeadredemption2/mods/3302

## Credits

- Alexander Blade for the Scripthook SDK for RDR2
