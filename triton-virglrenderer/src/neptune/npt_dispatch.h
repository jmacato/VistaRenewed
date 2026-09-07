/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Transport (group 0) dispatcher entry point.
 */

#ifndef NPT_DISPATCH_H
#define NPT_DISPATCH_H

#include "npt_common.h"
#include "npt_transport_defs.h"

struct npt_cs_decoder;
struct npt_cs_encoder;
struct npt_context;
struct npt_dispatch_context;

/* Caller has already consumed the header and passes it via `header`.
 * Returns false on unknown method (decoder left fatal).  `dispatch`
 * is consumed only by EXECUTE_COMMAND_STREAM. */
bool
npt_transport_dispatch(struct npt_context *ctx,
                       struct npt_dispatch_context *dispatch,
                       struct npt_cs_decoder *dec,
                       struct npt_cs_encoder *enc,
                       const struct npt_command_header *header);

#endif /* NPT_DISPATCH_H */
