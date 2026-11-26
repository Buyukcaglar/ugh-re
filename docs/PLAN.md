# UGH! 64bit: how it works and what is left

## How it works

- **The program.** `UGH.EXE` is PKLITE-compressed. Its own decompressor runs in the emulated
  machine. The unpacked program (Borland C++ 1991, far code model) has five code segments; at run
  time (load segment 0110h) these are 0110 (C runtime), 024B (the game), 0774 (AdLib driver),
  0988 (sound wrapper) and 0B42 (timer). DGROUP is 5D19h.
- **Recompiled code.** `tools/ughrec.py` turns every basic block it finds (recursive descent plus
  the addresses executed in scripted sessions, `tools/coverage.ps1`) into a C function over the
  shared CPU state (`src/gen/gen_all.c`, private: made from the player's own copy). `gen_rt.c`
  checks a hash of each code segment at the program's entry point before it uses the blocks.
  Anything else runs in the 286 interpreter (`src/core/cpu.c`).
- **The machine.** PIC, PIT, keyboard controller (and the BIOS key buffer), VGA (register level),
  YM3812 (AdLib), Sound Blaster/DMA, PC speaker, joystick port, and the DOS and BIOS services the
  game uses (`src/hal`). Virtual time runs at 24 million instructions a second and 70.086 frames a
  second.
- **Hooks.** `gen_watch(cs, ip, fn)` runs a callback whenever execution reaches an address, in
  recompiled and interpreted code alike. `src/hal/game.c` uses this on the DOS sound calls.
- **Amiga sound.** `src/audio/amiga_data.c` reads the TFMX module from the Amiga disk (OFS/FFS).
  `src/audio/tfmx.c` is the Amiga game's own TFMX player (`TFMX.obj`), translated routine by
  routine from 68000 code to C, driving four emulated Paula channels at the PAL clock with the
  A500 low-pass. Both sound outputs always run, so Ctrl+Shift+A switches at once.
- **Music mapping.** DOS tune → Amiga subsong: title 0, level start 3, in-game the level's own
  subsong (4, 5 or 6, from the Amiga level table; DOS level index at 5D19:261C) at half volume as
  on the Amiga, game over 2, high score 1; DOS "stop" fades out.
- **Sound effects.** The DOS game has a few AdLib effects at most. The Amiga version's 15 effects
  (TFMX macros 0–14, played as notes 14MM3300 on channel 3) start from hooks on the DOS game's
  matching events (`src/hal/game.c`). The DOS object descriptors (DS:7630..7926) are the Amiga
  ones with longs turned into words, so every Amiga state handler that plays an effect has one
  DOS handler for the same state (segment 024B, BX = object slot): stone hits, pterodactyl cry
  and wing beats, stone landing, dinosaur skid, steps and roar, death, passenger complaints and
  hails (pitch by passenger kind), stone pick-up and drop, blower in/out, finished passenger
  stop, typing clicks, and a low-energy beep below a third of the energy (the DOS game shows no
  warning; this one is synthesised as on the Amiga, every 8 frames).
- **Cutscene effects.** The Amiga cutscene scripts are commands (walk, jump, wait, play effect);
  the DOS ones (engine at 024B:09D7) are per-actor lists of "sprite, dx, dy" entries, and SI
  identifies the entry. Title (DS:24DE) and ending (DS:1020): the jumping actor lands, effect 02
  at the end of its list. Intro (DS:1792 = Amiga list 6B44): the fruit hits the caveman, effect
  00 at its first bounce entry; he raises his fist, effect 08 when that loop opens. The other
  intro (DS:1E40 = Amiga 6624) is silent on the Amiga too. `work/cut_dec.py` decodes the lists.
- **Watched addresses and the recompiler.** `tools/ughrec.py` reads the `gen_watch` calls in
  `src/hal/game.c` and starts a block at each watched address, so no recompiled block runs past a
  hook. Rerun it after adding a hook.

## Status

1. Reference files in `_references/dos` (and `_references/amiga`): never committed. Done.
2. Windows 64-bit build: done (`build.cmd`, `package.cmd`).
3. macOS port: later, on a Mac (the core is portable C; a Cocoa/Metal host is needed).
4. Version 1.0: done (resources, About box).
5. Theme-following window and About box (from Civ64): done.
6. CRT scanlines (from Civ64): done.
7. Repositories: private `ugh` (everything except game files), public `ugh-re` (no `gen_all.c`).
8. Amiga music and effects, switchable with Ctrl+Shift+A: done. All 15 effects are mapped to
   DOS events, the cutscene effects included.
9. Quality-of-life updates (widescreen and more): after 1–8.
10. Launcher support (RecompLauncher and sp00nznet's recomp-launcher): done on 26 November 2026,
    without a change to the program. See "Launcher support and releases" below.

## Launcher support and releases

- **RecompLauncher** (recomplauncher.com) installs the program from the GitHub release of `ugh-re`
  and reads `recomplauncher.json` from the release zip, next to `UGH!64.exe` inside the zip's one
  folder. The file names the game folder the player picks (proof: `UGH.EXE`), the start command
  (`UGH!64.exe "<folder>"`, the folder is the only argument), the settings it shows (`Scale`,
  `FullScreen`, `Crt` in section `UGH64` of `%APPDATA%\UGH64\UGH64.ini`), the saves
  (`%APPDATA%\UGH64\UGH!.HI`) and the meaning of exit code 1.
- **The rule.** Every release zip carries `recomplauncher.json`: the copy in the repository alone
  does nothing for players. `package.cmd` copies it into the zip's folder. The zip's name keeps
  the word `win64`.
- **Keep in step.** If the command line, a settings key or value, the program's file name, the
  ini's name or the saves folder changes, `recomplauncher.json` changes in the same release. A
  renamed program or release file also means editing `$Program` or `$Asset` in
  `launcher/setup.ps1`.
- **sp00nznet's recomp-launcher** builds one installer per game from `launcher/launcher.json`. The
  installer finds or asks for the game folder, downloads the repository and runs
  `launcher/setup.ps1` (Windows only). The setup compiles nothing: it checks the game folder
  (`UGH.EXE` or `UGH1.EXE`), downloads the newest release zip of `ugh-re` (checked against the
  SHA-256 GitHub lists), unpacks it into `play\` and writes `UGH (recomp).cmd` in the repository's
  root, which starts `play\UGH!64.exe` with the game folder. The project is not in sp00nznet's own
  `catalog.json`.
- **The `!` in the release file's name.** GitHub turns the `!` of an uploaded file name into `.`:
  `UGH!64-1.0-win64.zip` is shown and downloaded as `UGH.64-1.0-win64.zip`. This is why
  `launcher/setup.ps1` looks for `UGH*64-*-win64.zip`.
- **The installer workflow.** `.github/workflows/installer.yml` calls sp00nznet's reusable
  workflow, pinned to commit `005639c34bc2fc3096219fce2bd0a5ea13f8ac5b` (also as `launcher-ref`).
  Its job has `if: github.repository == 'Buyukcaglar/ugh-re'`, so it runs in the public
  repository only. There, a push to `main` that changes `launcher/**` or the workflow builds the
  installer as a workflow artifact. A pushed `v*` tag also attaches
  `UGH64-Installer-windows-x64.exe` and `SHA256SUMS.txt` to that tag's release, and creates the
  release with generated notes if it does not exist yet: create the GitHub release first, or edit
  the notes afterwards.
- **The 1.0 release.** On 26 November 2026 the v1.0 zip on `ugh-re` was replaced in place: the
  same tag, the same three files byte for byte (`UGH!64.exe`, `README.md`, `LICENSE.txt`) plus
  `UGH!64-1.0-win64/recomplauncher.json`. No new version and no rebuild. The asset GitHub shows as
  `UGH.64-1.0-win64.zip` is now 519,473 bytes, SHA-256
  `337d665357d6664a29757764e7452801c8fad1fa5b540d59351cb986570ef95f`. That release has no
  installer yet: it comes with the next tagged version.
- **Making a release**, as far as it is known: commit in the private repository; run
  `tools\publish.ps1 -Message "..."` (copies HEAD without `src/gen/gen_all.c` to `ugh-re` and
  pushes); run `package.cmd`; upload `dist\UGH!64-<version>-win64.zip` to the release on `ugh-re`.
  The version (`1.0`) is written out in `package.cmd`.

## Notes for later

- The DOS key bindings (table at DS:281C): player 1 = grey arrows + right Ctrl (or keypad 0);
  player 2 = W/Z/A/S or keypad 8/2/4/6 + left Ctrl (or keypad Enter). P pauses.
- The game's own int 9 handler does not chain to the BIOS; text input (password, high-score
  name) runs with the BIOS handler restored and reads int 16h.
- The BIOS key buffer pointers (0040:001A/1C, 80/82) must start at 1Eh: left at 0 the keys
  overwrote the BIOS data area after 12 characters (long passwords failed).
