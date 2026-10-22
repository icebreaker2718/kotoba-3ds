# Kotoba3DS

An in-game Japanese dictionary for a modded (New) 3DS / 2DS, plus a PC script that turns the
words you save into Anki cards with the screenshot and the sentence.

**On the 3DS** (Luma3DS plugin, opened with **Select** while any game runs):
- **Read screen**: with `tools/ocr_server.py` running on your PC, the plugin sends your last
  screenshot to it, shows the line as text, and you pick the word, Yomitan-style. If the PC
  can't be reached, it offers the manual lookup instead.
- Look up words by typing **romaji**. You see the kana as you type, and conjugated forms
  work: `taberarenakatta` finds 食べる (passive › negative › past).
- Look up kanji you can't read by **picking their parts** (氵 + 青 → 清), then search or
  add okurigana.
- Words that *start* with what you typed are listed too, so typing one kanji (勉) finds 勉強.
- **Save** a word: it goes to `SD:/3ds/kotoba3ds/mined.tsv` together with the screenshot you
  took with the hotkey (default **ZL + ZR**).

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
dkp-pacman -Sy libctrpf 3gxtool 3ds-mbedtls
make -C plugin            # -> plugin/Kotoba3DS.3gx
```
Keep the project path free of spaces.

**Dictionary** (Python 3.9+):
```sh
pip install jamdict jamdict-data        # provides RADKFILE (kanji parts) + a fallback JMdict

# From Jitendex (recommended) - download jitendex-yomitan.zip from https://jitendex.org
python tools/build_dict.py --yomitan jitendex-yomitan.zip -o dict.bin

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

Then open the Rosalina menu (**L + Down + Select**), set **Plugin loader: [Enabled]**, and
start a game. After a few seconds the plugin is ready; press **Select** to open it.

> The plugin turns off CTRPF's cheat and search tools. Only its own menu is shown.

## 3. Use it while playing

**One button:** press the hotkey (default **ZL + ZR**) on a line. It saves the screenshot, pauses
the game, and opens the lookup on the **bottom screen**, so the game stays visible (slightly
dimmed) on the top screen for comparison. With the OCR server running, you get the line split
into words; otherwise, or if the PC can't be reached, you can type the word or pick kanji
parts. While you type romaji, it turns into kana right in the input box. As you move through
the results, the highlighted word's **full entry is shown on the top screen** over the darkened
game; open it to save it, scroll long entries, or hide the definition to see the game. Press B to back out
and the game continues. The **Select** menu still has everything, plus settings and saved
words. To make the hotkey only take screenshots, use **Hotkey** in that menu.


1. A line has a word you don't know → press the **screenshot hotkey** (ZL + ZR). The plugin
   saves `SD:/3ds/kotoba3ds/shots/shot - NNN.bmp`.
2. Press **Select** → **Look up word (romaji)** → type it (e.g. `yomenakatta`) → **Start** /
   Enter.
   Kanji you can't read → **Look up by kanji parts**: pick the parts you see, then *Show
   matching kanji*. Pick one, then *Search*, *Add another kanji*, or *Type the rest in romaji*.
3. Pick a result to read the full entry, then choose **Yes** to save it. The most recent
   screenshot (taken within 15 min) is attached. Save several words from one screenshot if
   you like.
4. **Saved words** lists what you've saved (◎ = has a screenshot), and lets you delete one.

If ZL + ZR does something in your game, change it under **Screenshot hotkey**. The setting
is remembered.

**Romaji tips:** `nn` or `n'` for ん before a vowel (`kon'ya`), double consonants for っ
(`gakkou`), `-` for ー (`ra-men`), `xa`/`xtu` for small kana. Long vowels are spelled out
(`toukyou`).

## 3b. Read screen (OCR from your PC)

1. On the PC (same Wi-Fi as the 3DS): `py -3.12 tools\ocr_server.py`. It loads manga-ocr,
   prints its address (e.g. `192.168.1.20:7331`), and logs every line it reads. If Windows
   asks, allow Python on **private networks**.
2. In-game: press the screenshot hotkey on the line, press **Select**, then choose
   **Read screen (tap a word)**. The first time, the plugin looks for your PC
   automatically and remembers it.
   The 3DS shows "Reading the screen…" while it waits. It sends only the top screen (about
   190 KB), so most of the wait is your Wi-Fi plus the OCR time shown in the server's log.
3. The line appears split into words. Pick where your word starts. Conjugations are matched
   as a whole (食べられなかった → 食べる). Then save it as usual; the sentence is saved with it,
   and `mine.py` bolds the exact text you picked.

**Set each game's text box once (important).** OCR is reliable only when it sees just the
text box. Given the whole screen, manga-ocr guesses. After using Read screen once in a game, run

    py -3.12 tools\pick_crop.py

pick the game, drag a rectangle around its text box, type the game's name, and press **Save**.
Green bars show the lines that will be read; furigana and icons (red) are skipped
automatically. **Test** shows what the running server reads with that box. The server uses
the new box immediately, and `mine.py` uses it too.

**Text box that moves around?** Tick **Box moves around (find it by colour)** before saving.
The picker remembers the box's background colour, and the server then finds the largest
rectangle of that colour on every screenshot, wherever it is. If the box isn't found (for
example, a cut-scene with no box), it falls back to the area you drew. If automatic search doesn't
find the PC, enter the address under **OCR server** in the plugin menu. There you can also
turn OCR off, which makes Read screen go straight to typing.

**Cards while you play.** While `ocr_server.py` is running (and Anki is open), every word you
save in-game also becomes a Lapis card right away. The server builds it in the background with
the same definition, audio, pitch and picture as `mine.py`. The word is always written to the SD
card first, so if the PC, the server or Anki isn't available, nothing is lost: the "Saved"
message says so, and `mine.py` (via ftpd) adds those words later, skipping any that already
became cards. Turn this off under **OCR server** in the plugin menu, or start the server with
`--no-cards`.

If the PC is off, the server isn't running, or the game won't allow network access, Read
screen says why and offers the romaji lookup instead. All the other lookups never need the
network.

## 3c. No PC? Google Lens

When the PC can't be found but the 3DS is on the internet (home Wi-Fi, or your phone's
hotspot), *Read screen* sends the top screen to **Google Lens** instead:

- The 3DS connects to Google itself over HTTPS. Nothing has to run on your PC.
- Lens reads everything on screen, so if there are several blocks of text you pick the line
  first; furigana is dropped. Then pick where the word starts, as usual.
- After one failed attempt the PC isn't searched again for 2 minutes, so later lookups go
  straight to Lens.
- Saved words wait in `mined.tsv` with their sentence; run `mine.py` later to make the cards.
- Turn it off in *Select → OCR server → Turn off Google Lens*.

Things to know: this uses the unofficial endpoint Chrome's Lens feature talks to (the same one
[owocr](https://github.com/AuroraWright/owocr) uses). It's against Google's terms of service,
it can stop working whenever Google changes it, and your screenshots go to Google. If Lens
stops working, Read screen still offers manual lookup.

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
- **Read screen can't find the PC**: check `ocr_server.py` is running and that Windows Firewall
  allows Python on private networks. Then type the address it prints under **OCR server**. The
  3DS only uses 2.4 GHz Wi-Fi.
- **"networking needs Luma3DS v13.2 or newer"**: plugins can only use the network on Luma
  v13.2+. Update Luma (the version shows at the top of the Rosalina menu).
- **"network unavailable in this game"**: some titles don't permit network access from
  plugins. Use the romaji or kanji-parts lookup there.

## Developing

- `tests/`: native tests of the plugin core (romaji, conjugation, dictionary search) against
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
- Google Lens message layout from Chromium's Lens overlay protos (BSD), as used by
  [owocr](https://github.com/AuroraWright/owocr). HTTPS by [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (Apache-2.0);
  JPEG encoding by [stb_image_write](https://github.com/nothings/stb) (public domain).
