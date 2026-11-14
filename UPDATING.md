# Updating Kotoba3DS

This page is for people who already have Kotoba3DS on their SD card. If you're installing it for
the first time, follow the [README](README.md) instead. To see what's new, read the
[changelog](CHANGELOG.md).

## How to update

Download **Kotoba3DS-SD.zip** from the
[latest release](https://github.com/icebreaker2718/kotoba-3ds/releases/latest) and extract it to
the **root** of your SD card, replacing the files that are already there. You can do this with a
card reader on your computer, or on the 3DS itself with 3dsDL (see
[Install over Wi-Fi](README.md#install-over-wi-fi-with-3dsdl); choose **Overwrite** when it asks
about existing files).

This works from any earlier version. The zip has only the plugin, the dictionary and the OCR
models, so your saved words, screenshots, known words and settings are kept.

- **If you built your own `dict.bin`,** copy it somewhere safe first: the zip replaces it with
  the standard one.
- **If you installed the plugin for one game only** (`SD:/luma/plugins/<TitleID>/Kotoba3DS.3gx`),
  delete that copy: the zip puts the new plugin at `SD:/luma/plugins/default.3gx`.

## If something goes wrong

Send `SD:/3ds/kotoba3ds/ocr_log.txt` and, for a screen that was read wrong, its screenshot from
`SD:/3ds/kotoba3ds/shots/` (press **Start** on the lookup screen to keep it). See also
[Troubleshooting](README.md#troubleshooting).
