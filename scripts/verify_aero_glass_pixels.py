#!/usr/bin/env python3
"""Prove or reject Triton's controlled Vista glass/blur image from a PNG.

This reads a passive QMP PNG only.  It performs no guest input, VM control,
or image output.  The guest probe paints alternating dark/bright 40-pixel
stripes below a transparent-black DWM blur plate. A pass requires three things:

* the alternating backdrop signal is visible through the plate;
* its edge contrast is attenuated relative to its own stripe contrast; and
* its stripe edges are smoother than the uncovered backdrop.

An opaque Aero Basic plate fails the first condition.  A transparent but
unblurred surface fails the edge checks.  API return values and window
titles are deliberately ignored.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass

from PIL import Image, ImageDraw, ImageFilter


BACKDROP_X = 80
BACKDROP_Y = 100
BACKDROP_WIDTH = 640
BACKDROP_HEIGHT = 360
STRIPE_WIDTH = 40
GLASS_X = 200
GLASS_Y = 190
GLASS_WIDTH = 320
GLASS_HEIGHT = 180
SAMPLE_Y_OFFSETS = (65, 95, 125)
SAMPLE_RADIUS = 4


@dataclass(frozen=True)
class PixelMetrics:
    source_contrast: float
    plate_contrast: float
    source_edge_delta: float
    plate_edge_delta: float
    plate_edge_sharpness: float
    bright_count: int
    dark_count: int
    pass_signal: bool
    pass_attenuation: bool
    pass_blur: bool

    @property
    def passed(self) -> bool:
        return self.pass_signal and self.pass_attenuation and self.pass_blur


def luminance(rgb: tuple[float, float, float]) -> float:
    red, green, blue = rgb
    return red * 0.2126 + green * 0.7152 + blue * 0.0722


def mean_rgb(image: Image.Image, x: int, y: int, radius: int = SAMPLE_RADIUS) -> tuple[float, float, float]:
    if x - radius < 0 or y - radius < 0 or x + radius >= image.width or y + radius >= image.height:
        raise ValueError(f"sample ({x}, {y}) is outside the {image.width}x{image.height} PNG")
    crop = image.crop((x - radius, y - radius, x + radius + 1, y + radius + 1)).convert("RGB")
    pixels = list(crop.getdata())
    count = len(pixels)
    return tuple(sum(pixel[channel] for pixel in pixels) / count for channel in range(3))


def stripe_is_bright(x: int) -> bool:
    return ((x - BACKDROP_X) // STRIPE_WIDTH) & 1 == 1


def stripe_centers() -> list[int]:
    first = ((GLASS_X - BACKDROP_X) // STRIPE_WIDTH) * STRIPE_WIDTH + BACKDROP_X
    if first < GLASS_X:
        first += STRIPE_WIDTH
    first += STRIPE_WIDTH // 2
    return [x for x in range(first, GLASS_X + GLASS_WIDTH - SAMPLE_RADIUS, STRIPE_WIDTH)]


def stripe_edges() -> list[int]:
    first = ((GLASS_X - BACKDROP_X) // STRIPE_WIDTH + 1) * STRIPE_WIDTH + BACKDROP_X
    return [x for x in range(first, GLASS_X + GLASS_WIDTH - SAMPLE_RADIUS, STRIPE_WIDTH)]


def average(values: list[float]) -> float:
    if not values:
        raise ValueError("no samples")
    return sum(values) / len(values)


def evaluate(image: Image.Image) -> PixelMetrics:
    if image.width < GLASS_X + GLASS_WIDTH or image.height < GLASS_Y + GLASS_HEIGHT:
        raise ValueError("PNG does not contain the complete controlled glass proof layout")
    image = image.convert("RGB")

    bright_plate: list[float] = []
    dark_plate: list[float] = []
    bright_source: list[float] = []
    dark_source: list[float] = []
    source_edges: list[float] = []
    plate_edges: list[float] = []
    plate_sharpness: list[float] = []
    source_y = GLASS_Y - 24

    for x in stripe_centers():
        for y_offset in SAMPLE_Y_OFFSETS:
            plate_value = luminance(mean_rgb(image, x, GLASS_Y + y_offset))
            source_value = luminance(mean_rgb(image, x, source_y))
            if stripe_is_bright(x):
                bright_plate.append(plate_value)
                bright_source.append(source_value)
            else:
                dark_plate.append(plate_value)
                dark_source.append(source_value)

    # A 5-pixel pair straddles each stripe edge.  The raw backdrop changes
    # sharply; true DWM blur makes the corresponding plate pair smoother.
    for edge in stripe_edges():
        left_source = luminance(mean_rgb(image, edge - 5, source_y))
        right_source = luminance(mean_rgb(image, edge + 5, source_y))
        source_edges.append(abs(right_source - left_source))
        for y_offset in SAMPLE_Y_OFFSETS:
            left_plate = luminance(mean_rgb(image, edge - 5, GLASS_Y + y_offset))
            right_plate = luminance(mean_rgb(image, edge + 5, GLASS_Y + y_offset))
            plate_edges.append(abs(right_plate - left_plate))
            # Search around the expected edge, without horizontal averaging.
            # Normalize to this edge's own contrast so tint or a displaced
            # sharp edge cannot masquerade as blur. Bilinear resampling alone
            # still concentrates at least half the transition in one pixel.
            row = [luminance(image.getpixel(
                (x, GLASS_Y + y_offset))) for x in range(edge - 12, edge + 13)]
            span = max(row) - min(row)
            plate_sharpness.append(
                max(abs(b - a) for a, b in zip(row, row[1:])) / span
                if span >= 8.0 else 1.0)

    source_contrast = average(bright_source) - average(dark_source)
    plate_contrast = average(bright_plate) - average(dark_plate)
    source_edge_delta = average(source_edges)
    plate_edge_delta = average(plate_edges)

    # The thresholds are deliberately conservative.  The pattern's raw
    # luma separation is about 165.  Vista glass can tint and dim it heavily,
    # but it still preserves a broad-stripe signal; Basic cannot invent the
    # alternating correlation in the untouched client area.
    pass_signal = (
        len(bright_plate) >= 9
        and len(dark_plate) >= 9
        and source_contrast >= 100.0
        and plate_contrast >= 18.0
        and plate_contrast >= source_contrast * 0.10
    )
    # Wide stripe interiors can retain their contrast under genuine blur:
    # only their edges mix. Comparing plateau contrast to raw backdrop was
    # therefore a tint-strength test, not a blur test. Normalize edge contrast
    # by each surface's own plateau contrast to cancel uniform tint/gain.
    pass_attenuation = (
        source_contrast > 0 and plate_contrast > 0
        and plate_edge_delta / plate_contrast
            <= 0.90 * source_edge_delta / source_contrast
    )
    # A displaced sharp edge can fool the fixed edge pair above. Retain the
    # independent search for concentrated one-pixel transitions, normalized
    # locally so tint cannot disguise a sharp or bilinearly shifted edge.
    edge_sharpness = average(plate_sharpness)
    pass_blur = source_edge_delta >= 70.0 and edge_sharpness <= 0.35
    return PixelMetrics(
        source_contrast=source_contrast,
        plate_contrast=plate_contrast,
        source_edge_delta=source_edge_delta,
        plate_edge_delta=plate_edge_delta,
        plate_edge_sharpness=edge_sharpness,
        bright_count=len(bright_plate),
        dark_count=len(dark_plate),
        pass_signal=pass_signal,
        pass_attenuation=pass_attenuation,
        pass_blur=pass_blur,
    )


def as_dict(metrics: PixelMetrics, image: Image.Image) -> dict[str, object]:
    return {
        "png_size": [image.width, image.height],
        "source_contrast": round(metrics.source_contrast, 2),
        "plate_contrast": round(metrics.plate_contrast, 2),
        "source_edge_delta": round(metrics.source_edge_delta, 2),
        "plate_edge_delta": round(metrics.plate_edge_delta, 2),
        "plate_edge_sharpness": round(metrics.plate_edge_sharpness, 3),
        "bright_samples": metrics.bright_count,
        "dark_samples": metrics.dark_count,
        "backdrop_signal": metrics.pass_signal,
        "edge_contrast_attenuated": metrics.pass_attenuation,
        "edges_blurred": metrics.pass_blur,
        "aero_glass_proven": metrics.passed,
    }


def self_test() -> int:
    base = Image.new("RGB", (800, 600), (0, 0, 0))
    draw = ImageDraw.Draw(base)
    for index, x in enumerate(range(BACKDROP_X, BACKDROP_X + BACKDROP_WIDTH, STRIPE_WIDTH)):
        draw.rectangle((x, BACKDROP_Y, x + STRIPE_WIDTH - 1,
                        BACKDROP_Y + BACKDROP_HEIGHT - 1),
                       fill=(245, 215, 30) if index & 1 else (20, 35, 235))
    glass = base.filter(ImageFilter.GaussianBlur(radius=7))
    tint = Image.new("RGB", glass.size, (55, 90, 130))
    glass = Image.blend(glass, tint, 0.32)
    proof = base.copy()
    proof.paste(glass.crop((GLASS_X, GLASS_Y, GLASS_X + GLASS_WIDTH,
                            GLASS_Y + GLASS_HEIGHT)), (GLASS_X, GLASS_Y))
    if not evaluate(proof).passed:
        print("self-test glass model unexpectedly failed", file=sys.stderr)
        return 1
    # Blur should remain detectable at different tint strengths, including
    # untinted blur: colorization strength is not part of the requested proof.
    for radius in (3, 5, 7):
        for opacity in (0.0, 0.12, 0.32, 0.60):
            candidate = base.copy()
            filtered = Image.blend(base.filter(ImageFilter.GaussianBlur(radius)),
                                   tint, opacity)
            candidate.paste(filtered.crop((GLASS_X, GLASS_Y,
                GLASS_X + GLASS_WIDTH, GLASS_Y + GLASS_HEIGHT)),
                (GLASS_X, GLASS_Y))
            if not evaluate(candidate).passed:
                print(f"self-test blur radius={radius} tint={opacity} failed",
                      file=sys.stderr)
                return 1
    basic = base.copy()
    ImageDraw.Draw(basic).rectangle(
        (GLASS_X, GLASS_Y, GLASS_X + GLASS_WIDTH - 1,
         GLASS_Y + GLASS_HEIGHT - 1), fill=(72, 72, 72))
    if evaluate(basic).passed:
        print("self-test opaque Basic model unexpectedly passed", file=sys.stderr)
        return 1
    transparent = base.copy()
    if evaluate(transparent).passed:
        print("self-test unblurred transparent model unexpectedly passed", file=sys.stderr)
        return 1
    for displacement in (0, 4):
        tinted = Image.blend(base, tint, 0.45)
        unblurred = base.copy()
        unblurred.paste(tinted.crop(
            (GLASS_X - displacement, GLASS_Y,
             GLASS_X + GLASS_WIDTH - displacement, GLASS_Y + GLASS_HEIGHT)),
            (GLASS_X, GLASS_Y))
        if evaluate(unblurred).passed:
            print("self-test tinted/displaced sharp edge unexpectedly passed", file=sys.stderr)
            return 1
    decoy = basic.copy()
    decoy_draw = ImageDraw.Draw(decoy)
    # Put stripe-like reflections, a bright border, and a blurred patch away
    # from the fixed interior samples. None of these is backdrop transmission.
    decoy_draw.rectangle(
        (GLASS_X - 8, GLASS_Y - 8, GLASS_X + GLASS_WIDTH + 7,
         GLASS_Y + GLASS_HEIGHT + 7),
        outline=(235, 235, 255),
        width=5,
    )
    for x in range(GLASS_X, GLASS_X + GLASS_WIDTH, 20):
        decoy_draw.rectangle(
            (x, GLASS_Y + 2, x + 9, GLASS_Y + 15),
            fill=(210, 220, 245),
        )
    decoy.paste(
        glass.crop((BACKDROP_X, BACKDROP_Y, BACKDROP_X + 100,
                    BACKDROP_Y + 60)),
        (BACKDROP_X + BACKDROP_WIDTH - 100,
         BACKDROP_Y + BACKDROP_HEIGHT - 60),
    )
    if evaluate(decoy).passed:
        print("self-test border/reflection decoy unexpectedly passed", file=sys.stderr)
        return 1
    try:
        evaluate(Image.new("RGB", (400, 300), (0, 0, 0)))
    except ValueError:
        pass
    else:
        print("self-test undersized PNG unexpectedly passed", file=sys.stderr)
        return 1
    print(
        "self-test passed: 12 blur/tint models accepted; Basic, transparent, tinted/displaced, "
        "decoy, and undersized models rejected"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("png", nargs="?", help="passive QMP PNG to verify")
    parser.add_argument("--self-test", action="store_true", help="test the decision gate in memory")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    if not arguments.png:
        parser.error("PNG path required unless --self-test is used")
    try:
        with Image.open(arguments.png) as image:
            result = as_dict(evaluate(image.convert("RGB")), image)
    except (OSError, ValueError) as error:
        print(f"Aero glass verification failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0 if result["aero_glass_proven"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
