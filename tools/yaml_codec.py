#!/usr/bin/env python3
# ==============================================================================
# vendored: 上游副本 —— 来源 上游工作区 scripts/yaml_codec.py
# 为何复制：让 F4LoadText 仓自包含，做汉化不依赖工作区上层 scripts/
# 同步方式：手动（上游仍是权威版本）；复制日期 2026-10-10
# ==============================================================================

"""yaml 编辑流程 —— 三个脚本共用的一套「安全 yaml」约定。

    esp  →  yaml（人改译文）  →  esp
    export_esp_yaml.py        apply_esp_yaml.py

为什么需要单独这一层，而不直接 `yaml.safe_dump`
-------------------------------------------------
**FO4 的文本用 CRLF 换行**（`\\r\\n`），而 yaml 的 block scalar（`|` / `>`）会
**静默丢掉 CR** —— 这是实测结果：

    >>> yaml.safe_load("t: |\\n  A\\r\\n  B\\r\\n")["t"]
    'A\\nB\\n'          ← CR 没了，而且行尾还多出一个 \\n

人用编辑器打开 yaml 改译文时，最自然的写法就是把长文本改成 `|` 块。
**只要他这么写一次，CRLF 就没了**，写回 esp 后文本结构被破坏，而且不报错。

所以本模块强制：**所有来自/去往 esp 的字符串，一律用双引号 + `\\r\\n` 转义**，
PyYAML 不会为带 `\\r` 的字符串选择 block scalar，从格式上杜绝这个坑。

这与现有 `F4TCHook/translations/*.txt` 的约定完全一致（换行写成 `\\r\\n`），
不引入新的心智负担。

另外：`width` 调大到 10_000，避免 PyYAML 在长文本中间折行
（折行会把一个字符串拆成多行，人改的时候极易漏看）。
"""

from __future__ import annotations

import yaml

# PyYAML 默认在 80 列折行，长文本会被切成多行 —— 人改的时候极易漏看。
LINE_WIDTH = 10_000


class CRStr(str):
    """标记类型：让 Dumper 强制用双引号（而非 block scalar）序列化。"""


class PathStr(str):
    """标记类型：文件系统路径 → 单引号，不解析转义。"""


class _Dumper(yaml.SafeDumper):
    pass


def _represent_crstr(dumper: yaml.SafeDumper, data: CRStr) -> yaml.ScalarNode:
    # style='"' → PyYAML 自己做 \r \n \" 转义，绝不会用 | 或 >
    #
    # ⚠️ 但 PyYAML 的 style='"' **不转义反斜杠本身**：Windows 路径
    #   `X:\MO2\mods` 里的 `\M` 会被当成非法转义，load 时抛 ScannerError。
    #   所以反斜杠要自己先补一层（见 dump()）。
    return dumper.represent_scalar("tag:yaml.org,2002:str", str(data), style='"')


def _represent_pathstr(dumper: yaml.SafeDumper, data: PathStr) -> yaml.ScalarNode:
    # 单引号：yaml 不解析里面的转义，Windows 反斜杠原样保留
    return dumper.represent_scalar("tag:yaml.org,2002:str", str(data), style="'")


_Dumper.add_representer(CRStr, _represent_crstr)
_Dumper.add_representer(PathStr, _represent_pathstr)


def dump(data, **kwargs) -> str:
    """按安全约定序列化。所有 str 都会被强制成双引号转义式。"""
    kwargs.setdefault("Dumper", _Dumper)
    kwargs.setdefault("allow_unicode", True)
    kwargs.setdefault("default_flow_style", False)
    kwargs.setdefault("sort_keys", False)
    kwargs.setdefault("width", LINE_WIDTH)
    return yaml.dump(_wrap(data), **kwargs)


def _wrap(obj, _key=None):
    r"""递归把 str 包成 CRStr。

    ⚠️ `path` 这类**文件系统路径**要单独处理：PyYAML 的 style='"' 不转义反斜杠，
       而 Windows 路径含反斜杠+字母的序列（如 X:\MO2\mods）会被当成非法转义，
       load 时抛 ``found unknown escape character``。
       所以路径改用**单引号**（yaml 单引号不解析转义，正是我们要的）。

    其余字符串（source/dest 等）走 CRStr 双引号 —— 那里需要 \r\n 被正确解析。
    """
    if isinstance(obj, str):
        if _key == "path":
            return PathStr(obj)
        return CRStr(obj)
    if isinstance(obj, dict):
        return {k: _wrap(v, k) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_wrap(v, _key) for v in obj]
    return obj


def load(text: str):
    """反序列化。注意：导入侧要额外做一次「CR 归一化」兜底（见 apply 脚本）。"""
    return yaml.safe_load(text)


def load_file(path) -> dict:
    return load(path.read_text(encoding="utf-8"))
