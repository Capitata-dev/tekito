#!/usr/bin/env python3
"""Blends training examples for train-japanese-ai-model.py (one JSON object
a line, {"prompt", "text"}) from several prepared sets, an even sample of
each, leaving out every example whose text is, or holds, a sentence of the
evaluation sets: so the model is never trained on what it is measured on.

  blend-japanese-ai-examples.py --out examples.jsonl
      --take mix3m/examples.jsonl:700000 --take newdata2/examples.jsonl:700000
      --exclude-dir .cache/llm-eval [--seed 41]

--take FILE:N   N examples from FILE, sampled evenly (all of them when it
                has fewer).
--exclude-dir   each *.gen.tsv (id, reading, texts...) and *_keys.tsv (set,
                id, keys, texts...) in it: the texts left out, and any text
                with one of 12 characters or more inside it.
"""
from __future__ import annotations

import argparse
import json
import random
from pathlib import Path

LONG = 12  # a measured sentence this long is left out where it is inside a text too
HEAD = 6


def measured_texts(directories: list[str]) -> set[str]:
    texts: set[str] = set()
    for directory in directories:
        for path in Path(directory).glob("*.gen.tsv"):
            for line in open(path, encoding="utf-8"):
                texts.update(t for t in line.rstrip("\n").split("\t")[2:] if t)
        for path in Path(directory).glob("*_keys.tsv"):
            for line in open(path, encoding="utf-8"):
                texts.update(t for t in line.rstrip("\n").split("\t")[3:] if t)
    return texts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--take", action="append", required=True, help="FILE:N")
    parser.add_argument("--exclude-dir", action="append", default=[])
    parser.add_argument("--seed", type=int, default=41)
    args = parser.parse_args()
    rng = random.Random(args.seed)

    measured = measured_texts(args.exclude_dir)
    by_head: dict[str, list[str]] = {}
    for text in measured:
        if len(text) >= LONG:
            by_head.setdefault(text[:HEAD], []).append(text)

    def is_measured(text: str) -> bool:
        if text in measured:
            return True
        for i in range(len(text) - LONG + 1):
            for long in by_head.get(text[i:i + HEAD], ()):
                if text.startswith(long, i):
                    return True
        return False

    rows: list[str] = []
    for take in args.take:
        name, _, count = take.rpartition(":")
        with open(name, encoding="utf-8") as f:
            lines = f.readlines()
        wanted = min(int(count), len(lines))
        kept = left = 0
        order = list(range(len(lines)))
        rng.shuffle(order)
        for index in order:
            if kept == wanted:
                break
            try:
                row = json.loads(lines[index])
            except json.JSONDecodeError:
                continue
            if not row.get("prompt") or not row.get("text"):
                continue
            if is_measured(row["text"]):
                left += 1
                continue
            rows.append(json.dumps({"prompt": row["prompt"], "text": row["text"]}, ensure_ascii=False))
            kept += 1
        print(f"{name}: {kept} examples, {left} left out as measured", flush=True)
    rng.shuffle(rows)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        for row in rows:
            f.write(row + "\n")
    print(f"{len(rows)} examples written", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
