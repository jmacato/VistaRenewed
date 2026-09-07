/* SPDX-License-Identifier: MIT */

#include "../triton9_shader_token_contract.h"

#include <stdio.h>

#define CHECK(expr) do {                                                     \
    if (!(expr)) {                                                           \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr);             \
        return 1;                                                            \
    }                                                                        \
} while (0)

int
main(void)
{
    uint32_t first_scalar = 0;
    uint32_t scalar_count = 0;

    /* Exact fxc/Vista-era tokens used by the VirtualBox D3D9 tests. */
    CHECK(triton9_sm2_dcl_semantic(0, 0) == 0x80000000u); /* position */
    CHECK(triton9_sm2_dcl_semantic(10, 0) == 0x8000000au); /* color */
    CHECK(triton9_sm2_dcl_semantic(5, 1) == 0x80010005u); /* texcoord1 */
    CHECK(triton9_sm2_dcl_sampler(2u << 27) == 0x90000000u); /* 2D */
    CHECK(triton9_sm2_replicate_x(0xa0e40000u) == 0xa0000000u); /* c0.x */
    CHECK(triton9_shader_constant_scalar_range(2, 4, &first_scalar,
                                               &scalar_count));
    CHECK(first_scalar == 8 && scalar_count == 4); /* DEF c2 */
    CHECK(triton9_shader_constant_scalar_range(7, 1, &first_scalar,
                                               &scalar_count));
    CHECK(first_scalar == 7 && scalar_count == 1); /* DEFB b7 */
    CHECK(!triton9_shader_constant_scalar_range(UINT32_MAX, 4,
                                                &first_scalar, &scalar_count));

    puts("triton9 shader token contract: PASS");
    return 0;
}
