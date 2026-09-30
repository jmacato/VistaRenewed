#include <cassert>
#include <cstdio>
#include <initializer_list>
using BOOL=int; using UINT=unsigned;
union D3DDDI_BLTFLAGS {
 struct { UINT Point:1; UINT Linear:1; UINT Rest:30; };
 UINT Value;
};
/* SOURCE_UNDER_TEST */
int main() {
 // Ordinary blits and the one-, two- and multi-copy DWM sequences.
 for (UINT hint : {0u,0x100u,0x200u,0x400u,0x500u}) {
  for (UINT filter : {0u,1u,2u}) {
   D3DDDI_BLTFLAGS f={};f.Value=hint|filter;
   assert(triton9BltFlagsSupported(f));
  }
  D3DDDI_BLTFLAGS f={};f.Value=hint|3u;
  assert(!triton9BltFlagsSupported(f));
 }
 for (UINT unsupported : {4u,8u,0x10u,0x20u,0x40u,0x80u,0x800u,0x80000000u}) {
  D3DDDI_BLTFLAGS f={};f.Value=unsupported|0x501u;
  assert(!triton9BltFlagsSupported(f));
 }
 puts("PASS DWM single/multi-copy markers; invalid filters and unimplemented operations rejected");
}
