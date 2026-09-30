# D3D9On12 ShaderConverter source

This directory contains the `ShaderConverter` directory from Microsoft
D3D9On12 at commit `0e4a97fb9cb8b3f8f46b5769219915c3ea098bff`.

Source: https://github.com/microsoft/D3D9On12

The imported source is under the Microsoft MIT license in `LICENSE`. Triton
does not use the upstream `d3d12translationlayer` build dependency.

The local patches keep the converter suitable for the Vista UMD target:

- `ShaderConv/pch.h` removes ATL and the conflicting `d3dhal.h` include. The
  local pointer supports only the three upstream `CComPtr<CCodeBlob>` uses.
  `CCodeBlob` is a ShaderConverter-owned reference-counted object.
- The portability changes add missing SAL fallbacks, use standard C++
  declaration scope, and use `fminf` and `fmaxf`.
- The ownership changes use `delete[]` for buffers that the converter allocates
  as arrays.

The game-compatibility changes additionally:

- Validate bounded SM1–3 token streams before analysis, including phase/coissue
  lengths, register ranges, control-flow nesting, defined labels and acyclic
  call graphs. Allocate loop temporaries by call-graph rank and nesting depth;
  restore `aL` after nested loops and calls. Sequential loops reuse temporaries.
- Preserve all 256 public VS float constants and provide a separate internal
  constant range for the fixed-function indexed matrix palette.
- Add independent PS/VS sampler swizzles for D3D9 luminance, two-channel and
  X-channel texture formats. Generic channel-swizzle controls remain available;
  A2R10 uses canonical GPU channels with CPU-boundary resource conversion.
- Apply flat interpolation to both color outputs and implicit fog to the
  internally generated fixed-function pixel shader.
- Emit correct zero-source RCP, RSQ and LOG behavior without reading a masked
  destination operand as a source, and guard sampler sentinels before enum casts.
- Decode predication after the destination/address operand, accept SM2.x
  profiles, and read the old destination through a valid source swizzle.
- Correct TEXBEM/TEXBEML matrix order and bump-source luminance. Preserve the
  full TEXM3x3SPEC/VSPEC reflection coordinate by dividing by the normal's
  squared length; volume maps expose the scale error hidden by cube addressing.
- Initialize the translator for standalone geometry conversion, and use array
  deletion consistently for instruction, blob and assembler storage.

`tests/vista/run-d3d9-shaders.py --host` builds this exact converter and the
production fixed-function generator, executes converted shaders through native
DXVK, and compares actual pixels with independent arithmetic/geometry oracles.
`--host --sanitize` adds address and undefined-behavior instrumentation; Clang
is supported using `CC=clang CXX=clang++`. Leak detection is disabled for the
process-lifetime Vulkan/DXVK runtime, while invalid access and allocation/delete
mismatches remain fatal. `--build-public` produces x64/x86 public D3D9 fixtures
for the Vista integration runner.

These changes add no ATL runtime, `d3d12translationlayer`, or DLL import.

- Preserve PS1.4 DZ/DW zero-divisor semantics in register and immediate operands: divided coordinates become one; rendered postdivision arithmetic prevents final output clamping from hiding infinity.

- Floor VS1.1 MOV writes to the address register, matching Wine D3D9 visual test_mova and DXVK native-behavior handling; later MOVA retains rounding to nearest.
