# Kotoba3DS

An in-game Japanese dictionary for a modded (New) 3DS / 2DS, plus a PC script that turns the
words you save into Anki cards with the screenshot and the sentence.

**Latest: 1.1.0.** A newer, faster OCR model, text found anywhere on the screen, a fast mode and
reading areas. **Already using Kotoba3DS? See [UPDATING.md](UPDATING.md)** (this version needs the
new OCR model zip). What changed in each release: [CHANGELOG.md](CHANGELOG.md).

Massive thanks go to bee-san and the [vita-ocr](https://github.com/bee-san/vita-vn-ocr) model she built. Without it this project would not be possible!

## Features

**On the 3DS** (a Luma3DS plugin, works in any game)
- **Reads the screen itself.** Press **ZL + ZR** (any buttons you choose; an original 3DS,
  which has no ZL/ZR, needs another hotkey set in the menu) and the game pauses while the 3DS reads the
  text with on-device OCR, in under a second. It needs no PC and no internet. The touchscreen,
  or both screens, can be read too, each with its own hotkey.
- **Finds the text wherever it is.** Dialogue boxes, speech bubbles, menus and subtitles over the
  scene are found by a line spotter; a *Fast mode* and a fixed *Reading area* per game are there
  for speed. Furigana and the "next" arrow are left out, and the dictionary corrects
  kanji the OCR wasn't sure of (造跡 → 遺跡).
- **Tap a word to look it up.** The full Jitendex entry shows over the paused game, with the
  reading, how common the word is (JPDB rank) and other matches for the same spelling.
  Conjugations are undone (食べられなかった → 食べる, with the conjugation shown).
- **Save words for Anki.** One button saves the word with its sentence and a screenshot. Saving
  is instant, and a mistaken save can be undone just as quickly.
- **Known words in green.** Words you saved, words you marked known with **Y** (grammar, words
  you already know) and the words of your Anki cards are shown in green.
- **Your choice of spelling.** Saved words are written in their usual kanji, or as the game
  wrote them.
- **Easy on the game.** The dictionary is searched on the SD card instead of being loaded into
  memory, a New 3DS runs at full speed only while a lookup is open.

**On the PC** (optional; `tools/ocr_server.py` or `tools/mine.py`)
- **Anki cards in one step.** Choose *Make Anki cards* on the 3DS and every saved word becomes a
  [Lapis](https://github.com/donkuri/lapis) card, sent over Wi-Fi through AnkiConnect. You can
  also fetch the saved words over ftpd or from the SD card.
- **Full cards.** Each card gets the screenshot, the sentence (word in bold), the full Jitendex
  definition, pitch accent, frequency and word audio. Cards missing audio or a definition are
  tagged and flagged red, so they're easy to finish by hand.
- **No duplicates.** Words that are cards already are skipped.
- **Your Anki words on the 3DS.** *Known words* copies the words of your Anki cards to the 3DS
  so they show in green.

The on-console dictionary is built from [Jitendex](https://jitendex.org) (or JMdict, or any
Yomitan dictionary) and lives on the SD card.

---

## 1. Build the plugin (`Kotoba3DS.3gx`) and the dictionary (`dict.bin`)

### Option A: download the ready-made files (easiest)
Open the **[latest release](../../releases/latest)** and download `Kotoba3DS.3gx` and
`dict.bin`. They are rebuilt automatically every time the code changes.

### Option A2: let GitHub build both in your own copy (no tools to install)
1. Fork this repository (or create a new one and upload this whole folder, including `.github/`).
2. Open the **Actions** tab. The `build` workflow starts on its own (or press *Run workflow*).
3. When it's green, download the **Kotoba3DS-plugin** and **Kotoba3DS-dict** artifacts from
   the run page, or take them from your copy's **Releases** page.

### Option B: build on your PC
**Plugin**: install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS tools,
then add the CTRPluginFramework package repository and install it with 3gxtool:

```sh
# Windows: run inside C:/devkitPro/msys2/msys2_shell.bat and use pacman.
# Linux/macOS: use dkp-pacman; the config is /opt/devkitpro/pacman/etc/pacman.conf
cat >> /opt/devkitpro/pacman/etc/pacman.conf <<'EOF'

[thepixellizeross-lib]
Server = https://thepixellizeross.gitlab.io/packages/any
SigLevel = Optional

[thepixellizeross-linux]
Server = https://thepixellizeross.gitlab.io/packages/x86_64/linux
SigLevel = Optional
EOF
# on Windows use [thepixellizeross-win] with .../packages/x86_64/win instead of -linux
dkp-pacman -Sy libctrpf 3gxtool
make -C plugin            # -> plugin/Kotoba3DS.3gx
```
Keep the project path free of spaces.

**Dictionary** (Python 3.9+):
```sh
pip install jamdict jamdict-data        # provides RADKFILE (kanji parts) + a fallback JMdict

# From Jitendex (recommended) - download jitendex-yomitan.zip from https://jitendex.org - with
# JPDB's frequency ranks (shown as #1,234 on the 3DS; homographs listed most common first) from
# https://github.com/Kuuuube/yomitan-dictionaries (JPDB v2.2 Frequency Kana)
python tools/build_dict.py --yomitan jitendex-yomitan.zip --freq JPDB_v2.2_Frequency_Kana_2024-10-13.zip -o dict.bin

# Any Yomitan term dictionary works the same way; repeat --yomitan to combine several, and
# add --freq <frequency dictionary .zip> to list common words first.

# Or plain JMdict:
python tools/build_dict.py -o dict.bin                       # bundled copy
python tools/build_dict.py --jmdict JMdict_e.gz -o dict.bin  # current one from EDRDG
```

## 2. Install on the 3DS

| File | Put it at |
|---|---|
| `Kotoba3DS.3gx` | `SD:/luma/plugins/default.3gx` (every game), **or** `SD:/luma/plugins/<TitleID>/Kotoba3DS.3gx` for one game |
| `dict.bin` (~55 MB) | `SD:/3ds/kotoba3ds/dict.bin` |
| `Kotoba3DS-ocr-model.zip` (the OCR models) | unzip to the root of the SD card: `SD:/3ds/kotoba3ds/ocr/` (`VOCR6.2_N_w8g2.vocr`, `textspot8_t64n8c16_w8.vtsp`) |

Then open the Rosalina menu (**L + Down + Select**), set **Plugin loader: [Enabled]**, and
start a game. After a few seconds the plugin is ready; press **Select** to open its menu.

> The plugin turns off CTRPF's cheat and search tools. Only its own menu is shown.

## 3. Use it while playing

1. A line has a word you don't know → press **ZL + ZR**. The game pauses and the 3DS reads the
   top screen ("Reading the screen…", under a second). Text on the touchscreen: set a hotkey for
   *Read the touchscreen* (or *Read both screens*) in the Select menu.
2. The text appears in large type on the **touchscreen**, broken into lines where the game broke
   them (the type gets a little smaller when the game's lines are long). **Tap a word**: its entry shows on the
   top screen over the dimmed game (the layout follows
   [Rubi for Vita](https://github.com/bee-san/Rubi_for_vita)'s overlay), the word in yellow, its
   reading in blue, how common it is (its [JPDB](https://jpdb.io) frequency rank, e.g. #1,234), any
   conjugation under it. Conjugated forms are matched whole
   (食べられなかった → 食べる).

   | Control | Does |
   |---|---|
   | Tap a character | look up the word starting there |
   | Circle Pad ◀ ▶ | previous / next word (held: keeps going) |
   | Circle Pad ▲ ▼ | the row above / below |
   | D-pad ▲ ▼ (or C-stick) | scroll a long definition |
   | D-pad ◀ ▶ (or L / R) | the other matches at that spot (homographs, most common first, then shorter words), shown as 1/3 |
   | **A** (or the Save button) | save the word; on a word you just saved (the button reads Undo), take the save back |
   | Y | mark the word known without saving it (grammar, words you know); Y again takes it back |
   | X (or Hide) | uncover the screen that was read: see-through or hidden (*Hide (X)* in the menu); again to come back |
   | **B** (or Close) | close; the game continues once you let go of the buttons |

   If the screen had several text boxes (two speech bubbles, a menu and a dialogue box), they are
   all there, top to bottom, with a line between them (both screens: the top screen's first). When the
   touchscreen was read, its text can also go on the top screen and the entry on the touchscreen
   (*Touchscreen text shows on*).
   If nothing could be read, the screen is kept as `shots/failed_*.qoi`, which helps when
   reporting a problem.
3. **Save** writes the word to `mined.tsv` with the sentence and a screenshot of the screen(s)
   that were read (in `SD:/3ds/kotoba3ds/shots/`); a saved word turns green. The screenshot is
   written once the lookup closes, while the game goes on, so saving is instant.
4. **Words you know are green**: the words you saved before, the ones you marked with **Y**
   (kept in `SD:/3ds/kotoba3ds/marked.txt`), and the words of your Anki cards once *Known words*
   has their list (below), wherever they are in the text.

The **Select** menu (drawn like the lookup: tap, or D-pad / Circle Pad and A) has:

| Item | Does |
|---|---|
| *Read the top screen* / *Read the touchscreen* / *Read both screens* | each reading's hotkey: hold the button(s) you want, one or several, then let go (or *Turn off*) |
| *Touchscreen text shows on* | the touchscreen (tap the words) or the top screen (the entry then goes on the touchscreen) |
| *Hide (X)* | see-through, hidden, or see-through then hidden |
| *Saved words are written* | in kanji (the word's usual kanji form, even when the game wrote it in kana: ちょっと → 一寸) or as in the game (kana stays kana: たべた → たべる) |
| *Fast mode* | on: only text boxes of one flat colour (most dialogue boxes) are read, which is quicker, but text out in the scene, menus and shaded bubbles may be missed or read in part; off (the default): every line on the screen is found |
| *Reading area* | for a game whose text always shows in one place (subtitles, a fixed dialogue box): only that part of the screen is read, quicker and with nothing else getting in. Set per game and per screen on the paused game: D-pad moves it, L + D-pad sizes it (R for bigger steps), or drag with the stylus on the touchscreen; X the other screen, A saves, Y clears |
| *Last lookup* | reopen the text of your last lookup |
| *Saved words* | review them with their entry; Y deletes one |
| *Make Anki cards* | send the saved words to your PC (below) |
| *Known words* | get the words of your Anki cards from your PC, so they show in green (below); forget that list, or forget the words marked with Y |
| *PC* | find your PC, or type its address |
| *How to use* | the controls in short |

**How it reads the screen.** A line spotter (textspot8) finds every line of text on the screen,
wherever it is: dialogue boxes, speech bubbles, menus, names, subtitles over the scene. Each line is
read, and furigana and stray marks (the "next" arrow) are dropped. With *Fast mode* on, only
flat-colour text boxes are looked for instead (with no box on screen, the whole screen is read); with
a *Reading area* set, only that area is read. Where the recognizer was unsure of a kanji, the
dictionary decides: a guess that makes a word replaces one that doesn't (造跡 → 遺跡). The OCR is VOCR6 from
[vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr), the recognizer
[Rubi](https://github.com/bee-san/Rubi_for_vita) uses on the PS Vita, trained for game dialogue.
The older `VOCR5_N_w8.vocr` still works if VOCR6 isn't in `/3ds/kotoba3ds/ocr/` (VOCR6 is used when
both are there); without the spotter file, text boxes are found by their colour, as in earlier versions. The weights are for
**non-commercial use only**. Each read's timing goes into `SD:/3ds/kotoba3ds/ocr_log.txt`.

On a New 3DS / 2DS the plugin runs the CPU at full speed (804 MHz + L2 cache, as Rosalina's
*New 3DS menu* does) while a lookup is open and reads two or three lines at once (three when the
system lets the plugin use the fourth CPU core); the game's own
setting is restored when you close the lookup.

**Cards from the 3DS.** Saving never needs a PC: words wait on the SD card. When you're near
your PC, run `py -3.12 tools\ocr_server.py` there (same Wi-Fi, Anki open) and choose **Make Anki
cards** in the Select menu: every saved word is sent and becomes a Lapis card, with the
definition, audio, pitch and picture (words that are cards already are skipped), and you can
then clear them from the list. When it starts, `ocr_server.py` says what the cards will get
and warns if a dictionary is missing. The plugin finds your PC automatically; you can also type its
address under **PC**. `mine.py` (below) still works too, over ftpd or from the SD card.

**Words you have cards for, in green.** With `ocr_server.py` running (Anki open), choose
**Known words** → *Get them from my PC*: the words of all your Anki notes go to the 3DS
(`SD:/3ds/kotoba3ds/known.txt`), and from then on they show in green in lookups, like the words
you saved. `py -3.12 tools\mine.py known` does the same over ftpd or an SD card reader. The word
is taken from each note's `Expression` field (or `Word`, `Vocab`, `Vocabulary`, `Term`, `Kanji`,
`Japanese`, `Front`: the first one the note has), furigana and HTML removed; to choose, set
`known.query` (an Anki search, e.g. `"deck:Mining Deck"`) and `known.fields` in `config.json`.
Words that *Make Anki cards* turns into cards are added to the list as they go.

## 4. Make the Anki cards on your PC

Cards go to your deck **Mining Deck** with the **Lapis** note type by default.

### One-time setup
1. **Anki**: install the **AnkiConnect** add-on (code `2055492159`) and restart Anki. The Lapis
   note type must exist (it does if you already mine into Lapis; otherwise import the `.apkg`
   from [Lapis' releases](https://github.com/donkuri/lapis/releases)).
2. **Python packages**: `pip install pillow`. (No OCR is needed on the PC: the 3DS reads the
   screen and saves the sentence with each word.)
3. **Dictionaries**: make a folder `tools/dictionaries/` and put Yomitan `.zip` dictionaries
   in it. The script works out what each one is:
   - definitions: `jitendex-yomitan.zip` from [jitendex.org](https://jitendex.org). Other term
     dictionaries work too; Jitendex/JMdict are used first.
   - pitch accent: e.g. **[Pitch] NHK2016** from
     [MarvNC's dictionary collection](https://github.com/MarvNC/yomitan-dictionaries), or
     Kanjium.
   - frequency (optional, fills `Frequency`/`FreqSort`): e.g. JPDB from
     [jpdb-freq-list releases](https://github.com/MarvNC/jpdb-freq-list/releases).

   If you already use Yomitan, these are the same zip files. Don't unzip them.
4. **ftpd**: install it on the 3DS from Universal-Updater.
5. Run `python tools/mine.py setup` with Anki open. It writes `tools/config.json`, checks that
   Lapis has all its fields and that *Mining Deck* exists, and indexes the dictionaries (the
   first time takes a minute or two). The last lines list which dictionaries were found.
6. Edit `tools/config.json`:
   - `source.ftp_host`: your 3DS's IP address, shown on screen while **ftpd** is open. If you'd
     rather use an SD card reader, set `source.sd_root` to the card's drive (e.g. `E:/`).
   - `games`: title ID → game name. It goes into `MiscInfo` and becomes the card's tag. The
     title ID shows in the `MiscInfo` field of your first card.

### Each time
1. Close the game, open **ftpd** on the 3DS, and keep Anki open on the PC.
2. Run `python tools/mine.py`. Add `--dry-run` to preview, or
   `--clear` to remove imported words from the 3DS log afterwards. `python tools/mine.py known`
   puts the words of your Anki cards on the 3DS (green in lookups).

The script remembers what it imported (`tools/imported.json`), so running it twice never
makes duplicate cards.

### What goes in each Lapis field

| Field | Filled with |
|---|---|
| Expression / ExpressionReading | the word and its reading |
| ExpressionFurigana | `食[た]べる` (Anki furigana format, like Yomitan's `{furigana-plain}`) |
| ExpressionAudio | word audio (see below) |
| MainDefinition, Glossary | the Jitendex entry, same HTML and styling as Yomitan's |
| Sentence | OCR'd line with the word in `<b>` |
| Picture | the screenshot of the screen(s) that were read, at 2× |
| PitchPosition / PitchCategories | e.g. `[2]` / `kifuku`, which Lapis turns into the pitch graph and colors |
| Frequency / FreqSort | from your frequency dictionaries (harmonic rank, like Yomitan) |
| MiscInfo | game name and time |
| Is…Card, SentenceFurigana, SentenceAudio, Hint, … | left empty (a normal vocab card) |

To change any of this, edit `anki.fields` in `config.json`. Each value is a template using
these placeholders: `{expression} {reading} {furigana} {audio} {definition_html}
{definition} {sentence} {picture} {pitch_position} {pitch_categories} {frequency}
{freq_sort} {game} {game_tag} {time} {inflection} {query} {title_id}`. For example, set
`"IsWordAndSentenceCard": "x"` to make every card a word+sentence card.

### Cards to finish by hand
A card whose word isn't in any of your dictionaries (it gets the 3DS's short definition instead)
is tagged `no_definition`; one for which no audio source had a recording is tagged `no_audio`.
Either way its card also gets **flag 1 (red)**, so you can find them in the Anki browser
(*Flag → Red*, or search `tag:no_definition` / `tag:no_audio`) and add what's missing. Change
the tags with `"missing_definition_tag"` and `"missing_audio_tag"` in the `anki` section (`""`
for none), and the flag with `"flag_missing"` (0 turns it off; 2 = orange, 3 = green, 4 = blue…). If Anki
has the card open in an editor, it won't take the flag yet; the flag is set as soon as the editor
closes.

### Word audio
`audio.sources` is tried in order for each word, until one has a recording:

```json
"sources": ["local", "jpod101", "jpod101-alt", "lingualibre", "forvo"],
"forvo_key": ""
```

| Source | What it is |
|---|---|
| `"local"` | **Local Audio Server for Yomitan** (an Anki add-on with offline NHK, Shinmeikai, Forvo and JPod recordings) at `http://127.0.0.1:5050`. Used when it's running; skipped quietly when it isn't. The best free way to get Forvo audio. |
| `"jpod101"` | JapanesePod101's word recordings (Yomitan's default). Needs internet. |
| `"jpod101-alt"` | JapanesePod101's dictionary search, which sometimes has words the first one doesn't. |
| `"lingualibre"` | Native-speaker recordings from [Lingua Libre](https://lingualibre.org) on Wikimedia Commons. Free; fewer words. |
| `"forvo"` | [Forvo](https://forvo.com) through its official API. Needs your own key from [api.forvo.com](https://api.forvo.com) (a paid plan) in `forvo_key`; skipped without one. |

Reorder or remove entries to taste. Any other URL works too, using `{term}` and `{reading}`,
either returning an audio file or Yomitan's JSON audio-source list:
```json
{"name": "my server", "url": "http://example.com/audio?term={term}&reading={reading}"}
```

## Files on the SD card

```
/luma/plugins/default.3gx      the plugin
/3ds/kotoba3ds/dict.bin        dictionary
/3ds/kotoba3ds/mined.tsv       saved words (tab-separated, UTF-8)
/3ds/kotoba3ds/shots/          screenshots of the screen(s) read (400x240, 320x240, or both 400x480; QOI images, a third of a BMP)
/3ds/kotoba3ds/settings.txt    hotkeys and the menu's choices
/3ds/kotoba3ds/known.txt       words of your Anki cards (green in lookups; from Known words or mine.py known)
/3ds/kotoba3ds/marked.txt      words you marked known with Y
/3ds/kotoba3ds/ocr/            the OCR models (VOCR6.2_N_w8g2.vocr, textspot8_t64n8c16_w8.vtsp)
/3ds/kotoba3ds/areas.txt       the reading areas you set, per game
/3ds/kotoba3ds/ocr_log.txt     each read's timing, and notes for bug reports
```

## Troubleshooting

- **The game crashes or shows an error at start**: update to the latest release first; it
  fixes this for most games. If it still happens, turn the plugin loader off in the Rosalina
  menu to play, and please report the game with `SD:/3ds/kotoba3ds/ocr_log.txt` (its `boot:`
  lines say what the plugin did with the game's memory).
- **Menu doesn't open**: make sure the plugin loader is enabled, the file is named
  `default.3gx` (or sits in the game's title ID folder), and you waited ~5 s after the game
  started.
- **"Dictionary not found"**: check that the file is exactly `SD:/3ds/kotoba3ds/dict.bin`.
- **"wrong format / truncated"**: download `dict.bin` again from the latest release (or rebuild
  it with `build_dict.py`), and copy it again.
- **Japanese shows as boxes**: the plugin draws text with the console's system font. Please
  report it, with your console's region.
- **mine.py can't connect**: ftpd must be open on the 3DS, and the PC and 3DS on the same network.
- **"Couldn't read the screen: No OCR model found"**: unzip `Kotoba3DS-ocr-model.zip` to the
  root of the SD card (the model goes to `SD:/3ds/kotoba3ds/ocr/`).
- **Cards aren't sent to the PC**: check `ocr_server.py` is running and that Windows Firewall
  allows Python on private networks. Then type the address it prints under **PC**. The
  3DS only uses 2.4 GHz Wi-Fi. Saved words are kept either way; `mine.py` adds them later.
- **"networking needs Luma3DS v13.2 or newer"**: plugins can only use the network on Luma
  v13.2+. Update Luma (the version shows at the top of the Rosalina menu).
- **"network unavailable in this game"**: some titles don't permit network access from
  plugins. Lookups still work; make the cards with `mine.py` later.

## Developing

- `tests/`: native tests of the plugin core (conjugation, dictionary search, OCR plumbing) against
  a real `dict.bin`: `make -C tests run DICT=../dict.bin`. Also `python tests/test_builder.py
  <jamdict.db>` and `python tests/test_mine.py [jitendex-yomitan.zip]` (needs
  `pip install pyftpdlib pillow`).
- `tools/yomitan.py`: Yomitan dictionary reader and HTML renderer; `tools/enrich.py`: card
  extras (definitions, pitch, frequency, audio, furigana).
- `plugin/Sources/core/`: platform-independent C++ (compiled both for the 3DS and the tests).
- `plugin/Sources/main.cpp`: the CTRPF menu/UI.

## Credits & licences
- On-device OCR: the VOCR runtime, the VOCR6 and VOCR5 models and the textspot8 line spotter of
  [bee-san/vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr) (GPL-3.0-or-later; the model weights
  are for non-commercial use only, see the attribution files that come with them), the OCR of
  [Rubi](https://github.com/bee-san/Rubi_for_vita).
- [JMdict](https://www.edrdg.org/jmdict/j_jmdict.html) and
  [RADKFILE](https://www.edrdg.org/krad/kradinf.html) are property of the Electronic
  Dictionary Research and Development Group, used under CC BY-SA 4.0.
- [Jitendex](https://jitendex.org) by Stephen Kraus, CC BY-SA 4.0 (includes Tatoeba examples,
  CC BY 2.0 FR). Card layout by [Lapis](https://github.com/donkuri/lapis).
- Built on [CTRPluginFramework](https://gitlab.com/thepixellizeross/ctrpluginframework)
  (The Pixellizer Group licence) and [Luma3DS](https://github.com/LumaTeam/Luma3DS)'s plugin
  loader.
- Conjugation handling follows the approach of [Yomitan](https://github.com/yomidevs/yomitan).


## Licence

Kotoba3DS is free software under the GNU General Public License v3.0 or later ([LICENSE](LICENSE)),
because it includes the GPL-licensed VOCR runtime. The dictionaries, the OCR models and the
third-party code keep their own licences (above).
