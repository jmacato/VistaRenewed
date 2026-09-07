/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef NEPTUNE_HW_H
#define NEPTUNE_HW_H

#include <stdint.h>

struct virgl_renderer_capset_neptune {
   uint32_t wire_format_version;
   uint32_t pad[14]; /* reserved for future use */
};

#endif /* NEPTUNE_HW_H */
