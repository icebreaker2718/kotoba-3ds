# Kotoba3DS

An in-game Japanese dictionary for a modded (New) 3DS / 2DS, plus a PC script that turns the
words you save into Anki cards with the screenshot and the sentence.

**On the 3DS** (Luma3DS plugin): press **ZL + ZR** in any game when a line has a word you
don't know. The game pauses, the 3DS **reads the screen itself** (on-device OCR, no PC or
internet) and shows the text on the touchscreen: tap a word and its full entry appears on the top
screen over the paused game. Conjugated forms are matched (食べられなかった → 食べる). **Save** the word: it goes
to `SD:/3ds/kotoba3ds/mined.tsv` with the sentence and a screenshot.

**On the PC** (`tools/mine.py`): fetches the saved words over Wi-Fi (ftpd) or from the SD card
and makes [Lapis](https://github.com/donkuri/lapis) cards through AnkiConnect, with the
screenshot, the OCR'd sentence (word in bold), the full Jitendex definition, pitch accent,
frequency and word audio.

The on-console dictionary is built from [Jitendex](https://jitendex.org) (or JMdict, or any
Yomitan dictionary). It lives on the SD card and is searched on disk, so the plugin needs only
about 100 KB of RAM.

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
| `dict.bin` (~41 MB) | `SD:/3ds/kotoba3ds/dict.bin` |
| `Kotoba3DS-ocr-model.zip` (the OCR model) | unzip to the root of the SD card: `SD:/3ds/kotoba3ds/ocr/VOCR5_N_w8.vocr` |

Then open the Rosalina menu (**L + Down + Select**), set **Plugin loader: [Enabled]**, and
start a game. After a few seconds the plugin is ready; press **Select** to open it.

> The plugin turns off CTRPF's cheat and search tools. Only its own menu is shown.

## 3. Use it while playing

1. A line has a word you don't know → press **ZL + ZR**. The game pauses and the 3DS reads the
   top screen ("Reading the screen…", under a second).
2. The text appears in large type on the **touchscreen**. **Tap a word**: its entry shows on the
   top screen over the dimmed game (the layout follows
   [Rubi for Vita](https://github.com/bee-san/Rubi_for_vita)'s overlay), the word in yellow, its
   reading in blue, how common it is (its [JPDB](https://jpdb.io) frequency rank, e.g. #1,234), any
   conjugation under it. Conjugated forms are matched whole
   (食べられなかった → 食べる).

   | Control | Does |
   |---|---|
   | Tap a character | look up the word starting there |
   | D-pad or Circle Pad ◀ ▶ | previous / next word (held: keeps going) |
   | D-pad or Circle Pad ▲ ▼ | the row above / below |
   | L / R | the other matches at that spot (homographs, most common first, then shorter words), shown as 1/3 |
   | C-stick (New 3DS / 2DS) | scroll a long definition |
   | **A** (or the Save button) | save the word |
   | X (or Hide) | hide the definition to see the game |
   | **B** (or Close) | close; the game continues |

   If the screen had several blocks of text (a menu and a dialogue box), they are all there, the
   longest first, with a line between them.
3. **Save** writes the word to `mined.tsv` with the sentence and a screenshot of the paused game
   (in `SD:/3ds/kotoba3ds/shots/`); a saved word turns green.

The **Select** menu has: *Last results* (reopen the text of your last lookup), *Saved words* (review or
delete), *Hotkey* (if ZL + ZR does something in your game), *Anki cards* (see below),
*How to use* and *Speed test* (the next read is timed in several ways, into `ocr_log.txt`).

**How it reads the screen.** The text box is found by its flat colour (so a box that moves
around is fine), its lines are read, furigana and stray marks (the "next" arrow) are dropped.
With no box on screen it reads the whole screen. Where the recognizer was unsure of a kanji, the
dictionary decides: a guess that makes a word replaces one that doesn't (造跡 → 遺跡). The OCR is VOCR from
[vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr), the recognizer
[Rubi](https://github.com/bee-san/Rubi_for_vita) uses on the PS Vita, trained for game dialogue.
`VOCR5_S_w8.vocr` (smaller, faster) or `VOCR5_L_w8.vocr` (bigger, a bit more accurate) from its
[releases](https://github.com/bee-san/vita-vn-ocr/releases) also work: put one in
`/3ds/kotoba3ds/ocr/` (Normal is used when several are there). The weights are for
**non-commercial use only**. Each read's timing goes into `SD:/3ds/kotoba3ds/ocr_log.txt`.

On a New 3DS / 2DS the plugin runs the CPU at full speed (804 MHz + L2 cache, as Rosalina's
*New 3DS menu* does) while a lookup is open and reads two or three lines at once (three when the
system lets the plugin use the fourth CPU core); the game's own
setting is restored when you close the lookup.

**Cards while you play.** With `py -3.12 tools\ocr_server.py` running on your PC (same Wi-Fi,
Anki open), every word you save also becomes a Lapis card right away, with the definition,
audio, pitch and picture. The first time, the plugin looks for your PC automatically; you can
also enter its address under **Anki cards** in the Select menu, or turn this off there. The word
is always written to the SD card first, so if the PC isn't reachable nothing is lost: run
`mine.py` later and it adds those words, skipping any that already became cards.

## 4. Make the Anki cards on your PC

Cards go to your deck **Mining Deck** with the **Lapis** note type by default.

### One-time setup
1. **Anki**: install the **AnkiConnect** add-on (code `2055492159`) and restart Anki. The Lapis
   note type must exist (it does if you already mine into Lapis; otherwise import the `.apkg`
   from [Lapis' releases](https://github.com/donkuri/lapis/releases)).
2. **Python packages**: `pip install pillow`, plus `pip install manga-ocr` for sentence OCR
   (needs Python 3.12 or older; the first run downloads a ~400 MB model).
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
   - `ocr.crop`: the box `[left, top, right, bottom]` on the 400×240 top screen that holds the
     game's text. OCR is much better when it only sees the text box. Per-game boxes go in
     `ocr.game_crops`.

### Each time
1. Close the game, open **ftpd** on the 3DS, and keep Anki open on the PC.
2. Run `python tools/mine.py`. Add `--dry-run` to preview, `--no-ocr` to skip OCR, or
   `--clear` to remove imported words from the 3DS log afterwards.

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
| Picture | the top screen at 2× |
| PitchPosition / PitchCategories | e.g. `[2]` / `kifuku`, which Lapis turns into the pitch graph and colors |
| Frequency / FreqSort | from your frequency dictionaries (harmonic rank, like Yomitan) |
| MiscInfo | game name and time |
| Is…Card, SentenceFurigana, SentenceAudio, Hint, … | left empty (a normal vocab card) |

To change any of this, edit `anki.fields` in `config.json`. Each value is a template using
these placeholders: `{expression} {reading} {furigana} {audio} {definition_html}
{definition} {sentence} {picture} {pitch_position} {pitch_categories} {frequency}
{freq_sort} {game} {game_tag} {time} {inflection} {query} {title_id}`. For example, set
`"IsWordAndSentenceCard": "x"` to make every card a word+sentence card.

### Cards whose sentence missed the word
If a card's sentence doesn't contain the word you looked up (OCR misread it, or the wrong line
was read), the card is given **flag 1 (red)** and the tag `check_sentence`, so you can find
them in the Anki browser (*Flag → Red*, or search `tag:check_sentence`) and fix the sentence.
Conjugated forms count as found (食べる in 食べられなかった). Change the flag with
`"flag_missing_word"` in the `anki` section (0 turns it off; 2 = orange, 3 = green, 4 = blue…)
and the tag with `"missing_word_tag"` (`""` for none).

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
Configs from older versions that only had `"jpod101"` get the new list automatically.

## Files on the SD card

```
/luma/plugins/default.3gx      the plugin
/3ds/kotoba3ds/dict.bin        dictionary
/3ds/kotoba3ds/mined.tsv       saved words (tab-separated, UTF-8)
/3ds/kotoba3ds/shots/          screenshots (top screen above bottom screen, 400x480 BMP)
/3ds/kotoba3ds/settings.txt    hotkey choice
```

## Troubleshooting

- **Menu doesn't open**: make sure the plugin loader is enabled, the file is named
  `default.3gx` (or sits in the game's title ID folder), and you waited ~5 s after the game
  started.
- **"Dictionary not found"**: check that the file is exactly `SD:/3ds/kotoba3ds/dict.bin`.
- **"wrong format / truncated"**: rebuild `dict.bin` with this version of `build_dict.py`,
  and copy it again.
- **Japanese shows as boxes**: the plugin draws text with the console's system font. Please
  report it, with your console's region.
- **mine.py can't connect**: ftpd must be open on the 3DS, and the PC and 3DS on the same network.
- **"Couldn't read the screen: No OCR model found"**: unzip `Kotoba3DS-ocr-model.zip` to the
  root of the SD card (the model goes to `SD:/3ds/kotoba3ds/ocr/`).
- **Cards aren't sent to the PC**: check `ocr_server.py` is running and that Windows Firewall
  allows Python on private networks. Then type the address it prints under **Anki cards**. The
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

- [JMdict](https://www.edrdg.org/jmdict/j_jmdict.html) and
  [RADKFILE](https://www.edrdg.org/krad/kradinf.html) are property of the Electronic
  Dictionary Research and Development Group, used under CC BY-SA 4.0.
- [Jitendex](https://jitendex.org) by Stephen Kraus, CC BY-SA 4.0 (includes Tatoeba examples,
  CC BY 2.0 FR). Card layout by [Lapis](https://github.com/donkuri/lapis).
- Built on [CTRPluginFramework](https://gitlab.com/thepixellizeross/ctrpluginframework)
  (The Pixellizer Group licence) and [Luma3DS](https://github.com/LumaTeam/Luma3DS)'s plugin
  loader.
- Conjugation handling follows the approach of [Yomitan](https://github.com/yomidevs/yomitan).
- On-device OCR: the VOCR runtime and VOCR5 models of
  [bee-san/vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr) (GPL-3.0-or-later; the model weights
  are for non-commercial use only, see the `ATTRIBUTION.md` that comes with them), the OCR of
  [Rubi](https://github.com/bee-san/Rubi_for_vita).

## Licence

Kotoba3DS is free software under the GNU General Public License v3.0 or later ([LICENSE](LICENSE)),
because it includes the GPL-licensed VOCR runtime. The dictionaries, the OCR model and the
third-party code keep their own licences (above).
