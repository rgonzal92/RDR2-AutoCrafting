# Ammo crafting diagnostic

`AutoCraftDiagnostic.asi` is a temporary logging build used to study one normal
RDR2 ammunition craft. It does not multiply quantities and does not automate
crafting prompts.

The build writes `AutoCraft-diagnostic.log` beside the ASI. Each tab-separated
record includes the call sequence, frame, script and thread, native arguments,
before/after counts, and the original result when available. The file is
truncated each time the game starts and flushed after every record.

## Safety

- Keep only `AutoCraftDiagnostic.asi` active while collecting traces.
- Do not install `AutoCraft.asi` beside it.
- Back up the active ASI and `SRDR*` save files before testing.
- Trigger exactly one manual craft per scenario.
- Reload the backed-up save when a scenario would affect later inventory state.

## Scenarios

Use one ammunition recipe for all scenarios and retain a separate copy of the
log after each run:

1. No ingredients available.
2. Ingredients for exactly one craft.
3. Ingredients for exactly two crafts; trigger only one craft.
4. Ingredients for at least ten crafts; trigger only one craft.
5. Output ammo nearly at capacity.
6. Output ammo at capacity.

Record the visible input and output counts before and after each attempt. A
transaction-level batching prototype is permitted only if the traces show a
stable one-craft boundary, validation before mutation, a definitive result,
consistent capacity handling, and no unrelated operations inside the boundary.
