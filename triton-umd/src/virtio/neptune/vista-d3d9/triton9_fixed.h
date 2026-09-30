/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */
#ifndef TRITON9_FIXED_H
#define TRITON9_FIXED_H

#include <d3d9.h>
#include <array>
#include <vector>

/* Public D3D9 values keep this generator independent of the Windows DDI. The
 * driver and native renderer both call these exact production entry points. */
namespace Triton9Fixed {
constexpr unsigned StageCount = 8;
constexpr unsigned MatrixBase = 128;
constexpr unsigned MatrixStride = 7;
constexpr unsigned VertexConstantCount = MatrixBase + 256 * MatrixStride;
constexpr unsigned PixelConstantCount = 32;
using Constant = std::array<float, 4>;
struct Input {
    unsigned usage, index, reg, components;
};
struct Stage {
    unsigned colorOp = D3DTOP_DISABLE, alphaOp = D3DTOP_DISABLE;
    unsigned colorArg[3] = {D3DTA_CURRENT, D3DTA_TEXTURE, D3DTA_CURRENT};
    unsigned alphaArg[3] = {D3DTA_CURRENT, D3DTA_TEXTURE, D3DTA_CURRENT};
    unsigned result = D3DTA_CURRENT, texcoord = 0, transform = 0;
    unsigned constant = 0, textureType = D3DSTT_2D;
    unsigned bound = false;
    float bump[4] = {}, bumpScale = 0, bumpOffset = 0;
};
struct State {
    unsigned render[256] = {};
    Stage stages[StageCount];
    D3DMATRIX world[256] = {}, view = {}, projection = {}, texture[StageCount] = {};
    D3DMATERIAL9 material = {};
    std::vector<D3DLIGHT9> lights;
    std::vector<Input> inputs;
    bool diffuse = true, specular = true, transformed = false;
    bool normalizedBlendIndices = false;
    unsigned texcoords = 0xff;
    float viewportHeight = 1;
};
struct Program {
    bool fixedFunction = false;
    std::vector<unsigned> tokens;
    std::vector<Constant> constants;
};
bool equal(const State &a, const State &b);
HRESULT vertex(const State &state, Program &program);
HRESULT pixel(const State &state, Program &program);
void multiply(const D3DMATRIX &a, const D3DMATRIX &b, D3DMATRIX &out);
bool inverse(const D3DMATRIX &matrix, D3DMATRIX &out);
}
#endif
