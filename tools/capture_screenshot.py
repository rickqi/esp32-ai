#!/usr/bin/env python3
"""
capture_screenshot.py — Send a prompt to the ESP32-S3, wait for generation to
complete, then capture a screenshot of the RLCD and save it as a PNG to output/.

The screenshot shows the prompt text + the generated story (the firmware keeps
the story on screen instead of overwriting with a stats card).

Requires: pyserial, tokenizers, Pillow  →  uv run --with pyserial,Pillow python tools/capture_screenshot.py

Usage:
  # Default demo prompt
  uv run --with pyserial,Pillow python tools/capture_screenshot.py COM4

  # Custom prompt
  uv run --with pyserial,Pillow python tools/capture_screenshot.py COM4 --prompt "The dragon"

  # Custom prompt + max tokens
  uv run --with pyserial,Pillow python tools/capture_screenshot.py COM4 --prompt "today is raining" --max-tokens 100
"""
import argparse, base64, io, os, sys, time
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed.  Run: uv run --with pyserial,Pillow ...")
try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow not installed.  Run: uv run --with pyserial,Pillow ...")
try:
    from tokenizers import Tokenizer
except ImportError:
    sys.exit("tokenizers not installed.  Run: uv sync")


def find_tokenizer() -> Path:
    start = Path(__file__).resolve().parent.parent
    cand = start / "data"
    if cand.is_dir():
        for f in sorted(cand.glob("bpe*.json")):
            return f
    return start / "data" / "bpe32768.json"


def main() -> None:
    ap = argparse.ArgumentParser(description="Prompt + screenshot capture for ESP32-S3 RLCD")
    ap.add_argument("port", help="Serial port (e.g. COM4)")
    ap.add_argument("--prompt", default=None, help="Prompt text (default: device's demo prompt)")
    ap.add_argument("--max-tokens", type=int, default=None, help="Max tokens to generate")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--tokenizer", default=None)
    ap.add_argument("--outdir", default=None, help="Output directory (default: output/ next to this file's parent)")
    args = ap.parse_args()

    # Output directory: project root / output
    project_root = Path(__file__).resolve().parent.parent
    outdir = Path(args.outdir) if args.outdir else project_root / "output"
    outdir.mkdir(parents=True, exist_ok=True)

    # --- Load tokenizer (only if we have a custom prompt) ---
    custom_prompt = args.prompt is not None
    if custom_prompt:
        tok_path = Path(args.tokenizer) if args.tokenizer else find_tokenizer()
        tok = Tokenizer.from_file(str(tok_path))
        ids = tok.encode(args.prompt).ids
        payload = {"ids": ids}
        if args.max_tokens is not None:
            payload["max"] = args.max_tokens
        payload_str = __import__("json").dumps(payload, separators=(",", ":"))
        print(f"Prompt: {args.prompt!r}  ({len(ids)} tokens)")

    # --- Open serial ---
    ser = serial.Serial(args.port, args.baud, timeout=10)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    print(f"Serial: {args.port} @ {args.baud}")

    # --- Wait for device ready ---
    print("Waiting for {\"ready\":true} ...")
    deadline = time.time() + 60
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if '"ready"' in line:
            print("  Device ready.")
            break

    # --- Send prompt (or wait for demo to start) ---
    if custom_prompt:
        ser.write((payload_str + "\n").encode("utf-8"))
        ser.flush()
        print(f"  Sent prompt tokens.")
    else:
        print("  (no custom prompt — waiting for device demo timeout)")

    # --- Wait for generation to complete ---
    print("Waiting for generation to complete...")
    tok_s = None
    deadline = time.time() + 120
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if line.startswith("{") and '"done"' in line:
            try:
                import json
                msg = json.loads(line)
                tok_s = msg.get("tok/s")
            except Exception:
                pass
            print(f"  Generation done. ({tok_s:.2f} tok/s)" if tok_s else "  Generation done.")
            break

    # --- Wait for ready again (device loops back), then capture screenshot ---
    print("Capturing screenshot...")
    # The device emits {"ready":true} after {"done":...}. Send SHOOT then.
    deadline = time.time() + 15
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if '"ready"' in line:
            break

    ser.write(b"SHOOT\n")
    ser.flush()

    # --- Capture base64 PBM between markers ---
    b64_chunks = []
    in_shot = False
    deadline = time.time() + 30
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if "SCREENSHOT_START" in line:
            in_shot = True
            continue
        if "SCREENSHOT_END" in line:
            break
        if in_shot and len(line) > 5:
            b64_chunks.append(line)

    ser.close()

    if not b64_chunks:
        print("ERROR: no screenshot data captured")
        sys.exit(1)

    # --- Decode base64 → PBM → PNG ---
    b64_str = "".join(b64_chunks)
    pbm_data = base64.b64decode(b64_str)
    img = Image.open(io.BytesIO(pbm_data))
    # Upscale 2x for readability
    img_big = img.resize((img.size[0] * 2, img.size[1] * 2), Image.NEAREST)

    # --- Save ---
    ts = time.strftime("%Y%m%d_%H%M%S")
    png_path = outdir / f"screenshot_{ts}.png"
    pbm_path = outdir / f"screenshot_{ts}.pbm"
    img_big.save(str(png_path))
    with open(pbm_path, "wb") as f:
        f.write(pbm_data)
    print(f"\nSaved: {png_path}  ({img_big.size[0]}x{img_big.size[1]})")
    print(f"Saved: {pbm_path}  ({len(pbm_data)} bytes, {img.size[0]}x{img.size[1]})")
    if tok_s:
        print(f"Throughput: {tok_s:.2f} tok/s")


if __name__ == "__main__":
    main()
