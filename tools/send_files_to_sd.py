#!/usr/bin/env python3
"""
send_files_to_sd.py — 通过 COM (USB-Serial-JTAG) 传输 RAG 索引文件到设备 SD 卡.

协议 (固件 do_xfer):
  PC → MCU: "XFER <filename> <size>\n"
  MCU → PC: "XFER_OK\n"        (就绪)
  PC → MCU: 4KB 原始块
  MCU → PC: "OK\n"             (每块 ACK)
  MCU → PC: "XFER_DONE\n"      (完成)

用法:
  python tools/send_files_to_sd.py COM3
  python tools/send_files_to_sd.py COM3 --files index.bin docs.bin meta.bin
"""
import argparse, os, sys, time, serial

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

SRC_DIR = r"D:\codes\esp32-ai\data_v4\sd_rag"
FILES = ["index.bin", "docs.bin", "meta.bin", "term_overlay.bin"]
CHUNK = 256   # 与固件 XFER_CHUNK 一致 (USB-Serial-JTAG 单次传输上限)


def wait_for(ser, marker, timeout=30, verbose=True):
    """等待固件输出 marker 行 (忽略其他日志行)"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if marker in line:
            return True
        if verbose and line and ('xfer' in line.lower() or 'ERR' in line or 'timeout' in line):
            print(f"  fw: {line}", flush=True)
    print(f"  [ERR] timeout waiting {marker}")
    return False


def wait_ack(ser, seq, timeout=30):
    """等待固件 XFER_ACK_<seq> (独特前缀, 防二进制数据误判)"""
    marker = f"XFER_ACK_{seq}"
    t0 = time.time()
    while time.time() - t0 < timeout:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if marker in line:
            return True
        if line and ('xfer' in line.lower() or 'ERR' in line or 'timeout' in line):
            print(f"  fw: {line}", flush=True)
    print(f"  [ERR] timeout waiting {marker}")
    return False


def send_file(ser, path, fname):
    fsize = os.path.getsize(path)
    print(f"\n=== XFER {fname} ({fsize/1024:.1f}KB) ===", flush=True)
    ser.write(f"XFER {fname} {fsize}\n".encode())
    ser.flush()
    if not wait_for(ser, "XFER_OK", 10):
        return False

    sent = 0
    seq = 0
    t0 = time.time()
    with open(path, 'rb') as f:
        while sent < fsize:
            # 一次读 CHUNK 发送 (固件每 CHUNK 写 SD + ACK)
            data = f.read(CHUNK)
            if not data:
                break
            ser.write(data)
            ser.flush()
            sent += len(data)
            seq += 1
            # 等固件 ACK (写入 SD 完成) — 按序号精确匹配
            if not wait_ack(ser, seq, 30):
                return False
    if not wait_for(ser, "XFER_DONE", 10):
        return False
    dt = time.time() - t0
    print(f"  done {fname}: {fsize/1024:.1f}KB in {dt:.1f}s ({fsize/1024/dt:.0f}KB/s)", flush=True)
    time.sleep(0.5)   # 让固件回到命令循环 (do_xfer 末尾刷新 g_last_activity)
    ser.reset_input_buffer()
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="COM port")
    ap.add_argument("--files", nargs='*', default=FILES, help="files to send")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=1)
    ser.reset_input_buffer()
    time.sleep(0.5)

    # 打断 auto 循环: 等待 auto done (推理间隙) 或直接 Tab
    # 先尝试 Tab (若设备空闲则立即生效)
    ser.write(b"KEY 0x2B\n")
    ser.flush()
    time.sleep(0.5)
    ser.write(b"KEY 0x2B\n")
    ser.flush()
    time.sleep(0.3)
    # 等待串口空闲 (auto 推理中则排队)
    ser.reset_input_buffer()
    # 若 auto 推理阻塞, 等 done 信号后 Tab
    t0 = time.time()
    interrupted = False
    while time.time() - t0 < 160:
        line = ser.readline().decode('utf-8', errors='replace')
        if 'auto: done' in line:
            ser.write(b"KEY 0x2B\n"); ser.flush()
            time.sleep(0.3)
            ser.write(b"KEY 0x2B\n"); ser.flush()
            time.sleep(0.3)
            interrupted = True
            break
        if 'KEY sim' in line:  # Tab 已生效
            interrupted = True
            break
    ser.reset_input_buffer()
    print(f"auto 打断: {'OK' if interrupted else '可能仍忙'}", flush=True)

    # 逐个文件传输
    for fn in args.files:
        path = os.path.join(SRC_DIR, fn)
        if not os.path.exists(path):
            print(f"[ERR] {fn} 不存在: {path}", flush=True)
            continue
        if not send_file(ser, path, fn):
            print(f"[ERR] {fn} 传输失败", flush=True)
            break

    ser.close()
    print("\n=== 全部完成 ===", flush=True)


if __name__ == "__main__":
    main()
