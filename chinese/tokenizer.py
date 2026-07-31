"""
Chinese character-level tokenizer for the ESP32-LLM project.

Each Chinese character is mapped to a single token ID (no BPE subword split).
This gives deterministic encoding and a small vocabulary (~8000 entries),
which is ideal for microcontroller deployment.

Special tokens:
  0 = <PAD>   padding / end-of-text
  1 = <UNK>   unknown character (fallback for OOV)
  2 = <BOS>   begin of sequence
  3 = <EOS>   end of sequence
  4+          real characters sorted by frequency
"""

import json
import os
from collections import Counter
from pathlib import Path
from typing import List, Optional


class CharTokenizer:
    PAD = 0
    UNK = 1
    BOS = 2
    EOS = 3
    SPECIAL = [ "<PAD>", "<UNK>", "<BOS>", "<EOS>" ]

    def __init__(self, vocab: Optional[dict] = None):
        self.stoi = vocab or {}          # char → id
        self.itos = {v: k for k, v in self.stoi.items()} if vocab else {}
        self.vocab_size = len(self.stoi)

    def train(self, text: str, vocab_size: int = 8000, min_freq: int = 2):
        """Build character vocabulary from text, keeping the top `vocab_size` chars."""
        counter = Counter(text)
        # Filter by minimum frequency
        chars = [ch for ch, cnt in counter.most_common(vocab_size) if cnt >= min_freq]

        # Build mapping: specials first, then sorted by frequency
        self.stoi = {}
        for i, s in enumerate(self.SPECIAL):
            self.stoi[s] = i
        for ch in chars:
            self.stoi[ch] = len(self.stoi)

        self.itos = {v: k for k, v in self.stoi.items()}
        self.vocab_size = len(self.stoi)
        print(f"CharTokenizer: vocab_size={self.vocab_size}, "
              f"chars={len(chars)}, min_freq={min_freq}")

    def encode(self, text: str, bos: bool = True, eos: bool = True) -> List[int]:
        ids = []
        if bos: ids.append(self.BOS)
        for ch in text:
            ids.append(self.stoi.get(ch, self.UNK))
        if eos: ids.append(self.EOS)
        return ids

    def decode(self, ids: List[int]) -> str:
        chars = []
        for i in ids:
            if i == self.EOS:
                break
            if i >= self.BOS and i in self.itos:
                chars.append(self.itos[i])
        return "".join(chars)

    def save(self, path: str):
        data = {
            "vocab_size": self.vocab_size,
            "stoi": self.stoi,
            "special": self.SPECIAL,
        }
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)

    @classmethod
    def load(cls, path: str) -> "CharTokenizer":
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        return cls(vocab=data["stoi"])


# Quick test
if __name__ == "__main__":
    tok = CharTokenizer()
    tok.train("hello world 你好世界 测试中文tokenizer", vocab_size=100)
    ids = tok.encode("你好世界")
    print(f"encode: {ids}")
    print(f"decode: {tok.decode(ids)}")
    print(f"vocab_size: {tok.vocab_size}")
