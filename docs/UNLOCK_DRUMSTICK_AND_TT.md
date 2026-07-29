# Unlocking Drumstick and T.T. via the save file

The decomp's `init_title_screen_variables` reads the SaveConfig bits for
Drumstick (bit 1) and T.T. (bits 4-23, all-TT-courses mask) and calls
`set_magic_code_flags(CHEAT_CONTROL_DRUMSTICK)` / `CHEAT_CONTROL_TT` on
boot (`Diddy-Kong-Racing/src/menu.c:3194-3200`). In the recomp build this
read does not propagate reliably into the magic-code path, so even a
correctly-written save leaves both characters locked. The
`recomp_api/cheats.hpp` placeholder documents that the unlock-progression
hooks were removed because they caused heap corruption and unreliability.

This guide describes a minimal local workaround: force
`set_magic_code_flags(CHEAT_CONTROL_DRUMSTICK | CHEAT_CONTROL_TT)` at the
top of the recompiled `init_title_screen_variables`. The patch lives in
the gitignored generated file `RecompiledFuncs/funcs_23.c`, so nothing
leaves the machine and no `dkr.us.v80.toml` change is required.

## 1. Write the unlocked save

```bash
./scripts/unlock_save.py
```

This writes the fully-unlocked `dkr-us-v80.bin` (all races cleared,
all trophies, 39+ balloons, Adventure 2, Drumstick + all 20 TT courses
+ subtitles in the SaveConfig) to
`~/.config/DiddyKongRacingRecompiled/saves/dkr-us-v80.bin`. Without the
runtime patch below, the SaveConfig bit alone is not enough.

## 2. Patch the recompiled function

`RecompiledFuncs/` is gitignored (it is regenerated from
`baserom.us.v80.z64` by `N64Recomp`), so this edit is local and never
committed.

Open `RecompiledFuncs/funcs_23.c` and search for `init_title_screen_variables`
(it is the only function with that name; near line 17948 after a clean
regeneration). Insert the force-unlock call right after
`recomp_enter_function`:

```c
RECOMP_FUNC void init_title_screen_variables(uint8_t* rdram, recomp_context* ctx) {
    uint64_t hi = 0, lo = 0, result = 0;
    int c1cs = 0;
    recomp_enter_function("init_title_screen_variables", 0x800833FC);
    { gpr _unlock = 3; ctx->r4 = _unlock; set_magic_code_flags(rdram, ctx); } //You Just Need ADD THIS LINE TO UNLOCK DRUMSTICK AND TT
    // 0x800833FC: lui         $t7, 0x8012
    ...
```

`CHEAT_CONTROL_TT = 1` and `CHEAT_CONTROL_DRUMSTICK = 2`
(`Diddy-Kong-Racing/src/menu.h:194-195`), so `flags = 3` unlocks both.
The block uses the same `ctx->r4` / recompiled-call convention the
generated code already uses throughout `funcs_*.c` to invoke
`set_magic_code_flags`; this is exactly how the recompiler itself
emits those calls, so the runtime dispatches normally.

## 3. Rebuild and run

```bash
cmake --build build-linux --parallel
./build-linux/DiddyKongRacingRecompiled --skip-launcher
```

Drumstick and T.T. are now available from the first boot. The patched
binary reads the same save directory, so step 1 must be done at least
once before the first run.

## Caveats

- **Re-running `scripts/generate-recomp.sh` wipes the patch.** The
  generated file is regenerated from the rom on every run. Re-apply the
  one-line edit after each regeneration, or skip regeneration entirely
  if you have a working build.
- **Rerun `unlock_save.py` whenever the game overwrites the save.** The
  game writes the eeprom back on every save, including the
  SaveConfig block; the script restores the unlocked bits.
- **This is a local-only modification.** `RecompiledFuncs/` is in
  `.gitignore`, so the patch cannot be pushed. Anyone wanting the same
  behaviour must re-apply the one-line edit and rebuild.
- **Risk.** The call runs at the very top of
  `init_title_screen_variables` before the eeprom read. This mirrors
  what `set_magic_code_flags` does in the decomp and follows the same
  recompiled-call convention, so it should be safe; the previous
  attempt (documented in `recomp_api/cheats.hpp`) was abandoned because
  of heap corruption from a different hooking strategy, not from the
  force-set itself.
