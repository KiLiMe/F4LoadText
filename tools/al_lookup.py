#!/usr/bin/env python3
"""RVA ↔ 地址库 ID（F4SE 用的 V0 格式）。

文件布局（1.11.x 的 version-1-11-240-0.bin）：
    u64 条目数 count
    count × { u64 id, u64 offset }      ← offset 就是函数 RVA，按 id 排序

用途：/tmp 探针在游戏里记录了「读某个 chunk 的调用者 RVA」，用这个把它翻成地址库 ID，
     再去 CommonLibF4 的 include/RE/IDs.h 里找名字，或直接按 ID 挂钩子。

用法：
    uv run python tools/al_lookup.py <version-*.bin> 2F81A0            # RVA → ID（找不到精确就列最近的）
    uv run python tools/al_lookup.py <version-*.bin> --id 2192527      # ID → RVA
    uv run python tools/al_lookup.py <version-*.bin> --ids 2F81A0,2F8C00
    uv run python tools/al_lookup.py <version-*.bin> --stats
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path


def load_pairs(path: Path) -> list[tuple[int, int]]:
    raw = path.read_bytes()
    (count,) = struct.unpack_from("<Q", raw, 0)
    need = 8 + count * 16
    if len(raw) < need:
        raise SystemExit(f"文件比声明的条目数短：声明 {count:,} 条（需要 {need:,} B），实际 {len(raw):,} B")
    pairs = list(struct.iter_unpack("<QQ", raw[8:need]))
    return pairs


def main() -> int:
    ap = argparse.ArgumentParser(description="RVA ↔ 地址库 ID（V0 格式）")
    ap.add_argument("bin", type=Path)
    ap.add_argument("rva", nargs="?", help="要查的 RVA（十六进制）")
    ap.add_argument("--id", type=int, help="反向：ID → RVA")
    ap.add_argument("--ids", help="一次查多个 RVA（逗号分隔十六进制）")
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()

    pairs = load_pairs(args.bin)
    by_id = dict(pairs)

    if args.stats or (args.id is None and not args.rva and not args.ids):
        print(f"{args.bin.name}: {len(pairs):,} 条；id 范围 {pairs[0][0]:,} .. {pairs[-1][0]:,}；"
              f"RVA 范围 0x{min(o for _, o in pairs):X} .. 0x{max(o for _, o in pairs):X}")
        return 0

    if args.id is not None:
        rva = by_id.get(args.id)
        print(f"ID {args.id} → {'RVA 0x%X' % rva if rva is not None else '不在表里'}")
        return 0

    rvas = [int(x, 16) for x in args.ids.split(",")] if args.ids else [int(args.rva, 16)]
    offsets = sorted(o for _, o in pairs)
    for rva in rvas:
        if rva in by_id.values():
            hits = [i for i, o in pairs if o == rva]
            print(f"RVA 0x{rva:X} → ID {', '.join(str(h) for h in hits)}")
            continue
        # 调用点通常在函数内部：取最近的函数起点（RVA 比起点大、差得最少）
        cands = [(o, i) for i, o in pairs if o <= rva]
        if cands:
            o, i = max(cands)
            print(f"RVA 0x{rva:X} → 最可能 ID {i}（RVA 0x{o:X}，调用点在其内 +0x{rva - o:X}）")
        else:
            print(f"RVA 0x{rva:X} → 比表里任何函数都小，无法判定")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
