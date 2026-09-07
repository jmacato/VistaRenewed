from pathlib import Path
import json, pefile
from dissect.hypervisor.disk.qcow2 import QCow2
from dissect.volume.disk import Disk
from dissect.ntfs import NTFS
package=Path('test-artifacts/linux-build/package-x64')
results=[]
with open('winvista-3.qcow2','rb') as backing:
 ntfs=NTFS(Disk(QCow2(backing).open()).partitions[0].open())
 cache={}
 for path in sorted(package.iterdir()):
  if path.suffix.lower() not in ['.sys','.dll','.exe']:continue
  pe=pefile.PE(str(path));arch='x64' if pe.FILE_HEADER.Machine==0x8664 else 'x86'
  checks=[]
  for module in pe.DIRECTORY_ENTRY_IMPORT:
   dll=module.dll.decode(); key=(arch,dll.lower())
   if key not in cache:
    directories=['System32','System32/drivers'] if arch=='x64' else ['SysWOW64']
    data=None
    for directory in directories:
     try: data=ntfs.mft.get('Windows/'+directory+'/'+dll).open().read();break
     except Exception:pass
    if data is None:raise RuntimeError(f'Missing guest module {key}')
    exports=pefile.PE(data=data).DIRECTORY_ENTRY_EXPORT.symbols
    cache[key]=({e.name for e in exports if e.name},{e.ordinal for e in exports})
   names,ordinals=cache[key]
   absent=[i.name.decode() if i.name else '#'+str(i.ordinal) for i in module.imports if (i.name not in names if i.name else i.ordinal not in ordinals)]
   checks.append({'module':dll,'imports':len(module.imports),'missing':absent})
  results.append({'file':path.name,'arch':arch,'checks':checks})
Path('test-artifacts/linux-build/vista-import-exports.json').write_text(json.dumps(results,indent=2)+'\n')
missing=[(r['file'],c) for r in results for c in r['checks'] if c['missing']]
print('Missing imports:',missing)
print('Audited',sum(c['imports'] for r in results for c in r['checks']),'imports against the actual checked base exports')
if missing:raise SystemExit(1)
