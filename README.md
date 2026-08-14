# RDR2 AutoCrafting Mod

One ammunition craft can run up to 50 of RDR2's normal crafting transactions
without waiting for each repeated animation. Every transaction is validated by
the game, consumes its required ingredients, and stops when ingredients run out
or ammunition reaches capacity. Non-ammunition recipes still craft once.

AutoCraft is always enabled and has no custom menu or keyboard shortcut. One
inventory notification is shown for each ammunition batch.

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
