# Chinese PLE Model Training for ESP32-S3

Train a tiny Transformer model on Chinese insurance/business documents,
using the same PLE (Per-Layer Embedding) architecture that powers the
English TinyStories model on the ESP32-S3.

## Design

| Aspect | English (original) | Chinese (this) |
|---|---|---|
| Tokenizer | BPE subword (vocab=32768) | **Character-level** (vocab≈8000) |
| Data | TinyStories 300MB | D:\docs\source documents |
| Data dir | `data/` | `data_chinese/` |
| Runs dir | `runs/` | `runs_chinese/` |
| Training code | `src/train.py` | `chinese/train.py` (wrapper) |
| Model | Same `src/model.py` | Same (changes only `vocab_size`) |

## Usage

```powershell
# 1. Extract + clean text from D:\docs\source
uv run python chinese/prepare.py

# 2. Train the model (default: 5000 steps, small config)
uv run python chinese/train.py

# 3. Speed test first (recommended)
uv run python chinese/train.py --steps 50 --eval-every 10 --tag speedtest
```

## Overrides

```powershell
# Larger model (if data is plentiful)
uv run python chinese/train.py --d-model 96 --n-layers 6 --target-core 560000 --steps 10000

# Custom vocab size
uv run python chinese/prepare.py --vocab-size 6000

# Custom source directory
uv run python chinese/prepare.py --source "D:/other_docs"
```

## File structure

```
chinese/
  prepare.py     # Extract, clean, tokenize → data_chinese/
  train.py       # Training wrapper → runs_chinese/
  tokenizer.py   # Character-level tokenizer
  README.md      # This file
data_chinese/
  corpus.txt      # Cleaned raw text
  tokenizer.json  # CharTokenizer vocabulary
  train.bin       # Encoded training tokens (uint16)
  val.bin         # Encoded validation tokens (uint16)
runs_chinese/
  ple-zh-s42.pt          # Trained model checkpoint
  ple-zh-s42.json        # Training history
```
