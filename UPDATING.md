# Updating Kotoba3DS

This page is for people who already have Kotoba3DS on their SD card. If you're installing it for
the first time, follow the [README](README.md) instead. To see what's new, read the
[changelog](CHANGELOG.md).

## Updating from 1.0.0 to 1.1.0

Download **Kotoba3DS.3gx** and **Kotoba3DS-ocr-model.zip** from the
[latest release](https://github.com/icebreaker2718/kotoba-3ds/releases/latest).

1. **The plugin:** copy `Kotoba3DS.3gx` over your old one: `SD:/luma/plugins/default.3gx`, or
   `SD:/luma/plugins/<TitleID>/Kotoba3DS.3gx` if you installed it for one game.
2. **The OCR models (new in this version):** unzip `Kotoba3DS-ocr-model.zip` to the **root** of
   the SD card. It adds two files to `SD:/3ds/kotoba3ds/ocr/`:
   - `VOCR6.2_N_w8g2.vocr`, the new OCR model;
   - `textspot8_t64n8c16_w8.vtsp`, the line spotter that finds text anywhere on the screen.

   Both are from bee-san's [vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr) and are for
   non-commercial use only. Your old `VOCR5_N_w8.vocr` can stay or go: the new model is used when
   both are there.
3. **Nothing else changes.** Keep your `dict.bin`, your saved words (`mined.tsv`), screenshots,
   known words and settings. The PC tools (`tools/`) are the same as in 1.0.0.

If you skip step 2, the plugin still works: it reads with the old model and finds text the way
1.0.0 did.

## After updating

- **New settings in the Select menu:** *Fast mode* and *Reading area*. The changelog explains
  both. Fast mode starts off, so the plugin finds text anywhere on the screen. If a game's
  dialogue box reads more slowly than before, try turning fast mode on.
- **`Kotoba3DS-2MiB-test.3gx`:** if you downloaded this test copy for a game that crashed at
  start, you don't need it anymore. The normal plugin now starts those games. You can delete it.
- **If something goes wrong:** send `SD:/3ds/kotoba3ds/ocr_log.txt` and, for a screen that was
  read wrong, its screenshot from `SD:/3ds/kotoba3ds/shots/` (press **Start** on the lookup
  screen to keep it). See also [Troubleshooting](README.md#troubleshooting).
