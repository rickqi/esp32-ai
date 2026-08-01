"""Extract LVGL SimSun CJK font glyphs 鈫?compact 1bpp font library for the
ESP32 RLCD. Maps the Chinese model's char vocab to monochrome 16x16 glyphs.

Output: firmware/esp32_llm_zh/cjk_font.h (and copy to esp32_llm/ later)
  - CJK_CP[]   sorted UTF-8 codepoints present in the model vocab + font
  - CJK_OFF[]  byte offset of each 16x16 (32-byte) 1bpp glyph in CJK_BLOB
  - CJK_N      count
Missing chars (in vocab but not in font) are dropped; firmware renders 鈻?
"""
import re, sys, struct
from pathlib import Path

PROJECT = Path(r"D:\codes\esp32-ai")
FONT = Path(r"D:\codes\ESP32-S3-RLCD\projects\wifi_sta\managed_components\lvgl__lvgl\src\font\lv_font_simsun_16_cjk.c")
OUT = PROJECT / "firmware" / "esp32_llm_zh" / "cjk_font.h"

# ---- 1. parse the .c file ------------------------------------------------
txt = FONT.read_text(encoding="utf-8", errors="replace")

def parse_array(name):
    m = re.search(r"static.*?const \w+ " + re.escape(name) + r"\[\]\s*=\s*\{(.*?)\};", txt, re.S)
    if not m: raise ValueError(f"array {name} not found")
    body = m.group(1)
    out = []
    for tok in re.findall(r"0[xX][0-9a-fA-F]+|\d+", body):
        out.append(int(tok, 16) if tok[:2].lower() == "0x" else int(tok.lstrip("0") or "0"))
    return out

def parse_struct_array(name, fields):
    """Parse an array of {.f=0, ...} structs into list of dicts."""
    m = re.search(r"static const \w+ " + re.escape(name) + r"\[\]\s*=\s*\{(.*?)\};", txt, re.S)
    if not m: raise ValueError(f"struct array {name} not found")
    body = m.group(1)
    # split top-level {} entries
    entries = []
    depth = 0; cur = ""
    for ch in body:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
        if depth == 0 and ch == ',':
            entries.append(cur); cur = ""
        else: cur += ch
    if cur.strip(): entries.append(cur)
    out = []
    for e in entries:
        d = {}
        for f in fields:
            mm = re.search(rf"\.{f}\s*=\s*(-?\d+|0x[0-9a-fA-F]+)", e)
            d[f] = int(mm.group(1), 0) if mm else 0
        out.append(d)
    return out

def parse_unicode_list(name):
    m = re.search(r"static const uint16_t " + re.escape(name) + r"\[\]\s*=\s*\{(.*?)\};", txt, re.S)
    if not m: return None
    return [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", m.group(1))]

glyph_bitmap = parse_array("glyph_bitmap")
glyph_dsc = parse_struct_array("glyph_dsc", ["bitmap_index","adv_w","box_w","box_h","ofs_x","ofs_y"])

# cmaps: find all unicode_list_X
uni_lists = {}
for mm in re.finditer(r"static const uint16_t (unicode_list_\d+)\[\]\s*=\s*\{(.*?)\};", txt, re.S):
    name = mm.group(1)
    uni_lists[name] = [int(t,16) if t[:2].lower()=="0x" else int(t.lstrip("0") or "0")
                       for t in re.findall(r"0[xX][0-9a-fA-F]+|\d+", mm.group(2))]
print("uni_lists:", {k: len(v) for k, v in uni_lists.items()})

# parse cmaps structs (top-level brace-aware split)
cm = re.search(r"static const lv_font_fmt_txt_cmap_t cmaps\[\]\s*=\s*\{(.*?)\};", txt, re.S)
cm_body = cm.group(1)
cmap_entries = []
depth = 0; cur = ""
for ch in cm_body:
    if ch == '{': depth += 1
    elif ch == '}': depth -= 1
    if depth == 0 and ch == ',':
        cmap_entries.append(cur); cur = ""
    else: cur += ch
if cur.strip(): cmap_entries.append(cur)

cmaps = []
type_map = {"LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY":0, "LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL":1,
            "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY":2, "LV_FONT_FMT_TXT_CMAP_SPARSE_FULL":3}
for body in cmap_entries:
    d = {}
    for f in ["range_start","range_length","glyph_id_start","list_length"]:
        m = re.search(rf"\.{f}\s*=\s*(-?\d+|0x[0-9a-fA-F]+)", body)
        d[f] = int(m.group(1),0) if m else 0
    m = re.search(r"\.unicode_list\s*=\s*(\w+)", body)
    d["unicode_list"] = m.group(1) if m else None
    m = re.search(r"\.type\s*=\s*(\w+)", body)
    d["type"] = type_map.get(m.group(1), 0) if m else 0
    cmaps.append(d)

# ---- 2. build codepoint -> glyph_id map -----------------------------------
cp2gid = {}
for cmap in cmaps:
    rs, rl, gs, typ = cmap["range_start"], cmap["range_length"], cmap["glyph_id_start"], cmap["type"]
    ul = uni_lists.get(cmap["unicode_list"], None) if cmap["unicode_list"] else None
    for i in range(rl):
        if typ == 0:   # FORMAT0_TINY: contiguous
            cp = rs + i; gid = gs + i
        elif ul is not None and i < len(ul):  # SPARSE: cp = rs + ul[i]
            cp = rs + ul[i]; gid = gs + i
        else:
            continue
        if cp > 0 and gid < len(glyph_dsc):
            cp2gid[cp] = gid

print(f"font: {len(glyph_dsc)} glyphs, {len(cp2gid)} unicode codepoints mapped")

# ---- 3. load model vocab (codepoints) ------------------------------------
sys.path.insert(0, str(PROJECT))
from chinese.tokenizer import CharTokenizer
tok = CharTokenizer.load(str(PROJECT / "data_chinese" / "tokenizer.json"))
# model char set: real chars (exclude specials like <PAD> <EOS> <user>...)
model_cps = set()
for i in range(tok.vocab_size):
    ch = tok.itos.get(i, "")
    if not ch or ch.startswith("<") or ch == "\x00": continue
    cp = ord(ch)
    if 0x4E00 <= cp <= 0x9FFF:   # CJK unified ideographs
        model_cps.add(cp)
print(f"model vocab: {tok.vocab_size} tokens, {len(model_cps)} CJK chars")

# ---- 4. extract glyphs -> 16x16 1bpp cells --------------------------------
CELL = 16
def get_glyph_bits(gid):
    g = glyph_dsc[gid]
    bw, bh = g["box_w"], g["box_h"]
    if bw <= 0 or bh <= 0: return None
    idx = g["bitmap_index"]
    # 4bpp: 2 px per byte
    row_bytes = (bw + 1) // 2
    cell = bytearray(CELL * CELL // 8)  # 16x16 1bpp, MSB first per row
    for row in range(bh):
        for col in range(bw):
            byte_off = idx + row * row_bytes + col // 2
            if byte_off >= len(glyph_bitmap): break
            nib = glyph_bitmap[byte_off]
            px = nib >> 4 if (col % 2 == 0) else (nib & 0xF)
            if px > 0:
                x = col + g["ofs_x"]; y = row + g["ofs_y"]
                if 0 <= x < CELL and 0 <= y < CELL:
                    cell[y * CELL // 8 + x // 8] |= (0x80 >> (x % 8))
    return bytes(cell)

found = []
missing = []
for cp in sorted(model_cps):
    gid = cp2gid.get(cp)
    if gid is None:
        missing.append(cp); continue
    bits = get_glyph_bits(gid)
    if bits is None:
        missing.append(cp); continue
    found.append((cp, bits))

print(f"glyphs extracted: {len(found)}, missing: {len(missing)}")

# ---- 5. emit .h file ------------------------------------------------------
if not found:
    print("ERROR: no glyphs extracted"); sys.exit(1)

blob = bytearray()
offs = []
cps = []
for cp, bits in found:
    offs.append(len(blob))
    blob.extend(bits)
    cps.append(cp)

with open(OUT, "w", encoding="utf-8") as f:
    f.write("// Generated by chinese/gen_cjk_font.py -- 16x16 1bpp CJK glyphs from LVGL SimSun.\n")
    f.write("#ifndef CJK_FONT_H\n#define CJK_FONT_H\n")
    f.write(f"#define CJK_N {len(cps)}\n")
    f.write(f"static const uint16_t CJK_CP[{len(cps)}] = {{\n")
    for i in range(0, len(cps), 16):
        f.write("  " + ",".join(f"0x{c:04X}" for c in cps[i:i+16]) + ",\n")
    f.write("};\n")
    f.write(f"static const uint16_t CJK_OFF[{len(offs)}] = {{\n")
    for i in range(0, len(offs), 16):
        f.write("  " + ",".join(str(o) for o in offs[i:i+16]) + ",\n")
    f.write("};\n")
    f.write(f"static const uint8_t CJK_BLOB[{len(blob)}] = {{\n")
    for i in range(0, len(blob), 20):
        f.write("  " + ",".join(str(b) for b in blob[i:i+20]) + ",\n")
    f.write("};\n#endif\n")

print(f"wrote {OUT}: {len(cps)} glyphs, blob {len(blob)/1024:.0f} KB")
print(f"coverage: {len(cps)}/{len(model_cps)} ({100*len(cps)/len(model_cps):.1f}%)")

