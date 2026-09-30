#pragma once

/* The virtual raster reserves a short blanking interval, described by the
 * same total-line count in its VidPN target and monitor signal tuples. */
static inline unsigned VioGpuBlankingLines(unsigned height)
{
    return height / 20 ? height / 20 : 1;
}

static inline unsigned VioGpuRasterLine(unsigned long long now,
    unsigned long long epoch, unsigned long long period, unsigned height,
    bool *inBlank)
{
    *inBlank = true;
    if (!height || !period) return 0;
    unsigned long long phase = now >= epoch ? (now - epoch) % period : 0;
    unsigned long long total = (unsigned long long)height + VioGpuBlankingLines(height);
    unsigned long long line = phase * total / period;
    if (line < VioGpuBlankingLines(height)) return 0;
    *inBlank = false;
    return (unsigned)(line - VioGpuBlankingLines(height));
}

/* Pure timing arithmetic, shared with the host-side regression tests. */
static inline unsigned long VioGpuPreferredRefreshRate(const unsigned char *edid,
                                                       unsigned length)
{
    static const unsigned char signature[8] = {0,255,255,255,255,255,255,0};
    if (!edid || length < 128) return 60;
    unsigned checksum = 0;
    for (unsigned i = 0; i < 128; ++i) checksum += edid[i];
    if (checksum & 255) return 60;
    for (unsigned i = 0; i < 8; ++i)
        if (edid[i] != signature[i]) return 60;
    const unsigned char *d = edid + 54;
    unsigned clock = (d[0] | (unsigned(d[1]) << 8)) * 10000;
    unsigned width = d[2] | ((unsigned(d[4]) & 0xf0) << 4);
    unsigned height = d[5] | ((unsigned(d[7]) & 0xf0) << 4);
    unsigned htotal = width + (d[3] | ((unsigned(d[4]) & 0x0f) << 8));
    unsigned vtotal = height + (d[6] | ((unsigned(d[7]) & 0x0f) << 8));
    if (!clock || !width || !height || (d[17] & 0x80)) return 60;
    unsigned pixels = htotal * vtotal;
    unsigned rate = (clock + pixels / 2) / pixels;
    return rate >= 24 && rate <= 1000 ? rate : 60;
}

/* Preserve the original phase; skip missed ticks instead of emitting a burst.
 * Source-ready wakes never call this and therefore cannot postpone vblank. */
static inline unsigned long long VioGpuNextVsyncDeadline(
    unsigned long long deadline, unsigned long long now,
    unsigned long long period)
{
    if (deadline <= now)
        deadline += ((now - deadline) / period + 1) * period;
    return deadline;
}
