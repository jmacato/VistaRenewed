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

These changes add no ATL runtime, `d3d12translationlayer`, or DLL import.
