# F4Text

> 展示名 `F4Text`；**目录名 / DLL 名 / ini 名 / 日志名仍是 `F4LoadText`**（未改路径）。

B 社引擎不支持运行时多语言，mod 文本靠作者自填表，多语言需要依赖（`STRINGS` / `ILSTRINGS` / `DLSTRINGS`），
没填就是英文 —— 于是每装一个 mod 还得再装一个汉化包；新装的 esp 自带英文，
会覆盖上层加载的文本。

本项目将终结这件事。

## 设计原理

**mod 读取拦截**：hook 文本加载环节，按 **插件名 + 插件内 rawFormID** 判定记录身份；
**名单内**记录若被**名单外**插件覆盖，则丢弃那份覆盖、写回基底文本。

另有两件事：

* **`[汉化]` 译文字典** —— 读 `F4SE\Plugins\F4LoadText\translations\*.yaml`（`dest`），
  写进**插件自己的记录** ⇒ 改完 yaml 重开游戏就生效，不用重建任何二进制字典。
  优先级：**yaml 译文 > 基底**。
  ⚠️ **本仓不附带译文**（那是各人自己的运行数据）；发布包只提供一个空目录 + 说明。
* **`DESC`（描述文本）** —— 它在内存里只是延迟解析的 4 字节 id，读盘期写不进去
  ⇒ 用**显示层** `TESDescription::GetDescription` 换出参。
  这是本项目**唯一**的显示层钩子（`DESC` 属介绍/描述，不受「不碰 UI」约束）。

⇒ 加载期只 hook 加载文件；`DESC` 是受控例外。

## 安装（玩家）

1. 需要 **F4SE** 与 [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327)。
2. 把发布包 `F4SE\` 目录解压到 MO2 的 mod 目录（或直接放进 `<游戏>\Data`）。
3. 在 MO2 里启用。日志在 `<Documents>\My Games\Fallout4\F4SE\F4LoadText.log`。
4. 默认开关全在 `F4SE\Plugins\F4LoadText.ini`，每一项都有注释。

## 构建（开发者）

依赖：MSVC 2022、CMake ≥ 3.24、[vcpkg](https://vcpkg.io)（依赖由 `vcpkg.json` 的 `builtin-baseline` 锁定）、[uv](https://docs.astral.sh/uv/)。

```powershell
cmake --preset default                    # 只需一次：生成 VS2022 工程
uv run python tools\deploy.py             # 编译 + 部署到 MO2 mod 目录
uv run python tools\deploy.py --check     # 只看状态，不动文件
uv run python tools\selftest.py           # 自检（不开游戏）
uv run python tools\package_release.py    # 打发布包 → dist/
```

- 配置：仓内 `config/F4LoadText.ini` 是**唯一权威** —— 部署会整份覆盖到
  `<游戏>\Data\F4SE\Plugins\F4LoadText.ini`。要调开关就改仓内那份再部署。
- 部署目标（MO2 mods 目录）不走源码：按「命令行参数 > 环境变量 `F4LT_MODS_DIR` >
  仓根 `.f4lt_local.json`（照 `.f4lt_local.example.json` 建）」取。
- 日志：`<Documents>\My Games\Fallout4\F4SE\F4LoadText.log`
  启动应有 **3 行 `hook ok`** + **1 行 `hook 延后`**（`DESC` 那条由 `kGameDataReady` 装）。
- ⚠️ 本机 Documents 可能被重定向，别按 `%USERPROFILE%` 猜。
- ⚠️ `http_proxy` / `https_proxy` 若是用户级变量，会让 MSBuild 抛 `ArgumentException` ——
  `tools\deploy.py` 第 0 步已自动清理。

## 授权

[GPL-3.0](LICENSE)。

