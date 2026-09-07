/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest-side access to the host backend's workaround flags. The host reports
 * the NPT_WA_* bits for the loaded backend once at ring create; the guest reads
 * them from the primary ring blob and gates the corresponding host-backend-
 * specific shader/cap patches on them.
 */
#ifndef NPT_WORKAROUND_H
#define NPT_WORKAROUND_H

#include <stdint.h>

/* Host backend workaround flag bits, guest side.  The host defines the same
 * wire values in its own npt_library.h; the two are matched by review, not by a
 * shared header -- this one deliberately includes nothing but <stdint.h>,
 * because the Triton translation units include the real Windows DXGI/D3D11
 * headers and pulling in the neptune protocol here would redefine DXGI_FORMAT
 * et al.  Each bit names a shader/cap patch the guest applies ONLY when the bit
 * is set; a backend that needs none leaves them all clear.  These compensate for
 * a specific host backend's defects, not general correctness. */
#define NPT_WA_FLAGS_PRESENT                     (1u << 31) /* host wrote the word */
#define NPT_WA_WIDEN_SCALAR_VS_INPUT_MASK        (1u << 0)  /* scalar .x IA VS input -> .xy */
#define NPT_WA_TYPE_VS_INPUT_FROM_VERTEX_FORMAT  (1u << 1)  /* VS ISGN comp type from bound format */
#define NPT_WA_LINEARIZE_NOPERSPECTIVE_PS_INPUT  (1u << 2)  /* dcl_input_ps noperspective -> linear */
#define NPT_WA_SYNTHESIZE_IO_SIGNATURE_FROM_SHDR (1u << 3)  /* empty GS ISGN/OSGN from SHDR DCLs */

/* Fixed byte offset within every ring blob's header (head@0, tail@64,
 * status@128, buffer@192) at which the host writes the workaround-flags word.
 * Lands in the free 132..192 gap, 4-aligned and disjoint from the other
 * host-owned words. */
#define NPT_RING_WORKAROUND_OFFSET 160u

/* The host workaround flags (NPT_WA_* bits, without NPT_WA_FLAGS_PRESENT) that
 * the host reported for this device's backend. Read once from the primary ring
 * blob and cached; returns 0 before the host has written the word or when no
 * device exists. Safe to call from any Triton shader/query path. */
uint32_t npt_host_workaround_flags(void);

#endif /* NPT_WORKAROUND_H */
