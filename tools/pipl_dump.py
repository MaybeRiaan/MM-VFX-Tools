"""Dump PiPL resources from an AE .aex (Windows PE) straight off disk.

Walks the PE resource tree, extracts every resource of type 'PiPL',
and decodes the property list. Also lists all resource type names
present in the binary (embedded kernels often live in custom types).

Layout (validated against EdgeRepair's PiPLTool-generated .rc):
  header:  uint16 0x0001, uint32 version(0), uint32 count
  record:  4CC vendor ('8BIM', byte-reversed on disk), 4CC key (reversed),
           uint32 propID, uint32 length, data padded to 4-byte multiple
  numeric data little-endian; 4CC data values byte-reversed.
"""
import struct, sys

def rva_to_off(rva, sections):
    for va, vsz, ptr, rsz in sections:
        if va <= rva < va + max(vsz, rsz):
            return ptr + (rva - va)
    raise ValueError(f"RVA {rva:#x} not in any section")

def read_pe(data):
    if data[:2] != b'MZ':
        raise ValueError("not a PE (no MZ)")
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe+4] != b'PE\0\0':
        raise ValueError("no PE signature")
    nsec, = struct.unpack_from('<H', data, pe+6)
    optsz, = struct.unpack_from('<H', data, pe+20)
    opt = pe + 24
    magic, = struct.unpack_from('<H', data, opt)
    ddoff = opt + (0x70 if magic == 0x20B else 0x60)
    rsrc_rva, rsrc_sz = struct.unpack_from('<II', data, ddoff + 2*8)
    sections = []
    secoff = opt + optsz
    for i in range(nsec):
        s = secoff + i*40
        name = data[s:s+8].rstrip(b'\0').decode('ascii', 'replace')
        vsz, va, rsz, ptr = struct.unpack_from('<IIII', data, s+8)
        sections.append((va, vsz, ptr, rsz))
    return rsrc_rva, sections

def res_name(data, base, val):
    if val & 0x80000000:
        off = base + (val & 0x7FFFFFFF)
        ln, = struct.unpack_from('<H', data, off)
        return data[off+2:off+2+ln*2].decode('utf-16-le')
    return f"#{val}"

def walk_dir(data, base, off, depth=0, path=()):
    """Yield (path_tuple, data_rva, data_size) leaves."""
    nnamed, nid = struct.unpack_from('<HH', data, base+off+12)
    for i in range(nnamed + nid):
        e = base + off + 16 + i*8
        nameval, dataval = struct.unpack_from('<II', data, e)
        label = res_name(data, base, nameval)
        if dataval & 0x80000000:
            yield from walk_dir(data, base, dataval & 0x7FFFFFFF,
                                depth+1, path + (label,))
        else:
            rva, size = struct.unpack_from('<II', data, base + dataval)
            yield path + (label,), rva, size

def fourcc(b):  # on-disk reversed -> logical
    return b[::-1].decode('ascii', 'replace')

def pascal(b):
    if not b: return ''
    ln = b[0]
    return b[1:1+ln].decode('ascii', 'replace')

KNOWN = {
    'kind': '4CC', '8664': 'str', 'eMNA': 'pas', 'name': 'pas',
    'catg': 'pas', 'ePVR': '2sh', 'eSVR': '2sh', 'eVER': 'u32',
    'eGLO': 'u32', 'eGL2': 'u32', 'eINF': 'u32', 'aeFL': 'u32',
    'eURL': 'pas', 'ma64': 'str', 'mi64': 'str', 'eSVA': 'u32',
}

def dump_pipl(raw):
    out = []
    if len(raw) < 10:
        out.append(f"  !! too short ({len(raw)} bytes)")
        return out
    magic, ver, count = struct.unpack_from('<HII', raw, 0)
    out.append(f"  header magic={magic:#x} version={ver} count={count}")
    p = 10
    n = 0
    while p + 16 <= len(raw) and n < count:
        vend = fourcc(raw[p:p+4]); key = fourcc(raw[p+4:p+8])
        pid, ln = struct.unpack_from('<II', raw, p+8)
        d = raw[p+16:p+16+ln]
        pad = (4 - ln % 4) % 4
        desc = ''
        kind = KNOWN.get(key)
        if kind == 'u32' and ln >= 4:
            v, = struct.unpack_from('<I', d, 0)
            vbe, = struct.unpack_from('>I', d, 0)
            desc = f"u32 LE={v} ({v:#010x})" + (f"  [BE would be {vbe:#010x}]" if vbe != v else '')
        elif kind == '2sh' and ln >= 4:
            a, b2 = struct.unpack_from('<HH', d, 0)
            desc = f"shorts LE = {a}.{b2}"
        elif kind == '4CC' and ln >= 4:
            desc = f"4CC '{fourcc(d[:4])}'"
        elif kind == 'str':
            desc = f"cstr {d.split(b'\\0')[0].decode('ascii','replace')!r}"
        elif kind == 'pas':
            desc = f"pstr {pascal(d)!r}"
        else:
            if ln == 4:
                v, = struct.unpack_from('<I', d, 0)
                desc = f"u32? LE={v} ({v:#010x}) raw={d.hex()}"
            else:
                desc = f"raw={d[:32].hex()}{'...' if ln>32 else ''} ascii={d[:32]!r}"
        out.append(f"  [{n:2}] {vend}/{key}  id={pid} len={ln:<4} {desc}")
        p += 16 + ln + pad
        n += 1
    if n != count:
        out.append(f"  !! parsed {n} of declared {count} (offset {p}/{len(raw)})")
    return out

def main(path):
    data = open(path, 'rb').read()
    rsrc_rva, sections = read_pe(data)
    base = rva_to_off(rsrc_rva, sections)
    print(f"=== {path} ({len(data)} bytes) ===")
    # top-level: list all resource types
    types = {}
    nnamed, nid = struct.unpack_from('<HH', data, base+12)
    for i in range(nnamed + nid):
        e = base + 16 + i*8
        nameval, dataval = struct.unpack_from('<II', data, e)
        types[res_name(data, base, nameval)] = dataval
    print("resource types:", ', '.join(types.keys()))
    for tname, tval in types.items():
        if tname.upper() != 'PIPL':
            continue
        if not (tval & 0x80000000):
            continue
        for path_t, rva, size in walk_dir(data, base, tval & 0x7FFFFFFF, 1, (tname,)):
            off = rva_to_off(rva, sections)
            raw = data[off:off+size]
            print(f"\nPiPL resource {'/'.join(path_t)} size={size}")
            for line in dump_pipl(raw):
                print(line)
    print()

if __name__ == '__main__':
    for p in sys.argv[1:]:
        try:
            main(p)
        except Exception as ex:
            print(f"=== {p} ===\n  ERROR: {ex}\n")
