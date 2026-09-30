#!/usr/bin/env python3
"""Exact-symbol acquisition/verification for the DLLs copied from the Vista guest.

Requires pefile, llvm-readobj and llvm-pdbutil. Does not modify the guest or DLLs.
Artifacts are regenerable under build/mshtml-native-re; never trust a filename
alone as a symbol match. Microsoft CodeView GUID and age must both agree.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import urllib.request
import uuid

import pefile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/mshtml-native-re"
SOURCES = {
    "mshtml-x86.dll": r"C:\Windows\SysWOW64\mshtml.dll",
    "ieframe-x86.dll": r"C:\Windows\SysWOW64\ieframe.dll",
    "urlmon-x86.dll": r"C:\Windows\SysWOW64\urlmon.dll",
    "mshtml-x64.dll": r"C:\Windows\System32\mshtml.dll",
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def binary_info(name):
    path = OUT / name
    pe = pefile.PE(str(path))
    records = []
    for entry in pe.DIRECTORY_ENTRY_DEBUG:
        if entry.struct.Type != 2:
            continue
        data = pe.__data__[entry.struct.PointerToRawData:
                           entry.struct.PointerToRawData + entry.struct.SizeOfData]
        if data[:4] == b"RSDS":
            guid = str(uuid.UUID(bytes_le=data[4:20])).upper()
            age = struct.unpack_from("<I", data, 20)[0]
            pdb = data[24:].split(b"\0")[0].decode("ascii").replace("\\", "/").split("/")[-1]
            records.append(dict(guid=guid, age=age, name=pdb))
    if len(records) != 1:
        raise ValueError(f"{name}: expected one RSDS record, got {records}")
    pdb = records[0]
    pdb["key"] = pdb["guid"].replace("-", "") + f'{pdb["age"]:X}'
    pdb["url"] = ("https://msdl.microsoft.com/download/symbols/"
                  f'{pdb["name"]}/{pdb["key"]}/{pdb["name"]}')
    fixed = pe.VS_FIXEDFILEINFO[0]
    version = ".".join(str(v) for v in (fixed.FileVersionMS >> 16,
        fixed.FileVersionMS & 65535, fixed.FileVersionLS >> 16,
        fixed.FileVersionLS & 65535))
    result = dict(name=name, sha256=sha(path), bytes=path.stat().st_size,
                  machine=pe.FILE_HEADER.Machine, image_base=pe.OPTIONAL_HEADER.ImageBase,
                  timestamp=pe.FILE_HEADER.TimeDateStamp, version=version,
                  file_flags=fixed.FileFlags, file_flags_mask=fixed.FileFlagsMask,
                  debug_build=bool(fixed.FileFlags & fixed.FileFlagsMask & 1), pdb=pdb,
                  sections=[dict(name=s.Name.rstrip(b"\0").decode("ascii"),
                    rva=s.VirtualAddress, virtual_size=s.Misc_VirtualSize,
                    raw_offset=s.PointerToRawData, raw_size=s.SizeOfRawData)
                    for s in pe.sections])
    pe.close()
    return result


def inventory():
    result = {name: binary_info(name) for name in SOURCES}
    (OUT / "binaries.json").write_text(json.dumps(result, indent=2) + "\n")
    for name, info in result.items():
        dump = subprocess.check_output(["llvm-readobj", "--file-header",
            "--section-headers", "--coff-debug-directory", str(OUT / name)])
        (OUT / (name + ".pe.txt")).write_bytes(dump)
        print(f'{name}: {info["version"]} DEBUG={info["debug_build"]} '
              f'PDB={info["pdb"]["key"]}', flush=True)
    return result


def pdb_path(name):
    return OUT / (Path(name).stem + ".pdb")


def pdb_summary(name, expected, path=None):
    path = path or pdb_path(name)
    summary = subprocess.check_output(["llvm-pdbutil", "dump", "--summary", str(path)], text=True)
    headers = subprocess.check_output(["llvm-pdbutil", "pdb2yaml", "--dbi-stream",
                                      "--pdb-stream", str(path)], text=True)
    guid = re.search(r"GUID:\s*\{([^}]+)\}", summary)
    age = re.search(r"Age:\s*(\d+)", summary)
    dbi_age = re.search(r"DbiStream:.*?Age:\s*(\d+)", headers, re.S)
    if not guid or not age or not dbi_age:
        raise ValueError(f"{name}: no GUID/age in PDB summary")
    # Microsoft's PDB1::OpenValidate4 requires PDB age >= image age and
    # DBI age == image age. Stripped public files here have PDB age 2, DBI 1.
    # https://github.com/microsoft/microsoft-pdb/blob/master/PDB/dbi/pdb.cpp
    if (guid[1].upper() != expected["guid"] or int(age[1]) < expected["age"] or
            int(dbi_age[1]) != expected["age"]):
        raise ValueError(f"{name}: PDB does not match PE CodeView: {summary}")
    return summary + "\n" + headers


def fetch_one(item):
    name, info = item
    path = pdb_path(name)
    if not path.exists():
        temp = path.with_suffix(".pdb.download")
        if not temp.exists():
            request = urllib.request.Request(info["pdb"]["url"], headers={"User-Agent": "Microsoft-Symbol-Server/10.0"})
            print(f'FETCH {info["pdb"]["url"]}', flush=True)
            with urllib.request.urlopen(request, timeout=60) as response, temp.open("xb") as dest:
                while block := response.read(1024 * 1024):
                    dest.write(block)
        # Check identity before promoting the downloaded file to the symbol cache.
        pdb_summary(name, info["pdb"], temp)
        temp.rename(path)
    summary = pdb_summary(name, info["pdb"])
    (OUT / (name + ".pdb-summary.txt")).write_text(summary)
    metadata = dict(url=info["pdb"]["url"], bytes=path.stat().st_size,
                    sha256=sha(path), guid=info["pdb"]["guid"], age=info["pdb"]["age"])
    (OUT / (name + ".pdb-provenance.json")).write_text(json.dumps(metadata, indent=2) + "\n")
    print(f'MATCHED {name}: {metadata["bytes"]} bytes {metadata["guid"]} age={metadata["age"]}', flush=True)


def verify_binaries():
    provenance = json.loads((OUT / "provenance.json").read_text())
    saved = json.loads((OUT / "binaries.json").read_text())
    for name, source in SOURCES.items():
        actual = binary_info(name)
        if actual != saved[name]:
            raise ValueError(f"{name}: recorded PE metadata differs from actual binary")
        record = provenance[name]
        if record["source"] != source:
            raise ValueError(f"{name}: wrong guest source")
        for field in ("bytes", "version", "sha256"):
            if record[field] != actual[field]:
                raise ValueError(f"{name}: extraction provenance mismatch: {field}")
        if actual["machine"] != (0x8664 if "x64" in name else 0x14c):
            raise ValueError(f"{name}: unexpected architecture")
    print("VISTA NATIVE BINARY PROVENANCE VERIFIED")


def verify_symbols():
    verify_binaries()
    for name in SOURCES:
        info = binary_info(name)
        pdb_summary(name, info["pdb"])
        record = json.loads((OUT / (name + ".pdb-provenance.json")).read_text())
        if (record["sha256"] != sha(pdb_path(name)) or
                record["bytes"] != pdb_path(name).stat().st_size or
                record["url"] != info["pdb"]["url"] or
                record["guid"] != info["pdb"]["guid"] or
                record["age"] != info["pdb"]["age"]):
            raise ValueError(f"{name}: symbol provenance mismatch")
    print("VISTA NATIVE PDB MATCHES VERIFIED")


def r2(name, commands):
    return subprocess.check_output(["radare2", "-2", "-q", "-e", "scr.color=0",
        "-e", "asm.sub.var=false", "-e", "asm.sub.names=false",
        "-e", "bin.relocs.apply=true", "-c",
        f'idp {pdb_path(name)};{commands};q', str(OUT / name)], text=True, timeout=120)


def index_symbols():
    verify_symbols()
    for name in SOURCES:
        symbols = json.loads(r2(name, "fj"))
        (OUT / (name + ".r2-symbols.json")).write_text(json.dumps(symbols, indent=2) + "\n")
        listing = "\n".join(f'0x{s["offset"]:x} {s["name"]}' for s in symbols)
        (OUT / (name + ".symbols.txt")).write_text(listing + "\n")
        with (OUT / (name + ".publics.txt")).open("w") as output:
            subprocess.run(["llvm-pdbutil", "dump", "--publics", str(pdb_path(name))],
                           stdout=output, check=True)
        print(f"INDEXED {name}: {len(symbols)} radare2 flags")


def disassemble(name, pattern, refresh=False):
    info = binary_info(name)
    pdb_summary(name, info["pdb"])
    symbols = json.loads((OUT / (name + ".r2-symbols.json")).read_text())
    if refresh:
        addresses = {int(p.stem, 16) for p in (OUT / (name + ".disassembly")).glob("*.txt")}
        selected = [s for s in symbols if s["offset"] in addresses and s["name"].startswith("pdb.")]
        selected = list({s["offset"]: s for s in selected}.values())
    else:
        selected = [s for s in symbols if re.search(pattern, s["name"])]
    if not selected:
        raise ValueError("no symbol matched")
    folder = OUT / (name + ".disassembly")
    folder.mkdir(exist_ok=True)

    def one(symbol):
        address = symbol["offset"]
        text = r2(name, f"af @ {address};pdf @ {address}")
        if not text.strip() or "Cannot find function" in text:
            raise ValueError(f"function analysis failed: {symbol}")
        header = (f'{name} sha256={info["sha256"]}\n'
                  f'PDB GUID={info["pdb"]["guid"]} image/DBI age={info["pdb"]["age"]}\n'
                  f'{symbol["name"]}\nVA=0x{address:x} RVA=0x{address-info["image_base"]:x}\n'
                  'radare2: idp PDB; af @ VA; pdf @ VA (preferred image base, not live ASLR)\n'
                  'asm.sub.var=false, asm.sub.names=false: keep raw operands; call labels remain\n\n')
        path = folder / f"{address:08x}.txt"
        path.write_text(header + text)
        print(f'DISASSEMBLED {path.name} {symbol["name"]}', flush=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        list(executor.map(one, selected))


def table(name, pattern, slots):
    info = binary_info(name)
    pdb_summary(name, info["pdb"])
    symbols = json.loads((OUT / (name + ".r2-symbols.json")).read_text())
    selected = [s for s in symbols if re.search(pattern, s["name"])]
    if len(selected) != 1 or not 1 <= slots <= 128:
        raise ValueError("table requires exactly one symbol and 1..128 slots")
    start = selected[0]["offset"]
    pe = pefile.PE(str(OUT / name))
    width = 8 if info["machine"] == 0x8664 else 4
    result = []
    for slot in range(slots):
        address = start + slot * width
        target = int.from_bytes(pe.get_data(address - info["image_base"], width), "little")
        result.append(dict(slot=slot, offset=slot * width, address=hex(address), target=hex(target),
            names=[s["name"] for s in symbols if s["offset"] == target and s["name"].startswith("pdb.")]))
    pe.close()
    artifact = dict(binary=info, table=selected[0], entries=result)
    path = OUT / f"{name}.table-{start:x}.json"
    path.write_text(json.dumps(artifact, indent=2) + "\n")
    for item in result:
        print(f'{item["slot"]:2d} +0x{item["offset"]:02x} {item["target"]} {", ".join(item["names"])}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("inventory", "fetch", "verify-binaries", "verify-symbols",
                                           "index", "disassemble", "refresh-disassembly", "table"))
    parser.add_argument("--module", choices=tuple(SOURCES), default="mshtml-x86.dll")
    parser.add_argument("--match", help="regular expression matched against symbol names")
    parser.add_argument("--slots", type=int, default=11, help="pointer count for table")
    args = parser.parse_args()
    if args.command == "inventory":
        inventory()
    elif args.command == "fetch":
        infos = inventory()
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
            list(executor.map(fetch_one, infos.items()))
    elif args.command == "verify-binaries":
        verify_binaries()
    elif args.command == "verify-symbols":
        verify_symbols()
    elif args.command == "index":
        index_symbols()
    elif args.command == "disassemble":
        if not args.match:
            parser.error("disassemble requires --match")
        disassemble(args.module, args.match)
    elif args.command == "refresh-disassembly":
        disassemble(args.module, None, refresh=True)
    elif args.command == "table":
        if not args.match:
            parser.error("table requires --match")
        table(args.module, args.match, args.slots)


if __name__ == "__main__":
    main()
