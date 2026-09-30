#!/usr/bin/env python3
"""Execute actual primary-copy functions against a bounded CPU COM model."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[6];TRITON=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('extract',ROOT/'tests/vista/test-d3d10-texture-map.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
resource=(TRITON/'tritonResource.c').read_text();view=(TRITON/'tritonView.c').read_text();dxgi=(TRITON/'tritonDxgi.c').read_text()
code=module.function(view,'tritonResourceHostViewFormat')+'\n'+'\n'.join(module.function(dxgi,n) for n in ('tritonRawColorFormat','tritonResourceCopyConverted'))+'\n'+'\n'.join(module.function(resource,n) for n in ('tritonResourceNeedsColorConversion','tritonResourceCopy','tritonResourceCopyRegion','tritonResourceCopyRegion_11_1','tritonResourceResolveSubresource'))
fixture=(Path(__file__).with_name('tritonD3D10CopyTest.c')).read_text()
for variant in ('current','direct-copy','unpredicated-final','wrong-srgb-resolve','missing-meta'):
 mutant=code
 if variant=='direct-copy':
  old='if (tritonResourceNeedsColorConversion(d, s))'
  assert old in mutant;mutant=mutant.replace(old,'if (FALSE)')
 elif variant=='unpredicated-final':
  old='ID3D11DeviceContext1_SetPredication(pD->pCtx1, predicate, predicateValue);'
  assert old in mutant;mutant=mutant.replace(old,'ID3D11DeviceContext1_SetPredication(pD->pCtx1, NULL, FALSE);',1)
 elif variant=='wrong-srgb-resolve':
  old='resolveDesc.Format = physicalFormat;'
  assert old in mutant;mutant=mutant.replace(old,'resolveDesc.Format = sourceRaw;')
 elif variant=='missing-meta':
  old='hr = tritonSharedBridgeCopyColor(pD->pCtx1, rtv, srv);'
  assert old in mutant;mutant=mutant.replace(old,'hr = S_OK;')
 with tempfile.TemporaryDirectory(prefix='triton-copy-conversion-') as d:
  d=Path(d);(d/'test.c').write_text(fixture.replace('// PRODUCTION',mutant))
  result=subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-O1','-g','-fsanitize=address,undefined',str(d/'test.c'),'-o',str(d/'test')],capture_output=True,text=True);assert result.returncode==0,result.stdout+result.stderr
  result=subprocess.run([str(d/'test')],capture_output=True,text=True)
  if variant=='current':assert result.returncode==0,result.stdout+result.stderr;print(result.stdout.strip())
  else:assert result.returncode!=0 and 'FAIL conversion' in result.stderr,result.stdout+result.stderr;print(variant+' negative control rejected')
print('D3D10 primary conversion checks passed (CPU contracts; GPU pixels and throughput pending)')
