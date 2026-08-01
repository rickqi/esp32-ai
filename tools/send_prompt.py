#!/usr/bin/env python3
"""
send_prompt.py — Tokenize a text prompt on the PC and send the token IDs to
the ESP32-S3 over serial.  The device runs inference and streams generated
text back over the same serial line.

Requires:
    pyserial       →  pip install pyserial  (or: uv add pyserial)
    tokenizers     →  uv sync  (already in the project)

Usage:
  # Interactive mode (prompt loop until empty line)
  python tools/send_prompt.py COM3

  # Single-shot mode
  python tools/send_prompt.py COM3 --prompt "Once upon a time" --max-tokens 100

  # Non-interactive, custom tokenizer path
  python tools/send_prompt.py /dev/ttyUSB0 --tokenizer data/bpe4096.json
"""

import argparse
import json
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit(
        "pyserial not installed.\n"
        "  Run:  uv add pyserial\n"
        "  Or:   pip install pyserial"
    )

try:
    from tokenizers import Tokenizer
except ImportError:
    sys.exit(
        "tokenizers not installed.\n"
        "  Run:  uv sync\n"
        "  Or:   pip install tokenizers"
    )


# ---------------------------------------------------------------------------
#  Tokenizer auto-discovery
# ---------------------------------------------------------------------------

def find_tokenizer() -> Path:
    """Walk up from *tools/* and look for *data/bpe*.json*."""
    start = Path(__file__).resolve().parent
    for parent in [start, start.parent, start.parent.parent]:
        cand = parent / "data"
        if cand.is_dir():
            for f in sorted(cand.glob("bpe*.json")):
                return f
    return start / "data" / "bpe32768.json"


# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Send a text prompt to ESP32-S3 via serial"
    )
    parser.add_argument("port", help="Serial port (e.g. COM3, /dev/ttyUSB0, /dev/cu.usbmodem2101)")
    parser.add_argument("--prompt", default=None, help="Prompt text for single-shot mode")
    parser.add_argument("--max-tokens", type=int, default=None,
                        help="Tokens to generate (default: model's context limit)")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--tokenizer", default=None,
                        help="Path to tokenizer JSON (auto-detected if omitted)")
    args = parser.parse_args()

    # ---- load tokenizer -------------------------------------------------------
    tok_path = Path(args.tokenizer) if args.tokenizer else find_tokenizer()
    if not tok_path.is_file():
        print(f"Tokenizer not found: {tok_path}")
        print("Run the training pipeline first, or specify --tokenizer <path>.")
        sys.exit(1)
    tok = Tokenizer.from_file(str(tok_path))
    V = tok.get_vocab_size()
    print(f"Tokenizer: {tok_path.name}  (vocab size: {V})")

    # ---- open serial -----------------------------------------------------------
    try:
        ser = serial.Serial(args.port, args.baud, timeout=10)
    except serial.SerialException as exc:
        sys.exit(f"Serial error: {exc}")

    ser.reset_input_buffer()
    ser.reset_output_buffer()
    print(f"Serial:    {args.port} @ {args.baud} baud\n")

    # ---- wait for device boot ------------------------------------------------
    print("Waiting for device to boot...")
    while True:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        print(f"  {line}")
        if '"ready"' in line:
            break
    print()

    # ---- prompt loop -----------------------------------------------------------
    single = args.prompt is not None      # single-shot vs interactive

    while True:
        # Get prompt text
        if single:
            text = args.prompt
        else:
            try:
                text = input("Prompt> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break

        if not text:
            break

        # Tokenize
        ids = tok.encode(text).ids
        preview = ",".join(str(i) for i in ids[:16])
        if len(ids) > 16:
            preview += ",..."
        print(f"  tokenized: {len(ids)} ids → [{preview}]")

        if len(ids) > 512:
            print("  WARNING: prompt exceeds 512-token limit; extra IDs will be ignored.")

        # Build JSON payload
        payload: dict = {"ids": ids}
        if args.max_tokens is not None:
            payload["max"] = args.max_tokens
        payload_str = json.dumps(payload, separators=(",", ":"))

        # Send
        ser.write((payload_str + "\n").encode("utf-8"))
        ser.flush()
        print(f"  sent: {payload_str}")
        print()

        # Read generated stream
        while True:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace")

            # Check for JSON control message
            ls = line.strip()
            if ls.startswith("{"):
                try:
                    msg = json.loads(ls)
                except json.JSONDecodeError:
                    # Not a valid JSON — treat as generated text
                    sys.stdout.write(line)
                    sys.stdout.flush()
                    continue

                if msg.get("done"):
                    print(f"\n  --- {msg['tok/s']:.2f} tok/s ---")
                    break
                # Other JSON messages (like "ready") are printed below
                print(f"  {ls}")
                continue

            # Plain text: generated tokens
            sys.stdout.write(line)
            sys.stdout.flush()

        if single:
            break

        print()  # blank line between generations

    ser.close()
    print("Done.")


if __name__ == "__main__":
    main()
