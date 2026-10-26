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
- **Releases (there are real users now):** merging to `main` publishes the release. Before it,
  bump the version in `plugin/Kotoba3DS.plgInfo` (and the `PluginMenu` numbers in `main.cpp`),
  add the user-facing changes to `CHANGELOG.md` (and README if needed), and have the owner read
  them before pushing.
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

## Status (2026-10-26)

- `main` = release 1.1.0: the general memory fix (commit limit raised by the plugin's share,
  and every GetSystemInfo / GetProcessInfo / resource-limit call in the game's code patched to
  leave the share out of the answer; only answers measured at boot to include it; all calls or
  none) and shaded text boxes (Pokémon Sun/Moon). Confirmed on the owner's console with
  大逆転裁判2 (00040000001AE200), Corpse Party BR (000400000015D500), 逆転裁判123
  (0004000000108800) and Mario & Luigi (0004000000060600). Before it, 1.0.0 (unversioned
  releases until then).
- On `alpha`, not yet tried on a console: Start on the lookup screen keeps the screenshot
  without saving a word (Start again takes it back; undoing a word save keeps it). For
  collecting screens where reading goes wrong, ahead of a reader that doesn't need text boxes.
- Ideas the owner was offered but hasn't decided on: an easier dictionary builder or prebuilt
  dictionaries in releases; switching dictionaries on the console; a workflow diagram in the
  README.
