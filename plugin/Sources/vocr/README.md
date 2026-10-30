# VOCR runtime (vendored)

The on-console Japanese OCR from [bee-san/vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr),
`runtime/` at commit `205f47ec4e56ca87c00e7dba0a61c9aaec726707` (v0.5.0 and after): `vocr.c`, `vocr_lines.c`,
`vocr_lines_refine.c`, `vocr_lines_rows.c`, and the text spotter `vocr_textspot6.c` (headers in
`plugin/Includes/vocr/`, with `vocr_textspot.h` for the spotter's types). GPL-3.0-or-later, the same as Kotoba3DS.

Kotoba3DS changes (the 3DS's ARM11 has no NEON, so the upstream speed-ups for the Vita don't apply):
- `vocr.c`: ARMv6 SIMD kernels (SMLAD; `vocr_pack_simd32` reorders the weights at load), two positions per
  multiply pass (`vocr__k2x8`), VFP assembly for the depthwise and first-layer taps, the head's per-class
  compares by the floats' bits, ReLU without compares. The same text as upstream, bit for bit.
- `vocr_textspot6.c`: the spotter's 1x1 layers through `vocr__k2x8` (`vocr_textspot6_pack_simd32` reorders
  their weights at load) and its requantisation with SMLAL and SSAT. The same boxes as upstream, bit for bit.

The model files are not in this repository; see the README ("On-device OCR"). The recognizer weights
(`VOCR6.2_N_w8g2.vocr`, `VOCR5_*_w8.vocr`) and the spotter (`textspot8_t64n8c16_w8.vtsp`) are for
non-commercial use only (see their attribution files in the vita-vn-ocr release).
