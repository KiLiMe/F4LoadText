#!/usr/bin/env python3
# ==============================================================================
# vendored: 上游副本 —— 来源 上游工作区 scripts/formid_strings_map.py
# 为何复制：让 F4LoadText 仓自包含，做汉化不依赖工作区上层 scripts/
# 同步方式：手动（上游仍是权威版本）；复制日期 2026-10-10
# ==============================================================================

"""
Fallout 4  FormID <-> .STRINGS 文本映射 与 EN/CN 翻译对比工具
=============================================================

两步核心逻辑：
  1. 解析 ESP/ESM/ESL 插件：定位记录 -> 取出 FULL / DESC 等文本字段里的 String ID
  2. 拿 String ID 去 .STRINGS / .DLSTRINGS / .ILSTRINGS 字典里查文本

并额外做 EN <-> CN 双语言包对比，输出「已翻译 / 未翻译 / 缺失」报表。

用法示例：
  # 扫描全部插件（慢，Fallout4.esm 约 174 万条记录）
  uv run formid_strings_map.py

  # 只扫几个插件 + 只看 WEAP/ARMO 的 FULL/DESC
  uv run formid_strings_map.py --plugins Fallout4.esm DLCCoast.esm \
      --record-type WEAP ARMO --field FULL DESC

  # 只打印「还没翻译」的条目
  uv run formid_strings_map.py --plugins ccBGSFO4115-X02.esl --only untranslated

  # 顺便把结果落盘
  uv run formid_strings_map.py --plugins DLCworkshop01.esm --json out/map.json --csv out/map.csv
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import struct
import sys
import zlib
from pathlib import Path

# ---------------------------------------------------------------- 默认路径

# ⚠️ 本机专属路径**不写进源码**（本仓是公开的）。
# 取值顺序：命令行参数 > 环境变量 > 仓根 .f4lt_local.json > 空（由 main() 报错）。
# 详见 tools/local_paths.py。
sys.path.insert(0, str(Path(__file__).resolve().parent))
import local_paths  # noqa: E402  （同目录模块）

DEFAULT_DATA_DIR = local_paths.local_path("game_data_dir") or ""
DEFAULT_STRINGS_DIR = local_paths.local_path("strings_dir") or ""

# ---------------------------------------------------------------- 二进制常量

RECORD_HEADER_SIZE = 24
GRUP_HEADER_SIZE = 24
SUB_HEADER_SIZE = 6

FLAG_COMPRESSED = 0x00040000       # 记录数据被 zlib 压缩
TES4_FLAG_LOCALIZED = 0x00000080   # TES4 头：启用本地化（文本存 String ID 而非原文）

# 默认抽取的文本字段（Bethesda 约定）
DEFAULT_TEXT_FIELDS = ("FULL", "DESC", "SHRT", "NAM1", "NNAM")

# 查表顺序：STRINGS -> DLSTRINGS -> ILSTRINGS
TABLE_ORDER = ("STRINGS", "DLSTRINGS", "ILSTRINGS")

# 判定「像文本」的可打印字节
PRINTABLE = set(range(0x20, 0x7F)) | {0x09, 0x0A, 0x0D}
CJK_RE = re.compile(r"[\u4e00-\u9fff\u3040-\u30ff]")


# ---------------------------------------------------------------- 编码

def decode_text(raw: bytes, encoding: str) -> str:
    """按用户指定的编码策略把字节解成字符串。"""
    if encoding == "auto":
        for enc in ("utf-8", "cp1252", "gbk"):
            try:
                return raw.decode(enc)
            except UnicodeDecodeError:
                continue
        return raw.decode("utf-8", errors="replace")
    return raw.decode(encoding, errors="replace")


def looks_like_text(raw: bytes) -> bool:
    """非本地化插件里，判断一段字节是不是人类可读文本（用来排除二进制噪声）。"""
    if not raw:
        return False
    good = sum(1 for b in raw if b in PRINTABLE)
    return good / len(raw) > 0.85


def cjk_ratio(text: str) -> float:
    if not text:
        return 0.0
    return sum(1 for ch in text if CJK_RE.match(ch)) / len(text)


# ---------------------------------------------------------------- .STRINGS 解析

class StringsTable:
    """解析单个 .STRINGS / .DLSTRINGS / .ILSTRINGS 文件。

    布局：
        uint32  count        目录条目数
        uint32  dataSize     数据区字节数
        count x { uint32 id; uint32 offset }   目录（按 id 升序）
        dataSize 字节的数据区
            STRINGS   : 以 NUL 结尾的字节串
            DL/IL     : uint32 length + length 字节
    """

    __slots__ = ("kind", "path", "entries", "count", "data_size", "error")

    def __init__(self, path: Path, kind: str):
        self.kind = kind
        self.path = path
        self.entries: dict[int, bytes] = {}
        self.count = 0
        self.data_size = 0
        self.error: str | None = None
        if path.exists():
            self._load()
        else:
            self.error = "文件不存在"

    def _load(self) -> None:
        try:
            blob = self.path.read_bytes()
        except OSError as exc:
            self.error = f"读取失败: {exc}"
            return
        if len(blob) < 8:
            self.error = "文件过小"
            return

        self.count, self.data_size = struct.unpack_from("<II", blob, 0)
        base = 8 + self.count * 8
        if base > len(blob):
            self.error = "目录区越界"
            return
        end = base + self.data_size
        if end > len(blob):
            self.error = f"数据区越界 (声明 {end} > 实际 {len(blob)})"
            return

        for i in range(self.count):
            sid, off = struct.unpack_from("<II", blob, 8 + i * 8)
            pos = base + off
            if pos >= len(blob):
                self.entries[sid] = b""
                continue
            if self.kind == "STRINGS":
                stop = blob.find(b"\x00", pos)
                if stop < 0:
                    stop = len(blob)
                self.entries[sid] = blob[pos:stop]
            else:
                if pos + 4 > len(blob):
                    self.entries[sid] = b""
                    continue
                length = struct.unpack_from("<I", blob, pos)[0]
                if pos + 4 + length > end:
                    length = max(0, end - pos - 4)
                self.entries[sid] = blob[pos + 4 : pos + 4 + length].rstrip(b"\x00")

    def __len__(self) -> int:
        return len(self.entries)

    def get(self, sid: int) -> bytes | None:
        return self.entries.get(sid)


class LanguagePack:
    """某个插件的某种语言包：STRINGS + DLSTRINGS + ILSTRINGS。"""

    def __init__(self, strings_dir: Path, plugin_stem: str, lang: str):
        self.lang = lang
        self.tables: dict[str, StringsTable] = {}
        for kind in TABLE_ORDER:
            path = self._find(strings_dir, plugin_stem, lang, kind)
            self.tables[kind] = StringsTable(path, kind)

    @staticmethod
    def _find(strings_dir: Path, stem: str, lang: str, kind: str) -> Path:
        """Windows 文件系统大小写不敏感，但仍做一次大小写无关查找以防万一。"""
        exact = strings_dir / f"{stem}_{lang}.{kind}"
        if exact.exists():
            return exact
        target = f"{stem}_{lang}.{kind}".lower()
        if strings_dir.is_dir():
            for p in strings_dir.iterdir():
                if p.name.lower() == target:
                    return p
        return exact  # 保留原路径，让 StringsTable 报「文件不存在」

    @property
    def available(self) -> bool:
        return any(len(t) > 0 for t in self.tables.values())

    @property
    def total(self) -> int:
        return sum(len(t) for t in self.tables.values())

    def lookup(self, sid: int) -> tuple[str | None, str | None]:
        """返回 (命中的表名, 原始字节)。三个表都查不到时返回 (None, None)。"""
        for kind in TABLE_ORDER:
            raw = self.tables[kind].get(sid)
            if raw is not None:
                return kind, raw
        return None, None

    def report_errors(self, plugin_name: str) -> list[str]:
        out = []
        for kind, table in self.tables.items():
            if table.error and table.error != "文件不存在":
                out.append(f"    ! {plugin_name} [{self.lang}] {kind}: {table.error}")
        return out


# ---------------------------------------------------------------- 插件解析

class Plugin:
    """流式解析 ESP / ESM / ESL，产出 (form_id, record_type, edid, field -> data)。"""

    def __init__(self, path: Path):
        self.path = path
        self.name = path.name
        self.blob = path.read_bytes()
        self.localized = False
        self.num_records_declared = 0
        self.record_count = 0
        self.compressed_count = 0
        self.warnings: list[str] = []
        self._read_tes4()

    def _read_tes4(self) -> None:
        if len(self.blob) < RECORD_HEADER_SIZE or self.blob[:4] != b"TES4":
            self.warnings.append("不是合法的插件文件（缺少 TES4 头）")
            return
        size = int.from_bytes(self.blob[4:8], "little")
        flags = int.from_bytes(self.blob[8:12], "little")
        self.localized = bool(flags & TES4_FLAG_LOCALIZED)
        body = self.blob[RECORD_HEADER_SIZE : RECORD_HEADER_SIZE + size]
        pos = 0
        while pos + SUB_HEADER_SIZE <= len(body):
            sig = body[pos : pos + 4]
            subsize = int.from_bytes(body[pos + 4 : pos + 6], "little")
            data = body[pos + SUB_HEADER_SIZE : pos + SUB_HEADER_SIZE + subsize]
            if sig == b"HEDR" and len(data) >= 12:
                self.num_records_declared = struct.unpack("<I", data[4:8])[0]
            pos += SUB_HEADER_SIZE + subsize

    # -- 记录遍历 ---------------------------------------------------

    def iter_records(self, wanted_types: set[bytes] | None = None):
        """产出 (form_id, record_type, edid, [(field_sig, field_data), ...])。

        wanted_types 为 None 时不过滤；给出时会在解压之前就跳过无关记录类型，
        能显著加速大文件扫描。
        """
        buf = self.blob
        n = len(buf)
        # 先跳过 TES4：24 字节头 + 它声明的数据长度（探针已验证 TES4 后紧跟 GRUP）
        pos = RECORD_HEADER_SIZE + int.from_bytes(buf[4:8], "little")

        while pos + RECORD_HEADER_SIZE <= n:
            sig = buf[pos : pos + 4]
            size = int.from_bytes(buf[pos + 4 : pos + 8], "little")

            if sig == b"GRUP":
                # GRUP 是容器，size 包含自身 24 字节头。
                # 必须只跳过头、走进内部，否则会整块跳过里面所有的记录。
                if size < GRUP_HEADER_SIZE:
                    self.warnings.append(f"偏移 {pos}: GRUP size={size} 异常，中止")
                    return
                pos += GRUP_HEADER_SIZE
                continue

            if pos + RECORD_HEADER_SIZE + size > n:
                self.warnings.append(f"偏移 {pos}: 记录 {sig!r} 越界，中止")
                return

            self.record_count += 1
            flags = int.from_bytes(buf[pos + 8 : pos + 12], "little")
            form_id = int.from_bytes(buf[pos + 12 : pos + 16], "little")
            body = buf[pos + RECORD_HEADER_SIZE : pos + RECORD_HEADER_SIZE + size]

            if wanted_types is None or sig in wanted_types:
                if flags & FLAG_COMPRESSED:
                    self.compressed_count += 1
                    body = self._decompress(body)
                    if body is None:
                        pos += RECORD_HEADER_SIZE + size
                        continue
                edid, fields = self._parse_body(body)
                yield form_id, sig, edid, fields

            pos += RECORD_HEADER_SIZE + size

    @staticmethod
    def _decompress(body: bytes) -> bytes | None:
        """FO4 压缩记录：4 字节原始长度 + zlib 流。"""
        if len(body) < 4:
            return None
        try:
            return zlib.decompress(body[4:])
        except zlib.error:
            # 少数生成器不写前缀长度，直接试着解整个 body
            try:
                return zlib.decompress(body)
            except zlib.error:
                return None

    @staticmethod
    def _parse_body(body: bytes) -> tuple[str | None, list[tuple[bytes, bytes]]]:
        edid = None
        fields: list[tuple[bytes, bytes]] = []
        pos = 0
        n = len(body)
        pending_size: int | None = None  # XXXX 超大子记录

        while pos + SUB_HEADER_SIZE <= n:
            sig = body[pos : pos + 4]
            declared = int.from_bytes(body[pos + 4 : pos + 6], "little")
            pos += SUB_HEADER_SIZE

            if sig == b"XXXX":
                if declared == 4 and pos + 4 <= n:
                    pending_size = int.from_bytes(body[pos : pos + 4], "little")
                pos += declared
                continue

            size = pending_size if pending_size is not None else declared
            pending_size = None

            if pos + size > n:
                size = n - pos
            data = body[pos : pos + size]
            pos += size

            if sig == b"EDID" and edid is None:
                edid = data.split(b"\x00", 1)[0].decode("ascii", errors="replace")
            else:
                fields.append((sig, data))

        return edid, fields


# ---------------------------------------------------------------- 映射主流程

class Mapper:
    def __init__(self, data_dir: Path, strings_dir: Path, args):
        self.data_dir = data_dir
        self.strings_dir = strings_dir
        self.args = args
        self.encoding = args.encoding
        self.wanted_fields = {f.encode("ascii") for f in args.field}
        self.wanted_types = (
            {t.encode("ascii").ljust(4) for t in args.record_type}
            if args.record_type
            else None
        )
        self._pack_cache: dict[tuple[str, str], LanguagePack] = {}

    # -- 语言包 -----------------------------------------------------

    def pack(self, plugin_stem: str, lang: str) -> LanguagePack:
        key = (plugin_stem, lang)
        if key not in self._pack_cache:
            self._pack_cache[key] = LanguagePack(self.strings_dir, plugin_stem, lang)
        return self._pack_cache[key]

    # -- 单条记录的文本抽取 ------------------------------------------

    def extract(self, plugin: Plugin, form_id: int, rec_type: bytes,
                edid: str | None, fields: list[tuple[bytes, bytes]],
                en: LanguagePack, cn: LanguagePack) -> list[dict]:
        rows: list[dict] = []
        for sig, data in fields:
            if sig not in self.wanted_fields:
                continue

            if plugin.localized:
                # 本地化插件：4 字节 = String ID；否则是内嵌原文
                if len(data) != 4:
                    if not looks_like_text(data):
                        continue
                    string_id = None
                    raw_en = data
                    raw_cn = None
                    table = "内嵌"
                else:
                    string_id = int.from_bytes(data, "little")
                    table, raw_en = en.lookup(string_id)
                    if string_id is not None:
                        _, raw_cn = cn.lookup(string_id)
            else:
                if not looks_like_text(data):
                    continue
                string_id = None
                table = "内嵌"
                raw_en = data
                raw_cn = None

            if raw_en is None and string_id is None:
                continue

            # SID 0x00000000 是 Bethesda 的“空引用”（该字段没有文本），不是真的查表失败
            if string_id == 0:
                if self._passes_filter("空引用"):
                    rows.append({
                        "form_id": f"{form_id:08X}",
                        "plugin": plugin.name,
                        "type": rec_type.decode("ascii", errors="replace"),
                        "editor_id": edid or "",
                        "field": sig.decode("ascii", errors="replace"),
                        "string_id": "00000000",
                        "table": "",
                        "en": "",
                        "cn": "",
                        "state": "空引用",
                        "note": "String ID = 0，该字段无文本",
                    })
                continue

            text_en = decode_text(raw_en, self.encoding) if raw_en is not None else ""
            text_cn = decode_text(raw_cn, self.encoding) if raw_cn is not None else None

            state, note = self._classify(string_id, raw_en, raw_cn, text_en, text_cn, en, cn)

            if not self._passes_filter(state):
                continue

            rows.append({
                "form_id": f"{form_id:08X}",
                "plugin": plugin.name,
                "type": rec_type.decode("ascii", errors="replace"),
                "editor_id": edid or "",
                "field": sig.decode("ascii", errors="replace"),
                "string_id": f"{string_id:08X}" if string_id is not None else "",
                "table": table or "",
                "en": text_en,
                "cn": text_cn if text_cn is not None else "",
                "state": state,
                "note": note,
            })
        return rows

    def _classify(self, string_id, raw_en, raw_cn, text_en, text_cn, en, cn) -> tuple[str, str]:
        """给一条文本贴上翻译状态标签。"""
        if string_id is None:
            return "内嵌原文", "插件内直接存文本，不走 STRINGS"
        if raw_en is None:
            return "无原文", "EN 表里查不到该 String ID"
        if not text_en.strip():
            return "空原文", "EN 原文为空"
        if raw_cn is None:
            return "缺失", "CN 表里没有这个 String ID"
        if not cn.available:
            return "无CN包", "找不到该插件的 CN 语言包"
        if raw_cn == raw_en:
            # CN 与 EN 字节完全一致。注意：有些工程的 EN 包本身就是被回填过的中文，
            # 所以先按「是否含中日文字符」判断，避免把已经是中文的条目误报成未翻译。
            if cjk_ratio(text_cn) > 0:
                return "已翻译", "EN/CN 一致且已含中文"
            if not text_cn.strip():
                return "译文为空", "CN 条目存在但内容为空"
            return "未翻译", "CN 与 EN 完全一致且无中文"
        if not text_cn.strip():
            return "译文为空", "CN 条目存在但内容为空"
        if cjk_ratio(text_cn) == 0:
            return "疑似未译", "CN 内容不含任何中日文字符"
        return "已翻译", ""

    def _passes_filter(self, state: str) -> bool:
        mode = self.args.only
        if mode == "all":
            return True
        if mode == "untranslated":
            return state in ("未翻译", "缺失", "疑似未译", "译文为空", "无CN包")
        if mode == "issues":
            # 真正需要人去看的：查表失败或明显没翻
            return state in ("未翻译", "缺失", "疑似未译", "译文为空", "无CN包", "无原文")
        if mode == "translated":
            return state == "已翻译"
        return True

    # -- 扫描 --------------------------------------------------------

    def scan_plugin(self, path: Path) -> tuple[list[dict], dict]:
        plugin = Plugin(path)
        stem = path.stem
        en = self.pack(stem, "EN")
        cn = self.pack(stem, "CN")

        stats = {
            "plugin": path.name,
            "size_mb": round(path.stat().st_size / 1048576, 1),
            "localized": plugin.localized,
            "records_scanned": 0,
            "compressed": 0,
            "en_strings": en.total,
            "cn_strings": cn.total,
            "warnings": list(plugin.warnings),
        }

        if not en.available and not cn.available:
            stats["warnings"].append("EN 与 CN 语言包都不存在，跳过文本查表")

        rows: list[dict] = []
        seen: set[tuple[str, str]] = set()

        for form_id, rec_type, edid, fields in plugin.iter_records(self.wanted_types):
            for row in self.extract(plugin, form_id, rec_type, edid, fields, en, cn):
                key = (row["form_id"], row["field"])
                if key in seen:
                    continue
                seen.add(key)
                rows.append(row)
                if self.args.max_rows and len(rows) >= self.args.max_rows:
                    break
            if self.args.max_rows and len(rows) >= self.args.max_rows:
                break

        stats["records_scanned"] = plugin.record_count
        stats["compressed"] = plugin.compressed_count
        stats["warnings"].extend(en.report_errors(path.name))
        stats["warnings"].extend(cn.report_errors(path.name))
        return rows, stats


# ---------------------------------------------------------------- 输出

STATE_ORDER = ["已翻译", "未翻译", "疑似未译", "缺失", "译文为空", "无CN包",
               "内嵌原文", "空原文", "空引用", "无原文"]


def print_console(rows: list[dict], stats: list[dict], args) -> None:
    def line(ch="=", w=118):
        print(ch * w)

    line()
    print("Fallout 4   FormID <-> STRINGS 映射 / 翻译对比")
    line()
    print(f"数据目录   : {args.data_dir}")
    print(f"语言包目录 : {args.strings_dir}")
    print(f"抽取字段   : {', '.join(args.field)}")
    print(f"记录类型   : {', '.join(args.record_type) if args.record_type else '全部'}")
    print(f"过滤模式   : {args.only}")
    line()

    print("\n[ 插件概况 ]")
    print(f"{'插件':<34}{'大小MB':>8}{'记录数':>10}{'压缩':>8}{'本地化':>7}{'EN文本':>9}{'CN文本':>9}")
    print("-" * 118)
    for s in stats:
        print(f"{s['plugin']:<34}{s['size_mb']:>8}{s['records_scanned']:>10}"
              f"{s['compressed']:>8}{'是' if s['localized'] else '否':>7}"
              f"{s['en_strings']:>9}{s['cn_strings']:>9}")
    for s in stats:
        for w in s["warnings"]:
            print(f"    ! {s['plugin']}: {w}")

    counter: dict[str, int] = {}
    for r in rows:
        counter[r["state"]] = counter.get(r["state"], 0) + 1

    total = len(rows)
    print(f"\n[ 翻译状态统计 ]  共 {total} 条文本")
    print("-" * 118)
    for state in STATE_ORDER:
        if state in counter:
            c = counter[state]
            pct = c / total * 100 if total else 0
            print(f"  {state:<8} {c:>8}  ({pct:5.1f})  {'#' * int(pct / 2.5)}")

    if total:
        done = counter.get("已翻译", 0)
        print(f"\n  翻译完成度（已翻译 / 需查表条目）: "
              f"{done} / {total - counter.get('内嵌原文', 0)}")

    show = rows[: args.sample]
    if not show:
        print("\n（没有符合过滤条件的条目）")
        return

    print(f"\n[ 映射明细 ]  显示前 {len(show)} 条")
    print("-" * 118)
    print(f"{'FormID':<10}{'类型':<6}{'字段':<6}{'SID':<10}{'表':<5}{'状态':<8}EN / CN")
    print("-" * 118)
    for r in show:
        table = {"STRINGS": "STR", "DLSTRINGS": "DL", "ILSTRINGS": "IL"}.get(r["table"], r["table"] or "-")
        en_t = r["en"].replace("\r", " ").replace("\n", " ")[:44]
        cn_t = (r["cn"] or "-").replace("\r", " ").replace("\n", " ")[:44]
        print(f"{r['form_id']:<10}{r['type']:<6}{r['field']:<6}{r['string_id'] or '-':<10}"
              f"{table:<5}{r['state']:<8}{en_t}")
        if r["cn"]:
            print(f"{'':<45}└→ CN: {cn_t}")
    line()


def write_json(rows: list[dict], stats: list[dict], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "summary": stats,
        "count": len(rows),
        "entries": rows,
    }
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"JSON 已写入 : {path}")


def write_csv(rows: list[dict], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    cols = ["form_id", "plugin", "type", "editor_id", "field",
            "string_id", "table", "state", "en", "cn", "note"]
    with path.open("w", encoding="utf-8-sig", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    print(f"CSV  已写入 : {path}")


# ---------------------------------------------------------------- CLI

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Fallout 4 FormID <-> .STRINGS 文本映射与 EN/CN 翻译对比",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--data-dir", default=DEFAULT_DATA_DIR, help="插件目录 (Data)")
    p.add_argument("--strings-dir", default=DEFAULT_STRINGS_DIR, help="语言包目录 (Strings)")
    p.add_argument("--plugins", nargs="*", default=None,
                   help="只扫描这些插件文件名；默认扫描目录下全部 .esm/.esp/.esl")
    p.add_argument("--record-type", nargs="*", default=None,
                   help="只处理这些记录类型，如 WEAP ARMO NPC_（能大幅提速）")
    p.add_argument("--field", nargs="*", default=list(DEFAULT_TEXT_FIELDS),
                   help=f"抽取的文本字段，默认 {' '.join(DEFAULT_TEXT_FIELDS)}")
    p.add_argument("--only", choices=["all", "untranslated", "translated", "issues"], default="all",
                   help="只输出指定翻译状态的条目；issues=需要人工关注的")
    p.add_argument("--encoding", default="auto",
                   help="文本解码：auto（utf-8→cp1252→gbk）/ utf-8 / gbk / cp1252 ...")
    p.add_argument("--sample", type=int, default=30, help="控制台打印的明细条数")
    p.add_argument("--max-rows", type=int, default=0, help="每个插件最多收多少条（0=不限，调试用）")
    p.add_argument("--json", dest="json_out", default=None, help="结果 JSON 输出路径")
    p.add_argument("--csv", dest="csv_out", default=None, help="结果 CSV 输出路径")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    data_dir = Path(args.data_dir)
    strings_dir = Path(args.strings_dir)
    # 目录不存在时给出**怎么配**，而不只是报错 —— 路径不再硬编码在源码里。
    def _missing(label, path, flag, key, env):
        print(f"错误：{label}不存在 -> {path or '（未配置）'}", file=sys.stderr)
        print("     这是本机专属路径，不写在源码里。任选一种方式提供：", file=sys.stderr)
        print(f"       1) 命令行  {flag} <路径>", file=sys.stderr)
        print(f"       2) 环境变量  {env}=<路径>", file=sys.stderr)
        print(f'       3) 本地配置  {local_paths.LOCAL_CONFIG_PATH} 里的 "{key}"', file=sys.stderr)
        return 1

    if not data_dir.is_dir():
        return _missing("数据目录 (Data)", data_dir, "--data-dir", "game_data_dir", "F4LT_GAME_DATA_DIR")
    if not strings_dir.is_dir():
        return _missing("语言包目录 (Strings)", strings_dir, "--strings-dir", "strings_dir", "F4LT_STRINGS_DIR")

    if args.plugins:
        paths = [data_dir / n for n in args.plugins]
        missing = [str(p) for p in paths if not p.exists()]
        if missing:
            print("错误：找不到插件 -> " + ", ".join(missing), file=sys.stderr)
            return 1
        # 保持用户给的先后顺序
    else:
        found = [p for ext in ("*.esm", "*.esp", "*.esl") for p in data_dir.glob(ext)]
        paths = sorted(found, key=lambda p: -p.stat().st_size)

    mapper = Mapper(data_dir, strings_dir, args)

    all_rows: list[dict] = []
    all_stats: list[dict] = []
    for path in paths:
        print(f"  扫描 {path.name} ...", flush=True)
        rows, stats = mapper.scan_plugin(path)
        all_rows.extend(rows)
        all_stats.append(stats)

    print()
    print_console(all_rows, all_stats, args)

    if args.json_out:
        write_json(all_rows, all_stats, Path(args.json_out))
    if args.csv_out:
        write_csv(all_rows, Path(args.csv_out))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
