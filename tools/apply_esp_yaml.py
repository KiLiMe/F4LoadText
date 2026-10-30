#!/usr/bin/env python3
# ==============================================================================
# vendored: 上游副本 —— 来源 上游工作区 scripts/apply_esp_yaml.py
# 为何复制：让 F4LoadText 仓自包含，做汉化不依赖工作区上层 scripts/
# 同步方式：手动（上游仍是权威版本）；复制日期 2026-10-10
# ==============================================================================

"""yaml → esp：把改好的译文写回插件，产出汉化后的文件。

流程的第三棒：

    esp ──export_esp_yaml.py──▶ .yaml ──（人改 dest）──▶ **apply_esp_yaml.py** ──▶ 新 esp

用法
----
    # 单个 yaml
    uv run scripts/apply_esp_yaml.py --yaml "<mod>.yaml" --out "<mod>_cn.esp"

    # 拆分导出的整个目录（export 时加了 --split）
    uv run scripts/apply_esp_yaml.py --yaml-dir "<目录>" --out "<mod>_cn.esp"

    # 先演练
    uv run scripts/apply_esp_yaml.py --yaml "<mod>.yaml" --dry-run

    # 原地覆盖（先备份 .bak）
    uv run scripts/apply_esp_yaml.py --yaml "<mod>.yaml" --inplace

定位方式
--------
按 **(插件名, 类型, form_id, editor_id, 字段, 序号)** 定位到具体字段，
而不是按原文匹配。这样原文重复（比如十个按钮都叫 `Yes`）也不会写错位置。

`editor_id` 用于**兜底**：FormID 变了（mod 更新后）但 EditorID 没变时仍能命中。

安全
----
* 只改白名单 `TRANSLATABLE` 内的字段，其余原样拷贝。
* localized 插件拒绝写入（文本在 STRINGS 里，改字段 = 破坏数据）。
* 默认写新文件；`--inplace` 会先备份 `.bak`。
* 写盘后**自动回读校验**：记录数一致、字段序列未变、译文都已落盘。

底层二进制重建复用 `apply_esp_translation.py`（三层 size 链 / zlib / XXXX
这三个坑都在那里处理），本脚本只负责「译文 ↔ 字段」的定位。
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))

from yaml_codec import load_file  # noqa: E402
import apply_esp_translation as aet  # noqa: E402

TES4_FLAG_LOCALIZED = 0x00000080


def normalize_cr(text: str) -> str:
    """CR 归一化兜底。

    如果人在 yaml 里**用了 block scalar**（`|`），CR 会丢（见 yaml_codec 说明）。
    这里把「光秃秃的 LF」还原成 CRLF —— 因为 FO4 的文本一律用 CRLF，
    单独出现的 LF 几乎必然是 block scalar 造成的。

    ⚠️ 只在「文本里完全没有 \\r」时才动手；已经有 \\r 的（双引号转义式）原样不动。
    """
    if not text or "\r" in text:
        return text
    return text.replace("\n", "\r\n")


def build_lookup(docs: list[dict]) -> dict[tuple, str]:
    """(plugin, type, form_id, field, index) → 译文。"""
    out: dict[tuple, str] = {}
    for d in docs:
        for e in d.get("entries", []):
            dest = e.get("dest")
            if not dest or not str(dest).strip():
                continue
            if dest == e.get("source"):
                continue  # 没翻
            try:
                fid = int(str(e["form_id"]), 16)
            except (KeyError, ValueError):
                continue
            out[(e.get("plugin", ""), e.get("type", ""), fid,
                 e.get("field", ""), int(e.get("index", 0)))] = normalize_cr(str(dest))
    return out


def build_edid_lookup(docs: list[dict]) -> dict[tuple, str]:
    """(type, editor_id, field, index) → 译文。FormID 变了时的兜底。"""
    out: dict[tuple, str] = {}
    for d in docs:
        for e in d.get("entries", []):
            dest = e.get("dest")
            edid = e.get("editor_id")
            if not dest or not str(dest).strip() or not edid:
                continue
            if dest == e.get("source"):
                continue
            out[(e.get("type", ""), edid, e.get("field", ""),
                 int(e.get("index", 0)))] = normalize_cr(str(dest))
    return out


def collect(items: list, out: list) -> list:
    for it in items:
        if it[0] == "GRUP":
            collect(it[3], out)
        else:
            out.append(it[1])
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="yaml → esp（写回译文）")
    ap.add_argument("--yaml", help="单个 yaml 文件")
    ap.add_argument("--yaml-dir", help="export --split 产出的目录")
    ap.add_argument("--esp", help="源插件（默认取 yaml 里的 path）")
    ap.add_argument("--out", help="输出路径（默认 <原名>_cn.<ext>）")
    ap.add_argument("--inplace", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if not args.yaml and not args.yaml_dir:
        print("❌ 给 --yaml 或 --yaml-dir", file=sys.stderr)
        return 1

    # 1) 读 yaml
    docs: list[dict] = []
    if args.yaml:
        docs.append(load_file(pathlib.Path(args.yaml)))
    else:
        d = pathlib.Path(args.yaml_dir)
        files = sorted(d.glob("*.yaml")) + sorted(d.glob("*.yml"))
        if not files:
            print(f"❌ {d} 里没有 yaml", file=sys.stderr)
            return 1
        for f in files:
            docs.append(load_file(f))
    print(f"[1/5] 读入 {len(docs)} 个 yaml")

    lookup = build_lookup(docs)
    edid_lookup = build_edid_lookup(docs)
    if not lookup:
        print("❌ 没有填任何 dest（译文栏全空）", file=sys.stderr)
        return 1
    total_entries = sum(len(d.get("entries", [])) for d in docs)
    print(f"   条目 {total_entries}，有译文 {len(lookup)}")

    # 2) 定位源 esp
    esp = None
    if args.esp:
        esp = pathlib.Path(args.esp)
    else:
        for d in docs:
            p = d.get("path")
            if p and pathlib.Path(p).is_file():
                esp = pathlib.Path(p)
                break
    if esp is None or not esp.is_file():
        print("❌ 定位不到源 esp。用 --esp 指定。", file=sys.stderr)
        return 1

    blob = esp.read_bytes()
    if blob[:4] != b"TES4":
        print("❌ 不是合法插件", file=sys.stderr)
        return 1
    if int.from_bytes(blob[8:12], "little") & TES4_FLAG_LOCALIZED:
        print("❌ 本地化插件：文本在 STRINGS 里，改字段会破坏数据。", file=sys.stderr)
        return 1
    print(f"[2/5] 源插件 {esp.name} ({len(blob):,} B)")

    # 3) 解析
    items = aet.split_top_level(blob)
    recs = collect(items, [])
    n_comp = sum(1 for r in recs if r.was_compressed)
    print(f"[3/5] 解析：记录 {len(recs)}（压缩 {n_comp}）")

    # 4) 改写
    hit = miss = skipped = 0
    unresolved: list[str] = []
    for r in recs:
        type_str = r.sig.decode("ascii", "replace").rstrip("\x00")
        local: dict[bytes, int] = {}
        for f in r.fields:
            fsig = f.sig.decode("ascii", "replace").rstrip("\x00")
            if f"{type_str} {fsig}" not in aet.TRANSLATABLE:
                continue
            ordinal = local.get(f.sig, 0)
            local[f.sig] = ordinal + 1
            if (type_str, fsig) not in aet.MULTI_VALUE and ordinal > 0:
                continue
            if b"\x00" not in f.data:
                skipped += 1
                continue

            edid = ""
            for x in r.fields:
                if x.sig == b"EDID":
                    edid = x.data.split(b"\x00", 1)[0].decode("ascii", "replace")
                    break

            key = (esp.name, type_str, r.form_id, fsig, ordinal)
            dest = lookup.get(key)
            if dest is None:
                dest = edid_lookup.get((type_str, edid, fsig, ordinal))
            if dest is None:
                miss += 1
                if len(unresolved) < 5:
                    src = f.data.split(b"\x00", 1)[0].decode("utf-8", "replace")
                    unresolved.append(f"{type_str} {r.form_id:#010x} {fsig}#{ordinal} {src[:40]!r}")
                continue
            hit += 1
            if not args.dry_run:
                f.data = dest.encode("utf-8") + b"\x00"

    print(f"[4/5] 改写：命中 {hit}  未命中 {miss}  跳过(非文本) {skipped}")
    if hit == 0:
        print("\n⚠️ 一条都没命中。常见原因：\n"
              "   · yaml 里的 form_id 与 esp 不匹配（mod 更新过？）\n"
              "   · dest 栏是空的\n"
              "   · 字段不在可翻译白名单里")
        return 1
    if args.dry_run:
        print("\n[dry-run] 未写文件")
        return 0

    # 5) 序列化 + 校验
    out_path = esp if args.inplace else pathlib.Path(
        args.out or esp.with_name(f"{esp.stem}_cn{esp.suffix}"))
    new_blob = aet.serialize_top(items)
    print(f"[5/5] 序列化 {len(blob):,} → {len(new_blob):,} B ({len(new_blob)-len(blob):+,})")

    if args.inplace:
        bak = esp.with_suffix(esp.suffix + ".bak")
        shutil.copy2(esp, bak)
        print(f"   已备份 → {bak.name}")
    out_path.write_bytes(new_blob)

    problems = aet.verify(esp, out_path, {})
    if problems:
        print("\n❌ 校验失败：")
        for p in problems:
            print("   ", p)
        return 1
    print("   ✅ 结构自洽、记录一一对应")
    print(f"\n✅ 已写出: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
