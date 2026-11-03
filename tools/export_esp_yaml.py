#!/usr/bin/env python3
# ==============================================================================
# vendored: 上游副本 —— 来源 上游工作区 scripts/export_esp_yaml.py
# 为何复制：让 F4LoadText 仓自包含，做汉化不依赖工作区上层 scripts/
# 同步方式：手动（上游仍是权威版本）；复制日期 2026-10-10
# ==============================================================================

"""esp → yaml：把插件里的可翻译文本导成一份**给人改的** yaml。

流程的第二棒（配合 `apply_esp_yaml.py` 完成回写）：

    esp ──export_esp_yaml.py──▶ .yaml ──（人改 dest）──▶ apply_esp_yaml.py ──▶ 新 esp

用法
----
    uv run scripts/export_esp_yaml.py --esp "<mod>.esp" --out "<mod>.yaml"

    # 用 xTranslator 词典预填一部分译文
    uv run scripts/export_esp_yaml.py --esp "<mod>.esp" --sst "<词典>.sst" --out "<mod>.yaml"

    # 大 mod（>2000 条）自动按记录类型拆成多文件
    uv run scripts/export_esp_yaml.py --esp "<mod>.esp" --split --out "<目录>/"

产物长这样
----------
```yaml
esp: MyMod.esp
localized: false
is_light: false
entries:
  - plugin: MyMod.esp          # 来源插件名
    type: MESG                 # 记录类型
    form_id: "0x01000F9A"      # esp 内原始值（不是运行时值）
    editor_id: MS_SPW_NewGameOptions
    field: ITXT                # 字段签名
    index: 1                   # 同签名第几个（多值字段用）
    source: "[ ] Disable Tutorial Quest"   # 原文，别改
    dest: ""                               # ← 译文写这里
```

**只改 `dest`**，其余字段是身份信息，改了会打不中或写错位置。

为什么换行写成 `\\r\\n`
----------------------
FO4 文本用 CRLF。yaml 的 block scalar（`|`）会**静默丢掉 CR**，
所以本脚本统一用双引号转义式 —— 详见 `yaml_codec.py` 的说明。
改译文时按同样的写法：`第一行\\r\\n第二行`。

> 与 `export_esp_texts.py`（产出 json，给 F4TCHook 字典线用）是**两个不同出口**：
> 那个走运行时替换，这个走直接改写文件。本脚本额外带上 apply 需要的定位信息。
"""

from __future__ import annotations

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))

from formid_strings_map import Plugin  # noqa: E402
from yaml_codec import dump, sort_entries  # noqa: E402

# 可翻译字段白名单 / 多值字段 —— 与 apply_esp_translation.py 保持一致
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from apply_esp_translation import TRANSLATABLE, MULTI_VALUE  # noqa: E402

TES4_FLAG_LOCALIZED = 0x00000080
TES4_FLAG_LIGHT = 0x00000200

# 超过这个条目数就建议拆分（单文件太大编辑器会卡）
SPLIT_THRESHOLD = 2000


def decode_text(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("utf-8", "replace")


def load_sst(sst_path: pathlib.Path) -> dict[str, str]:
    """从 xTranslator 词典取「原文 → 译文」，用于预填 dest。"""
    mod = pathlib.Path(__file__).resolve().parent.parent / "SST2XML" / "sst_to_xml.py"
    if not mod.exists():
        print(f"⚠️ 找不到 {mod}，跳过 SST 预填", file=sys.stderr)
        return {}
    sys.path.insert(0, str(mod.parent))
    from sst_to_xml import read_sst  # noqa: E402

    def has_cjk(s: str) -> bool:
        return any("\u4e00" <= ch <= "\u9fff" for ch in s)

    table: dict[str, str] = {}
    skipped = 0
    for e in read_sst(sst_path).entries:
        if not e.source or not e.dest or e.source == e.dest or not has_cjk(e.dest):
            skipped += 1
            continue
        table.setdefault(e.source, e.dest)
    print(f"  SST {sst_path.name}: 可用对照 {len(table)} 条（跳过未译 {skipped}）")
    return table


def export(esp_path: pathlib.Path, sst: dict[str, str] | None = None) -> dict:
    pl = Plugin(esp_path)
    blob = esp_path.read_bytes()
    flags = int.from_bytes(blob[8:12], "little")

    entries = []
    counters: dict[bytes, int] = {}
    for fid, sig, edid, fields in pl.iter_records():
        type_str = sig.decode("ascii", "replace").rstrip("\x00")
        local: dict[bytes, int] = {}
        for fsig, data in fields:
            fsig_str = fsig.decode("ascii", "replace").rstrip("\x00")
            if f"{type_str} {fsig_str}" not in TRANSLATABLE:
                continue
            ordinal = local.get(fsig, 0)
            local[fsig] = ordinal + 1
            if (type_str, fsig_str) not in MULTI_VALUE and ordinal > 0:
                continue
            if b"\x00" not in data:
                continue  # 不是 NUL 结尾的字符串，跳过
            src = decode_text(data)
            if not src.strip():
                continue
            key = (type_str, fsig_str, ordinal)
            counters[key] = counters.get(key, 0) + 1
            entries.append({
                "plugin": esp_path.name,
                "type": type_str,
                "form_id": f"0x{fid:08X}",
                "editor_id": edid or "",
                "field": fsig_str,
                "index": ordinal,
                "source": src,
                "dest": (sst or {}).get(src, ""),
            })

    return {
        "esp": esp_path.name,
        "path": str(esp_path),
        "localized": bool(flags & TES4_FLAG_LOCALIZED),
        "is_light": bool(flags & TES4_FLAG_LIGHT),
        "records": pl.record_count,
        "entries": entries,
    }


def write_yaml(data: dict, out: pathlib.Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    # 条目顺序：固定 `(form_id, type, field, index)` 升序（稳定排序）——
    # 约定在 yaml_codec.sort_entries()。
    data["entries"] = sort_entries(data["entries"])
    out.write_text(dump(data), encoding="utf-8")
    print(f"✅ {out}  ({len(data['entries'])} 条)")


def write_split(data: dict, outdir: pathlib.Path) -> None:
    """按记录类型拆成多个 yaml，避免单文件过大。"""
    outdir.mkdir(parents=True, exist_ok=True)
    bucket: dict[str, list] = {}
    for e in data["entries"]:
        bucket.setdefault(e["type"], []).append(e)
    meta = {k: v for k, v in data.items() if k != "entries"}
    for t in sorted(bucket):
        sub = dict(meta)
        sub["entries"] = sort_entries(bucket[t])
        sub["_split_of"] = data["esp"]
        sub["_record_type"] = t
        p = outdir / f"{data['esp'].rsplit('.', 1)[0]}_{t}.yaml"
        p.write_text(dump(sub), encoding="utf-8")
        print(f"✅ {p.name}  ({len(bucket[t])} 条)")
    print(f"\n共 {len(bucket)} 个文件 → {outdir}")


def main() -> int:
    ap = argparse.ArgumentParser(description="esp → yaml（给人改译文）")
    ap.add_argument("--esp", required=True)
    ap.add_argument("--out", required=True, help="输出 .yaml，或 --split 时的目录")
    ap.add_argument("--sst", help="xTranslator 词典，用于预填 dest")
    ap.add_argument("--split", action="store_true", help="按记录类型拆成多文件")
    ap.add_argument("--force", action="store_true", help="localized 插件也导出（只读，供参考）")
    args = ap.parse_args()

    esp = pathlib.Path(args.esp)
    if not esp.is_file():
        print(f"❌ 找不到 {esp}", file=sys.stderr)
        return 1
    if esp.read_bytes()[:4] != b"TES4":
        print("❌ 不是合法插件（缺 TES4 头）", file=sys.stderr)
        return 1

    sst = load_sst(pathlib.Path(args.sst)) if args.sst else None
    print(f"导出 {esp.name} ...")
    data = export(esp, sst)
    print(f"  记录 {data['records']}，可翻译条目 {len(data['entries'])}"
          + (f"（SST 预填 {sum(1 for e in data['entries'] if e['dest'])}）" if sst else ""))

    if data["localized"] and not args.force:
        print("\n⚠️ 这是**本地化**插件：文本在 STRINGS 文件里，本流程不适用。\n"
              "   要继续导出请加 --force（产物只读，不能回写）。", file=sys.stderr)
        return 1

    out = pathlib.Path(args.out)
    if args.split or len(data["entries"]) > SPLIT_THRESHOLD:
        if len(data["entries"]) > SPLIT_THRESHOLD and not args.split:
            print(f"   ⚠️ 条目数 {len(data['entries'])} > {SPLIT_THRESHOLD}，自动拆分")
        write_split(data, out)
    else:
        write_yaml(data, out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
