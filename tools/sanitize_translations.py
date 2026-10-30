#!/usr/bin/env python3
# ==============================================================================
# 发布前脱敏：把 translations\*.yaml 的 `path` 从「导出机的绝对路径」改成「文件名」
#
# 为什么必须改：yaml 是**可分发的数据**（跟着 DLL 一起发），而早期导出的 `path`
# 是 `X:\MO2\mods\...\xxx.esp` 这种导出机绝对路径 —— 泄露本机目录结构，换台机器
# 又必然指空。改成文件名后，运行时由 Translation::ResolveEspPath 依次回退到
# <Data>\<文件名>（MO2 的 USVFS 映射）与 DLL 旁目录，功能不受影响。
#
# 用法：
#   uv run python tools\sanitize_translations.py                    # 输出到 dist\translations
#   uv run python tools\sanitize_translations.py --check            # 只报不改
#   uv run python tools\sanitize_translations.py --out <目录>
#
# 只改 `path` 一个字段，其余行原样保留 —— 不走 yaml 重新序列化
# （那会重排键序、重算引号风格，把 diff 炸成整文件全变）。
# ==============================================================================

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import sys

PROJECT_ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = PROJECT_ROOT / "translations"
DST = PROJECT_ROOT / "dist" / "translations"

# 只匹配顶层 `path:` 那一行（顶格、无缩进）。导出脚本写的是单引号或裸值。
_PATH_RE = re.compile(r"^(path:[ \t]*)(.*)$", re.MULTILINE)


def sanitize_line(value: str) -> str:
    """把 `path` 的值剥成纯文件名，保留原来的引号风格。"""
    raw = value.strip()
    quote = ""
    if len(raw) >= 2 and raw[0] == raw[-1] and raw[0] in ("'", '"'):
        quote = raw[0]
        raw = raw[1:-1]
    name = re.split(r"[\\/]", raw.rstrip("\\/"))[-1] if raw else ""
    return f"{quote}{name}{quote}"


def sanitize_text(text: str) -> tuple[str, int]:
    changed = 0

    def sub(m: re.Match) -> str:
        nonlocal changed
        old = m.group(2)
        new = sanitize_line(old)
        if new != old:
            changed += 1
        return m.group(1) + new

    return _PATH_RE.sub(sub, text), changed


def main() -> int:
    ap = argparse.ArgumentParser(description="translations yaml 的 path 字段脱敏")
    ap.add_argument("--out", default=str(DST), help=f"输出目录（默认 {DST}）")
    ap.add_argument("--check", action="store_true", help="只检查，不写文件")
    ap.add_argument("--inplace", action="store_true", help="直接改源目录（不推荐）")
    args = ap.parse_args()

    if not SRC.is_dir():
        print(f"❌ 源目录不存在：{SRC}", file=sys.stderr)
        return 1

    files = sorted(list(SRC.glob("*.yaml")) + list(SRC.glob("*.yml")))
    if not files:
        print(f"❌ {SRC} 里没有 yaml", file=sys.stderr)
        return 1

    out = SRC if args.inplace else pathlib.Path(args.out)
    if not args.check and not args.inplace:
        out.mkdir(parents=True, exist_ok=True)

    total = 0
    dirty: list[tuple[pathlib.Path, int]] = []
    for src in files:
        text, n = sanitize_text(src.read_text(encoding="utf-8"))
        total += n
        if n:
            dirty.append((src, n))
        if args.check or args.inplace:
            if n and args.inplace:
                src.write_text(text, encoding="utf-8")
        else:
            (out / src.name).write_text(text, encoding="utf-8")

    # 非 yaml 的伴生文件（如 scaleform_literals.tsv）原样拷过去
    if not args.check and not args.inplace:
        for extra in SRC.iterdir():
            if extra.is_file() and extra.suffix not in (".yaml", ".yml"):
                shutil.copy2(extra, out / extra.name)

    mode = "检查" if args.check else ("就地" if args.inplace else f"输出到 {out}")
    print(f"═══ 译文脱敏（{mode}）═══")
    print(f"  文件 {len(files)} 个，改写 path {total} 行")
    for p, n in dirty:
        print(f"    {p.name:<42} {n} 行")
    if not args.check and not args.inplace:
        _report(out)
    if args.check and total:
        print("\n⚠️ 存在未脱敏的 path（上面这些）。去掉 --check 生成脱敏副本。")
        return 1
    return 0


def _report(out: pathlib.Path) -> None:
    import sys as _sys

    _sys.path.insert(0, str(pathlib.Path(__file__).parent))
    from yaml_codec import load_file

    leaked = []
    n = 0
    for p in sorted(out.glob("*.y*ml")):
        n += 1
        d = load_file(p)
        v = str(d.get("path") or "")
        if re.search(r"[A-Za-z]:[\\/]|^/|\\\\", v):
            leaked.append((p.name, v))
    print(f"\n  输出 {n} 个文件 → {out}")
    if leaked:
        print("  ❌ 仍有绝对路径：")
        for name, v in leaked:
            print(f"    {name}: {v}")
    else:
        print("  ✅ 复查：所有 path 都只剩文件名")


if __name__ == "__main__":
    raise SystemExit(main())
