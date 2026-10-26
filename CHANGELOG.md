# Changelog

What changed in each release of Kotoba3DS, newest first. Download the latest from the
[Releases page](https://github.com/icebreaker2718/kotoba-3ds/releases/latest); the version shows
in Luma's plugin loader.

## 1.1.0 (2026-10-26)

**More games start with the plugin**
- Games that crashed (or showed an error) at start with Kotoba3DS loaded should now start
  normally. The plugin uses 5 MB of the game's memory, and some games need every byte of it;
  the plugin now gives that memory back in a way that works whatever the game is, instead of
  only for games it recognized. Tested with 逆転裁判123, 大逆転裁判2, Corpse Party BR and
  Mario & Luigi: Dream Team.

**Better reading**
- Speech bubbles that fade from one colour to another (Pokémon Sun/Moon) are read whole.
  Before, only their first row was read.

If a game still crashes at start, see [Troubleshooting](README.md#troubleshooting).

## 1.0.0

The first release: on-device OCR of the top screen, the touchscreen or both; tap-to-look-up
with the full Jitendex entry; saving words with the sentence and a screenshot; known words in
green; and Anki (Lapis) cards made on the PC by `tools/ocr_server.py` or `tools/mine.py`. See
the [README](README.md) for the full feature list.
