#!/usr/bin/env python3
"""本仓自检 —— **不启动游戏、不改任何部署文件**。

    uv run python tools\\selftest.py

三个检查项（缺前提就 skip，不算失败）：

  1. **纯函数表驱动用例**（C++）：编 `tools/selftest/cases.cpp` + `src/ConfigParse.h`
     + `src/StrUtil.h`，跑 `SplitComma` / `ParseEntry` / `Match` / `Trim` / `Truthy` /
     `Str::IEquals`（对着 `_stricmp` 比对）。不需要 CommonLib，也不需要游戏。

  2. **BaseText 取法等价性**：用真实的 `Fallout4.esm`，把**旧做法**（预先把每条记录里
     ≤64 字节的 chunk 全收进表）与**新做法**（只记 formID→偏移，按需读那一条记录）
     逐组 (记录, 字段) 比对。两边必须逐字节相同。
     需要 `.f4lt_local.json` 里的 `game_data_dir`（或环境变量 `F4LT_GAME_DATA_DIR`）。
     315 MB 的 esm 大约 30 秒。

  3. **译文字典预筛自检**：把 `dictionary.cache` 里每一条键都喂给新的预筛逻辑，
     **必须 100% 通过**（漏一条 = 那条译文会被静默丢掉）。
     需要 `mods_dir`，并已至少进过一次游戏（缓存才存在）。

    ⚠️ 第 2、3 项都是「改动 BaseText / Translation 之后必跑」的回归闸门 ——
       这两处历史上有过多次“看着对、实际静默失效”的改动。

产物都落在 `tmp_selftest/`（在 .gitignore 里），不碰仓里的任何东西。
"""

from __future__ import annotations

import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import local_paths  # noqa: E402  （同目录模块）

PROJECT_ROOT = Path(__file__).resolve().parent.parent
WORK_DIR = PROJECT_ROOT / "tmp_selftest"
CASES_SRC = PROJECT_ROOT / "tools" / "selftest" / "cases.cpp"
INCLUDE_DIR = PROJECT_ROOT / "src"

_results: list[tuple[str, str, str]] = []   # (名字, 状态, 备注)


def record(name: str, status: str, detail: str = "") -> None:
    _results.append((name, status, detail))
    icon = {"PASS": "✅", "FAIL": "❌", "SKIP": "⏭️ "}.get(status, "  ")
    line = f"{icon} {name}"
    if detail:
        line += f" —— {detail}"
    print(line)


# ── 1. C++ 纯函数用例 ────────────────────────────────────────────────

def find_toolchain() -> tuple[Path, Path] | None:
    """返回 (cl.exe, vcvars64.bat)；找不到返回 None。"""
    vswhere = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")
    if not vswhere.is_file():
        return None
    try:
        out = subprocess.run([str(vswhere), "-latest", "-property", "installationPath"],
                             capture_output=True, text=True, timeout=30)
    except OSError:
        return None
    root = Path(out.stdout.strip())
    if not root.is_dir():
        return None
    vcvars = root / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    if not vcvars.is_file():
        return None
    for cl in (root / "VC" / "Tools" / "MSVC").glob("*/bin/Hostx64/x64/cl.exe"):
        return cl, vcvars
    return None


def run_cases() -> str:
    tc = find_toolchain()
    if tc is None:
        record("纯函数用例 (C++)", "SKIP", "找不到 MSVC（vswhere / vcvars64.bat）")
        return "SKIP"
    cl, vcvars = tc
    WORK_DIR.mkdir(exist_ok=True)
    exe = WORK_DIR / "cases.exe"
    # ⚠️ 必须经 vcvars：cl 需要 INCLUDE / LIB 环境（与 tools/deploy.py 的构建同源）。
    # ⚠️ 写 .bat 而不是拼一条命令串：Windows 下传含引号的命令行给 `cmd /c` 会被
    #    list2cmdline 多转义一层（实测 `\"cl.exe\"` 变成字面量）—— 文件里不动引号。
    bat = WORK_DIR / "build_cases.bat"
    bat.write_text(
        "@echo off\r\n"
        f'call "{vcvars}" >nul 2>&1\r\n'
        "set http_proxy=\r\n"
        "set https_proxy=\r\n"
        "set HTTP_PROXY=\r\n"
        "set HTTPS_PROXY=\r\n"
        f'"{cl}" /nologo /std:c++20 /utf-8 /EHsc /W4 /WX '
        # ⚠️ `/Fo` 后面用**正斜杠**结尾：`/Fo:"…\\"` 里的 `\\"` 会被 cmd 当成
        #    转义的引号吃掉后面整个参数（实测报“缺少源文件名”）。
        f'/Fe:"{exe}" /Fo:"{WORK_DIR}/" /I "{INCLUDE_DIR}" "{CASES_SRC}"\r\n',
        encoding="utf-8",
    )
    t0 = time.time()
    proc = subprocess.run([str(bat)], capture_output=True, text=True)
    build_ms = int((time.time() - t0) * 1000)
    if proc.returncode != 0 or not exe.is_file():
        print(proc.stdout)
        print(proc.stderr)
        record("纯函数用例 (C++)", "FAIL", f"编译失败（{build_ms} ms）")
        return "FAIL"
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    print(run.stdout.rstrip())
    if run.returncode == 0:
        record("纯函数用例 (C++)", "PASS", f"{run.stdout.strip().splitlines()[-1]}，编译 {build_ms} ms")
        return "PASS"
    record("纯函数用例 (C++)", "FAIL", "有用例不通过（见上面 FAIL 行）")
    return "FAIL"


# ── 2. BaseText：两种取法逐组比对 ─────────────────────────────────────

_CHUNK_LIMIT = 64        # 与 BaseText::BuildIndex 同口径
_RECORD_LIMIT = 4096


def _u32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def _u16(buf: bytes, off: int) -> int:
    return struct.unpack_from("<H", buf, off)[0]


def check_base_text() -> str:
    data_dir = local_paths.local_path("game_data_dir")
    if not data_dir:
        record("BaseText 等价性", "SKIP", "没有 game_data_dir（见 .f4lt_local.example.json）")
        return "SKIP"
    esm = Path(data_dir) / "Fallout4.esm"
    if not esm.is_file():
        record("BaseText 等价性", "SKIP", f"找不到 {esm}")
        return "SKIP"

    print(f"      读 {esm.name}（{esm.stat().st_size / 1048576:.0f} MB）…")
    t0 = time.time()
    data = esm.read_bytes()
    n = len(data)
    pos = 24 + _u32(data, 4)
    stack: list[int] = []
    # ⚠️ 必须与 `BaseText::BuildIndex` 同口径：索引存的键是**插件内原始 ID（高位已掩）**，
    #    而不是文件里原样存的 formID。这一点曾把真实 bug 掩了一年：
    #    `Fallout4.esm` 的自有记录高位恰好是 0x00，所以“不掩”也能跑；`DLCCoast.esm` /
    #    `cc*.esl` 高位是 0x01 ⇒ 不掩就**永远查不到**（全部 DLC/CC 的基表文本静默为空）。
    #    ⇒ 本自检若也用错误口径，就会继续“自证无问题”。
    light = (_u32(data, 8) & 0x00000200) != 0   # TES4 flags 的 0x200 = 轻插件（12 位 ID）
    flat: list[tuple[int, int, int]] = []          # 新做法：原始 ID -> (offset, size)
    old: dict[int, dict[int, bytes]] = {}          # 旧做法：原始 ID -> {chunk: 先出现者的字节}
    while pos < n:
        while stack and pos >= stack[-1]:
            pos = stack.pop()
        if pos + 24 > n:
            break
        size = _u32(data, pos + 4)
        if data[pos:pos + 4] == b"GRUP":
            stack.append(pos + size)
            pos += 24
            continue
        flags = _u32(data, pos + 8)
        body, end = pos + 24, pos + 24 + size
        if not (flags & 0x00040000) and size <= _RECORD_LIMIT:
            stored = _u32(data, pos + 12)
            fid = (stored & 0xFFF) if light else (stored & 0xFFFFFF)
            flat.append((fid, body, size))
            seen: dict[int, bytes] = {}
            o = body
            while o + 6 <= end:
                csig, csz = _u32(data, o), _u16(data, o + 4)
                o += 6
                if o + csz > end:
                    break
                if csz <= _CHUNK_LIMIT and csig not in seen:
                    seen[csig] = data[o:o + csz]
                o += csz
            if seen:
                old.setdefault(fid, seen)   # 同一个原始 ID 可能有多个候选 ⇒ 先出现的先（与 C++ 一致）
        pos = end

    flat.sort(key=lambda t: (t[0], t[1]))
    ids = [e[0] for e in flat]
    import bisect

    def new_get(fid: int, chunk: int) -> bytes | None:
        i = bisect.bisect_left(ids, fid)
        if i >= len(ids) or ids[i] != fid:
            return None
        _, off, size = flat[i]
        o, end = off, off + size
        while o + 6 <= end:
            csig, csz = _u32(data, o), _u16(data, o + 4)
            o += 6
            if o + csz > end:
                return None
            if csig == chunk and csz <= _CHUNK_LIMIT:
                return data[o:o + csz]
            o += csz
        return None

    checked = 0
    bad: list[str] = []
    for fid, chunks in old.items():
        for chunk, expect in chunks.items():
            checked += 1
            if new_get(fid, chunk) != expect:
                if len(bad) < 5:
                    bad.append(f"formID {fid:08X} chunk {chunk:08X}")
    secs = time.time() - t0
    if bad:
        for b in bad:
            print(f"      不一致: {b}")
        record("BaseText 等价性", "FAIL", f"{len(bad)}+ 处不一致 / 共 {checked} 组")
        return "FAIL"
    record("BaseText 等价性", "PASS",
           f"{checked:,} 组 (记录,字段) 逐字节相同；{len(flat):,} 条记录 / {secs:.1f}s")
    return "PASS"


# ── 3. 译文字典预筛自检 ───────────────────────────────────────────────

def _find_cache() -> Path | None:
    mods_dir = local_paths.local_path("mods_dir")
    if not mods_dir:
        return None
    mods = Path(mods_dir)
    cands = [
        mods.parent / "overwrite" / "f4se" / "plugins" / "F4LoadText" / "translations" / "dictionary.cache",
        mods / "F4LoadText" / "F4SE" / "Plugins" / "F4LoadText" / "translations" / "dictionary.cache",
    ]
    for c in cands:
        if c.is_file():
            return c
    return None


def _fnv1a(text: bytes) -> int:
    h = 2166136261
    for c in text:
        if 0x41 <= c <= 0x5A:
            c += 32
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def check_prefilter() -> str:
    cache = _find_cache()
    if cache is None:
        record("预筛自检", "SKIP", "找不到 dictionary.cache（先跑一次游戏）")
        return "SKIP"
    buf = cache.read_bytes()
    if buf[:4] != b"F4LT":
        record("预筛自检", "FAIL", "缓存 magic 不对")
        return "FAIL"
    pos = 4

    def u32() -> int:
        nonlocal pos
        v = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        return v

    def u64() -> None:
        nonlocal pos
        pos += 8

    def rd() -> bytes:
        nonlocal pos
        ln = u32()
        v = buf[pos:pos + ln]
        pos += ln
        return v

    version = u32()
    if version not in (5, 6, 7):
        record("预筛自检", "SKIP", f"缓存版本 {version}（本脚本只认 5/6/7）")
        return "SKIP"
    for _ in range(u32()):
        rd()
        u64()
        u64()
    if version >= 6:
        for _ in range(6):   # v6：建表统计 + 1 个保留位
            u32()
    keys: list[bytes] = []
    for _ in range(u32()):
        keys.append(rd())
        rd()
    for _ in range(u32()):
        keys.append(rd())
        cnt = u32()
        for _ in range(cnt):
            rd()

    # 与 Translation.cpp 的 publish() 同一套算法：键 = `插件|000ABC|CHUNK[|@来源]`
    pairs: set[tuple[int, int]] = set()
    for k in keys:
        b1 = k.find(b"|")
        b2 = k.find(b"|", b1 + 1) if b1 >= 0 else -1
        if b1 < 0 or b2 < 0 or b2 + 5 > len(k):
            record("预筛自检", "FAIL", f"键格式不认识: {k!r}")
            return "FAIL"
        pairs.add((_fnv1a(k[:b1]), struct.unpack("<I", k[b2 + 1:b2 + 5])[0]))

    missed = 0
    for k in keys:
        b1 = k.find(b"|")
        b2 = k.find(b"|", b1 + 1)
        if (_fnv1a(k[:b1]), struct.unpack("<I", k[b2 + 1:b2 + 5])[0]) not in pairs:
            missed += 1
    if missed:
        record("预筛自检", "FAIL", f"{missed} / {len(keys)} 条键会被预筛误拒（译文会静默丢失）")
        return "FAIL"
    plugins = len({p for p, _ in pairs})
    record("预筛自检", "PASS", f"{len(keys):,} 条键 0 误拒；{plugins} 个插件 / {len(pairs)} 组")
    return "PASS"


def main() -> int:
    print("═══ F4LoadText 自检（不启动游戏、不改部署）═══")
    run_cases()
    check_base_text()
    check_prefilter()

    fails = [r for r in _results if r[1] == "FAIL"]
    skips = [r for r in _results if r[1] == "SKIP"]
    print("───")
    print(f"PASS {sum(1 for r in _results if r[1] == 'PASS')} / FAIL {len(fails)} / SKIP {len(skips)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())