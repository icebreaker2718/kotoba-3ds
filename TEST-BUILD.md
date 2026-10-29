# Kotoba3DS test build: what's new

This is the **test build**: features that work on the developer's console but haven't been
confirmed widely yet. If something goes wrong, please report it (see *Reporting a problem* below).
For normal use, stay on the latest release.

## Installing

1. Download **Kotoba3DS.3gx** and **Kotoba3DS-ocr-model.zip** from the
   *Kotoba3DS test build* pre-release.
2. Copy `Kotoba3DS.3gx` to your SD card as `/luma/plugins/default.3gx`, replacing the old one.
3. Unzip `Kotoba3DS-ocr-model.zip` to the **root** of the SD card. It adds two files to
   `/3ds/kotoba3ds/ocr/`: the new OCR model (`VOCR6.2_N_w8g2.vocr`) and the line spotter
   (`textspot8_t64n8c16_w8.vtsp`), both from bee-san's
   [vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr) and for non-commercial use only. Your old `VOCR5_N_w8.vocr` can stay; the new model is used
   when both are there.
4. Keep your existing `dict.bin`. The dictionary hasn't changed.

If you skip step 3, the plugin still works with the old model and reads the screen the way it
did before.

## New features

### A newer, faster OCR model (VOCR6)

The text is now read by VOCR6, the newest model from
[vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr). It reads about as accurately as VOCR5,
a little better on hard text (small print, text with furigana), and recognizes each line about
20% faster.

### Text is found anywhere on the screen

Earlier versions only found text inside solid-coloured text boxes. Subtitles over the game
scene (Resident Evil Revelations) and text without a box could be missed or cut off. The plugin
now uses **textspot8**, the line spotter from bee-san's
[vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr) (the same project as the OCR model). It
looks at the whole screen and finds every line of text: dialogue boxes, speech bubbles, menus,
names and subtitles.

Because it finds *all* the text, a lookup may also show menu labels or other words around the
dialogue. If that gets in the way, use a *Reading area* (below).

### Fast mode

**Select → Fast mode** switches between two ways of finding text:

- **Off (default):** vita-vn-ocr's line spotter finds every line on the screen.
- **On:** only solid-coloured text boxes are read, as in earlier versions. This is quicker in
  games with plain dialogue boxes (Mario & Luigi, for example). Text over the game scene,
  menus, and shaded speech bubbles may be missed or only partly read.

### Reading area

For games where the text always appears in the same place (a fixed dialogue box, subtitles),
you can tell the plugin to read only that part of the screen. This is the quickest and
cleanest way to read: nothing else on the screen gets in, and it's about 70 ms faster per read
than reading with the line spotter.

To set one, pause a game with the text showing, open **Select → Reading area**, and press A.

| Button | Does |
|---|---|
| D-Pad | move the area |
| L + D-Pad | resize it |
| Hold R | bigger steps |
| Stylus | draw the area (touchscreen) |
| X | switch to the other screen |
| A | save |
| Y | clear it (read the whole screen again) |
| B | back without saving |

The area is saved **for this game only** and **for each screen** separately, in
`/3ds/kotoba3ds/areas.txt`. Leave a little room around the text, because longer lines in
later scenes may be wider than the one you set it on. Anything outside the area is ignored, so
if a lookup comes up empty or cut off, the text was probably outside it.

### Pokémon speech bubbles read in full

Pokémon Sun/Moon's speech bubbles fade from top to bottom, and earlier versions only read their
first line. The whole bubble is read now.

### Games that crashed at start now work

Some games crashed when they started with the plugin installed (逆転裁判123 /
Ace Attorney Trilogy, for example), because the plugin's memory left them short. The plugin
now makes room for both. 大逆転裁判2, Corpse Party BR, Mario & Luigi: Dream Team and
逆転裁判123 have all been tested and start normally.

### Keep a screenshot without saving a word

On the lookup screen, **Start** keeps the screenshot of what was read, without saving a word.
Press Start again to take it back. This is useful for reporting screens that were read wrong.

### Faster overall

- **Reading the text:** VOCR6 recognizes each line about 20% faster than VOCR5, saving about
  45 ms per read.
- **Finding text boxes:** on screens with text boxes, the box search takes about half the time
  it did before.
- **Redrawing:** the lookup screens redraw about 15% faster.
- **Reading area:** about 30% faster per read than finding the text with the line spotter.

The line spotter (fast mode off) costs a fixed amount of time per screen, so on a plain dialogue
box it can be a little slower than fast mode. For the quickest reads, use fast mode or a reading
area.

## Reporting a problem

The most useful things to send are:

- **`/3ds/kotoba3ds/ocr_log.txt`**: a line for every read with its timings and the text it
  found, plus a line for each game start.
- **The screenshot of the screen that went wrong**: press **Start** on the lookup screen to keep
  it. Screenshots are in `/3ds/kotoba3ds/shots/`. When nothing could be read, the screen is
  saved there automatically as `failed_*.qoi`.
- The game, and what you expected to see.

If a game crashes, the crash dump in `/luma/dumps/arm11/` helps too.
