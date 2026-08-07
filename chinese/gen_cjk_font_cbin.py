"""Extract 14px/1bpp CJK glyphs from XiaoZhi's CBin font (font_noto_qwen_14_1.bin)
and emit cjk_font.h for the Chinese ESP32 firmware.

Modes:
  default  GB2312 (7445) ∪ model vocab → 7854 glyphs (~276KB binary)
  --full   ALL cbin characters (~18129 glyphs, ~638KB binary) — Plan B

Usage:
  python gen_cjk_font_cbin.py                    # default: GB2312 + vocab
  python gen_cjk_font_cbin.py --full             # full cbin extraction
  python gen_cjk_font_cbin.py --full --out path  # custom output

Output: cjk_font.h (CJK_CP/CJK_OFF/CJK_BLOB format, see docs/FONT_SYSTEM_ANALYSIS.md)
"""
import struct, sys, io, argparse, json
from pathlib import Path

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

PROJECT = Path(r"D:\codes\esp32-ai")
DEFAULT_CBIN = Path(r"D:\codes\xiaozhi-esp32\managed_components\78__xiaozhi-fonts\cbin\font_noto_qwen_14_1.bin")
DEFAULT_OUT = PROJECT / "firmware" / "esp32_llm_zh_v2" / "cjk_font.h"
DEFAULT_TOKENIZER = PROJECT / "data_chinese" / "tokenizer.json"

CELL = 14                     # 14x14 1bpp cell
ROW_BYTES = (CELL + 7) // 8   # 2 bytes per 14-bit row
CELL_BYTES = CELL * ROW_BYTES # 28

# ---- 0. parse args ----------------------------------------------------------
ap = argparse.ArgumentParser(description="Generate cjk_font.h from XiaoZhi CBin font")
ap.add_argument("--full", action="store_true",
                help="Extract ALL characters from cbin (~18129 glyphs) instead of GB2312 subset")
ap.add_argument("--max-glyphs", type=int, default=0,
                help="Cap glyph count (0=unlimited). Removes highest codepoints first (least useful).")
ap.add_argument("--cbin", type=Path, default=DEFAULT_CBIN, help="Path to font_noto_qwen_14_1.bin")
ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="Output cjk_font.h path")
ap.add_argument("--tokenizer", type=Path, default=DEFAULT_TOKENIZER,
                help="Tokenizer JSON (only used in default mode)")
args = ap.parse_args()

CBIN = args.cbin
OUT = args.out
TOKENIZER = args.tokenizer
FULL_MODE = args.full

if not CBIN.exists():
    print(f"ERROR: CBin font not found: {CBIN}")
    sys.exit(1)

mode_label = "FULL (all cbin chars)" if FULL_MODE else "GB2312 + model vocab"
print(f"Mode: {mode_label}")
print(f"CBin: {CBIN}")

# ---- 1. parse CBin ---------------------------------------------------------
data = CBIN.read_bytes()

dsc_off = struct.unpack_from("<I", data, 24)[0]
glyph_bitmap_rel, glyph_dsc_rel, cmaps_rel, kern_dsc_rel = struct.unpack_from("<iiii", data, dsc_off)
bf = struct.unpack_from("<H", data, dsc_off + 18)[0]
cmap_num = bf & 0x1FF
bpp = (bf >> 9) & 0xF
cmaps_base = dsc_off + cmaps_rel
glyph_dsc_base = dsc_off + glyph_dsc_rel
glyph_bitmap_base = dsc_off + glyph_bitmap_rel
print(f"CBin: bpp={bpp} cmaps={cmap_num} dsc_off={dsc_off}")

# glyph_dsc is 16 bytes when LV_FONT_FMT_TXT_LARGE=1:
#   uint32 bitmap_index; uint32 adv_w; uint16 box_w; uint16 box_h; int16 ofs_x; int16 ofs_y
def get_glyph_dsc(gid):
    p = glyph_dsc_base + gid * 16
    bitmap_index, adv_w = struct.unpack_from("<II", data, p)
    box_w, box_h = struct.unpack_from("<HH", data, p + 8)
    ofs_x, ofs_y = struct.unpack_from("<hh", data, p + 12)
    return bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y

# build codepoint -> glyph_id from cmaps (20-byte records)
char_gid = {}
for i in range(cmap_num):
    p = cmaps_base + i * 20
    range_start = struct.unpack_from("<I", data, p)[0]
    range_length, glyph_id_start = struct.unpack_from("<HH", data, p + 4)
    ul_rel, ofs_rel = struct.unpack_from("<II", data, p + 8)
    list_length = struct.unpack_from("<H", data, p + 16)[0]
    ctype = data[p + 18]
    ul_base = cmaps_base + ul_rel if ul_rel else None
    ofs_base = cmaps_base + ofs_rel if ofs_rel else None
    for k in range(range_length):
        if ctype in (1, 3):            # SPARSE_FULL / SPARSE_TINY
            if ul_base is None:
                continue
            if k >= list_length:
                break
            u = struct.unpack_from("<H", data, ul_base + 2 * k)[0]
            ucp = range_start + u
            if ctype == 1:
                gid = glyph_id_start + struct.unpack_from("<H", data, ofs_base + 2 * k)[0]
            else:
                gid = glyph_id_start + k
        else:                          # FORMAT0_FULL / FORMAT0_TINY
            ucp = range_start + k
            if ctype == 0 and ofs_base is not None:
                gid = glyph_id_start + struct.unpack_from("<B", data, ofs_base + k)[0]
            else:
                gid = glyph_id_start + k
        char_gid[ucp] = gid
print(f"font codepoints: {len(char_gid)}")

# ---- 2. target char set ----------------------------------------------------
if FULL_MODE:
    # Plan B: extract ALL codepoints present in the cbin font
    target_cps = sorted(char_gid.keys())
    target = {chr(cp) for cp in target_cps}
    print(f"FULL mode: {len(target)} chars (all cbin codepoints)")
else:
    # Default: full GB2312 + model vocab
    def gb2312_chars():
        out = []
        for q in range(1, 88):
            for w in range(1, 95):
                if 10 <= q <= 15:          # unused GB2312 zones
                    continue
                try:
                    ch = bytes([q + 0xA0, w + 0xA0]).decode("gb2312")
                except Exception:
                    continue
                out.append(ch)
        return out

    target = set(gb2312_chars())          # 7445
    print(f"GB2312 set: {len(target)} chars")

    # model vocab ALL printable chars (not just CJK): the model can emit any
    # char in its vocabulary, including en-dash / pipe / ‖ / © etc.
    if TOKENIZER.exists():
        tok = json.loads(TOKENIZER.read_text(encoding="utf-8"))
        for ch in tok["stoi"]:
            if ch and not ch.startswith("<") and len(ch) == 1:
                cp = ord(ch)
                if cp >= 0x20 and cp not in range(0x7F, 0xA0):   # printable, skip C1 controls
                    target.add(ch)
        print(f"target union (GB2312 + full model vocab): {len(target)} chars")
    else:
        print(f"WARN: tokenizer not found ({TOKENIZER}), using GB2312 only")

# ---- 3. extract glyphs -> 14x14 1bpp cells ---------------------------------
def get_glyph_bits(gid):
    bi, adv, bw, bh, ox, oy = get_glyph_dsc(gid)
    if bw <= 0 or bh <= 0:
        return None
    nbytes = (bw * bh + 7) // 8
    start = glyph_bitmap_base + bi
    raw = data[start:start + nbytes]
    cell = bytearray(CELL_BYTES)
    # center the glyph box inside the 14x14 cell; pixels outside the cell are
    # clipped (some glyphs like 'j' / '|' are 15px tall, descenders go below).
    cx = (CELL - bw) // 2
    cy = (CELL - bh) // 2
    for r in range(bh):
        for c in range(bw):
            idx = r * bw + c
            on = (raw[idx // 8] >> (7 - idx % 8)) & 1 if idx // 8 < len(raw) else 0
            if not on:
                continue
            x, y = cx + c, cy + r
            if 0 <= x < CELL and 0 <= y < CELL:
                cell[y * ROW_BYTES + x // 8] |= 0x80 >> (x % 8)
    return bytes(cell)

found = []
missing = []
for ch in sorted(target):
    cp = ord(ch)
    gid = char_gid.get(cp)
    if gid is None:
        missing.append(ch)
        continue
    bits = get_glyph_bits(gid)
    if bits is None:
        missing.append(ch)
        continue
    found.append((cp, bits))

print(f"extracted: {len(found)}, missing: {len(missing)}")
if missing:
    print(f"missing sample: {''.join(missing[:30])}")

# Apply glyph cap (removes highest codepoints first — least useful chars)
if args.max_glyphs > 0 and len(found) > args.max_glyphs:
    trimmed = len(found) - args.max_glyphs
    found = found[:args.max_glyphs]
    print(f"Capped to {args.max_glyphs} glyphs (removed {trimmed} highest codepoints)")

# ---- 4. emit cjk_font.h ------------------------------------------------------
if not found:
    print("ERROR: no glyphs"); sys.exit(1)

blob = bytearray()
offs = []
cps = []
for cp, bits in found:
    offs.append(len(blob))
    blob.extend(bits)
    cps.append(cp)

with open(OUT, "w", encoding="utf-8") as f:
    source_note = "ALL cbin codepoints (~18129 glyphs)" if FULL_MODE else "GB2312 ~98.7% + model vocab"
    f.write("// Generated by chinese/gen_cjk_font_cbin.py -- 14x14 1bpp CJK glyphs\n")
    f.write(f"// Source: XiaoZhi 78/xiaozhi-fonts {CBIN.name} ({source_note})\n")
    f.write(f"// Mode: {'FULL' if FULL_MODE else 'GB2312+vocab'}\n")
    f.write("#ifndef CJK_FONT_H\n#define CJK_FONT_H\n")
    f.write(f"#define CJK_N {len(cps)}\n")
    f.write(f"#define CJK_W {CELL}\n#define CJK_H {CELL}\n")
    f.write(f"static const uint32_t CJK_CP[{len(cps)}] = {{\n")
    for i in range(0, len(cps), 16):
        f.write("  " + ",".join(f"0x{c:04X}" for c in cps[i:i + 16]) + ",\n")
    f.write("};\n")
    f.write(f"static const uint32_t CJK_OFF[{len(offs)}] = {{\n")
    for i in range(0, len(offs), 16):
        f.write("  " + ",".join(str(o) for o in offs[i:i + 16]) + ",\n")
    f.write("};\n")
    f.write(f"static const uint8_t CJK_BLOB[{len(blob)}] = {{\n")
    for i in range(0, len(blob), 20):
        f.write("  " + ",".join(str(b) for b in blob[i:i + 20]) + ",\n")
    f.write("};\n#endif\n")

print(f"wrote {OUT}")
print(f"glyphs={len(cps)} blob={len(blob)/1024:.0f}KB  (+CP {len(cps)*4/1024:.0f}KB +OFF {len(offs)*4/1024:.0f}KB)")
total_kb = (len(blob) + len(cps)*4 + len(offs)*4) / 1024
print(f"total binary: {total_kb:.0f}KB")
cjk_count = sum(1 for cp,_ in found if 0x4E00<=cp<=0x9FFF)
print(f"GB2312 CJK coverage: {cjk_count}/6763 ({100*cjk_count/6763:.1f}%)")
