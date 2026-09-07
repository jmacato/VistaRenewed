# Accepted Glass captures

This directory contains unchanged 800×600 guest captures from two accepted boots.
The publication identifier is `ea971975b1d65c3fc7a59f52d66e552c38ac26d79746062cb82052569e17c261`.
The `SHA256SUMS` file records cryptographic hashes of the image bytes.
The original filenames are `glass-ea97-boot1-settled.png` and `glass-ea97-boot2.png`.
The local run identifier is `neptune-22gmrwll`.

From the repository root, repeat the measurements:

```sh
python3 scripts/verify_aero_glass_pixels.py docs/evidence/glass-boot1.png
python3 scripts/verify_aero_glass_pixels.py docs/evidence/glass-boot2.png
```

Both commands return `aero_glass_proven: true` and normalized edge sharpness `0.199`.
Edge sharpness measures abrupt pixel changes relative to stripe contrast.
These commands check image content.
They do not repeat the original installed-file or reboot-order checks.
The original audit checked local run records.
See [the full report](../../notes/AERO_GLASS_VERIFICATION.md).
