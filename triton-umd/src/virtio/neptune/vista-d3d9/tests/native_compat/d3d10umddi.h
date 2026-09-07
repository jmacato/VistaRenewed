/*
 * ShaderConverter uses the UMD header only for the tokenized-program format
 * and this resource-kind enum.  Pulling the complete WDK DDI into a native
 * macOS test would also pull in the Windows kernel ABI.
 */
#ifndef TRITON9_TEST_D3D10UMDDI_H
#define TRITON9_TEST_D3D10UMDDI_H

#include <d3d12TokenizedProgramFormat.hpp>

typedef enum D3D10DDIRESOURCE_TYPE {
    D3D10DDIRESOURCE_BUFFER = 1,
    D3D10DDIRESOURCE_TEXTURE1D = 2,
    D3D10DDIRESOURCE_TEXTURE2D = 3,
    D3D10DDIRESOURCE_TEXTURE3D = 4,
    D3D10DDIRESOURCE_TEXTURECUBE = 5,
} D3D10DDIRESOURCE_TYPE;

#endif
