#!/usr/bin/env python3
# ==============================================================================
# 打发布包（本地，与 .github/workflows/build.yml 的 "Assemble deploy package" 同结构）
#
# 产物：dist\F4LoadText-<版本>.zip
#   F4SE\Plugins\F4LoadText.dll
#   F4SE\Plugins\F4LoadText.ini
#   F4SE\Plugins\F4LoadText\translations\README.txt   ← 只有说明，**不含 yaml**
#
# ⚠️ 译文**不入包**：translations\ 是各人自己的运行数据（且体积 5 MB），
#    仓里也已在 .gitignore 排除。发布包只给空目录 + 用法说明。
#    ⇒ 本脚本**不**调用 tools\sanitize_translations.py；那个脚本是给
#      「想把译文一起分发」的场景用的（如另发一个汉化包）。
#
# 用法：
#   uv run python tools\package_release.py                # 版本取 src\main.cpp 的 PluginVersion
#   uv run python tools\package_release.py --version 0.3.0
#   uv run python tools\package_release.py --no-build     # 用现有 build\Release
# ==============================================================================

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import zipfile

PROJECT_ROOT = pathlib.Path(__file__).resolve().parent.parent
DLL = PROJECT_ROOT / "build" / "Release" / "F4LoadText.dll"
INI = PROJECT_ROOT / "config" / "F4LoadText.ini"
MAIN_CPP = PROJECT_ROOT / "src" / "main.cpp"
DIST = PROJECT_ROOT / "dist"


def version_from_source() -> str:
    """从 src\\main.cpp 的 `v.PluginVersion(REL::Version{ 0, 3, 0, 0 })` 取版本。

    单一真相源是 C++（DLL 里编进去的就是它），从这里读可避免 meta.ini 再飘。
    """
    text = MAIN_CPP.read_text(encoding="utf-8")
    m = re.search(r"PluginVersion\s*\(\s*REL::Version\s*\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", text)
    if not m:
        raise SystemExit("❌ 没能从 src\\main.cpp 解析出 PluginVersion")
    a, b, c, _d = (int(x) for x in m.groups())
    return f"{a}.{b}.{c}"


TRANSLATIONS_README = (
    "译文目录（F4LoadText.ini 的 [Translation] TranslationDir = F4LoadText\\translations）\r\n"
    "\r\n"
    "把 translations\\*.yaml 放进本目录即可生效（改完重开游戏）。\r\n"
    "yaml 模板可用本仓库的 tools\\export_esp_yaml.py 从 esp 导出。\r\n"
    "yaml 里的 path 写插件文件名即可（如 path: \"unofficial fallout 4 patch.esp\"）；\r\n"
    "插件会自动到 <Data> 下找（MO2 的 mod 目录也认），不需要写完整路径。\r\n"
    "\r\n"
    "首次启动会解析 yaml 并写 dictionary.cache（约 1 秒）；\r\n"
    "之后启动走缓存（约 4 ms）。MO2 下缓存写在 overwrite\\ 里，属正常现象。\r\n"
)


def build() -> None:
    """只编不部署。

    ⚠️ 不走 tools\deploy.py：那个脚本编完就拷到 MO2 mod 目录，
       而要打发布包时**不该动部署目录**（发布包用 build\Release 的产物）。
       但 deploy.py 的 build() 里有一件本脚本必须复用的事：**构建前清
       http_proxy / https_proxy**（本机是用户级变量，会让 MSBuild 抛 ArgumentException）。
       所以这里直接复刻那一步，再调 cmake。
    """
    import os

    for k in list(os.environ):
        if k.lower() in ("http_proxy", "https_proxy"):
            del os.environ[k]
    print("🔨 编译中…")
    r = subprocess.run(["cmake", "--build", "build", "--config", "Release", "--target", "F4LoadText"],
                       cwd=PROJECT_ROOT)
    if r.returncode != 0:
        raise SystemExit("❌ 编译失败")


def main() -> int:
    ap = argparse.ArgumentParser(description="打发布包")
    ap.add_argument("--version", default=None, help="默认从 src\\main.cpp 读")
    ap.add_argument("--no-build", action="store_true", help="跳过编译")
    args = ap.parse_args()

    ver = args.version or version_from_source()
    if not args.no_build:
        build()

    for p in (DLL, INI):
        if not p.is_file():
            raise SystemExit(f"❌ 缺少 {p}")

    DIST.mkdir(parents=True, exist_ok=True)
    out = DIST / f"F4LoadText-{ver}.zip"
    if out.exists():
        out.unlink()

    # 与 CI 的解压结构一致：zip 根下就是 F4SE\
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(DLL, "F4SE/Plugins/F4LoadText.dll")
        z.write(INI, "F4SE/Plugins/F4LoadText.ini")
        z.writestr("F4SE/Plugins/F4LoadText/translations/README.txt",
                   TRANSLATIONS_README.encode("utf-8"))

    print(f"═══ 发布包 ═══")
    print(f"  {out}  ({out.stat().st_size:,} B)")
    with zipfile.ZipFile(out) as z:
        for info in z.infolist():
            print(f"    {info.filename:<58} {info.file_size:>10,} B")
    print(f"\n  解压到 MO2 的 mod 目录（或 <游戏>\\Data）即可。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
