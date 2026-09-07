/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Minimal Wine unixlib ABI types so nptunix.so builds standalone
 * without any Wine headers.  Also internal cross-file helpers shared
 * between npt_unix*.c source files.
 */
#ifndef NPT_UNIX_INTERNAL_H
#define NPT_UNIX_INTERNAL_H

#include <stddef.h>

typedef unsigned int NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *);
#define STATUS_SUCCESS 0
#define DECLSPEC_EXPORT __attribute__((visibility("default")))

struct npt_output_list;

/*
 * Two-pass DRM device walk: PCI virtio-gpu first, then any other KMS
 * device (dev-host / vtest fallback).  Returns 0 and writes the primary
 * and (if requested) render node paths of the chosen device, or
 * -ENOENT if no device satisfies either pass.  Warns once if more than
 * one PCI virtio-gpu is enumerated.
 */
int npt_pick_drm_device_paths(char primary[], size_t primary_sz,
                              char render[], size_t render_sz);

/*
 * Enumerate every DRM_MODE_CONNECTED connector on the picked device
 * and populate list->outputs[].  Returns true on at least one connector.
 */
bool npt_enumerate_outputs(struct npt_output_list *list, bool expose_all_modes);

#endif /* NPT_UNIX_INTERNAL_H */
