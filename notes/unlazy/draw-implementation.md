# Draw and shader implementation

Date: 2026-08-26

## Result

Leaf 1.2.2.2 is complete for local source, build, conversion, raster, and readback evidence. The Vista guest validation remains open.

The implementation changes Triton's normal draw path. It does not inspect the public probe or return a fabricated pixel.

No VM ran during this leaf. This report makes no PresentEx, public guest, Aero, or glass claim.

## Correct draw contract

`triton9_draw_contract.c:8-62` calculates source and target element counts for every D3D9 primitive type. It rejects all arithmetic overflow.

Triangle fans now expand into triangle-list indices. The order `(0, i + 1, i + 2)` preserves the fan winding.

All four draw callbacks use this expansion. Nonindexed draws retain their first vertex through the D3D11 base-vertex value.

Indexed draws retain the signed base vertex. `DrawIndexedPrimitive2` retains its byte offset and shifts its vertex window by `MinIndex`.

`triton9_draw_contract.c:167-198` compares each actual index with the declared range. It supports 16-bit and 32-bit streams.

The normal indexed path reads the authoritative CPU shadow after pending lock synchronization. The UP paths read their copied runtime data.

Every draw entry records its raw primitive type and count before validation. Indexed entries also record base, minimum, range, and start values.

Successful indexed and nonindexed calls reach the same pipeline preparation. They bind topology, streams, indices, constants, shaders, viewport, scissor, and raster state.

Optional edge-flag buffers still return `D3DDDIERR_NOTAVAILABLE`. This behavior does not change the null-flag triangle path or advertise edge-flag support.

## Shader and constant fixes

ShaderConverter reports inline `DEF` and `DEFI` locations as register numbers. The old code treated those values as scalar offsets.

`triton9_shader.cpp:1240-1318` now multiplies float and integer register numbers by four. Boolean registers keep one scalar each.

The host test puts the position constant in `c2`. Its successful red triangle proves that nonzero inline registers reach the correct buffer bytes.

The fixed pixel shader previously assigned specular and texture zero to the same input register. Texture inputs now use registers two and three.

Fog uses input register four. This layout keeps diffuse, specular, two texture coordinates, and fog distinct.

## Vertex fog

The fixed vertex converter now implements LINEAR, EXP, and EXP2 vertex fog. It uses the absolute camera-space Z value.

The vertex shader writes a saturated `oFog` factor. The pixel shader blends only RGB with the preserved fog color.

The code uploads world-view, world-view-projection, start, end, inverse distance, density, and fog color values before each draw.

The fixed vertex cache key includes fog enable and vertex mode. A mode change therefore creates the correct shader variant.

The programmable pixel path passes fog enable and extension constants to ShaderConverter. ShaderConverter applies this state to applicable pixel shader versions.

Transformed fixed-function vertices use the specular alpha fog factor. This matches the D3D9 transformed-vertex convention.

The sibling state owner preserves these legal states under `shaderLock`: fog enable, mode, color, start, end, density, and dither enable.

Range fog remains unsupported and unadvertised. Table fog remains `D3DFOG_NONE`, as required by the sibling contract.

The native results prove all three fog equations at camera-space distance `0.5`:

```text
LINEAR center=0xff7f007f
EXP    center=0xff9b0064
EXP2   center=0xffc70038
```

Each value is a real red and blue blend. The outside pixel remains the clear color `0xff050a14`.

## Dither assessment

D3D11 has no rasterizer or blend descriptor field for dithering. The local DXMT source also contains no dither execution path.

Triton advertises only A8R8G8B8 and X8R8G8B8 color render targets. It does not expose a lower-precision color target.

DXVK preserves `D3DRS_DITHERENABLE` in state blocks but has no execution use for it. This agrees with the local backend behavior.

The sibling state code therefore accepts and preserves zero or one. No backend dirty bit is needed for the advertised 32-bit target set.

This assessment is conditional on eight-bit-target inertness. A requirement for sub-LSB noise would need a later output-shader implementation.

This leaf does not claim that such a backend implementation exists.

## Native raster and readback evidence

The host test executes Microsoft's ShaderConverter, wraps its output in DXBC, and creates real DXMT D3D11 shader objects.

It draws into a 128 by 128 BGRA8 target. It copies that target into staging memory and reads pixels with `RowPitch`.

The normal fixed and public pairs produce center `0xffff0000`. Their outside pixel is the clear value `0xff050a14`.

The test changes front-face selection. Exactly one winding state draws the triangle.

The test moves one vertex beyond clip space. The clipped triangle still produces the expected center pixel.

The test enables a narrow scissor and then restores a full scissor. The center changes from clear to the draw color.

The test changes triangle-list to triangle-strip topology. It also moves and restores the viewport.

Each case clears, draws, copies, flushes, maps, checks, and unmaps. This sequence tests state changes and ordered readback.

## Exact ownership

`PLAN.md:29` assigns these seven source files only to leaf 1.2.2.2:

- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_shader.cpp`
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_draw_contract.c`
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_draw_contract.h`
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_shader_token_contract.h`
- `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_draw_contract_test.c`
- `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_shader_token_contract_test.c`
- `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_shaderconv_test.cpp`

This leaf changed no sibling source. It changed only its report and gate outside that list.

The public probe, package files, service files, VM state, disk state, and framebuffer state did not change.

## Verification

The direct portable tests used strict warnings:

```text
cc -std=c11 -Wall -Wextra -Werror \
  triton-umd/src/virtio/neptune/vista-d3d9/triton9_draw_contract.c \
  triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_draw_contract_test.c \
  -o /tmp/triton9-draw-contract
/tmp/triton9-draw-contract
triton9 draw contract: PASS

cc -std=c11 -Wall -Wextra -Werror \
  triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_shader_token_contract_test.c \
  -o /tmp/triton9-shader-token-contract
/tmp/triton9-shader-token-contract
triton9 shader token contract: PASS
```

Both Vista UMD targets compiled and linked:

```text
PYTHONPATH=/opt/homebrew/lib/python3.13/site-packages ninja -C triton-umd/build-vista-x64-unified -j1 src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
[2/2] Linking target src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
exit 0

PYTHONPATH=/opt/homebrew/lib/python3.13/site-packages ninja -C triton-umd/build-vista-x86-unified -j1 src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
[2/2] Linking target src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
exit 0
```

Only the existing ShaderConverter header warnings and a C-only compiler-option warning appeared. No owned source warning appeared.

Both complete Meson suites passed:

```text
meson test -C triton-umd/build-vista-x64-unified --print-errorlogs
Ok: 4  Fail: 0  Skipped: 1

meson test -C triton-umd/build-vista-x86-unified --print-errorlogs
Ok: 4  Fail: 0  Skipped: 1
```

The skipped item is zlib's example. Draw, token, CPU-layout, and clear tests all passed.

The native test linked these local components:

- All ten ShaderConverter and ShaderBinary translation units
- `tritonDxbc.c`
- `triton9_shaderconv_test.cpp`
- `triton-dxmt/build-native/src/dxmt-native/libdxmt-native.dylib`

The converter sources used native DirectX headers and the WDK token format header. Compatibility definitions supplied unavailable SAL and Win32 helper macros.

The final native commands were:

```text
conv_root=triton-umd/src/virtio/neptune/vista-d3d9/third_party/d3d9on12-shaderconverter
conv_flags=(
  -std=c++20 -O1 -Wno-switch -Wno-unused-parameter -Wno-unused-variable
  -Wno-missing-field-initializers -Wno-nontrivial-memcall
  -Wno-nonportable-include-path -Wno-deprecated-enum-compare
  -Wno-tautological-constant-out-of-range-compare
  -I triton-dxmt/include/native/windows -I triton-dxmt/include/native/directx
  -I "$conv_root/Inc" -I "$conv_root/ShaderConv" -I "$conv_root/ShaderBinary"
  -isystem driver/sdk/microsoft.windows.wdk.x64/c/Include/10.0.28000.0/um
  -isystem driver/sdk/microsoft.windows.sdk.cpp/c/Include/10.0.28000.0/shared
  -include triton-dxmt/include/native/windows/windows.h
  -include driver/sdk/microsoft.windows.wdk.x64/c/Include/10.0.28000.0/um/d3d12TokenizedProgramFormat.hpp
  -D_D3D10UMDDI_H -D_STRSAFE_H_INCLUDED_ -DStringCchPrintfA=snprintf
  -D_LIBCPP___CSTDDEF_BYTE_H -Dbyte=uint8_t
  '-DC_ASSERT(x)=static_assert(x)'
  '-D_countof(x)=(sizeof(x)/sizeof((x)[0]))'
  '-DARRAYSIZE(x)=(sizeof(x)/sizeof((x)[0]))'
  '-D__int64=long long' -D_In_= -D_Inout_= -D_Out_=
  '-D__analysis_assume(x)=((void)0)' -DDebugBreak=__builtin_trap
  '-D__out_ecount(x)=' -D__nullterminated= '-D__in_range(a,b)='
  '-D__success(x)=' -D__checkReturn= '-D_Field_range_(a,b)='
  '-D__fallthrough=[[fallthrough]]' -DD3D10DDIRESOURCE_TYPE=int
  -DD3D10DDIRESOURCE_TEXTURE2D=1 -DD3D10DDIRESOURCE_TEXTURE3D=2
  -DD3D10DDIRESOURCE_TEXTURECUBE=3
  '-DInterlockedIncrement(p)=__sync_add_and_fetch((p),1)'
  '-DInterlockedDecrement(p)=__sync_sub_and_fetch((p),1)'
)
for source_file in "$conv_root"/ShaderConv/*.cpp "$conv_root"/ShaderBinary/*.cpp; do
  object_name=$(basename "$source_file" .cpp)
  clang++ "${conv_flags[@]}" -c "$source_file" -o "/tmp/triton9-${object_name}.o"
done

clang -std=c11 -O1 \
  -I triton-dxmt/include/native/windows -I triton-dxmt/include/native/directx \
  -I triton-umd/src/virtio/neptune/triton -I triton-umd/src/virtio/neptune \
  -include triton-dxmt/include/native/windows/windows.h \
  '-DGetProcessHeap()=0' '-DHeapAlloc(heap,flags,size)=calloc(1,(size))' \
  '-DHeapFree(heap,flags,pointer)=(free((pointer)),1)' \
  '-DOutputDebugStringA(message)=((void)0)' \
  '-D_snprintf_s(buffer,size,truncate,...)=snprintf((buffer),(size),__VA_ARGS__)' \
  -D_TRUNCATE=0 -include stdlib.h -include stdio.h \
  -c triton-umd/src/virtio/neptune/triton/tritonDxbc.c \
  -o /tmp/triton9-tritonDxbc.o

clang++ "${conv_flags[@]}" -I triton-umd/src/virtio/neptune/triton \
  '-DGetProcessHeap()=0' \
  '-DHeapFree(heap,flags,pointer)=(free((pointer)),1)' -include stdlib.h \
  triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_shaderconv_test.cpp \
  /tmp/triton9-ShaderBinary.o /tmp/triton9-ShaderBinaryEx.o \
  /tmp/triton9-ShaderConv.o /tmp/triton9-context.o \
  /tmp/triton9-disasm20.o /tmp/triton9-gsconv.o /tmp/triton9-psconv.o \
  /tmp/triton9-tlvsconv.o /tmp/triton9-translator.o /tmp/triton9-vsconv.o \
  /tmp/triton9-tritonDxbc.o -L triton-dxmt/build-native/src/dxmt-native \
  -ldxmt-native \
  -Wl,-rpath,/Users/jumar/winvistachecked/triton-dxmt/build-native/src/dxmt-native \
  -o /tmp/triton9-shaderconv-test
/tmp/triton9-shaderconv-test
triton9 shader conversion/render: PASS
```

The compile emitted one native header compatibility warning. The executable returned zero.

## Defect hunt and remaining validation

The defect hunt found the inline-register offset, triangle-fan rejection, fixed input collision, and missing fixed vertex fog.

It also found missing actual-index validation and missing transformed specular fog. The implementation and focused tests now cover these cases.

The final polish pass replaced a reused source-count variable with a separate draw start. It also added final indexed HRESULT records.

No remaining owned-file defect affects the triangle acceptance path.

The installed guest UMDs previously differed from current builds. Old guest results therefore cannot prove this implementation.

The guest-owned deployment must install the current package. The public probe must then record the expected triangle center and outside values.

That guest result is the only open clause in G4. A later root leaf owns deployment and guest verification.
