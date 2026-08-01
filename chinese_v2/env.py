"""
chinese_v2 environment config — independent Chinese training environment.

Fully isolated from chinese/ (v1, OCR-corpus based):
  v1: chinese/       data_chinese/       runs_chinese/       firmware/model_chinese/
  v2: chinese_v2/    data_v2/            runs_v2/            firmware/model_v2/

v2 uses clean medical data (ModelScope zjydiary/Medical: encyclopedia + textbooks)
instead of OCR'd business documents, per PLAN_AB_RETRAIN_RAG.md 方案 A.
"""

from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent

DATA_DIR = PROJECT_ROOT / "data_v2"
RUNS_DIR = PROJECT_ROOT / "runs_v2"
OUT_DIR = PROJECT_ROOT / "firmware" / "model_v2"
FW_DIR = PROJECT_ROOT / "firmware" / "esp32_llm_zh"

# Model config (same proven zh4 architecture)
MODEL = {
    "d_model": 160,
    "n_layers": 8,
    "n_heads": 8,
    "ple_dim": 192,
    "target_core": 2500000,
    "seq_len": 256,
}

# Data sources (ModelScope mirrors, fast in China)
SOURCES = {
    "pretrain_medical": {
        "dataset": "zjydiary/Medical",
        "files": ["pretrain/train_encyclopedia.json", "medical_book_zh.json"],
        "desc": "医学百科 360K + 教材 8.5K (shibing624/medical 镜像)",
    },
    "sft_bench": {
        "dataset": "AI-ModelScope/firefly-train-1.1M",
        "desc": "通用指令 (采样 1-2K)",
    },
}
