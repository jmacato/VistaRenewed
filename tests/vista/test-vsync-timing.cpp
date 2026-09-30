#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "../../triton-kmd/viogpu/viogpu3d/viogpu_vsync_timing.h"

static void checksum(unsigned char *e) {
    unsigned sum=0;for(unsigned i=0;i<127;i++)sum+=e[i];e[127]=(unsigned char)-sum;
}
static void edid(unsigned char *e,unsigned rate) {
    memset(e,0,128);memset(e+1,255,6);
    unsigned char *d=e+54;unsigned clock=1000*640*rate/10000;
    d[0]=clock;d[1]=clock>>8;
    d[2]=800&255;d[3]=200;d[4]=0x30;
    d[5]=600&255;d[6]=40;d[7]=0x20;checksum(e);
}
int main() {
    unsigned char e[128];
    assert(VioGpuPreferredRefreshRate(nullptr,0)==60);
    for(unsigned rate : {60u,144u,300u}) {
        edid(e,rate);assert(VioGpuPreferredRefreshRate(e,128)==rate);
        assert(VioGpuPreferredRefreshRate(e,127)==60);
    }
    e[30]^=1;assert(VioGpuPreferredRefreshRate(e,128)==60);
    edid(e,300);e[54+17]=0x80;checksum(e);assert(VioGpuPreferredRefreshRate(e,128)==60);
    edid(e,300);e[54]=e[55]=0;checksum(e);assert(VioGpuPreferredRefreshRate(e,128)==60);
    edid(e,300);e[0]=1;checksum(e);assert(VioGpuPreferredRefreshRate(e,128)==60);
    edid(e,10);assert(VioGpuPreferredRefreshRate(e,128)==60);
    assert(VioGpuNextVsyncDeadline(100,99,10)==100);
    assert(VioGpuNextVsyncDeadline(100,100,10)==110);
    assert(VioGpuNextVsyncDeadline(100,149,10)==150);
    assert(VioGpuNextVsyncDeadline(100,150,10)==160);
    for(unsigned rate : {60u,144u,300u}) {
        unsigned long long period=(10000000ull+rate/2)/rate;
        unsigned long long next=period;
        for(unsigned tick=1;tick<=10000;tick++) {
            // Half-ms clock rounding plus variable completion work.
            unsigned long long now=((next+4999)/5000)*5000+tick%7000;
            next=VioGpuNextVsyncDeadline(next,now,period);
            assert(next>now && next-now<=period);
            assert(next%period==0); // No cumulative rounding/processing drift.
        }
    }
    for (unsigned height : {1u, 480u, 720u, 1080u, 2160u}) {
        for (unsigned rate : {60u, 300u}) {
            unsigned long long period = (10000000ull + rate/2) / rate;
            bool blank;
            assert(VioGpuRasterLine(100, 100, period, height, &blank) == 0 && blank);
            unsigned last = 0, active = 0, blanks = 0;
            for (unsigned long long phase = 0; phase < period; ++phase) {
                unsigned line = VioGpuRasterLine(100 + phase, 100, period, height, &blank);
                assert(line < height);
                if (blank) { assert(line == 0 && active == 0); ++blanks; }
                else { assert(line >= last); last = line; ++active; }
                bool wrappedBlank;
                assert(line == VioGpuRasterLine(100 + phase + period * 100000,
                                                100, period, height, &wrappedBlank));
                assert(blank == wrappedBlank);
            }
            assert(active && blanks && last == height - 1);
        }
    }
    puts("PASS EDID refresh validation; fractional vblank cadence; missed-tick skip without bursts; coherent raster phase and wrap");
}
