# Kotoba3DS: notes for working on this repo

A Luma3DS / CTRPluginFramework plugin that reads a game's screen on the console (VOCR OCR),
looks words up in an on-SD dictionary and saves them; PC tools turn saved words into Anki
(Lapis) cards. The README describes the features and setup for users; this file is for whoever
works on the code.

## Standing rules (from the owner)

- **New work goes to the `test` branch.** Nothing new reaches `main` until the owner has
  confirmed it on real hardware and says to merge. Docs-only changes (README, this file) may go
  straight to `main`; then merge `main` into `test`.
- **Experiments the owner wants kept away from the test branch's users go to `alpha`.** No
  release for it: CI's run artifact goes straight onto the owner's SD card. It moves to `test`
  only when the owner says so.
- **When unsure about a design decision, stop and ask** before building it.
- **Anonymity:** no personal information (names, home paths, emails) in commits or files.
- **Commits** are authored `Claude <noreply@anthropic.com>` and end with the attribution lines the
  session gives. The owner's own README edits made in GitHub's web editor (e.g. 33b1e4b,
  8a36095) are theirs: never rewrite or re-author them, even if a hook flags them as unverified.
- **OCR changes are backtested** against every saved screenshot before they go to `test`
  (see below), and the result is reported: how many screens changed, better or worse.
- After a change is on `test`, put the build on the owner's SD card when it's connected.

## Branches, CI and releases

- `.github/workflows/build.yml`: jobs `plugin` (devkitPro + CTRPF), `dictionary` (builds
  `dict.bin`, ~7 min), `release` (on `main`: the **latest** release with `Kotoba3DS.3gx`,
  `Kotoba3DS-2MiB-test.3gx`, `dict.bin`, `Kotoba3DS-ocr-model.zip`), `test-release` (on `test`:
  the **test-build** prerelease with the plugin only).
- The plugin is only built by CI (no devkitPro locally). Download assets through the REST API;
  GraphQL (`gh release download`, `gh run watch` output) can be blocked:
  `gh api repos/icebreaker2718/kotoba-3ds/releases/tags/test-build` → asset id →
  `gh api -H "Accept: application/octet-stream" repos/icebreaker2718/kotoba-3ds/releases/assets/<id> > file`.
- `test-release` waits for the `dictionary` job, so a test build appears ~10 min after a push.

## The owner's SD card and laptop (when linked)

- SD card (drive `E:` on the laptop): plugin at `/luma/plugins/default.3gx`; data in
  `/3ds/kotoba3ds/` (`dict.bin`, `ocr/VOCR5_N_w8.vocr`, `mined.tsv`, `shots/`, `known.txt`,
  `marked.txt`, `settings.txt`, `ocr_log.txt`); crash dumps in `/luma/dumps/arm11/`.
- Writing to the card: stage the file under a **new, unique name** each time (reusing a staged
  path can write stale content) and check the size on the card afterwards.
- `ocr_log.txt` has a line per read (timings, boxes found, heap, text) and `boot:` lines with the
  game's title ID and what the memory fix did. Read it first when a game misbehaves.
- The laptop has a copy of `tools/` (ask the owner where); keep it in step with the repo when the
  PC tools change, and leave its `config.json` and `imported.json` alone.

## Code map

- `plugin/Sources/main.cpp`: menus, hotkeys, `ReadScreens`, saving, screenshots (QOI, written on
  their own thread), lid-close handling, known words, network to the PC, and at boot
  `GiveGameItsMemory` (raises the game's commit limit by the plugin's 5 MiB, then patches the
  game's memory system calls so the plugin's share is left out of their answers: `mem_hooks.s`
  stand-ins reached through their own trampolines (not CTRPF hooks) + `core/mem_share.cpp`,
  tested by `tests/test_memshare.cpp` and `tests/build_memhooks_arm.sh` under qemu).
- `plugin/Sources/core/` (platform independent, also built natively for tests):
  `textbox.cpp` (finds flat-colour text boxes; grows shaded ones), `local_ocr.cpp`
  (`localReadScreen`: reads each box, dedupes overlapping boxes, whole-screen fallback),
  `textlines.cpp` (lines → paragraphs, furigana and scraps dropped), `correct.cpp` (dictionary
  picks among uncertain kanji), `search.cpp`/`deinflect.cpp`/`dict.cpp` (lookup),
  `lookup_view.cpp` (the lookup screens), `known_words.cpp`, `bmp.cpp` (BMP/QOI).
- `plugin/Sources/vocr/`: the VOCR runtime from bee-san/vita-vn-ocr (GPL; model weights
  non-commercial).
- `tools/`: `ocr_server.py` (the 3DS sends saved words; makes cards, serves known words),
  `mine.py` (same over ftpd/SD; `setup`, `known`), `enrich.py`, `yomitan.py`, `build_dict.py`.

## Testing

- Native tests: `make -C tests test_core test_ocr_core test_ui test_wrap`, then run each;
  `test_core` and `test_ui` take the path of a real `dict.bin` (download it from the latest
  release). Python: `tests/test_mine.py`, `tests/test_ocr.py`, `tests/test_builder.py`, ...
- `tests/ocr_cli.cpp`: runs the whole screen reader on a BMP:
  `ocr_cli <model.vocr> <screenshot.bmp> <dict.bin>`; prints `PARAGRAPH:` and `LINES:` lines.
  Build it natively for backtests (-O2, no sanitizers, all of `plugin/Sources/core/*.cpp` plus the
  `vocr/*.c` files); printing the boxes from `findTextBoxes` helps.
- `tests/build_arm_cli.sh`: the same CLI for the 3DS's CPU, run with `qemu-arm`. Check OCR changes
  give the same result there.
- **Backtest:** stage every screenshot from the SD card's `/3ds/kotoba3ds/shots/` (BMPs, and QOI
  files decoded with `tools/mine.py`'s `open_screenshot`), run the old and new CLI over all of
  them, and diff the `PARAGRAPH`/`FAILED` lines. About 320 screens so far, mostly Mario & Luigi:
  Dream Team, plus Pokémon Sun/Moon and Ace Attorney.

## Status (2026-10-28)

- `main` = the confirmed release (lid-close fix, no OCR needed on the PC, flag retry,
  overlapping-box OCR fixes, saved-word spelling option, README feature list).
- On `test`, waiting for the owner's hardware check (the SD card has this build):
  1. Shaded text boxes (Pokémon Sun/Moon bubbles fade top to bottom; only the first row was
     read). Backtest: 4 other screens changed, all better.
  2. The general memory fix (confirmed by the owner on `alpha`, 2026-10-26): the commit limit is
     raised by the plugin's share, and every GetSystemInfo / GetProcessInfo / resource-limit call
     in the game's code is patched to leave the share out of the answer (only answers measured
     at boot to include it; all calls or none). Replaces the old SDK-formula patch. 大逆転裁判2
     (00040000001AE200), Corpse Party BR (000400000015D500) and Mario & Luigi (0004000000060600)
     each asked "in use" (resource-limit current value) once at start; 逆転裁判123
     (0004000000108800) asks nothing and is covered by the raised limit alone. All four start.
  3. Start on the lookup screen keeps the screenshot without saving a word (Start again takes it
     back; undoing a word save keeps it), for collecting screens where reading goes wrong.
  4. A speed pass over what changed since the last one (04d3b1e), tried on the console
     (2026-10-27). The shaded-box growth (aeb83e1) had made the box search much slower; it now
     grows as a flood fill from the box's edge (same boxes on all 661 saved screens: the plugin's
     shots/ and luma/screenshots/). Console, median box search: Mii Plaza top 313 -> 116 ms,
     Corpse Party 80 -> 42 ms, RE Revelations (0004000000053B00) 148 -> 158 ms, unchanged: its
     screens mostly find no box, so the cost is elsewhere in findTextBoxes (next to look at; needs
     its screenshots). Mario & Luigi not measured yet. Pictures painted/taken and glyphs on 16-bit
     screens with the format chosen once per column (byte-identical, test_ocr_core): redraws
     26 -> 22 ms (RGB565 screens). The read log line splits "box" into memory check / boxes /
     pictures (the box search is nearly all of it) and keeps its end; the view line names the
     screens' formats. The memory fix scans the game's code once.
- On `alpha` (2026-10-28, 2b3a59b), benchmark build, not yet tried on a console: **VOCR6 and the text spotter**
  (vita-vn-ocr v0.5.0, runtime at 205f47e). `VOCR6.2_N_w8g2.vocr` (preferred over VOCR5 when on the SD card; run
  with its head gate and two-level head) and `textspot8_t64n8c16_w8.vtsp` (core/text_spot.cpp) are in the SD card's
  `ocr/` folder. `readScreenSpot` (core/local_ocr.cpp): the spotter finds every line on the screen with its kind
  (dialogue, name, choice, UI, scene); no text-box search, line finder or bands. Furigana lines dropped (kana-only,
  pressed above a line with kanji), boxes widened by 35% of their height (the spotter cuts faint last glyphs: ー,
  …), lines kept by option 1's scene rule (`keepLine`, not lenient). Without the .vtsp the old reader runs.
  Our ARM11 kernels were carried into the new vocr.c (same text as upstream); the spotter's 1x1 layers go through
  `vocr__k2x8` (`vocr_textspot6_pack_simd32`): 242 -> 151 M ARM instructions per top screen, boxes bit-identical.
  Backtest (labelled 171, labels 92/145 corrected for VOCR5's own misreads): test build P 0.991 R 0.985, 154
  fully right; option 1 0.996 / 0.992 / 158; spotter + VOCR6 0.987 / 0.995 / 156, 0 missed, 0 junk; VOCR6 with
  the old finder 0.981 / 0.984 / 151. Held-out 537: 328 identical, 82 layout only; the rest mostly menus, item
  lists and name tags it now reads, and better kanji (叶える, ちょっと), some losses (遠心カ, 確楽). ARM instructions
  over 8 screens (qemu): test build 3.2 G, VOCR6 + old finder 2.5 G (-21%), spotter + VOCR6 3.3 G (+3%; the
  spotter is 115-151 M a screen, single-threaded). Option 1 (2bc79be, `readScreenScored`) is still in core/ but
  no longer called. The bench build also runs the current reader each read ("bench:" lines) and keeps the
  screenshot when they disagree: turn KT_READER_BENCH off before `test`; the old finder code goes once the
  spotter is confirmed. Next speed step if needed: the spotter's 3x3 layers (`feed`, 45 M), splitting it over cores.
  Then (0bf2357) two settings: "Fast mode" (`fast=`: `findTextBoxes(..., shaded=false)`, no growing into a
  gradient, + the box reader / whole-screen fallback, no spotter; P 0.991 R 0.959 with VOCR5, 0.989 / 0.958 with
  VOCR6: loses Pokemon bubbles' second lines and scene text; ARM 2.8 G / 2.2 G over the 8 screens, box search
  -45%) and "OCR model" (`ocr_model=`: VOCR6 or VOCR5). In fast mode the bench compares with the spotter.
- `release-1.1.0` (on GitHub, not merged): version 1.1.0, CHANGELOG.md, README additions, release
  notes with the version; drafted on items 1-2 above. Rebase it onto `test` and add 3-4 to the
  changelog before the owner reviews it again.
- Ideas the owner was offered but hasn't decided on: an easier dictionary builder or prebuilt
  dictionaries in releases; switching dictionaries on the console; a workflow diagram in the
  README.
