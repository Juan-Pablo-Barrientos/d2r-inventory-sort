# Inventory Sort

A [D2RLoader](https://d2rloader.net) plugin for Diablo II: Resurrected. It adds
two buttons to the inventory, in the empty strip right of the gold display.

**Sort** arranges items by kind:

- Gear goes on the left: weapons first, then armor, bigger items first.
- Single-cell items fill in from the right: charms, rings and amulets, gems
  and runes, potions, scrolls, keys. Identical items sit side by side.
- When that does not fit, it packs the inventory more tightly instead.

**Compact** keeps the free space together so you can pick up big items:

- The free cells become a block the size of an item: with 6 free cells you get
  a 2 x 3 space, enough for body armor. With 8, a 2 x 4 space for a bow.
- With more free space it keeps the biggest block first, then the next.
- Item order doesn't matter in this mode.

Both work in single player and when you join a TCP/IP game.

The plugin moves items the same way you would by hand: pick up, put down, or
drop onto another item to swap it. The game checks each move, so the plugin
can't create or duplicate items. It works out every move before it starts, so
when the inventory can't be sorted, nothing moves.

## Install

1. Install [D2RLoader](https://d2rloader.net). Tested with 1.3.1-beta.
2. Download `d2rl-inventory-sort.dll` from the
   [latest release](../../releases/latest).
3. Copy it into `<game folder>\d2rloader\plugins\`.
4. Start the game with D2RLoader.

To remove it, delete the DLL. The console commands `inventory-sort` and
`inventory-compact` do the same as the buttons.

## Settings

The first launch creates `<game folder>\d2rloader\config\inventory-sort.toml`:

```toml
[inventory-sort]
# Show the Sort and Compact buttons in the inventory.
show_button = true

# Inventory grid size. 0 reads it from the game data (10 x 4 in the base game).
columns = 0
rows = 0

# Write every move to d2rloader\logs\inventory-sort.log.
log_moves = false
```

## When it does nothing

Messages appear in the D2RLoader console and in `inventory-sort.log`.

- **"Put down the item on the cursor before sorting."**
- **"There is not enough free space to sort this inventory."** With one free
  hand, a nearly full inventory often can't be rearranged. Free a few cells.
- **"The inventory changed while sorting."** Something moved an item during the
  sort, such as picking up gold or drinking a potion. Press Sort again.

## Building

Run `build.ps1` (Visual Studio 2022 C++ tools). `-Install` copies the DLL into
the game. `-PlannerTest` builds and runs `tools/planner_test.cpp`, which plans
thousands of random inventories and replays every move against the server's
rules (`-PlannerArgs "compact"` for the Compact button, `"show"` for example
layouts). `tools/make_sprites.py` draws the buttons.
