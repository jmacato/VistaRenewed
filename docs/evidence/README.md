# Accepted Glass captures

These are unmodified 800×600 guest captures from the two accepted boots of
publication `ea971975b1d65c3fc7a59f52d66e552c38ac26d79746062cb82052569e17c261`.
`SHA256SUMS` records the image bytes. Their original filenames were
`glass-ea97-boot1-settled.png` and `glass-ea97-boot2.png` in local run
`neptune-22gmrwll`.

From the repository root, remeasure them with:

```sh
python3 scripts/verify_aero_glass_pixels.py docs/evidence/glass-boot1.png
python3 scripts/verify_aero_glass_pixels.py docs/evidence/glass-boot2.png
```

Both return `aero_glass_proven: true` and normalized edge sharpness `0.199`.
This rechecks the image content, not the original deployment's installed hashes
or reboot ordering. Those were checked against local run records as described
in [the full report](../../notes/AERO_GLASS_VERIFICATION.md).
