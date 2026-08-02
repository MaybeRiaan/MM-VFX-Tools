"""Sweep all .aex under given roots; one line per PiPL with GPU-relevant fields."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pipl_dump import read_pe, rva_to_off, walk_dir, res_name, fourcc, pascal

def pipl_props(raw):
    props = {}
    if len(raw) < 10: return props
    _, _, count = struct.unpack_from('<HII', raw, 0)
    p, n = 10, 0
    while p + 16 <= len(raw) and n < count:
        key = fourcc(raw[p+4:p+8])
        _, ln = struct.unpack_from('<II', raw, p+8)
        props[key] = raw[p+16:p+16+ln]
        p += 16 + ln + (4 - ln % 4) % 4
        n += 1
    return props

def u32(b): return struct.unpack_from('<I', b, 0)[0] if len(b) >= 4 else -1
def sh2(b): return '%d.%d' % struct.unpack_from('<HH', b, 0) if len(b) >= 4 else '?'

def scan(path):
    rows = []
    try:
        data = open(path, 'rb').read()
        rsrc_rva, sections = read_pe(data)
        base = rva_to_off(rsrc_rva, sections)
        nnamed, nid = struct.unpack_from('<HH', data, base+12)
        for i in range(nnamed + nid):
            e = base + 16 + i*8
            nameval, dataval = struct.unpack_from('<II', data, e)
            if res_name(data, base, nameval).upper() != 'PIPL': continue
            if not (dataval & 0x80000000): continue
            for _, rva, size in walk_dir(data, base, dataval & 0x7FFFFFFF):
                off = rva_to_off(rva, sections)
                pr = pipl_props(data[off:off+size])
                if 'eGL2' not in pr and 'eGLO' not in pr: continue
                gl2 = u32(pr.get('eGL2', b''))
                rows.append({
                    'file': path, 'name': pascal(pr.get('name', b'')),
                    'mna': pascal(pr.get('eMNA', b'')),
                    'svr': sh2(pr.get('eSVR', b'')),
                    'glo': u32(pr.get('eGLO', b'')), 'gl2': gl2,
                    'gpu': bool(gl2 != -1 and gl2 & (1 << 25)),
                    'dx': bool(gl2 != -1 and gl2 & (1 << 29)),
                })
    except Exception as ex:
        rows.append({'file': path, 'err': str(ex)})
    return rows

def main(roots):
    all_rows, errs = [], 0
    for root in roots:
        for dirp, _, files in os.walk(root):
            for f in files:
                if f.lower().endswith('.aex'):
                    all_rows.extend(scan(os.path.join(dirp, f)))
    gpu_rows = [r for r in all_rows if r.get('gpu')]
    print(f"scanned PiPLs: {len(all_rows)}  |  with SUPPORTS_GPU_RENDER_F32: {len(gpu_rows)}\n")
    print("--- GPU_RENDER_F32 declarers ---")
    for r in gpu_rows:
        print(f"{r['glo']:#010x} {r['gl2']:#010x} svr={r['svr']:<6} dx={'Y' if r['dx'] else 'n'} "
              f"{r['name']!r:<28} mna={r['mna']!r}  {r['file']}")
    print("\n--- non-declarers (name, gl2, svr only) ---")
    for r in all_rows:
        if r.get('gpu') or 'err' in r: continue
        print(f"{r['gl2']:#010x} svr={r['svr']:<6} {r['name']!r:<30} {os.path.basename(r['file'])}")
    errbad = [r for r in all_rows if 'err' in r]
    if errbad:
        print(f"\n--- errors: {len(errbad)} ---")
        for r in errbad: print(f"{r['file']}: {r['err']}")

if __name__ == '__main__':
    main(sys.argv[1:])
