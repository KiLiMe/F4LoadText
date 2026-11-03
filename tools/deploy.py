#!/usr/bin/env python3
"""F4LoadText 一键部署 —— 编译 + 部署到 MO2 mod 目录。

自包含：只用标准库，不依赖仓内其它脚本。

    uv run python tools/deploy.py              # 编译 + 部署
    uv run python tools/deploy.py --no-build   # 只部署
    uv run python tools/deploy.py --fresh      # 先清空 mod 目录再部署
    uv run python tools/deploy.py --check      # 只看状态，不动任何文件

⚠️ 本机四个真实踩过的坑，脚本已经处理：
  1. 代理变量大小写混用会让 MSBuild 子进程环境字典抛 ArgumentException，
     **任何** MSVC 构建都失败 —— 构建前必须清掉。
  2. 游戏运行时 DLL 被锁，复制会失败 —— 脚本先检测进程。
  3. 版本迭代里删过的文件会留在部署目录（如旧版的 .ini）—— 脚本会清掉。
  4. **本脚本在 tools/ 下**：项目根是 `__file__` 的上一层（parent.parent），
     不是 parent —— 写错会让 CMakeLists / config/ / build/ 全部找不到。

📌 ini 是**每次部署整份覆盖**（仓内 config/F4LoadText.ini 为唯一权威）：
   要调开关就改仓内模板再部署，别只改部署位那份。
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import local_paths  # noqa: E402  （同目录模块，tools/ 下）

# ── 常量 ─────────────────────────────────────────────────────────────
# ⚠️ 本脚本在 tools/ 下，项目根是它的**上一层** —— 别改成 parent（那会指到 tools/，
#    CMakeLists / config/ / build/ 全部找不到）。
PROJECT_ROOT = Path(__file__).resolve().parent.parent

# MO2 mods 目录：**本机专属路径，不写进源码**（本仓是公开的）。
# 取值顺序：--mods-dir 参数 > 环境变量 F4LT_MODS_DIR > 仓根 .f4lt_local.json
# 的 "mods_dir" > 报错并给出设置方法。详见 tools/local_paths.py。
# ⚠️ 这是 **MO2 里那个 mod 文件夹的名字**，不是本项目的名字（2026-10-31 用户裁定）。
#    本机装的这个 mod 是从 Nexus 发布包直接解开的，文件夹保留了发布名
#    `F4Text - Load-Time Text Replacement`，`modlist.txt` 里启用的也只有它。
#    以前这里写 `F4LoadText` ⇒ 部署到 `mods\F4LoadText\`，那个目录**根本没在 MO2 里启用**
#    ⇒ 游戏一直加载旧 DLL，改了代码却“没修好”，排查了半天（日志里只有两行）。
#    ⇒ 改名前先确认 `profiles\<配置>\modlist.txt` 里是哪个名字。
MOD_NAME = "F4Text - Load-Time Text Replacement"

# 延迟解析：命令行参数要等 argparse 跑完才知道，所以这里先占位，
# 由 main() 里的 refresh_paths() 一次性算好。
MODS_DIR: Path = Path()
MOD_DIR: Path = Path()
PLUGINS_DIR: Path = Path()
TRANSLATIONS_DEST: Path = Path()


def refresh_paths(mods_dir: Path) -> None:
    """按 mods 目录重算所有派生路径。main() 里解析完参数后调用一次。"""
    global MODS_DIR, MOD_DIR, PLUGINS_DIR, TRANSLATIONS_DEST
    MODS_DIR = mods_dir
    MOD_DIR = MODS_DIR / MOD_NAME
    PLUGINS_DIR = MOD_DIR / "F4SE" / "Plugins"
    # 与 ini 里的 TranslationDir 必须一致（相对 PLUGINS_DIR）。
    TRANSLATIONS_DEST = PLUGINS_DIR / "F4LoadText" / "translations"

# ── 译文字典来源（`[汉化]`）─────────────────────────────────────────
# 译文字典现在**自包含在本仓内**（`F4LoadText\translations\`）。
#
# 历史：这些 yaml 原本来自 F4TCHook 的 `translations\`（本项目从那里拷）。
# 2026-10-09 改：① F4TCHook 的 translations\ 已整目录脱离 git 跟踪；
#               ② 为了不再跨仓依赖（删了 F4TCHook 就没了），把 15 个文件
#                  （14 个 yaml + 1 个 scaleform_literals.tsv）搬进本仓。
# ⇒ 改 yaml 只需重新部署一次（见 src/Translation.cpp 的缓存机制）。
TRANSLATIONS_SRC = PROJECT_ROOT / "translations"
# 脱敏后的译文（`tools\sanitize_translations.py` 的产物）：`path` 只剩文件名、不带本机目录。
# **发布包必须用这份**（源目录里的 `path` 是导出机绝对路径）。
TRANSLATIONS_SANITIZED = PROJECT_ROOT / "dist" / "translations"
# TRANSLATIONS_DEST 在 refresh_paths() 里算（它依赖 MODS_DIR）。

# meta.ini：**可选**。仓内有就用它覆盖部署位；没有就整个跳过（MO2 自己会建一份）。
#
# 为什么是可选：meta.ini 是 MO2 亲手写的元数据（modid / lastNexusQuery / tracked /
# validated / color / [installedFiles] …），里面一半以上是**机器本地状态**，随每次
# 启动变化 ⇒ 不适合入库，已在 .gitignore 里。
#
# ⚠️ 但 version / name / description / notes 这几行是有意义的（改版本号要同步），
#    所以想让它入库就 `git add -f config/meta.ini` —— 有文件就照旧整份拷贝。
META_TEMPLATE = PROJECT_ROOT / "config" / "meta.ini"

# 插件 ini 模板：仓内 config/F4LoadText.ini = **发布默认（静默）**。
#
# ⚠ 部署**不覆盖**已存在的部署位（2026-10-31 用户裁定，`--force-ini` 才覆盖）：
#   调参（Info=true / AuditText=… / AuditLimit=…）改的是 **MO2 里那份**，
#   一次部署把它冲回模板 = “明明开了日志却一行不写”的陷阱（实际踩过两次）。
#   代价：代码里**新增的 ini 键不会自动出现在磁盘上** —— 新键一律写成“缺省即安全”的默认，
#   并在 `--check` 里提示；要刷模板就显式 `--force-ini`。
CONFIG_TEMPLATE = PROJECT_ROOT / "config" / "F4LoadText.ini"

# 已经废弃、要在部署目录清理掉的老文件（本版为空）。
STALE_FILES: list[str] = []

# 编译前必须清掉的代理变量（本机 MSVC 老坑）。
PROXY_VARS = ["http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY", "NO_PROXY", "no_proxy"]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def stamp(path: Path) -> str:
    """可读的时间戳。用 datetime 而不是 f-string 的格式说明符 ——
    `st_mtime` 是 float，`{x:%Y-%m-%d}` 会抛 ValueError。"""
    return datetime.fromtimestamp(path.stat().st_mtime).strftime("%Y-%m-%d %H:%M:%S")


def game_running() -> bool:
    """游戏在跑就锁着 DLL，复制会失败 —— 先问清楚比事后报错好。"""
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq Fallout4.exe", "/NH"],
            capture_output=True, text=True, timeout=15)
        return "Fallout4.exe" in out.stdout
    except Exception:
        return False  # 查不到就不拦，让复制自己去报错


def clean_proxy_env() -> None:
    for name in PROXY_VARS:
        if name in os.environ:
            del os.environ[name]


def build(cfg: str) -> bool:
    print("  🔨 编译中…")
    clean_proxy_env()
    r = subprocess.run(
        ["cmake", "--build", "build", "--config", cfg],
        cwd=PROJECT_ROOT, capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        print("  ❌ 编译失败：")
        tail = (r.stdout or "")[-1500:] + (r.stderr or "")[-800:]
        print(tail)
        return False

    errors = [l for l in (r.stdout or "").splitlines() if "error C" in l]
    warns = [l for l in (r.stdout or "").splitlines() if "warning C" in l]
    print(f"  ✅ 编译成功（error={len(errors)} warning={len(warns)}）")
    if errors:
        for l in errors[:5]:
            print(f"       {l.strip()}")
    return True


def find_dll(cfg: str) -> Path | None:
    dll = PROJECT_ROOT / "build" / cfg / "F4LoadText.dll"
    if dll.exists():
        return dll
    found = list((PROJECT_ROOT / "build").rglob("F4LoadText.dll"))
    return found[0] if found else None


def write_meta() -> bool:
    """如果仓内有 `config/meta.ini`，就拷到部署位；没有则跳过（不算失败）。

    meta.ini 主体是 MO2 的机器本地状态，不入库；仓内那份是**可选**的手工副本，
    用来钉住 version / name / description 这类有意义的字段。
    """
    if not META_TEMPLATE.exists():
        print("  · meta.ini  ：跳过（仓内没有 config/meta.ini，保留 MO2 自己那份）")
        return True
    shutil.copy2(META_TEMPLATE, MOD_DIR / "meta.ini")
    return True


def find_duplicate_esp(src_dir: Path) -> list[tuple[str, Path, Path]]:
    r"""同一个 `esp` 被多份 yaml 声明 ⇒ 运行时只留一份、另一份**整份丢弃**。

    怎么看谁是“另一份”：`src/Translation.cpp` 的加载顺序是
    `priority`（大者胜）→ `generated`（ISO-8601 UTC，新者胜）→ 文件 mtime → 文件名。

    ⚠️ 为什么部署前必须拦：两份并存时**不是**逐条合并，输者的条目一条都不进库；
    但玩家看到的是“有的词翻对了、有的一直是旧的”—— 极难排查。真正的修法是删掉旧文件。
    这里只读每个 yaml 的前 64 KB（`esp` 是顶层键，一定在开头）。
    """
    seen: dict[str, Path] = {}
    dup: list[tuple[str, Path, Path]] = []
    for f in sorted(src_dir.glob("*.y*ml")):
        try:
            head = f.read_bytes()[: 64 * 1024].decode("utf-8", "replace")
        except OSError:
            continue
        esp = ""
        for line in head.splitlines():
            if line.startswith("entries:"):
                break
            if line.startswith("esp:"):
                esp = line[4:].strip().strip('"').strip("'")
                break
        if not esp:
            continue  # 没 esp 的文件运行时本来就会跳过
        key = esp.lower()
        if key in seen:
            dup.append((esp, seen[key], f))
        else:
            seen[key] = f
    return dup


def copy_translations(src_dir=None):
    r"""把 `translations\*.yaml` 拷进 mod（`[汉化]` 用）。

    返回 (文件数, 字节数)；源目录缺失时返回 None（**只告警不失败** ——
    汉化功能可以关着用）。

    `src_dir` 默认为仓内 `translations\`；传 `--translations dist` 时用脱敏副本。

    ⚠️ 必须拷到 mod 目录：游戏进程只能看到 MO2 虚拟目录，仓内路径它根本看不到。
    ⚠️ 只拷 `*.yaml`/`*.yml` —— `Translation.cpp` 也只认这两种扩展名；
       `translations` 目录里别的文件（如 `scaleform_literals.tsv`）**故意不拷**，
       它们是同源历史工具的输入，不属于本插件的运行数据。
    """
    src_dir = src_dir or TRANSLATIONS_SRC
    if not src_dir.is_dir():
        print(f"  ⚠️  译文源目录不存在，跳过（{src_dir}）")
        return None

    TRANSLATIONS_DEST.mkdir(parents=True, exist_ok=True)
    # 先清掉上一次的（yaml 可能被重命名/删除 —— 留着旧文件会写成一份不存在的译文）
    for old in TRANSLATIONS_DEST.glob("*.y*ml"):
        old.unlink()

    n_files = n_bytes = 0
    for src in sorted(src_dir.glob("*.y*ml")):
        shutil.copy2(src, TRANSLATIONS_DEST / src.name)
        n_files += 1
        n_bytes += src.stat().st_size
    return (n_files, n_bytes) if n_files else None


def deploy(cfg: str, fresh: bool, sanitized: bool = False, force_ini: bool = False) -> int:
    print(f"═══ {MOD_NAME} 部署 ═══")
    print(f"  配置 : {cfg}")
    print(f"  目标 : {MOD_DIR}")
    print()

    dll = find_dll(cfg)
    if not dll:
        print("  ❌ 未找到 F4LoadText.dll，请先编译：")
        print(f"     cd {PROJECT_ROOT}")
        print("     cmake --preset default")
        print("     cmake --build build --config Release")
        return 1
    print(f"  ✅ DLL  : {dll}  ({dll.stat().st_size:,} B)")

    # 译文源里同一个 esp 出现两次 ⇒ 运行时只会用其中一份（另一份整份丢弃）。
    # 这是“换了新版译文忘了删旧文件”的典型症状，部署前拦下来（还没动任何文件）。
    dup = find_duplicate_esp(TRANSLATIONS_SANITIZED if sanitized else TRANSLATIONS_SRC)
    if dup:
        print("  ❌ 译文源里有**同一个 esp** 的多份 yaml（运行时只留一份，其余整份丢弃）：")
        for esp, a, b in dup:
            print(f"     {esp}: {a.name}  ⇄  {b.name}")
        print("     → 删掉旧的那份；确实要两份并存时，把该留下的那份的 priority/generated 调高")
        print("       （规则：priority 大者胜 → generated 新者胜 → 文件 mtime → 文件名靠后者胜）")
        return 1

    if fresh and MOD_DIR.exists():
        print("  🗑️  清空 mod 目录 (--fresh)…")
        shutil.rmtree(MOD_DIR)

    PLUGINS_DIR.mkdir(parents=True, exist_ok=True)

    # 清掉本版不再产出的文件（旧版 .ini 等）—— 留着会让人以为开关还在那儿。
    for name in STALE_FILES:
        stale = PLUGINS_DIR / name
        if stale.exists():
            stale.unlink()
            print(f"  🧹 删除过时文件: {stale.name}")

    target = PLUGINS_DIR / "F4LoadText.dll"
    try:
        shutil.copy2(dll, target)
    except PermissionError:
        print(f"  ❌ 无法写入 {target} —— DLL 被占用（游戏还在运行？）")
        return 1
    print(f"  📦 DLL  → {target}")

    # 复制后校验：别让「以为部署好了」变成排查时的假前提。
    src_hash, dst_hash = sha256(dll), sha256(target)
    if src_hash != dst_hash:
        print("  ❌ 复制后哈希不一致（磁盘问题？）")
        return 1
    print(f"  🔒 SHA256 {src_hash[:16]}… ✓")

    # 配置：<游戏>\\Data\\F4SE\\Plugins\\F4LoadText.ini（与 ImeControl 同一约定）= 也就是 mod 目录里这份。
    # **默认保留现场副本**：仓内的 config/F4LoadText.ini 是唯一权威，避免“磁盘上有新键吗”这类假前提。
    ini_dest = PLUGINS_DIR / "F4LoadText.ini"
    if not CONFIG_TEMPLATE.exists():
        print(f"  ⚠️  ini 模板缺失，跳过（{CONFIG_TEMPLATE}）")
    else:
        # ⚠ 默认**不覆盖已存在的部署副本**（2026-10-31 用户裁定）：
        #   仓内模板 = 发布默认（**默认静默**：Info=false / AuditLimit=0 / 审计文本空），
        #   调参时改的是 MO2 里那份（Info=true、AuditText=… 之类），
        #   部署一次就把现场开关冲掉 = “明明开了日志却一行不写”的经典陷阱。
        #   要拿模板重写就显式 `--force-ini`（旧文件先备份成 .bak-<时间戳>）。
        if ini_dest.exists() and not force_ini:
            same = CONFIG_TEMPLATE.read_bytes() == ini_dest.read_bytes()
            note = "与模板一致" if same else "**保留现场开关**，不覆盖"
            print(f"  📋 ini   = {ini_dest}  ({ini_dest.stat().st_size:,} B，{note})")
            if not same:
                print("      ↳ 要拿模板重写：--force-ini（旧文件先备份成 .bak-<时间戳>）")
        else:
            if ini_dest.exists():
                stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
                backup = ini_dest.with_name(f"F4LoadText.ini.bak-{stamp}")
                shutil.copy2(ini_dest, backup)
                print(f"  🗂  ini   → {backup.name}（旧副本已备份）")
            shutil.copy2(CONFIG_TEMPLATE, ini_dest)
            print(f"  📋 ini   → {ini_dest}  ({ini_dest.stat().st_size:,} B，已用模板覆盖)")

    if not write_meta():
        return 1
    if META_TEMPLATE.exists():
        print(f"  📋 meta.ini → {MOD_DIR / 'meta.ini'}")

    # 译文字典：把本仓 translations\*.yaml 拷进本 mod。
    #
    # 为什么拷而不“直接读仓内”：游戏进程只能看到 MO2 虚拟目录下的东西，
    # 本仓路径（源码目录）在游戏里根本不存在。拷到 mod 目录 → 改完 yaml
    # **重新部署一次**就生效（不用再跑 build_flatbuffer_dict.py）。
    #
    # ⚠️ 源目录缺失（没 clone F4TCHook）时**只告警不失败** —— 汉化功能可以关着用。
    counts = copy_translations(TRANSLATIONS_SANITIZED if sanitized else None)
    if counts is not None:
        n_files, n_bytes = counts
        tag = "（脱敏副本）" if sanitized else ""
        print(f"  📖 译文 → {TRANSLATIONS_DEST}  ({n_files} 个 yaml，{n_bytes:,} B){tag}")

    print()
    print("═══ 部署完成 ═══")
    print(f"  {MOD_DIR}")
    print()
    print("  下一步：")
    print(f"    1. MO2 里启用「{MOD_NAME}」mod（已启用则跳过）")
    print("    2. 进游戏 → 读一个存档 → 进到世界 → 退出")
    print("    3. 看日志（F4SE 日志目录）：")
    print(f"       Get-Content \"$([Environment]::GetFolderPath('MyDocuments'))\\My Games\\Fallout4\\F4SE\\F4LoadText.log\"")
    print("         应有 3 行 `hook ok`（LoadFullNameChunk / Load(2194235) / Load2(2194240)）")
    print("         + 1 行 `hook defer`（GetDescription(2193019)，由 kGameDataReady 装）")
    print("         读档完成时：`DESC map: scanned-forms=…  with-TESDescription=…`")
    print("         以及一行汇总：`replace: load-time fields  incoming-dropped=… (of which from base=…)  translated=…`")
    print("       日志开关都在 ini（信息 / 审计上限 / 审计签名 / 审计文本）—— 调参改**部署位那份**，部署不会覆盖它。")
    return 0


def check(cfg: str) -> int:
    print(f"═══ {MOD_NAME} 状态检查 ═══")
    print(f"  项目 : {PROJECT_ROOT}")
    print(f"  mod  : {MOD_DIR}  {'存在' if MOD_DIR.exists() else '不存在'}")
    print()

    dll = find_dll(cfg)
    if dll:
        print(f"  构建产物 : {dll}")
        print(f"             {dll.stat().st_size:,} B   {stamp(dll)}")
    else:
        print("  构建产物 : ❌ 未编译")
        return 1

    target = PLUGINS_DIR / "F4LoadText.dll"
    if target.exists():
        print(f"  部署位   : {target}")
        print(f"             {target.stat().st_size:,} B   {stamp(target)}")
        same = sha256(dll) == sha256(target)
        print(f"  是否同步 : {'✅ 一致' if same else '⚠️  不一致（部署位是旧的，需重新部署）'}")
    else:
        print(f"  部署位   : ❌ 未部署（{target}）")

    print()
    print(f"  游戏进程 : {'⚠️  正在运行（DLL 被锁，无法部署）' if game_running() else '未运行 ✓'}")
    print(f"  MO2 mods : {'存在' if MODS_DIR.exists() else '❌ 不存在（查 --mods-dir / F4LT_MODS_DIR / .f4lt_local.json）'}")

    d = MOD_DIR / "F4SE" / "Plugins" / "F4LoadText.ini"
    if CONFIG_TEMPLATE.exists() and d.exists():
        same = CONFIG_TEMPLATE.read_bytes() == d.read_bytes()
        print(f"  ini 模板 : {CONFIG_TEMPLATE}  ({CONFIG_TEMPLATE.stat().st_size:,} B)")
        print(f"  ini 部署 : {d}  ({d.stat().st_size:,} B)")
        print(f"             {'✅ 与模板一致' if same else 'ℹ️  与模板不同（现场调了开关；部署**不会**覆盖它，--force-ini 才覆盖）'}")
    else:
        print("  ini 模板/部署 : ❌ 缺一个（模板或部署位不存在）")

    # meta.ini：模板 vs 部署位。MO2 会自己重写这份文件，所以两边「不同」是常态 ——
    # 分别报出来，让人自己判断该不该拿磁盘上的那份去更新模板。
    dest_meta = MOD_DIR / "meta.ini"
    if META_TEMPLATE.exists():
        print(f"  meta 模板 : {META_TEMPLATE}  ({META_TEMPLATE.stat().st_size:,} B)")
        if dest_meta.exists():
            same = META_TEMPLATE.read_bytes() == dest_meta.read_bytes()
            print(f"  meta 部署 : {dest_meta}")
            print(f"              {'✅ 与模板一致' if same else '⚠️  与模板不同（MO2 可能重写过；部署会以模板覆盖）'}")
        else:
            print(f"  meta 部署 : ❌ 不存在")
    else:
        print(f"  meta 模板 : ❌ 缺失（{META_TEMPLATE}）—— 部署时会报错")

    print()
    print(f"  过时文件 : {', '.join(STALE_FILES)}（本版不再产出，部署时会自动清理）")

    # 译文（`[汉化]`）：源目录 vs 部署位。
    # 这里是排查“改了 yaml 没生效”的第一站（最常见原因就是忘了重新部署）。
    print()
    if not TRANSLATIONS_SRC.is_dir():
        print(f"  译文源   : ❌ 不存在（{TRANSLATIONS_SRC}）")
    else:
        src_files = sorted(TRANSLATIONS_SRC.glob("*.y*ml"))
        dst_files = sorted(TRANSLATIONS_DEST.glob("*.y*ml")) if TRANSLATIONS_DEST.is_dir() else []
        src_names = {f.name for f in src_files}
        dst_names = {f.name for f in dst_files}
        print(f"  译文源   : {TRANSLATIONS_SRC}")
        print(f"             {len(src_files)} 个 yaml")
        print(f"  译文部署 : {TRANSLATIONS_DEST}")
        if not dst_files:
            print("             ❌ 未部署（部署时会拷过去）")
        elif src_names != dst_names:
            only_src = sorted(src_names - dst_names)
            only_dst = sorted(dst_names - src_names)
            extra = (f"; 只在源: {', '.join(only_src)}" if only_src else "") \
                  + (f"; 只在部署位: {', '.join(only_dst)}" if only_dst else "")
            print(f"             {len(dst_files)} 个 yaml，⚠️  与源不同名{extra}（重新部署即可对齐）")
        else:
            # 同名时再比内容：源是人手改的、部署位是拷的 ⇒ “改了 yaml 没部署”很常见。
            diff = [f.name for f in src_files
                    if not (TRANSLATIONS_DEST / f.name).exists()
                    or f.read_bytes() != (TRANSLATIONS_DEST / f.name).read_bytes()]
            if diff:
                more = " …" if len(diff) > 4 else ""
                print(f"             {len(dst_files)} 个 yaml，⚠️  {len(diff)} 个内容不同（改了 yaml 没部署？）: "
                      f"{', '.join(diff[:4])}{more}")
            else:
                print(f"             {len(dst_files)} 个 yaml，✅ 与源一致")

        # 二进制缓存（译文字典快路径）。
        # ⚠️ MO2（USVFS）下 DLL 的写入会被重定向到 `MO2\overwrite\f4se\plugins\F4LoadText\translations\`，
        #    所以**不在 mod 目录里找**，而是看 overwrite（mod 目录那份是直部署旧版才可能有的）。
        cache_in_mod = TRANSLATIONS_DEST / "dictionary.cache"
        cache_in_ow = MODS_DIR.parent / "overwrite" / "f4se" / "plugins" / "F4LoadText" / "translations" / "dictionary.cache"
        where, cache = ("overwrite", cache_in_ow) if cache_in_ow.is_file() else \
                       ("mod 目录", cache_in_mod) if cache_in_mod.is_file() else (None, None)
        if cache is not None:
            print(f"  字典缓存 : {cache.stat().st_size / 1024:,.0f} KB（在 {where}）—— 仅加速用；yaml 变了会自动重建")
            print(f"             {cache}")
        else:
            print("  字典缓存 : （无）—— 首次启动会解析 yaml 并生成（MO2 下落盘在 overwrite）")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(
        description=f"{MOD_NAME} 一键部署（编译 + 复制到 MO2 mod 目录）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    p.add_argument("--debug", action="store_true", help="用 Debug 配置（默认 Release）")
    p.add_argument("--no-build", action="store_true", help="跳过编译，只部署现有产物")
    p.add_argument("--fresh", action="store_true", help="部署前清空 mod 目录")
    p.add_argument("--check", action="store_true", help="只看状态，不改任何文件")
    p.add_argument("--force", action="store_true", help="游戏运行时也强行部署（通常没用）")
    p.add_argument("--sanitize-translations", dest="sanitized", action="store_true",
                   help="用 dist\\translations（脱敏副本，path 只剩文件名）—— 发布包用这个")
    p.add_argument("--force-ini", dest="force_ini", action="store_true",
                   help="用仓内模板重写已部署的 F4LoadText.ini（默认保留现场开关；旧文件先备份）")
    p.add_argument("--mods-dir", default=None,
                   help="MO2 mods 目录。默认取环境变量 F4LT_MODS_DIR，或仓根 .f4lt_local.json 的 mods_dir")
    args = p.parse_args()

    # 解析 MO2 mods 目录：参数 > 环境变量 > 本地配置 > 报错。
    # 必须在任何 MOD_DIR / PLUGINS_DIR 使用之前完成。
    if args.mods_dir:
        refresh_paths(Path(args.mods_dir))
    else:
        refresh_paths(Path(local_paths.require_path("mods_dir", "--mods-dir <MO2 mods 目录>")))

    if not MODS_DIR.is_dir():
        print(f"  ❌ MO2 mods 目录不存在：{MODS_DIR}")
        print("     请检查 --mods-dir / F4LT_MODS_DIR / .f4lt_local.json 里的 mods_dir。")
        return 1

    cfg = "Debug" if args.debug else "Release"

    if args.check:
        return check(cfg)

    if game_running() and not args.force:
        print("  ⚠️  检测到 Fallout4.exe 正在运行 —— DLL 被锁，复制会失败。")
        print("      请先完全退出游戏再部署（--force 可强行尝试）。")
        return 1

    if not args.no_build:
        if not build(cfg):
            return 1
    else:
        print("  ⏭️  跳过编译 (--no-build)")

    return deploy(cfg, args.fresh, args.sanitized, args.force_ini)


if __name__ == "__main__":
    sys.exit(main())

