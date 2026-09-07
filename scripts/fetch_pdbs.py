#!/usr/bin/env python3
"""
Parse the CodeView (RSDS) debug entry of each PE and download the matching PDB
from the Microsoft public symbol server (msdl.microsoft.com).

Symbol-server path convention:
    <server>/<pdbBasename>/<GUID><AGE>/<pdbBasename>
where GUID is the 16-byte CV_INFO_PDB70 signature formatted as
    %08X%04X%04X + 16 hex chars   (Data1 LE u32, Data2/Data3 LE u16, Data4 raw)
and AGE is appended as uppercase hex with no leading zeros.
"""
import os, sys, struct, binascii
import pefile
import requests

SERVER = "https://msdl.microsoft.com/download/symbols"
OUT = os.path.join(os.path.dirname(__file__), "..", "pdbs")
BINS = os.path.join(os.path.dirname(__file__), "..", "extracted")

MACHINE = {0x14c: "x86 (I386)", 0x8664: "x64 (AMD64)", 0x1c0: "ARM", 0xaa64: "ARM64"}

def cv_info(path):
    """Return (pdb_basename, guid_str, age, machine, tsig) for a PE, or None."""
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DEBUG']])
    machine = MACHINE.get(pe.FILE_HEADER.Machine, hex(pe.FILE_HEADER.Machine))
    tsig = pe.FILE_HEADER.TimeDateStamp
    sizeofimage = pe.OPTIONAL_HEADER.SizeOfImage
    out = None
    for dbg in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []):
        if dbg.struct.Type != 2:  # IMAGE_DEBUG_TYPE_CODEVIEW
            continue
        off = dbg.struct.PointerToRawData
        size = dbg.struct.SizeOfData
        data = pe.__data__[off:off+size]
        if data[:4] != b"RSDS":
            continue
        d1, d2, d3 = struct.unpack_from("<IHH", data, 4)
        d4 = data[12:20]
        age = struct.unpack_from("<I", data, 20)[0]
        pdb = data[24:].split(b"\x00", 1)[0].decode("utf-8", "replace")
        guid = "%08X%04X%04X%s" % (d1, d2, d3, binascii.hexlify(d4).decode().upper())
        out = (os.path.basename(pdb.replace("\\", "/")), guid, age, machine, tsig, sizeofimage)
    pe.close()
    return out

def download(pdb, guid, age):
    key = "%s%X" % (guid, age)
    url = "%s/%s/%s/%s" % (SERVER, pdb, key, pdb)
    dest_dir = os.path.join(OUT, pdb, key)
    os.makedirs(dest_dir, exist_ok=True)
    dest = os.path.join(dest_dir, pdb)
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        return dest, os.path.getsize(dest), "cached"
    headers = {"User-Agent": "Microsoft-Symbol-Server/10.0.0.0"}
    with requests.get(url, headers=headers, stream=True, timeout=120, allow_redirects=True) as r:
        if r.status_code != 200:
            return None, 0, "HTTP %d at %s" % (r.status_code, url)
        n = 0
        with open(dest, "wb") as f:
            for chunk in r.iter_content(1 << 16):
                f.write(chunk); n += len(chunk)
    return dest, n, url

def main():
    targets = sys.argv[1:] or sorted(os.listdir(BINS))
    for name in targets:
        path = os.path.join(BINS, name)
        if not os.path.isfile(path):
            continue
        info = cv_info(path)
        if not info:
            print("[!] %-14s no CodeView/RSDS debug entry" % name); continue
        pdb, guid, age, machine, tsig, soi = info
        print("[*] %-14s %s  ts=0x%08X  SizeOfImage=0x%X" % (name, machine, tsig, soi))
        print("    PDB=%s GUID=%s AGE=%d" % (pdb, guid, age))
        dest, n, how = download(pdb, guid, age)
        if dest:
            print("    -> %s (%d bytes) [%s]" % (dest, n, "cached" if how=="cached" else "downloaded"))
        else:
            print("    -> FAILED: %s" % how)

if __name__ == "__main__":
    main()
