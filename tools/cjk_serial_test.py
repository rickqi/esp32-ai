# -*- coding: utf-8 -*-
"""Wait for device boot, send Chinese prompt (CharTokenizer stoi), capture SHOOT screenshot."""
import serial, json, time, sys, io, base64
from pathlib import Path

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

PORT = 'COM4'
BAUD = 115200
PROJECT = Path(r'D:\codes\esp32-ai')

def load_stoi():
    tok = json.loads((PROJECT / 'data_chinese' / 'tokenizer.json').read_text(encoding='utf-8'))
    return tok['stoi'], tok.get('extra_special', [])

def encode(stoi, text):
    ids = []
    for ch in text:
        if ch in stoi:
            ids.append(stoi[ch])
        else:
            ids.append(stoi.get('<UNK>', 1))
    return ids

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'boot'
    stoi, extras = load_stoi()
    ser = serial.Serial(PORT, BAUD, timeout=10)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    print(f'serial {PORT} open, mode={mode}', flush=True)

    # wait for ready
    print('waiting for ready...', flush=True)
    deadline = time.time() + 60
    ready = False
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode('utf-8', errors='replace').strip()
        print(f'[boot] {line}', flush=True)
        if '"ready"' in line:
            ready = True
            break
    if not ready:
        print('ERROR: device not ready'); ser.close(); sys.exit(1)

    if mode == 'boot':
        ser.close(); return

    # send Chinese prompt
    prompt = '检查结果' if len(sys.argv) < 3 else sys.argv[2]
    ids = encode(stoi, prompt)
    payload = json.dumps({'ids': ids, 'max': 150}, separators=(',', ':'))
    print(f'prompt={prompt!r} ids={ids} payload={payload}', flush=True)
    ser.write((payload + '\n').encode('utf-8'))
    ser.flush()

    # read generation until done
    print('generating...', flush=True)
    out = []
    deadline = time.time() + 180
    done = False
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode('utf-8', errors='replace')
        ls = line.strip()
        if ls.startswith('{'):
            try:
                msg = json.loads(ls)
                if msg.get('done'):
                    print(f'[done] {ls}', flush=True)
                    done = True
                    break
            except Exception:
                pass
        out.append(line.rstrip('\n'))
    if not done:
        print('WARNING: generation did not complete in time', flush=True)
    print('--- generated text ---', flush=True)
    print(''.join(out), flush=True)

    # wait for next ready, then SHOOT
    print('waiting for ready to shoot...', flush=True)
    deadline = time.time() + 15
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        if '"ready"' in raw.decode('utf-8', errors='replace'):
            break
    ser.write(b'SHOOT\n')
    ser.flush()
    print('SHOOT sent, capturing...', flush=True)

    b64 = []
    in_shot = False
    deadline = time.time() + 30
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode('utf-8', errors='replace').strip()
        if 'SCREENSHOT_START' in line:
            in_shot = True; continue
        if 'SCREENSHOT_END' in line:
            break
        if in_shot and len(line) > 5:
            b64.append(line)
    ser.close()

    if not b64:
        print('ERROR: no screenshot'); sys.exit(1)
    pbm = base64.b64decode(''.join(b64))
    outdir = PROJECT / 'output'
    outdir.mkdir(exist_ok=True)
    ts = time.strftime('%Y%m%d_%H%M%S')
    pbm_path = outdir / f'cjk_test_{ts}.pbm'
    pbm_path.write_bytes(pbm)
    print(f'saved {pbm_path} ({len(pbm)} bytes)')

if __name__ == '__main__':
    main()
