#!/usr/bin/env python3
# ==============================================================================
# vendored: 上游副本 —— 来源 上游工作区 scripts/apply_esp_translation.py
# 为何复制：让 F4LoadText 仓自包含，做汉化不依赖工作区上层 scripts/
# 同步方式：手动（上游仍是权威版本）；复制日期 2026-10-10
# ==============================================================================

"""把译文直接写进 esp/esm/esl，产出一个**汉化后的插件文件**。

与 F4TCHook（运行时字典替换）是两条互补的路：

    F4TCHook  : 不动 esp，运行时按 (插件名, FormID, 签名, 序号) 查字典替换
    本脚本    : 直接改写 esp 的二进制文本字段，游戏读到什么就是什么

优势是覆盖 100%（不依赖任何 hook 路径），代价是 mod 一更新就要重跑。

用法
----
    # ① 从 F4TCHook 的 translations/*.yaml 取译文（推荐，可复用现有劳动）
    uv run scripts/apply_esp_translation.py --esp "<mod>.esp" --from-f4tchook

    # ② 从「原文<TAB>译文」对照表取译文
    uv run scripts/apply_esp_translation.py --esp "<mod>.esp" --table "<名字>.txt"

    # ③ 两者都给：先 yaml，再由 table 覆盖（table 优先）
    uv run scripts/apply_esp_translation.py --esp "<mod>.esp" --from-f4tchook --table "x.txt"

    # 先演练看会改什么
    uv run scripts/apply_esp_translation.py --esp "<mod>.esp" --from-f4tchook --dry-run

    # 原地覆盖（默认行为是写 <原名>_cn.esp，不碰原文件）
    uv run scripts/apply_esp_translation.py --esp "<mod>.esp" --from-f4tchook --inplace

输出默认写到 **源 esp 同目录** 的 `<原名>_cn.<ext>`。

三个必须处理对的地方（都是真实事故，见 F4TCHook/docs/F4TCHook-踩坑.md 的同名条目）
------------------------------------------------------------------------------
1. **三层 size 链**：子记录 u16 → 记录 u32 → GRUP u32。少改任何一层文件即损坏。
   归档的 `_archive/translate_esp.py` 就是漏了 GRUP 层而报废。
2. **zlib 压缩记录**：实测 Clarity.esp 的 341 条译文里有 **329 条在压缩记录内**。
   只改明文只能覆盖 3.5%。必须解压 → 改 → 重压。
   （重压**无法** byte-identical，引擎读的是解压结果，不影响正确性。）
3. **`XXXX` 超大子记录**：>64KB 的字段用 `XXXX` + u32 扩展长度，不是 u16。
   `formid_strings_map.Plugin._parse_body` 已处理，本脚本照同一规则重建。
4. **子记录自己的 u16 长度必须跟着改**：只替换 data 不更新 size ⇒ 新值**更短**时看不出来，
   更长时被旧 size **截断**。（实测 2026-10-11：14 条里有 4 条新值更长，末尾各被砍 1 字节，
   而**前 10 条完全正确** ⇒ 极易漏检。）本脚本走 `Field.serialize()` 重算，天然正确；
   手写一次性脚本时最容易踩。

安全约束
--------
* **只改白名单内的文本字段**（`TRANSLATABLE`），其余字段原样拷贝 ——
  绝不猜「这个 4 字节是不是 FormID」。
* **非本地化插件才改**：localized 插件的文本在 STRINGS 文件里，字段是 4 字节
  stringID，改写它是破坏数据。遇到直接报错退出。
* 默认写新文件，`--inplace` 才覆盖（且会先备份 `.bak`）。
* 改完自动**回读校验**：重解析产物，确认记录数一致、size 链自洽、译文都在。
"""

from __future__ import annotations

import argparse
import shutil
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from formid_strings_map import Plugin  # noqa: E402

# ── 常量 ────────────────────────────────────────────────────────────
GRUP = b"GRUP"
REC_HEADER = 24
SUB_HEADER = 6
FLAG_COMPRESSED = 0x00040000
TES4_FLAG_LOCALIZED = 0x00000080
ZLIB_LEVEL = 9  # 与官方 xEdit 一致；实测长度吻合原文件，字节不保证一致

# 可翻译字段白名单（照抄 export_esp_texts.py，权威来源是 FallHook 的映射表）
TRANSLATABLE = {
    "ACTI FULL", "ALCH FULL", "AMMO FULL", "APPA FULL", "ARMO FULL", "AVIF FULL",
    "NOTE FULL",
    "BOOK FULL", "CELL FULL", "CONT FULL", "CLAS FULL", "DOOR FULL", "ENCH FULL",
    "EXPL FULL", "FACT FULL", "FLOR FULL", "FLST FULL", "FURN FULL", "HAZD FULL",
    "INGR FULL", "KEYM FULL", "KYWD FULL", "LCTN FULL", "LIGH FULL", "MESG FULL",
    # HDPT FULL / CLFM FULL / CMPO FULL —— 三个继承 `TESFullName` 的签名，此前**漏在白名单外**
    #   （2026-10-11 全仓 160 个插件扫描发现；扫描方法：统计「值已是中文却不在白名单」+ 对照
    #    CommonLibF4 里所有 `public TESFullName` 的类）。
    #   · HDPT（BGSHeadPart）—— 头部件名（发型/眼睛…），LooksMenu 菜单里显示。实测全仓 354 条，
    #     `MiscHairstyle.esp` 131/131 已是中文（**mod 自带**，说明确实是给人看的）。
    #   · CLFM（BGSColorForm）—— 颜色名（发色等）。实测 120 条；`Looks Menu Customization
    #     Compendium` 原版英文、其 -CHS 汉化包 59/59 手工译成中文 ⇒ 上游汉化人**知道要改**，
    #     是**工具链**不知道 ⇒ 补上就能自动带走。
    #   · CMPO（BGSComponent）—— 组件名。实测 1 条（Dank_LEO.esp 已手工汉化）。
    #   ⚠️ 刻意**不补** SNCT FULL：它是音频分类（值形如 `$Footsteps`、
    #     `AudioCategoryWPNplayer`），技术标识、游戏 UI 不显示。实测 5 条。
    #   ✓ 三个都走 `TESFullName::LoadFullNameChunk`(2193215) ⇒ **运行时 `[汉化]` 也能覆盖**，
    #     不存在 KYWD NNAM 那种「白名单加了也没用」的问题。
    "HDPT FULL", "CLFM FULL", "CMPO FULL",
    # KYWD NNAM —— 改造界面的**槽位名**（xEdit 里叫 `NNAM - Display Name`）。
    # ⚠️ 只改 FULL 不够：UI（改造/拼装列表左列）实际读的是 NNAM。
    #    实测 2026-10-11：NukaLord-ApocalypseBodysuit.esp 的 14 条 KYWD
    #    FULL 已是中文，游戏里仍显示 'Materials - Pads' / 'Outfit Type' 等英文。
    "KYWD NNAM",
    "MGEF FULL", "MISC FULL", "PERK FULL", "PROJ FULL", "QUST FULL", "RACE FULL",
    # OMOD FULL —— 改造（模组）的名字，改造列表「目前模组」列显示的就是它。
    # 实测（2026-09-25）：本白名单此前**缺**这一项（A 线的 export_esp_texts.py
    # 早在 2026-09-19 就补了），导致 D 线导出的 yaml 里 OMOD:FULL 一条不剩
    # （Laserremeshredux 实测：esp 内 103 条 → 导出 0 条），改造列表名全漏。
    # 值与 MISC:FULL 不同：MISC:FULL 带武器前缀（'Laser Tri-Beam Splitter'），
    # OMOD:FULL 是裸名（'Beam Splitter' / 'Long Barrel'）。
    "OMOD FULL",
    "SCRL FULL", "SHOU FULL", "SLGM FULL", "SPEL FULL", "TACT FULL", "TREE FULL",
    "WATR FULL", "WEAP FULL", "WOOP FULL", "WRLD FULL",
    "NPC_ FULL", "MSTT FULL", "SCOL FULL", "STAT FULL", "TERM FULL", "IDLM FULL",
    "LSCR DESC", "MESG DESC", "DIAL FULL", "ALCH DESC", "AMMO DESC", "ARMO DESC",
    "AVIF DESC", "BOOK DESC", "COBJ DESC", "OMOD DESC", "PERK DESC", "RACE DESC",
    "SCRL DESC", "SHOU DESC", "SPEL DESC", "WEAP DESC", "COLL DESC",
    "MGEF DNAM", "ALCH DNAM", "AMMO ONAM", "AVIF ANAM", "NPC_ SHRT",
    "REGN RDMP", "WOOP TNAM",
    "BPTD BPTN", "FACT MNAM", "FACT FNAM", "DOOR CNAM", "DOOR ONAM",
    "GMST DATA", "REFR FULL", "QUST CNAM", "QUST NNAM",
    "NOTE CNAM",
    "BOOK CNAM", "INFO RNAM",
    "INNR WNAM", "LVLI ONAM", "MESG NNAM",
    "RACE FMRN", "RACE MPPN", "RACE TTGP",
    "TERM UNAM", "TERM BTXT", "TERM ITXT", "TERM RNAM", "TERM NAM0", "TERM WNAM",
    "ACTI ATTX", "ACTI RNAM", "FLOR ATTX", "FLOR RNAM", "FURN ATTX", "NPC_ ATTX",
    "MESG ITXT", "PERK EPF2", "PERK EPFD", "INFO NAM1",
}

# 同签名可出现多次 → 需要序号区分（与 export_esp_texts.py 保持一致）
MULTI_VALUE = {
    ("MESG", "ITXT"), ("INFO", "NAM1"),
    ("TERM", "BTXT"), ("TERM", "ITXT"), ("TERM", "RNAM"), ("TERM", "UNAM"),
    ("QUST", "NNAM"), ("QUST", "CNAM"),
    ("PERK", "EPF2"), ("PERK", "EPFD"),
    ("FACT", "MNAM"), ("FACT", "FNAM"),
    ("REFR", "FULL"), ("NPC_", "ATTX"),
}


# ── 译文来源 ────────────────────────────────────────────────────────
def load_table(path: Path) -> dict[str, str]:
    """读「原文<TAB>译文」对照表（与 F4TCHook 的 translations/*.txt 同格式）。

    `\r\n` 会解成真换行（该格式把换行写成转义，因为表是逐行的）。
    """
    table: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip("\n")
        if not line or line.startswith("#"):
            continue
        if "\t" not in line:
            continue
        src, _, dst = line.partition("\t")
        src = src.replace("\\r\\n", "\r\n").replace("\\n", "\n")
        dst = dst.replace("\\r\\n", "\r\n").replace("\\n", "\n")
        if src and dst:
            table[src] = dst
    return table


def load_from_f4tchook(esp_name: str, esp_path: Path) -> dict[str, str]:
    """从 F4TCHook/translations/*.yaml 取「原文 → 译文」。

    复用 F4TCHook 的现有劳动：yaml 里每条都有 `text`（原文）与 `dest`（译文）。
    按 yaml 声明的 `esp` 名匹配 —— 也允许用 --esp 的文件名兜底。

    ⚠️ 这里**只用它的原文/译文对**，不用它的 FormID。原因：写入靠的是
       遍历 esp 时遇到的字段值去查表（原文即键），不需要身份键；
       而 yaml 的 FormID 是给运行时字典用的，与写入无关。

    ⚠️ 必须走 yaml_codec，不能 `yaml.safe_load` —— 译文里的 `\\r\\n`
       一旦被 block scalar 吞掉 CR，写回 esp 就静默损坏长文本。
       见 yaml_codec.py 开头的实测说明。
    """
    tdir = Path(__file__).resolve().parent.parent / "F4TCHook" / "translations"
    if not tdir.is_dir():
        print(f"⚠️ 没有 {tdir}", file=sys.stderr)
        return {}

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import json
    import yaml_codec

    table: dict[str, str] = {}
    matched: list[str] = []
    files = (sorted(tdir.glob("*.yaml")) + sorted(tdir.glob("*.yml"))
             + sorted(tdir.glob("*.json")))
    for jf in files:
        try:
            raw = jf.read_text(encoding="utf-8")
            data = (yaml_codec.load(raw) if jf.suffix.lower() in (".yaml", ".yml")
                    else json.loads(raw))
        except Exception as exc:  # noqa: BLE001
            print(f"⚠️ 跳过 {jf.name}: {exc}", file=sys.stderr)
            continue
        if not isinstance(data, dict):
            continue
        if data.get("esp") != esp_name and jf.stem != esp_path.stem:
            continue
        matched.append(jf.name)
        for e in data.get("entries", []):
            if e.get("is_header"):
                continue
            src, dst = e.get("text"), e.get("dest")
            if src and dst:
                table[src] = dst

    print(f"   F4TCHook 译文: 匹配 {matched or '无'} → {len(table)} 条对照")
    return table


# ── 解析 ────────────────────────────────────────────────────────────
class Field:
    """一个子记录。xxxx=True 表示它带 XXXX 扩展长度前缀。"""

    __slots__ = ("sig", "data", "xxxx")

    def __init__(self, sig: bytes, data: bytes, xxxx: bool = False):
        self.sig = sig
        self.data = data
        self.xxxx = xxxx

    def serialize(self) -> bytes:
        n = len(self.data)
        if self.xxxx or n > 0xFFFF:
            # XXXX + u16(4) + u32(len) + sig + u16(0) + data
            return (b"XXXX" + struct.pack("<H", 4) + struct.pack("<I", n)
                    + self.sig + struct.pack("<H", 0) + self.data)
        return self.sig + struct.pack("<H", n) + self.data


def parse_fields(body: bytes) -> list[Field]:
    """解析记录体 → 字段列表。处理 XXXX 扩展长度。"""
    out: list[Field] = []
    pos = 0
    n = len(body)
    pending: int | None = None
    while pos + SUB_HEADER <= n:
        sig = body[pos:pos + 4]
        declared = int.from_bytes(body[pos + 4:pos + 6], "little")
        pos += SUB_HEADER
        if sig == b"XXXX":
            if declared == 4 and pos + 4 <= n:
                pending = int.from_bytes(body[pos:pos + 4], "little")
            pos += declared
            continue
        size = pending if pending is not None else declared
        pending = None
        data = body[pos:pos + size]
        out.append(Field(sig, data, xxxx=False))
        pos += size
    return out


class Record:
    __slots__ = ("sig", "flags", "form_id", "fields", "was_compressed", "tail")

    def __init__(self, sig, flags, form_id, fields, was_compressed, tail=b""):
        self.sig = sig
        self.flags = flags
        self.form_id = form_id
        self.fields = fields
        self.was_compressed = was_compressed
        # 记录头 +16..+24 的 8 字节：版本控制信息（VCF）等。
        # 游戏按这个值判断记录是否被本地修改过，**必须原样保留** ——
        # 填 0 会让整个文件与原始不一致（实测 3 个插件都栽在这）。
        self.tail = tail

    def serialize(self) -> bytes:
        body = b"".join(f.serialize() for f in self.fields)
        if self.was_compressed:
            payload = struct.pack("<I", len(body)) + zlib.compress(body, ZLIB_LEVEL)
            # 压缩记录的 size 是「4+压缩流」的长度
            return (self.sig + struct.pack("<I", len(payload))
                    + struct.pack("<II", self.flags | FLAG_COMPRESSED, self.form_id)
                    + self.tail + payload)
        return (self.sig + struct.pack("<I", len(body))
                + struct.pack("<II", self.flags & ~FLAG_COMPRESSED, self.form_id)
                + self.tail + body)


def split_top_level(blob: bytes):
    """把插件拆成顶层条目列表。

    返回 list，元素为：
      ("GRUP", label_bytes, group_type_bytes, [children...])   ← 递归
      ("REC",  Record)
    顶层顺序与结构必须 1:1 保留，否则 GRUP size 与 FormID 语义会错。
    """
    pos = 0
    n = len(blob)
    out: list = []

    # TES4 头（顶层，不是 GRUP）
    if blob[:4] == b"TES4":
        size = int.from_bytes(blob[4:8], "little")
        rec = _read_record(blob, 0)
        out.append(("REC", rec))
        pos = REC_HEADER + size

    while pos + REC_HEADER <= n:
        sig = blob[pos:pos + 4]
        size = int.from_bytes(blob[pos + 4:pos + 8], "little")
        if sig == GRUP:
            label = blob[pos + 8:pos + 12]
            gtype = blob[pos + 12:pos + 16]
            inner_end = pos + size
            children = _parse_children(blob, pos + REC_HEADER, inner_end)
            out.append(("GRUP", label, gtype, children, blob[pos + 16:pos + REC_HEADER]))
            pos = inner_end
        else:
            out.append(("REC", _read_record(blob, pos)))
            pos += REC_HEADER + size
    return out


def _parse_children(blob: bytes, start: int, end: int) -> list:
    """解析一个 GRUP 内的条目（可能嵌套子 GRUP）。"""
    out: list = []
    pos = start
    while pos + REC_HEADER <= end:
        sig = blob[pos:pos + 4]
        size = int.from_bytes(blob[pos + 4:pos + 8], "little")
        if sig == GRUP:
            label = blob[pos + 8:pos + 12]
            gtype = blob[pos + 12:pos + 16]
            inner_end = pos + size
            # 防御：size 异常时不要死循环
            if inner_end > end or size < REC_HEADER:
                inner_end = end
            out.append(("GRUP", label, gtype,
                        _parse_children(blob, pos + REC_HEADER, inner_end),
                        blob[pos + 16:pos + REC_HEADER]))
            pos = inner_end
        else:
            out.append(("REC", _read_record(blob, pos)))
            pos += REC_HEADER + size
    return out


def _read_record(blob: bytes, pos: int) -> Record:
    sig = blob[pos:pos + 4]
    size = int.from_bytes(blob[pos + 4:pos + 8], "little")
    flags = int.from_bytes(blob[pos + 8:pos + 12], "little")
    form_id = int.from_bytes(blob[pos + 12:pos + 16], "little")
    body = blob[pos + REC_HEADER:pos + REC_HEADER + size]
    was_compressed = bool(flags & FLAG_COMPRESSED)
    if was_compressed:
        try:
            body = zlib.decompress(body[4:])
        except zlib.error:
            try:
                body = zlib.decompress(body)
            except zlib.error as exc:
                raise ValueError(f"@{pos:#x} 记录 {sig!r} 解压失败: {exc}") from exc
    tail = blob[pos + 16:pos + REC_HEADER]
    return Record(sig, flags, form_id, parse_fields(body), was_compressed, tail)


def serialize_top(items: list) -> bytes:
    """重建二进制。GRUP 的 size = 24 + 全部子项序列化长度之和（自底向上）。"""
    buf = bytearray()
    for it in items:
        if it[0] == "REC":
            buf += it[1].serialize()
        else:
            _, label, gtype, children, _tail = it
            inner = serialize_top(children)
            buf += GRUP + struct.pack("<I", len(inner) + REC_HEADER) + label + gtype
            buf += it[4]  # GRUP 头 +16..+24 的 8 字节，原样保留
            buf += inner
    return bytes(buf)


# ── 改写 ────────────────────────────────────────────────────────────
def apply_translations(items: list, table: dict[str, str], stats: dict, dry: bool):
    """递归遍历所有记录，按白名单替换文本字段。"""
    for it in items:
        if it[0] == "GRUP":
            apply_translations(it[3], table, stats, dry)
            continue
        rec = it[1]
        type_str = rec.sig.decode("ascii", "replace").rstrip("\x00")
        counters: dict[bytes, int] = {}
        for f in rec.fields:
            fsig = f.sig.decode("ascii", "replace").rstrip("\x00")
            key = f"{type_str} {fsig}"
            if key not in TRANSLATABLE:
                continue
            ordinal = counters.get(f.sig, 0)
            counters[f.sig] = ordinal + 1
            if (type_str, fsig) not in MULTI_VALUE and ordinal > 0:
                continue

            # NUL 结尾的裸字符串
            if b"\x00" not in f.data:
                stats["skipped_binary"] += 1
                continue
            src = f.data.split(b"\x00", 1)[0].decode("utf-8", "replace")
            dst = table.get(src)
            if not dst:
                stats["miss"] += 1
                continue
            if dst == src:
                stats["unchanged"] += 1
                continue
            stats["hit"] += 1
            if not dry:
                f.data = dst.encode("utf-8") + b"\x00"


# ── 回读校验 ────────────────────────────────────────────────────────
def verify(orig: Path, new: Path, table: dict[str, str]) -> list[str]:
    """重解析产物，确认结构自洽且译文都在。返回问题列表。"""
    problems: list[str] = []
    o = split_top_level(orig.read_bytes())
    nw = split_top_level(new.read_bytes())

    def collect(items, acc):
        for it in items:
            if it[0] == "GRUP":
                collect(it[3], acc)
            else:
                r = it[1]
                acc.append((r.sig, r.form_id, [f.sig for f in r.fields]))
        return acc

    oa, na = collect(o, []), collect(nw, [])
    if len(oa) != len(na):
        problems.append(f"记录数不一致: 原 {len(oa)} vs 新 {len(na)}")
    for (osig, ofid, ofsig), (nsig, nfid, nfsig) in zip(oa, na):
        if (osig, ofid) != (nsig, nfid):
            problems.append(f"记录对不上: {osig!r}{ofid:#x} vs {nsig!r}{nfid:#x}")
        elif ofsig != nfsig:
            problems.append(f"字段序列变了: {osig!r} {ofid:#010x}")
    if problems:
        return problems

    # 译文是否都落盘了
    found: set[str] = set()

    def walk(items):
        for x in items:
            if x[0] == "GRUP":
                walk(x[3])
            else:
                for f in x[1].fields:
                    if b"\x00" in f.data:
                        found.add(f.data.split(b"\x00", 1)[0].decode("utf-8", "replace"))

    walk(nw)

    missing = [v for v in table.values() if v not in found]
    if missing:
        problems.append(f"有 {len(missing)} 条译文没在产物里找到，例: {missing[:3]}")
    return problems


# ── 主流程 ──────────────────────────────────────────────────────────
def main() -> int:
    ap = argparse.ArgumentParser(description="把译文写进 esp，产出汉化后的插件")
    ap.add_argument("--esp", required=True, help="源插件（esp/esm/esl）")
    ap.add_argument("--from-f4tchook", action="store_true",
                    help="从 F4TCHook/translations/*.yaml 取译文")
    ap.add_argument("--table", help="「原文<TAB>译文」对照表（.txt），优先级高于 yaml")
    ap.add_argument("--out", help="输出路径（默认 <原名>_cn.<ext>）")
    ap.add_argument("--inplace", action="store_true", help="原地覆盖（先备份 .bak）")
    ap.add_argument("--dry-run", action="store_true", help="只统计，不写文件")
    args = ap.parse_args()

    esp = Path(args.esp)
    if not esp.is_file():
        print(f"❌ 找不到 {esp}", file=sys.stderr)
        return 1

    blob = esp.read_bytes()
    if blob[:4] != b"TES4":
        print("❌ 不是合法的插件文件（缺少 TES4 头）", file=sys.stderr)
        return 1
    flags = int.from_bytes(blob[8:12], "little")
    if flags & TES4_FLAG_LOCALIZED:
        print("❌ 这是**本地化**插件：文本在 STRINGS 文件里，字段是 4 字节 stringID。\n"
              "   改写它会破坏数据。请改用 STRINGS 或 F4TCHook 字典线。", file=sys.stderr)
        return 1

    print(f"源插件: {esp}  ({len(blob):,} B)")

    # 1) 收集译文
    print("\n[1/4] 收集译文")
    table: dict[str, str] = {}
    if args.from_f4tchook:
        table.update(load_from_f4tchook(esp.name, esp))
    if args.table:
        t = load_table(Path(args.table))
        print(f"   对照表 {Path(args.table).name}: {len(t)} 条")
        table.update(t)          # table 优先
    if not table:
        print("❌ 没有可用译文（给 --from-f4tchook 或 --table）", file=sys.stderr)
        return 1
    print(f"   合计 {len(table)} 条对照")

    # 2) 解析
    print("\n[2/4] 解析插件结构")
    items = split_top_level(blob)
    n_rec = sum(1 for it in items if it[0] == "REC")
    n_comp = 0

    def count(items):
        nonlocal n_rec, n_comp
        for it in items:
            if it[0] == "GRUP":
                count(it[3])
            else:
                n_rec += 1
                if it[1].was_compressed:
                    n_comp += 1

    count(items)
    print(f"   顶层条目 {len(items)}，记录 {n_rec}（其中压缩 {n_comp}）")

    # 3) 改写
    print("\n[3/4] 改写文本字段")
    stats = {"hit": 0, "miss": 0, "unchanged": 0, "skipped_binary": 0}
    apply_translations(items, table, stats, dry=args.dry_run)
    print(f"   命中替换  = {stats['hit']}")
    print(f"   未匹配    = {stats['miss']}（原文不在对照表里）")
    print(f"   值相同    = {stats['unchanged']}")
    print(f"   跳过(非文本/无NUL) = {stats['skipped_binary']}")

    if stats["hit"] == 0:
        print("\n⚠️ 一条都没命中，不写文件。检查对照表的原文是否与 esp 逐字节一致。")
        return 1

    if args.dry_run:
        print("\n[dry-run] 未写文件")
        return 0

    # 4) 序列化 + 校验
    print("\n[4/4] 序列化并回读校验")
    out_path = esp if args.inplace else Path(args.out or esp.with_name(f"{esp.stem}_cn{esp.suffix}"))
    new_blob = serialize_top(items)
    print(f"   {len(blob):,} → {len(new_blob):,} B ({len(new_blob)-len(blob):+,})")

    if args.inplace:
        bak = esp.with_suffix(esp.suffix + ".bak")
        shutil.copy2(esp, bak)
        print(f"   已备份 → {bak}")
    out_path.write_bytes(new_blob)

    problems = verify(esp, out_path, table)
    if problems:
        print("\n❌ 校验失败：")
        for p in problems:
            print("   ", p)
        return 1
    print("   ✅ 结构自洽、记录一一对应、译文已落盘")
    print(f"\n✅ 已写出: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
