# Kotoba3DS: notes for working on this repo

A Luma3DS / CTRPluginFramework plugin that reads a game's screen on the console (VOCR OCR),
looks words up in an on-SD dictionary and saves them; PC tools turn saved words into Anki
(Lapis) cards. The README describes the features and setup for users; this file is for whoever
works on the code.

## Standing rules (from the owner)

- **All new work goes to the `alpha` branch** (owner, 2026-11-03), unless the owner explicitly says
  otherwise for a change. **Never push to `test` or `main` without the owner's permission**: that
  includes docs-only changes. Work moves from `alpha` to `test` only when the owner says so, and
  from `test` to `main` only after the owner has confirmed it on real hardware and says to merge.
- **Every change to `alpha` goes on the owner's SD card** when it's connected (the `alpha-dev`
  build, below).
- `alpha` and `test` have separate histories: to bring `alpha` up to `test`, commit `test`'s tree
  on `alpha` (`git rm -r . && git checkout origin/test -- .`), no force push. CI publishes each alpha build as the `alpha-dev`
  pre-release, titled "DEVELOPMENT ONLY - do not use" (the owner's OK, 2026-10-28): run artifacts
  can't be downloaded from this environment (their storage host is blocked), release assets can.
  Fetch it through the REST API (as below, tag `alpha-dev`).
- **When unsure about a design decision, stop and ask** before building it.
- **Anonymity:** no personal information (names, home paths, emails) in commits or files.
- **Commits** are authored `Claude <noreply@anthropic.com>` and end with the attribution lines the
  session gives. The owner's own README edits made in GitHub's web editor (e.g. 33b1e4b,
  8a36095) are theirs: never rewrite or re-author them, even if a hook flags them as unverified.
- **OCR changes are backtested** against every saved screenshot before they go to `test`
  (see below), and the result is reported: how many screens changed, better or worse.
- After a change is on `alpha` (or, with permission, `test`), put that build on the owner's SD card
  when it's connected.

## Branches, CI and releases

- `.github/workflows/build.yml`: jobs `plugin` (devkitPro + CTRPF), `dictionary` (builds
  `dict.bin`, ~7 min; also zips the OCR models: VOCR6 Normal + textspot8 from vita-vn-ocr v0.5.0, SHA-256
  checked), `release` (on `main`: the **latest** release, titled with the version from
  `plugin/Kotoba3DS.plgInfo`, with `Kotoba3DS.3gx`, `dict.bin`, `Kotoba3DS-ocr-model.zip`, `Kotoba3DS-SD.zip`; its
  notes link CHANGELOG.md and UPDATING.md), `test-release` (on `test`: the **test-build** prerelease with the
  plugin, `Kotoba3DS-ocr-model.zip` and `Kotoba3DS-SD.zip`; its notes are CHANGELOG.md's newest entry, links made
  absolute), `alpha-release` (on `alpha`: `alpha-dev`).
- `Kotoba3DS-SD.zip` (`.github/workflows/sd-zip.sh`): plugin as `luma/plugins/default.3gx`, `3ds/kotoba3ds/dict.bin`
  (the same run's build) and the OCR zip's `3ds/kotoba3ds/ocr/`, for installing over Wi-Fi with 3dsDL (owner's
  choice, 2026-10-30): Files → the zip → Extract → Here at `sdmc:/`. 3dsDL (checked in its source at 04f3c98): a zip
  with several top-level folders offers Here / New Folder, makes missing folders, reads deflate, asks Overwrite /
  Skip Them (Skip preselected) for existing files; its "Ask to Extract" after a download is off by default. The
  QR codes in `docs/` (README, release notes) are fixed URLs: `releases/latest/download/…` and
  `releases/download/test-build/…`; regenerate them only if the asset name or tags change.
- The plugin is only built by CI (no devkitPro locally). Download assets through the REST API;
  GraphQL (`gh release download`, `gh run watch` output) can be blocked:
  `gh api repos/icebreaker2718/kotoba-3ds/releases/tags/test-build` → asset id →
  `gh api -H "Accept: application/octet-stream" repos/icebreaker2718/kotoba-3ds/releases/assets/<id> > file`.
- `test-release` waits for the `dictionary` job, so a test build appears ~10 min after a push.

## The owner's SD card and laptop (when linked)

- SD card (drive `E:` on the laptop): plugin at `/luma/plugins/default.3gx`; data in
  `/3ds/kotoba3ds/` (`dict.bin`, `ocr/` (`VOCR6.2_N_w8g2.vocr`, `textspot8_t64n8c16_w8.vtsp`, `VOCR5_N_w8.vocr`), `areas.txt`, `mined.tsv`, `shots/`, `known.txt`,
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

## Status (2026-10-29)

- `main` = **1.1.0**, released 2026-10-29 (merge of `test`, confirmed by the owner on hardware). `test` is
  the same; new work starts there again. CHANGELOG.md / UPDATING.md are the user-facing record; what went
  into 1.1.0, for whoever works on the code:
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
  5. From `alpha` (merged 2026-10-28, owner's request): **VOCR6 and the text spotter**, **fast mode**, **reading
     areas**; the benchmark reads (`KT_READER_BENCH`, `BenchAllReaders`) and the "OCR model" setting removed (VOCR6
     when on the SD card, else VOCR5; an old `ocr_model=` line is ignored); the 2 MiB test copy no longer built.
     - VOCR6 (vita-vn-ocr v0.5.0, runtime at 205f47e): `VOCR6.2_N_w8g2.vocr`, head gate + two-level head. The
       spotter `textspot8_t64n8c16_w8.vtsp` (core/text_spot.cpp, `readScreenSpot` in core/local_ocr.cpp) finds
       every line with its kind; furigana lines dropped, boxes widened by 35% of their height, lines kept by
       `keepLine`. Without the .vtsp: shaded box search + box reader, as before. Our ARM11 kernels carried into
       vocr.c; the spotter's 1x1 layers through `vocr__k2x8` (242 -> 151 M ARM instructions per top screen).
     - Fast mode (`fast=`): `findTextBoxes(..., shaded=false)` + box reader / whole-screen fallback, no spotter.
     - Reading area: per game and screen in `/3ds/kotoba3ds/areas.txt`; editor on the paused game (D-Pad move,
       L + D-Pad size, R bigger steps, stylus, X other screen, A save, Y clear). Read with the line finder (fast mode
       or not), no box search. It was the spotter with fast mode off until 2026-10-28: on 38 saved screens of the
       three games with areas, the same text on 35 (5 of them layout or ？/? only), 1 character worse (… -> べ), 1
       better, 1 tie; 25% less PC time, ~70 ms a read less on the console (158 vs 228 ms median).
     - Furigana rule in `textParagraphs` (textlines.cpp): a kana line over a line with kanji is dropped as its
       reading only if it has no katakana (「クリスとジェシカが」 over RE's next subtitle row was dropped). Box
       reader backtest, 405 saved screens: 4 changed, all better (RE x2, Mii Plaza ゴーストが, a ゴースト tag).
     - PC backtest (171 hand-labelled screens; labels partly seeded from VOCR5): test build P 0.991 R 0.985, 154
       fully right; spotter + VOCR6 0.987 / 0.995 / 156, 0 missed; VOCR6 + old finder 0.981 / 0.984 / 151; fast
       mode 0.991 / 0.959 (VOCR5), 0.989 / 0.958 (VOCR6). ARM instructions over 8 screens: test build 3.2 G,
       spotter + VOCR6 3.3 G, fast mode 2.8 G (VOCR5) / 2.2 G (VOCR6).
     - Console benchmark (alpha bb3e00a, 2026-10-28; 38 reads over 大逆転裁判2, Corpse Party, Mario & Luigi,
       Mii Plaza, RE Revelations; graded against the screenshots): VOCR6 ~21% faster than VOCR5 in recognition
       (~45 ms a read, faster on every box/area read), accuracy about equal (slightly better on hard text).
       Exact dialogue, VOCR6: area + line finder 20/22 (158 ms median), area + spotter 20/22 (228 ms), spotter
       29/36 (434 ms, but extra UI text on half the reads), box readers 28/36 (fast 260 ms / shaded 317 ms; same
       boxes and text either way on these games; weak on RE subtitles). Model swaps in place held up; heap before
       a read settles ~1,000 KB. Open question (owner's call): the default for games with no area.
     - Option 1 (`readScreenScored`) is still in core/, unused.
  6. Pictures kept in one block for the session (`g_picPool`, 337 KB, `ReservePictures` after the model at the first
     read): the game's picture behind the lookup / for X / for the screenshot, and first each read's colour copy for
     the box search. Before, ~280 KB was malloc'd per read; after a long session that malloc failed every time (owner,
     Mario & Luigi 2026-10-28, also 2 reads in the bench session: `pictures 0` ms in the read line, ~900 KB free),
     so no game behind the lookup, X did nothing, words saved without a screenshot. Now: a failed picture logs
     "picture: ... not kept" (falls back to malloc, 16-bit if 24 doesn't fit), X says why it can't hide, a save says
     "no screenshot: out of memory". Peak per read (fast mode, all 405 screens, beyond the model): 672 -> 728 KB.
- On `test` after 1.1.0 (not yet on hardware): `Kotoba3DS-SD.zip` (above); and the networking memory fix. A user
  got "not enough memory for networking" after a long session: `netStart` (core/net.cpp) memaligned one 512 KB
  page-aligned block for socInit from the plugin heap, which after a long session has ~1 MB free but in pieces (as
  with the pictures, item 6 below). Now tries 512 / 256 / 128 KB; main.cpp's `NetStart` logs the size used and the
  heap's free and largest block (`net:` lines in ocr_log.txt); Make Anki cards tells a memory failure apart from
  "couldn't reach your PC". Leak check, 2026-11-01: the reader (box and spotter + dict paths) under ASan/LSan on 8
  real screens: no reports, so fragmentation, not a leak. Unverified: that socInit and a card upload work with 128
  KB (check on the console); if not, the fallback sizes need raising.
- On `test` since 2026-11-01 (moved from `alpha` at the owner's request; not yet on hardware as a test build). The
  speed pass (owner's request: "optimize as much as possible", nothing may read differently): a speed
  pass, every change byte-identical on 94 saved screens x 4 modes (fast, shaded, spotter, whole screen) natively,
  with VOCR_SIMD32_EMULATE on x86, and the ARM build under qemu-arm (3 workers and 1). Tooling: `tests/bench_cli.cpp`
  (all modes, many screens, one process; KT_WORKERS, KT_READS); ARM instruction profiles from qemu-arm's
  `-d in_asm,exec,nochain` log (no TCG plugins in Ubuntu's qemu; building qemu was refused) counted per address by
  a small C program and mapped with nm/addr2line. Kept:
  1. Box reader: all boxes' lines recognized in one parallel batch (`LocalOcr::readRegions`; `readBoxLines` decides
     boxes in rounds: a box overlapping an earlier one waits for it). Before, one box after another: 43% of the
     owner's fast-mode reads had 2+ boxes.
  2. Box search: a filled pixel loses its colour bit (the start scan and the fill's row scans skip 4 at a time):
     -23% instructions in findTextBoxes.
  3. Table CRC-32 (`vocr__crc32_ieee`, shared with the spotter): the first read's model check 63 M -> 10 M.
  4. Spotter: ARM routine `vocr__k3x8_s1/_s2` for the 3x3 layers with one input plane (8 sums in r4-r11);
     requantization in 32-bit steps (`TS_RQ32`, every shipped channel: shift >= 33 and bias + sum in int32, USAT
     for ReLU), 1x1 inputs packed with PKHBT/SXTB16. ~126 M -> ~107 M instructions per screen.
  5. The two-level head runs both levels through `mm_i16_pairs` (quantised again in the plain layout for the escape).
  6. main.cpp: the read's log lines held in a 2 KB static buffer and written after the lookup's first frame; no model
     file check per read; the core-3 worker thread made once (LightEvent go/done) instead of per read.
  7. Option A (owner's choice, 2026-11-01): the spotter on every core. Each 3x3 layer's output row is computed in
     8-pixel blocks that are independent (they read the layer's input rows, write their own columns; scratch per
     worker), so `produce()` hands them to a team (`vocr_textspot6_find_par`, `TextSpotter::setParallel`): the runner's
     threads stay for the whole find(), member 0 runs the spotter, the rest spin on atomics and claim blocks by a CAS on
     ticket = (job << 16) | block (a row's block is ~50 us: a LightEvent wake-up would cost as much). Same boxes on all
     screens (3 workers, 1, 4 on 2 cores x3, qemu-arm); ThreadSanitizer clean. Console before: spotter 249-251 ms.
  8. Option B (owner's choice, 2026-11-01): with fewer lines than workers, each line by the whole team in turn
     (`lineTeamMember`, `vocr_set_par`): run_pw's quantisation (even position chunks) and output blocks, k_conv1/k_dw by
     column, the two-level head's level 1, escape and level 2 by 4-frame chunks (each frame's updates in block order);
     jobs under ~6000 multiply-accumulates stay on one thread (`par_on`). Shared spinning team in `core/team.hpp`
     (the spotter uses it too). 3+ lines: a worker per line as before. Same text (2, 3, 4 workers, oversubscribed;
     qemu-arm); ThreadSanitizer clean.
  Tried and dropped: an SXTB16/SMLABB 3x3 kernel in C (no fewer instructions than LDRSB+SMLABB), bigger VOCR tiles
  (weights reused more, but tile 8 = +130 KB per worker arena), a cheaper ReLU (results depend on the VFP's
  flush-to-zero). The alpha-only per-worker timing in the read line
  (`setWorkerClock`, `WorkerSummary`) was removed when it moved to `test`. Open question for the owner: all cores on each line (a team per
  step_op / run_pw, ~96% of a line is parallel per column or position), worth it only if core 3 pulls its weight.
- On `test` (2026-11-02, owner's request: the spotter's grouping too strict): `readScreenSpot` joins two blocks
  next in reading order whatever their kinds (the spotter's `joins()` needs one tag, and a bubble's first row was often
  tagged scene/name over dialogue): the next block's first line below the last line (gap <= the taller height, the
  spotter's rule), heights within 3:2, left edges within 30% of the taller height (`g_spotJoinGapPct` /
  `g_spotJoinLeftPct`; bench_cli takes KT_JOIN_GAP / KT_JOIN_LEFT, -1 off), both blocks with 3+ Japanese characters.
  Backtest, 125 screens (shots/ + luma/screenshots/): 25 changed, 24 better; left 40-75% also joined M&L's "あらすじ"
  heading (8 px off), 150% Pokémon's speaker names (24-33 px); gap 150-300% changed nothing. Log: "joined N".
- On `alpha` (2026-11-03, owner's choice): **a 128 KB networking block reserved at game start** (`kt::netReserve`
  in core/net.cpp, called in main() after the "start:" line; kept for the session; netStop no longer frees it; the
  512/256/128 KB fallback only when the reservation failed). Why: after the first read the heap has ~620 KB free
  (model ~1,160 KB, 3 worker contexts ~540 KB, picture pool 337 KB, glyph slab 96 KB, dict cache 96 KB, spotter
  52 KB); a 512 KB socket block taken then left ~100 KB, and the card upload's `new char[16384]` (reply buffer in the
  POST code) threw bad_alloc -> abort() (crash_dump_00000019, Mario & Luigi: Make Anki cards run twice; the first
  got 256 KB and sent 16 of 17). Reads lose 128 KB of headroom. Log: "net: 128 KB reserved ...", "started with a
  reserved 128 KB block". Unverified: socInit with 128 KB and a card upload (check on the console). Not done (owner
  declined for now): a larger plugin memory block (plgInfo MemorySize 10MiB / extra block at runtime); allocations in
  the upload path still abort on failure instead of reporting it.
- On `alpha` (2026-11-04, owner's choice): **smaller dictionary caches** + an alpha diagnostic. A top-screen spotter
  read failed ("not enough memory to look for text") after 36 reads in one M&L session: free before a read fell
  564 -> 476 KB (no fragmentation: largest == free); the touchscreen still read (spotter arena 178 KB vs 208 KB top,
  plus the grey copy 75 / 94 KB). Dictionary's recent-answer maps capped 512/512/4096 -> 128/128/1024
  (`kExactMemoMax` etc. in dict.hpp) and emptied with swap (clear() kept the bucket arrays). Same output on 219
  screens x 4 modes; dictionary SD reads +14% (0.3 a read). **Alpha-only diagnostic, remove before `test`:**
  "memory before the spotter: ... heap free, largest; dictionary caches ..." (ReadOneScreen). On paper ~480 KB
  should still have fitted the top screen's ~300 KB, so ~tens of KB used during a read are unaccounted for: that
  line tells. Owner declined a fallback to the box reader when the spotter can't get its memory.
  Console result (2026-11-07, two M&L sessions of 59 and 33 reads): no drift (before a read 543-576 KB, up when the
  caches empty), no failure; that line showed 450-480 KB. But the arena is exactly 212,855 bytes top / 182,047
  touchscreen (+ 1,684 for the result set; same on x86-64 and ARM), and the old build's log had top reads work at 404
  KB free at that point and fail at 388 KB while 388 KB in one piece was mallocable right after: the spotter step
  needs ~390-400 KB, ~190 KB more than it asks for. Nothing found in the read path (pictures in the pool, screenshot
  thread joined first, core-3 thread made once). Margin now ~50 KB.
- On `alpha` (2026-11-07, owner's OK): **more alpha-only diagnostics, remove before `test`**: `TextSpotter::setMemFail`
  (core/text_spot.cpp) calls main.cpp's `SpotterMemFail` at the failure itself: "spotter memory: no block of N KB
  (working memory got/FAILED, results ...); the same again now: ...; heap free, largest; malloc: from the system,
  free in it, top piece, not taken yet" (mallinfo arena / fordblks / keepcost, fake_heap_end - sbrk(0)). MakeCards logs
  "cards: N saved words, N new cards, N already in Anki, N failed[: error]; heap ..." and "cards: PC not reached: ...".
  Both settled 2026-11-07: socInit with the reserved 128 KB block worked (3 uploads, no crash); whether the cards
  arrived is what the new "cards:" line shows.
  Console result (same evening, M&L, mostly both-screens reads): 16 reads failed, each time the 207 KB arena malloc
  FAILED and the same malloc right after got it (heap free 380-386 KB in one piece; malloc: 224-230 KB free inside,
  top piece 4-79 KB, 156 KB beyond sbrk). Not a shortage: a read failed at 548 KB before and the next one at 548
  worked; reads worked with 374 KB left and failed with 386. The console's newlib malloc misses a big-enough free
  chunk on the first large request (the results' small malloc/free in between seems to put it where the retry
  finds it; newlib's source couldn't be fetched to confirm).
- On `alpha` (2026-11-07, owner's choice of 3 options): **the spotter's working memory kept for the session**.
  `TextSpotter::keepMemory(400, 240)` / `releaseMemory()` (core/text_spot.cpp): one 212,855-byte arena + the result set,
  used by every find() on a screen that size or smaller (both screens of a read in turn), else find() mallocs as before.
  main.cpp `KeepSpotterMemory(wanted)` in ReadScreens after LoadAreas: kept while the spotter is on and the game lacks a
  reading area on at least one screen; given back in "Text boxes only" or with areas on both screens. Log once per
  change: "spotter: N KB kept for its working memory ..." / "spotter: no N KB block to keep ... taken each read
  instead" / "spotter: working memory given back". Heap free drops ~208 KB for the session (the large-known-list and
  large-mined.tsv audit risks get closer). Check: 43 saved screens (39 top, 4 touchscreen) x 4 orders x 1 and 3
  workers, kept vs fresh memory: same boxes, ASan/UBSan clean. The alpha diagnostics stay until the owner confirms.
  Console result (2026-11-07, M&L, many both-screens reads): the kept arena worked ("207 KB kept", no spotter
  failure), but after ~30 reads the top screen's grey copy (greyFromRGB, 96,000 bytes) was refused 17 reads in a row
  with LargestFreeKB = 340 KB measured just before (nothing allocated in between) while the touchscreen's 76,800 worked;
  "Read both screens" then silently showed the touchscreen alone. A consistent dlmalloc can't refuse 94 KB right after
  granting 340 KB, and Ubuntu's arm-none-eabi newlib (same mallocr.c) under qemu-arm with a plugin-shaped heap refused
  nothing in 400 random states: so the console's heap is probably inconsistent (a chunk header overwritten, or sbrk
  moved by something other than malloc), not short. Recognition workers don't malloc (runLine writes preallocated
  buffers). Owner declined, for now, reserving the grey copies too; offered: say so when one screen of a both-screens
  read fails (not decided).
- On `alpha` (2026-11-07, owner's choice): **heap diagnostic, alpha only, remove before `test`**. `heap_walk.c` /
  `Includes/heap_walk.h` read newlib's malloc (dlmalloc 2.6.4 layout, 32-bit) through weak symbols (`__malloc_av_`,
  `__malloc_sbrk_base`, `__malloc_top_pad`, ...; missing -> "malloc's variables not found"), under `__malloc_lock`:
  malloc's top vs sbrk(0), a walk of every chunk from sbrk's base to top (stops at a fencepost, size 4 = a foreign sbrk,
  or at a bad size = a header overwritten), free pieces, how many are big enough and whether they're on the bins, the
  bins' totals, last remainder, bitmap, top pad / trim threshold / most sbrk'd. main.cpp `HeapReport(what, size)`: full
  report ("heap: ...", "heap walk: ...", "heap bins: ...") for the session's first 4 refusals, one line after; called
  for the grey copies (then greyFromRGB once more: "heap: the same again now: got/FAILED"), the spotter's fallback
  memory (`SpotterMemFail`), the pictures' fallback and the reading area's crop. Tested under qemu-arm with Ubuntu's
  newlib (scratch harness, not in the repo): totals equal mallinfo's; a foreign sbrk -> fencepost; an overwritten header
  -> BROKEN at it; a fitting free chunk found on the bins.
  Console result (2026-11-07, 68 reads, M&L, mostly both screens): the heap is consistent (walk start to top ok, sbrk at
  top's end, no fencepost) but full: 3,276 of 3,520 KB in use, ~214 KB free inside in 470-540 pieces + 20 KB beyond
  sbrk. From read 26 the touchscreen's grey copy (76,800) was refused on every both-screens read (the top's 96,000 had
  taken the biggest area), retry too; single-screen reads worked. The largest free piece (~75 KB, same address each
  time) counted as big enough by the walk (>= request + 8) yet malloc never used it: a few bytes short or misfiled, not
  settled. So: tight and fragmented, not corrupt; the earlier "allocator quirk" reading was wrong.
- On `alpha` (2026-11-07, owner's choice): **the grey copies kept for the session**. `g_greyPool` (172,800 bytes: top
  400x240 then touchscreen 320x240), `ReserveGreys()` after `ReservePictures()` in ReadScreens (once; logs "grey copies:
  N KB kept for them ..." or "no N KB block ... taken each read instead"); `ScreenGrey(i, boxes, owned, ...)` writes into
  it with `kt::greyFromRGBInto` (core/local_ocr.cpp; greyFromRGB now uses it), malloc only without the pool, and
  ReadScreens frees only owned copies. Peak during a read unchanged; free between reads ~340 -> ~170 KB. Offered next if
  still tight: fixed storage for the dictionary caches (fewer small pieces); a larger plugin memory block (declined).
  Console result (2026-11-08, owner: "can no longer get the plugin to run out of memory"): 48 reads in one M&L session
  (39 both screens, 5 top, 4 touchscreen), all read, no heap reports, no memory errors, no crash dump; "grey copies: 168
  KB kept" then "spotter: 207 KB kept" at the first read (heap free 258 KB); free before a read 158-186 KB the whole
  session, no drift. Make Anki cards' networking started with 148-161 KB free, but the PC wasn't on, so an upload with
  this little free memory is still untried on this build. The alpha diagnostics (memory before the spotter, spotter
  memory, cards:, heap_walk/HeapReport) are still in: remove before `test`.
- Known issue (owner, 2026-11-01; not new): game audio is sometimes silent after a lookup closes, for a few seconds or
  until the game gets input. Likely the game's sound engine after CTRPF's pause froze all its threads (the DSP plays out
  its queued buffers meanwhile); the screenshot write right after resume (75-400 ms of SD writes) is a smaller suspect.
  Not fixed.
- Releases: a version bump (plgInfo + PluginMenu) and a CHANGELOG entry, plus UPDATING.md steps when users
  must do more than replace the plugin; the owner reviews them before the merge to main. The old
  `release-1.1.0` branch (drafted on items 1-2 only) is superseded and can be deleted.
- Ideas the owner was offered but hasn't decided on: an easier dictionary builder or prebuilt
  dictionaries in releases; switching dictionaries on the console; a workflow diagram in the
  README.
