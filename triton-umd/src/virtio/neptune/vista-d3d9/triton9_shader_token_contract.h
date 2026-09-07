/* SPDX-License-Identifier: MIT */

#ifndef TRITON9_SHADER_TOKEN_CONTRACT_H
#define TRITON9_SHADER_TOKEN_CONTRACT_H

#include <stdint.h>

/* DEF and DEFI use a register number. Float and integer constant buffers use
 * four scalars per register. Boolean constants use one scalar per register. */
static inline int
triton9_shader_constant_scalar_range(uint32_t register_index,
                                     uint32_t scalars_per_register,
                                     uint32_t *first_scalar,
                                     uint32_t *scalar_count)
{
    uint64_t first;

    if (!first_scalar || !scalar_count || !scalars_per_register)
        return 0;
    first = (uint64_t)register_index * scalars_per_register;
    if (first > UINT32_MAX)
        return 0;
    *first_scalar = (uint32_t)first;
    *scalar_count = scalars_per_register;
    return 1;
}

/* Shader-model 2 declaration-info tokens are parameter tokens.  Bit 31 is
 * mandatory even though the public D3D9 masks expose only the semantic or
 * sampler-type fields.  Vista validates this before calling the UMD. */
static inline uint32_t
triton9_sm2_dcl_semantic(uint32_t usage, uint32_t usage_index)
{
    return 0x80000000u | (usage & 0x0fu) | ((usage_index & 0x0fu) << 16);
}

static inline uint32_t
triton9_sm2_dcl_sampler(uint32_t texture_type_token)
{
    return 0x80000000u | texture_type_token;
}

/* A masked destination does not select a source component.  The caller must
 * encode that source swizzle explicitly; this produces the c0.x token used
 * by "mov oPos.w, c0.x" in the public SM2 probe. */
static inline uint32_t
triton9_sm2_replicate_x(uint32_t source_parameter_token)
{
    return source_parameter_token & ~0x00ff0000u;
}

#endif
