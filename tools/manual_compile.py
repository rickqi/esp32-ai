"""
Manual firmware build — bypasses arduino-cli libsdetect deadlock on Windows.

Pipeline (all steps verified 2026-08-04):
  1. gen .ino.cpp   : ctags prototypes + preamble-ordered merge
  2. compile .o     : run exact commands from compile_commands.json (skip cached)
  3. link .elf      : platform.txt recipe.c.combine params
  4. elf2image .bin : esptool

Usage (Windows PowerShell):
    python tools/manual_compile.py --build-dir D:\\esp32-build-zh-v3-test
    python tools/manual_compile.py --build-dir D:\\esp32-build-zh-v3-test --force
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

CTAGS = os.environ.get(
    "CTAGS", r"C:\Users\szk220009\AppData\Local\Arduino15\packages\builtin\tools\ctags\5.8-arduino11\ctags.exe")
LOCAL = Path(os.environ["LOCALAPPDATA"])
T = LOCAL / "Arduino15" / "packages" / "esp32" / "tools" / "esp-x32" / "2601" / "xtensa-esp-elf"
SDK = LOCAL / "Arduino15" / "packages" / "esp32" / "tools" / "esp32s3-libs" / "3.3.11"


def gen_inocpp(ino, out):
    """Merge .ino -> .ino.cpp with ctags prototypes (arduino-builder method)."""
    raw = ino.read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    text = raw.decode("utf-8", errors="replace")

    r = subprocess.run(
        [CTAGS, "--c++-kinds=+pf", "--fields=+iaS", "--extra=+q",
         "--language-force=c++", "-o", "-", str(ino)],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    protos = []
    for t in r.stdout.splitlines():
        if "\tf\t" not in t:
            continue
        m = re.search(r"/\^(.*?)\$/\s*;\"\s*f", t)
        if not m:
            continue
        sig = m.group(1).strip()
        oi = sig.rfind("(")
        if oi < 0:
            continue
        head = sig[:oi].strip()
        params = sig[oi + 1:sig.rfind(")")].strip()
        hm = re.match(r"^(.*?)([\w:<>~]+)\s*$", head)
        if not hm:
            continue
        decl, name = hm.group(1).strip(), hm.group(2)
        if name in ("setup", "loop", "main"):
            continue
        params_c = re.sub(r"\s*=\s*[^,)]+", "", params)
        params_c = re.sub(r"\s+", " ", params_c).strip()
        protos.append(f"{decl} {name}({params_c});")

    lines = text.splitlines(keepends=True)
    fn_start = None
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith(("#", "//", "/*", "*", "struct ", "typedef ", "enum ", "class ")):
            continue
        if "(" in s and "{" in s and ";" not in s.split("{")[0] and \
           ("static " in s or s[0].isalpha()):
            fn_start = i
            break
    if fn_start is None:
        fn_start = 60
    merged = '#include <Arduino.h>\n#line 1 "esp32_llm_zh_v3.ino"\n'
    merged += "".join(lines[:fn_start]) + "\n" + "\n".join(protos) + "\n"
    merged += "".join(lines[fn_start:])
    out.write_text(merged, encoding="utf-8")
    print(f"[1/4] .ino.cpp: {len(protos)} protos -> {out.name}")


def compile_objs(build, force):
    cc_file = build / "compile_commands.json"
    if not cc_file.exists():
        sys.exit(f"compile_commands.json not found: {cc_file}")
    entries = json.loads(cc_file.read_text(encoding="utf-8", errors="replace"))
    ok = skip = fail = 0
    for e in entries:
        src = Path(e["file"])
        args_list = e["arguments"]
        out = None
        if "-o" in args_list:
            oi = args_list.index("-o")
            if oi + 1 < len(args_list):
                out = Path(args_list[oi + 1])
        if out is None:
            out = build / "sketch" / (src.stem + ".o")
        if not force and out.exists() and src.exists():
            if out.stat().st_mtime >= src.stat().st_mtime:
                skip += 1
                continue
        os.makedirs(out.parent, exist_ok=True)
        r = subprocess.run(args_list, cwd=e["directory"])
        if r.returncode == 0:
            ok += 1
        else:
            fail += 1
            print(f"  FAIL({r.returncode}): {src.name}")
    print(f"[2/4] objects: {ok} compiled, {skip} cached, {fail} failed")
    return fail == 0


def link_elf(build):
    gxx = str(T / "bin" / "xtensa-esp32s3-elf-g++.exe")
    objs = sorted(str(o) for o in build.rglob("*.o"))
    ld_flags = (SDK / "flags" / "ld_flags").read_text(encoding="utf-8", errors="replace").split()
    ld_scripts = (SDK / "flags" / "ld_scripts").read_text(encoding="utf-8", errors="replace").split()
    ld_libs = (SDK / "flags" / "ld_libs").read_text(encoding="utf-8", errors="replace").split()
    elf = build / "esp32_llm_zh_v3.ino.elf"
    cmd = [gxx,
           f"-Wl,--Map={build/'esp32_llm_zh_v3.ino.map'}",
           f"-L{SDK/'lib'}", f"-L{SDK/'ld'}", f"-L{SDK/'qio_opi'}",
           "-Wl,--wrap=esp_panic_handler", "-Wl,--wrap=esp_bt_mem_release",
           "-Wl,--wrap=esp_bt_controller_mem_release",
           ] + ld_flags + ld_scripts + \
          ["-Wl,--start-group"] + objs + ld_libs + \
          ["-Wl,--end-group", "-Wl,-EL", "-o", str(elf)]
    r = subprocess.run(cmd)
    if elf.exists():
        print(f"[3/4] link: exit={r.returncode}, elf {elf.stat().st_size/2**20:.1f}MB")
    else:
        print(f"[3/4] link exit={r.returncode}")
    return r.returncode == 0


def make_bin(build):
    esp = LOCAL / "Arduino15" / "packages" / "esp32" / "tools" / "esptool_py" / "5.3.1" / "esptool.exe"
    elf = build / "esp32_llm_zh_v3.ino.elf"
    out = build / "esp32_llm_zh_v3.ino.bin"
    r = subprocess.run([str(esp), "--chip", "esp32s3", "elf2image",
                        "--flash-mode", "dio", "--flash-freq", "80m",
                        "--flash-size", "16MB", "-o", str(out), str(elf)])
    if out.exists():
        print(f"[4/4] bin: exit={r.returncode}, {out.stat().st_size/1024:.0f}KB")
    else:
        print(f"[4/4] bin exit={r.returncode}")
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--no-gen", action="store_true", help="skip .ino.cpp regen")
    args = ap.parse_args()

    build = Path(args.build_dir)
    sketch = build / "sketch"
    sketch.mkdir(parents=True, exist_ok=True)
    ino = Path(r"D:\codes\esp32-ai\firmware\esp32_llm_zh_v3\esp32_llm_zh_v3.ino")
    inocpp = sketch / "esp32_llm_zh_v3.ino.cpp"

    if not args.no_gen:
        gen_inocpp(ino, inocpp)
    ok = compile_objs(build, args.force)
    if ok:
        ok = link_elf(build)
    if ok:
        ok = make_bin(build)
    print("\nBUILD " + ("SUCCESS" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
