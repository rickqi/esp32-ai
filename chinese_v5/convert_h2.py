# -*- coding: utf-8 -*-
"""
Convert MiniMind PLE1 model.bin -> llm_v5.h format (esp32_llm_zh_v5).

MiniMind export_ple1.py layout:
  header: magic + 8*i32 (V,D,L,H,F,P,S,G) + f32 rope
  tensor order (per plan in export_ple1.py):
    embed Q[V,D], ple_model_proj Q[L*P,D], ple_proj_norm F[P], ple_table Q[V,L*P]
    per layer:
      q_proj Q[D,D], k_proj Q[D,D], v_proj Q[D,D], o_proj Q[D,D],   (separate)
      q_norm F[D], k_norm F[D],
      input_layernorm F[D], post_attention_layernorm F[D],
      gate Q[F,D], up Q[F,D], down Q[D,F],
      ple_gate Q[P,D], ple_proj Q[D,P], ple_norm F[D]
    norm F[D]
  Q tensor: [i32 group][int4 codes][fp16 scales]   (NO bits byte)

llm_v5.h expects:
  attn_norm F[D], q_norm F[D], k_norm F[D],
  qkv Q[3D,D]  (q/k/v merged), attn_proj Q[D,D],
  ffn_norm F[D], gate, up, down, ple_gate, ple_proj, ple_norm, out_norm
  Q tensor: [u8 bits=4][i32 group][codes][fp16 scales]

Usage:
    python chinese_v5/convert_h2.py \
        --in firmware/model_v5/H2/model.bin \
        --out firmware/model_v5/H2/model_llm.bin
"""
import argparse
import struct
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")


def read_q_minimind(d, p, rows, cols, bits=4):
    """Read MiniMind Q tensor (no bits byte). Returns (packed_bytes, scales_bytes, next_p)."""
    g = struct.unpack("<i", d[p:p + 4])[0]; p += 4
    ng = (cols + g - 1) // g
    rb = cols if bits == 8 else (cols + 1) // 2   # 8bit: 1 byte/code
    codes_n = rows * rb
    scales_n = rows * ng * 2
    codes = d[p:p + codes_n]; p += codes_n
    scales = d[p:p + scales_n]; p += scales_n
    return codes, scales, p, g


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", default="firmware/model_v5/H2/model.bin")
    ap.add_argument("--out", dest="out", default="firmware/model_v5/H2/model_llm.bin")
    ap.add_argument("--bits", type=int, default=4, choices=[4, 8], help='Q tensor bits (must match export_ple1.py --bits)')
    args = ap.parse_args()

    d = Path(args.inp).read_bytes()
    hv = struct.unpack("<8i", d[4:36])
    V, D, L, H, F, P, S, G = hv
    print(f"input: V={V} D={D} L={L} H={H} F={F} P={P} S={S} group={G}")

    # ---- build MiniMind plan (must match export_ple1.py exactly) ----
    plan = []  # (kind, rows, cols) ; F tensors are 1D: rows=n, cols=1
    hd = D // H  # head_dim
    plan.append(("Q", V, D))            # embed
    plan.append(("Q", L * P, D))        # ple_model_proj
    plan.append(("F", P, 1))            # ple_proj_norm
    plan.append(("Q", V, L * P))        # ple_table
    for _ in range(L):
        plan.append(("Q", D, D))        # q_proj
        plan.append(("Q", D, D))        # k_proj
        plan.append(("Q", D, D))        # v_proj
        plan.append(("Q", D, D))        # o_proj
        plan.append(("F", hd, 1))       # q_norm [head_dim]
        plan.append(("F", hd, 1))       # k_norm [head_dim]
        plan.append(("F", D, 1))        # input_layernorm
        plan.append(("F", D, 1))        # post_attention_layernorm
        plan.append(("Q", F, D))        # gate
        plan.append(("Q", F, D))        # up
        plan.append(("Q", D, F))        # down
        plan.append(("Q", P, D))        # ple_gate
        plan.append(("Q", D, P))        # ple_proj
        plan.append(("F", D, 1))        # ple_norm
    plan.append(("F", D, 1))            # norm

    # ---- read all MiniMind tensors ----
    p = 40  # header
    tensors = []  # (kind, rows, cols, data_or_None)
    for kind, r, c in plan:
        if kind == "Q":
            codes, scales, p, g = read_q_minimind(d, p, r, c, bits=args.bits)
            tensors.append((kind, r, c, codes, scales, g))
        else:
            nbytes = r * c * 4
            tensors.append((kind, r, c, d[p:p + nbytes], None, 0))
            p += nbytes
    print(f"parsed {len(tensors)} tensors, end={p} file={len(d)} match={p == len(d)}")

    # ---- rewrite in llm_v5.h layout ----
    out = bytearray()
    out += d[:40]  # header unchanged

    def w_q(r, c, codes, scales, g):
        nonlocal out
        out.append(args.bits)               # bits = 4/8
        out += struct.pack("<i", g)
        out += codes
        out += scales

    def w_f(data):
        nonlocal out
        out += data

    # tok_emb, ple_model_proj, ple_proj_norm, ple_table
    i = 0
    def next_t():
        nonlocal i
        t = tensors[i]; i += 1
        return t

    t = next_t(); w_q(t[1], t[2], t[3], t[4], t[5])   # embed
    t = next_t(); w_q(t[1], t[2], t[3], t[4], t[5])   # ple_model_proj
    t = next_t(); w_f(t[3])                            # ple_proj_norm
    t = next_t(); w_q(t[1], t[2], t[3], t[4], t[5])   # ple_table

    for layer in range(L):
        # MiniMind order: q,k,v,o, q_norm,k_norm, in_norm, post_norm, gate,up,down,ple_gate,ple_proj,ple_norm
        tq = next_t(); tk = next_t(); tv = next_t(); to = next_t()
        tqn = next_t(); tkn = next_t()
        tin = next_t(); tpn = next_t()
        tg = next_t(); tu = next_t(); td = next_t()
        tpg = next_t(); tpp = next_t(); tpln = next_t()

        # llm_v5.h: attn_norm, q_norm, k_norm, qkv(merged), attn_proj, ffn_norm, gate,up,down,ple_gate,ple_proj,ple_norm
        w_f(tin[3])                      # attn_norm = input_layernorm
        w_f(tqn[3])                      # q_norm
        w_f(tkn[3])                      # k_norm
        # qkv: concatenate q/k/v codes row-wise -> [3D, D]
        # each is [D,D] = D rows of cols D. Merge: rows 3D
        def merge_qkv(q, k, v):
            r, c = D, D
            # codes: each tensor has r rows, each row rb=(c+1)//2 bytes
            rb = (c + 1) // 2
            # all three share same group layout
            codes = q[0] + k[0] + v[0]   # concatenate flat packed bytes
            scales = q[1] + k[1] + v[1]
            return codes, scales
        cq, sq = merge_qkv((tq[3], tq[4]), (tk[3], tk[4]), (tv[3], tv[4]))
        w_q(3 * D, D, cq, sq, G)
        w_q(D, D, to[3], to[4], G)       # attn_proj = o_proj
        w_f(tpn[3])                      # ffn_norm = post_attention_layernorm
        w_q(tg[1], tg[2], tg[3], tg[4], G)
        w_q(tu[1], tu[2], tu[3], tu[4], G)
        w_q(td[1], td[2], td[3], td[4], G)
        w_q(tpg[1], tpg[2], tpg[3], tpg[4], G)
        w_q(tpp[1], tpp[2], tpp[3], tpp[4], G)
        w_f(tpln[3])                     # ple_norm

    t = next_t(); w_f(t[3])              # out_norm

    Path(args.out).write_bytes(bytes(out))
    print(f"wrote {args.out} ({len(out)/2**20:.2f}MB)")


if __name__ == "__main__":
    main()
