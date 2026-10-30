#!/usr/bin/env python3
"""本机路径与备注解析 —— 让 tools/ 入库而不泄露任何本机专属信息。

为什么需要它：这些脚本要访问 MO2 mods 目录、游戏 Data 目录等**只属于当前机器**的
路径。把它们硬编码进源码，要么不能公开（泄露本机目录结构），要么别人 clone 后直接崩。
⇒ 一律走「显式参数 > 环境变量 > 本地配置文件 > 报错」四级回退，源码里不留路径。

本地配置文件（**不入库**，在 .gitignore 里）：
    <仓根>/.f4lt_local.json
    {
      "mods_dir":      "X:/MO2/mods",
      "game_data_dir": "X:/Steam/steamapps/common/Fallout 4/Data",
      "strings_dir":   "X:/MO2/mods/<汉化 mod>/Strings",

      "notes": {
        "log_dir":   "X:/Users/<you>/Documents/My Games/Fallout4/F4SE",
        "any key":   "任意自由备注，脚本不解释，只给人看 / 给其它本机脚本读"
      }
    }

三种字段的分工：
    * 顶层字符串键   —— 机器可读的路径，由 local_path() / require_path() 取
    * "_comment"   —— 给人看的说明，脚本一律忽略（下划线开头的键都是保留键）
    * "notes"      —— 本机专属的自由备注（日志目录、账号、口令以外的各种约定），
                      脚本不解释，由 local_note() 按键取，或 notes() 整份取

缺失的键就当没配置。也可以只用环境变量：
    F4LT_MODS_DIR / F4LT_GAME_DATA_DIR / F4LT_STRINGS_DIR

⚠️ 这个文件永远不要提交。模板见仓内的 .f4lt_local.example.json。
"""

from __future__ import annotations

import json
import os
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
LOCAL_CONFIG_PATH = PROJECT_ROOT / ".f4lt_local.json"

# 逻辑名 -> 环境变量名
_ENV_NAMES = {
    "mods_dir": "F4LT_MODS_DIR",
    "game_data_dir": "F4LT_GAME_DATA_DIR",
    "strings_dir": "F4LT_STRINGS_DIR",
}

# notes 段里这些子键也是路径，同样允许用环境变量覆盖。
#   notes 是自由格式，只有列在这里的键才会去查环境变量。
_NOTE_ENV_NAMES = {
    "log_dir": "F4LT_LOG_DIR",
    "saves_dir": "F4LT_SAVES_DIR",
}

_cache: dict | None = None


def _local_config() -> dict:
    """读本地配置文件；不存在或损坏都返回空 dict（不抛异常）。"""
    global _cache
    if _cache is not None:
        return _cache
    try:
        _cache = json.loads(LOCAL_CONFIG_PATH.read_text(encoding="utf-8"))
        if not isinstance(_cache, dict):
            _cache = {}
    except (OSError, ValueError):
        _cache = {}
    return _cache


def reload_config() -> None:
    """丢掉缓存，下次访问重新读盘（改完 .f4lt_local.json 想立即生效时用）。"""
    global _cache
    _cache = None


def local_path(key: str) -> str | None:
    """按「环境变量 > 本地配置文件」取路径；都没有返回 None。

    key 取值：mods_dir / game_data_dir / strings_dir
    """
    env = _ENV_NAMES.get(key)
    if env:
        value = os.environ.get(env)
        if value:
            return value
    value = _local_config().get(key)
    return value if isinstance(value, str) and value.strip() else None


def require_path(key: str, hint: str) -> str:
    """取一个**必需**的路径；没有就抛带指引的错误（而不是静默用错路径）。"""
    value = local_path(key)
    if value:
        return value
    env = _ENV_NAMES.get(key, key.upper())
    raise SystemExit(
        f"\u274c 未配置本机路径：{key}\n"
        f"   这是本机专属路径，不写进源码。任选一种方式提供：\n"
        f"   1) 环境变量  {env}=<路径>\n"
        f"   2) 本地配置文件  {LOCAL_CONFIG_PATH}\n"
        f'      {{ "{key}": "<路径>" }}\n'
        f"   3) 命令行参数  {hint}\n"
        f"   模板见 {PROJECT_ROOT / '.f4lt_local.example.json'}"
    )


# ── notes：本机专属的自由备注 ────────────────────────────────────────────

def notes() -> dict:
    """整份取 "notes" 段；没有就返回空 dict。

    值为 URL / 账号 / 备忘文字等都原样返回 —— 本模块不解释内容。
    """
    value = _local_config().get("notes")
    return dict(value) if isinstance(value, dict) else {}


def local_note(key: str, default: str | None = None) -> str | None:
    """按子键取一条备注；查不到返回 default。

    取值顺序：环境变量（仅 _NOTE_ENV_NAMES 里登记的键）> notes 段。
    值不是字符串时按 str() 转换（方便存数字/布尔）。
    """
    env = _NOTE_ENV_NAMES.get(key)
    if env:
        value = os.environ.get(env)
        if value:
            return value
    value = notes().get(key)
    if value is None:
        return default
    return value if isinstance(value, str) else str(value)


def require_note(key: str, hint: str) -> str:
    """取一条**必需**的备注；没有就抛带指引的错误。"""
    value = local_note(key)
    if value:
        return value
    env = _NOTE_ENV_NAMES.get(key, f"F4LT_{key.upper()}")
    raise SystemExit(
        f"\u274c 未配置本机备注：notes.{key}\n"
        f"   任选一种方式提供：\n"
        f"   1) 环境变量  {env}=<值>\n"
        f'   2) 本地配置文件  {LOCAL_CONFIG_PATH} 的 "notes" 段\n'
        f'      {{ "notes": {{ "{key}": "<值>" }} }}\n'
        f"   3) 命令行参数  {hint}"
    )
