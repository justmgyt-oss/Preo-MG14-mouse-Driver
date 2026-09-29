#!/usr/bin/env python3
"""
inspect_pe.py - Read a Windows PE and print what it links against.

This exists so the safety claims in docs/SAFETY.md can be checked rather than
asserted. In particular we verify that the binary imports no networking stack
(winhttp/wininet/ws2_32/dnsapi), which is the single most useful signal that a
"driver installer" is not phoning home.

Usage:  python3 tools/inspect_pe.py dist/PreoMG14Driver.exe
"""
import struct
import sys

NETWORK_DLLS = {
    "ws2_32.dll", "winhttp.dll", "wininet.dll", "dnsapi.dll",
    "urlmon.dll", "wship6.dll", "iphlpapi.dll", "netapi32.dll",
    "mswsock.dll", "secur32.dll", "crypt32.dll", "bcrypt.dll",
}
# Kernel-mode drivers we must never appear as, even as a payload.
KERNEL_ONLY = {"ntoskrnl.exe", "hal.dll", "win32k.sys"}


def read(path):
    return open(path, "rb").read()


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def parse(path):
    d = read(path)
    if d[:2] != b"MZ":
        raise SystemExit(f"{path}: not a PE file (no MZ signature)")

    pe_off = u32(d, 0x3C)
    if d[pe_off:pe_off + 4] != b"PE\0\0":
        raise SystemExit(f"{path}: bad PE signature")

    coff = pe_off + 4
    machine = u16(d, coff)
    num_sections = u16(d, coff + 2)
    opt_size = u16(d, coff + 16)
    opt = coff + 20
    magic = u16(d, opt)
    pe32plus = (magic == 0x20B)

    subsystem = u16(d, opt + 68)
    image_base = u64(d, opt + 24) if pe32plus else u32(d, opt + 28)

    # section table -> rva to file offset
    sec_off = opt + opt_size
    sections = []
    for i in range(num_sections):
        s = sec_off + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, s + 8)
        sections.append((vaddr, vsize, rawptr, rawsize))

    def rva2off(rva):
        for vaddr, vsize, rawptr, rawsize in sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return rawptr + (rva - vaddr)
        return None

    dlls = set()
    # data directory 1 = imports, 2 = resources
    dd = opt + (112 if pe32plus else 96)
    imp_rva, imp_size = struct.unpack_from("<II", d, dd + 8)
    if imp_rva:
        o = rva2off(imp_rva)
        while True:
            ilt, ts, fchain, name_rva, iat = struct.unpack_from("<IIIII", d, o)
            if name_rva == 0:
                break
            no = rva2off(name_rva)
            end = d.index(b"\0", no)
            dlls.add(d[no:end].decode("ascii", "replace").lower())
            o += 20

    subsys_name = {2: "WINDOWS_GUI", 3: "WINDOWS_CUI"}.get(subsystem, str(subsystem))
    machine_name = {0x8664: "x86-64", 0x14C: "x86", 0xAA64: "ARM64"}.get(machine, hex(machine))
    return d, dlls, machine_name, subsys_name, image_base, pe32plus


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    d, dlls, machine, subsys, base, pe32plus = parse(path)

    print(f"file      : {path}")
    print(f"size      : {len(d):,} bytes")
    print(f"format    : {'PE32+' if pe32plus else 'PE32'} {machine}")
    print(f"subsystem : {subsys}")
    print(f"image base: {hex(base)}")
    print(f"imports   : {len(dlls)} DLLs")
    for x in sorted(dlls):
        print(f"            {x}")

    net = sorted(dlls & NETWORK_DLLS)
    ker = sorted(dlls & KERNEL_ONLY)
    print()
    if net:
        print(f"FAIL  networking libraries present: {', '.join(net)}")
    else:
        print("PASS  no networking libraries imported")
    if ker:
        print(f"FAIL  kernel-mode references present: {', '.join(ker)}")
    else:
        print("PASS  no kernel-mode components referenced")
    return 1 if (net or ker) else 0


if __name__ == "__main__":
    sys.exit(main())
