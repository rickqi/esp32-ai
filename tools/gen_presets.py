#!/usr/bin/env python3
"""
gen_presets.py — 预烘焙中文医学问题 → presets.h (设备端键盘菜单用)

用真实 MiniMind BPE tokenizer 将每个问题编码为完整 ChatML token 序列
(与 tools/send_prompt_rag.py 语义一致: system 提示 + user 问题 + assistant 引导),
同时保存中文原文供 RLCD 显示。产物为 C 头文件,设备端零编码直接推理。

用法:
  python3 tools/gen_presets.py
  # 产物: firmware/esp32_llm_v5_idf/main/core/presets.h

输出格式:
  typedef struct { const char *text; const uint16_t *ids; uint8_t len; } KbdPreset;
  static const uint16_t kbd_preset_0_ids[] = {...};
  static const char kbd_preset_0_text[] = "...";
  static const KbdPreset kbd_presets[] = {...};
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from pathlib import Path
from tokenizers import Tokenizer

TOKENIZER = os.environ.get(
    "MINIMIND_TOKENIZER",
    "/mnt/d/codes/minimind/model/tokenizer.json",  # WSL 路径
)
# 产物路径: 在 WSL 下用 /mnt/d 前缀, Windows 下用 D:\
if sys.platform == "win32":
    OUT = Path(r"D:\codes\esp32-ai\firmware\esp32_llm_v5_idf\main\core\presets.h")
else:
    OUT = Path("/mnt/d/codes/esp32-ai/firmware/esp32_llm_v5_idf/main/core/presets.h")
MAX_PROMPT = 100  # 与 send_prompt_rag.py 一致 (seq_len=128, 留生成空间)

IM_START, IM_END = 1, 2

SYSTEM_PROMPT = "你是一个医学助手，请根据提供的参考资料准确回答问题。"

# 中文医学预设问题 (键盘菜单选择, 无 PC 检索时使用纯 Q->A 格式)
PRESETS = [
    "肺癌的早期症状有哪些",
    "高血压的诊断标准是什么",
    "糖尿病的临床表现有哪些",
    "病毒性肝炎的治疗原则是什么",
    "感染性休克的血象检查有什么特点",
    "感冒发烧如何治疗",
    "急性重型肝炎有哪些表现",
    "糖尿病酮症酸中毒怎么办",
    "宫外孕如何治疗",
    "带状疱疹后遗神经痛怎么办",
    "肝硬化腹水如何治疗",
    "儿童肺炎支原体感染怎么处理",
    "心肌梗死急救措施有哪些",
    "白疕皮损有什么特点",
    "高血压用药注意事项",
    "肝豆状核变性是什么病",
    "失眠如何改善",
    "胃溃疡的饮食注意事项",
    "颈椎病的预防措施",
    "中暑的急救方法",
    "贫血的原因有哪些",
    "哮喘发作时怎么办",
]


def build_chatml_ids(tok, question, max_prompt=MAX_PROMPT):
    """与 tools/send_prompt_rag.py build_chatml_ids 一致 (无证据版本)."""
    def enc(text):
        r = tok.encode(text, add_special_tokens=False)
        return r if isinstance(r, list) else r.ids

    head = [IM_START] + enc("system\n" + SYSTEM_PROMPT) + [IM_END]
    head += enc("\n")
    head += [IM_START] + enc("user\n")
    q_part = enc("问题：" + question) + [IM_END] + enc("\n")
    tail = [IM_START] + enc("assistant\n")

    ids = head + q_part + tail
    if len(ids) > max_prompt:
        keep = max_prompt - len(tail) - 4
        if keep > len(head):
            ids = ids[:keep] + tail
        else:
            ids = ids[:max_prompt - len(tail)] + tail
    return ids


def main():
    tok = Tokenizer.from_file(TOKENIZER)
    print(f"Tokenizer: {TOKENIZER} (vocab {tok.get_vocab_size()})")

    lines = []
    lines.append("/*")
    lines.append(" * presets.h — 键盘菜单预设中文问题 (由 tools/gen_presets.py 生成, 勿手改).")
    lines.append(" *")
    lines.append(" * 每个预设: 中文原文 (RLCD 显示) + 完整 ChatML token ids (直接推理).")
    lines.append(" * ChatML 模板与 tools/send_prompt_rag.py 一致, 无证据纯 Q->A 格式.")
    lines.append(" */")
    lines.append("#ifndef PRESETS_H")
    lines.append("#define PRESETS_H")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    const char     *text;   // 中文原文 (显示)")
    lines.append("    const uint16_t *ids;    // ChatML token ids")
    lines.append("    uint8_t         len;    // ids 长度")
    lines.append("} KbdPreset;")
    lines.append("")

    entries = []
    for i, q in enumerate(PRESETS):
        ids = build_chatml_ids(tok, q)
        if len(ids) > 255:
            print(f"  [warn] {q[:18]}... ids={len(ids)} 超 uint8 上限")
        # 转义 C 字符串 (中文原文)
        esc = q.encode("unicode_escape").decode("ascii").replace('"', '\\"')
        lines.append(f"static const char kbd_preset_{i}_text[] = \"{esc}\";")
        ids_str = ", ".join(str(x) for x in ids)
        lines.append(f"static const uint16_t kbd_preset_{i}_ids[] = {{{ids_str}}};")
        lines.append("")
        entries.append((q, len(ids)))

    lines.append("static const KbdPreset kbd_presets[] = {")
    for i, (q, n) in enumerate(entries):
        lines.append(f"    {{kbd_preset_{i}_text, kbd_preset_{i}_ids, {n}}},")
    lines.append("};")
    lines.append(f"#define KBD_PRESET_COUNT {len(entries)}")
    lines.append("")
    lines.append("#endif")
    lines.append("")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"\n写入 {OUT}")
    print(f"{len(entries)} 个预设:")
    for q, n in entries:
        print(f"  [{n:>3} tok] {q}")

    # 校验: 解码回中文确认无损
    for i, q in enumerate(PRESETS[:3]):
        ids = [x for x in build_chatml_ids(tok, q)]
        dec = tok.decode(ids, skip_special_tokens=False)
        if q in dec:
            print(f"  [ok] decode 保留问题: {q[:14]}...")
        else:
            print(f"  [!!] decode 丢失问题: {q[:14]}")


if __name__ == "__main__":
    main()
