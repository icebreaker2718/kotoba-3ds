# Changelog

## 1.2.0

This release makes reading faster and fixes running out of memory in long sessions. You can
download it from the [Releases page](https://github.com/icebreaker2718/kotoba-3ds/releases/latest);
the version shows in Luma's plugin loader. Updating from 1.1.0 only needs the new plugin: see
[UPDATING.md](UPDATING.md).

### Install over Wi-Fi

Each release now has **Kotoba3DS-SD.zip**: the plugin, the dictionary and the OCR models,
already in their SD card folders. With [3dsDL](https://ankush.one/3dsDL/), scan its QR code,
then extract the zip at the root of the card, and everything is in place. No SD card reader
needed. The steps are in the [README](README.md#install-over-wi-fi-with-3dsdl).

### Faster reading

Reading the screen is faster, with the same text as before. On a New 3DS, a normal read
(no reading area) went from about 425 ms to about 230 ms. Finding the lines of text now takes
about 100 ms instead of 250 ms. All of the console's cores now share the work: when a screen
has only one or two lines, each line is split between them, and when it has several text
boxes, they're read together instead of one after another.

### "Fast mode" is now "Text boxes only"

Normal reading is now nearly as fast as fast mode was, so the setting has a new name that says
what it does: it reads only flat-colour text boxes (most dialogue boxes), so nothing else on the
screen gets in. Your setting is kept. For the quickest reads, set a *Reading area*.

### Fixes

- **Running out of memory in a long session.** After many reads, especially *Read both screens*
  over and over, reading could start failing with "not enough memory to look for text" or "not
  enough memory for the picture", or show only one of the two screens. The plugin now sets aside
  the memory it reads with once, at the first read, and reuses it for the rest of the session, so
  the free memory no longer shrinks as you play. In a 104-read test session, every read worked.
- **"Not enough memory for networking", or a crash, in *Make Anki cards* after a long
  session.** *Make Anki cards* and *Get them from my PC* now use memory the plugin has already
  set aside, so they work however long you've been playing.
- **Long known-word lists.** A big `known.txt` used to take memory away from reading (a list of
  about 40,000 words left none). The list is now looked up on the SD card instead of being
  loaded, so it can be as long as you like. *Get them from my PC* writes the list straight to
  the card as it arrives, and if the download is cut short, your old list is kept.
- **A speech bubble's first line read as separate text.** A bubble's first row was sometimes
  treated as its own piece of text, so a sentence came out split in two
  (「さあここが」 and 「ツアーの見せ場だぞ!」). Lines right under each other with the same left
  edge and size are now kept together; a speaker's name beside the text stays separate. On the
  saved screenshots, 24 of 125 screens now read as one sentence where they were split before.

## 1.1.0

Thank you for using kotoba3ds! This is the changelog for the newest release, you can download it from the
[Releases page](https://github.com/icebreaker2718/kotoba-3ds/releases/latest); the version shows
in Luma's plugin loader. If you are updating from 1.0.0, check out [UPDATING.md](UPDATING.md).

This release makes use of the new OCR models from bee-san's vita-ocr project. Many thanks!! Check out the [Rubi for Vita](https://github.com/bee-san/Rubi_for_vita) project if you haven't already!

### A newer, faster OCR model (VOCR6)

The text is now read by VOCR6, the newest model from bee-san's
[vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr). It reads about as accurately as VOCR5,
a little better on hard text (small print, text with furigana), and recognizes each line about
20% faster.

### Text is found anywhere on the screen

Earlier versions only found text inside solid-coloured text boxes. Subtitles over the game
scene and text without a box could be missed or cut off. The plugin 
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
  games with plain dialogue boxes. Text over the game scene,
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
- **Redrawing:** the lookup screens redraw about 15% faster.
- **Reading area:** about 30% faster per read than finding the text with the line spotter.

The line spotter (fast mode off) costs a fixed amount of time per screen, so on a plain dialogue
box it can be a little slower than fast mode. For the quickest reads, use fast mode or a reading
area.

### Fixes

- After a long play session, the game could disappear from behind the lookup, X (Hide) stopped
  working, and saved words got no screenshot, until the game was restarted. Fixed.
- A line of katakana above a line with kanji (a subtitle starting with names, for example) was
  sometimes dropped as if it were furigana. It's read now.
- The `Kotoba3DS-2MiB-test.3gx` test copy is no longer included in releases: the games it was
  for now start with the normal plugin.

## 1.0.0

The first release: on-device OCR of the top screen, the touchscreen or both; tap-to-look-up
with the full Jitendex entry; saving words with the sentence and a screenshot; known words in
green; and Anki (Lapis) cards made on the PC by `tools/ocr_server.py` or `tools/mine.py`. See
the [README](README.md) for the full feature list.
