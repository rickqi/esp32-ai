# -*- coding: utf-8 -*-
"""Send prompt, capture FULL raw generation, analyze char coverage vs cjk_font.h"""
import serial, json, time, sys, io, re
from pathlib import Path

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
PROJECT = Path(r'D:\codes\esp32-ai')

# load char set from cjk_font.h
txt = (PROJECT / 'firmware' / 'esp32_llm_zh' / 'cjk_font.h').read_text(encoding='utf-8')
cps = [int(x, 16) for x in re.search(r'CJK_CP\[\d+\] = \{(.*?)\};', txt, re.S).group(1).split(',') if x.strip()]
cpset = set(cps)
ascii_ok = set(range(32, 127))

# load stoi
tok = json.loads((PROJECT / 'data_chinese' / 'tokenizer.json').read_text(encoding='utf-8'))
stoi = tok['stoi']

ser = serial.Serial('COM4', 115200, timeout=10)
ser.reset_input_buffer()
ser.reset_output_buffer()

# wait ready
deadline = time.time() + 120
while time.time() < deadline:
    raw = ser.readline()
    if not raw: continue
    if '"ready"' in raw.decode('utf-8', errors='replace'):
        break

# send prompt
prompt = '甲状腺切除' if len(sys.argv) < 2 else sys.argv[1]
ids = []
for ch in prompt:
    ids.append(stoi.get(ch, stoi.get('<UNK>', 1)))
payload = json.dumps({'ids': ids, 'max': 120}, separators=(',', ':'))
ser.write((payload + '\n').encode('utf-8'))
ser.flush()
print(f'sent {prompt!r} -> {ids}', flush=True)

# capture all raw bytes until done
raw_out = b''
done = False
deadline = time.time() + 180
while time.time() < deadline:
    n = ser.in_waiting
    if n > 0:
        chunk = ser.read(n)
        raw_out += chunk
        try:
            ls = raw_out.split(b'\n')[-1].decode('utf-8', errors='ignore').strip()
            if ls.startswith('{') and '"done"' in ls:
                done = True
        except Exception:
            pass
    if done:
        # drain remaining
        time.sleep(0.5)
        n = ser.in_waiting
        if n > 0:
            raw_out += ser.read(n)
        break
    time.sleep(0.05)
ser.close()

# strip control/status lines, keep pure text
text_lines = []
for ln in raw_out.decode('utf-8', errors='replace').split('\n'):
    ls = ln.strip()
    if ls.startswith('{') or ls.startswith('---') or ls.startswith('throughput') or ls.startswith('profile'):
        continue
    text_lines.append(ln)
gen_text = ''.join(text_lines)
print('=== raw generation text ===')
print(repr(gen_text[:600]))
print()

# char coverage analysis
missing = {}
for ch in gen_text:
    if ch in '\r\n\t ': continue
    cp = ord(ch)
    if cp in ascii_ok or cp in cpset:
        continue
    missing[ch] = missing.get(ch, 0) + 1
if missing:
    print('=== chars rendered as □ (missing from font) ===')
    for ch, cnt in sorted(missing.items(), key=lambda x: -x[1]):
        print(f'  U+{ord(ch):04X} {ch!r} x{cnt}')
else:
    print('ALL chars covered - no □ expected')
