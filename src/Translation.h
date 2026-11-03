#pragma once

#include <cstdint>
#include <string_view>

namespace RE
{
	class TESFile;
}

// 译文字典（`[汉化]`）—— 直接读**本仓** `translations\*.yaml`（源自 F4TCHook，2026-10-09 搬入并入库）。
//
// 为什么要它：那份 yaml 是**人手改的译文来源**（每条带 `dest`），原本要跑
// `scripts\build_flatbuffer_dict.py` 编成 `f4dict.fb` 才能进游戏。这里直接读文本 yaml
// ⇒ **改完 yaml 重新开游戏就生效**（不用再跑那个脚本）。
//
// ── 二进制缓存（2026-10-09 加）───────────────────────────────────
// yaml-cpp 解析 5 MB yaml 实测 **≈950 ms**（Release），而它在 `F4SEPluginLoad` 里
// 同步跑 ⇒ 直接拖慢游戏启动。故首次解析后把表序列化成 `字典目录\dictionary.cache`，
// 后续启动只校验 yaml 指纹（name+size+mtime）全等就直接加载（**实测 ≈4 ms**）。
// yaml 增删改 ⇒ 指纹不符 ⇒ 静默重建。缓存**只是加速手段**，任何异常都不会导致译文丢失。
// （注意：**不做惰性加载** —— 游戏中不可预测的卡顿比启动多 1 秒糟得多。）
//
// ⚠️ 与现有「保基底」的分工（必看，否则两边打架）：
//   * 本项目原有的唯一写入动作 = **名单内记录被名单外插件覆盖时写回基底**。
//   * yaml 管两类：插件**自己的记录** + 插件**覆盖官方**的记录（后者靠 master 反查把键
//     归到被覆盖的那个 master，如 `Fallout4.esm`）—— 与运行时归属判定同一口径。
//     优先级：**yaml 译文 > 基底**（用户 2026-10-08 裁定）。
//
// 键 = (插件名, 插件内原始 ID, 字段 4CC)；序号（同字段第几次出现）在查表时用：
//   · 插件内原始 ID：普通 esp `& 0xFFFFFF`、ESL `& 0xFFF`（由 yaml 的 `is_light` 决定）——
//     与 `Replace::PluginAndRawIDOf` 给出来的低位同一个口径 ✓
//   · 序号：**独立计数器**（见 .cpp 里 NextSeq 的注释：绝不与 Replace 的共用）
namespace Translation
{
	struct Stats
	{
		std::uint32_t files{ 0 };
		std::uint32_t entries{ 0 };          // 收进索引的
		std::uint32_t skippedForeign{ 0 };   // 被跳过的不可信条目（高位超出本文件 masters / master 反查为空）
		std::uint32_t skippedEmpty{ 0 };     // dest 为空 / 与 text 相同
		std::uint32_t skippedHeader{ 0 };    // TES4 头
		std::uint32_t skippedVariants{ 0 };  // FULL 的 INNR 多变体组（整组丢弃，见 .cpp 注释）
		std::uint32_t duplicates{ 0 };       // 同 (键, 序号) 重复（后写的胜）
		std::uint32_t conflicts{ 0 };        // 同一个 esp 被多份 yaml 声明 → 被整份忽略的文件数
	};

	// 扫 ini 指定的目录下的 `*.yaml` 建索引。返回收进索引的条目数（0 = 没读到/关着）。
	// 先试二进制缓存（`dictionary.cache`，指纹全等才用）；未命中则解析 yaml 并（可配）写回缓存。
	std::uint32_t Load();

	// 是否已加载（关着/失败时为 false；查表直接返回空）。
	bool Ready();

	// 取本次读取的序号（同一条记录里这个字段第几次出现）。
	// 返回 -1 = 超限（视为不可信）。
	int NextSeq(const RE::TESFile* a_file, std::uint32_t a_chunk4CC, std::string_view a_plugin,
		std::uint32_t a_rawID);

	// 查译文（显式给序号）。命中返回非空。
	[[nodiscard]] std::string_view Lookup(std::string_view a_plugin, std::uint32_t a_rawID,
		std::uint32_t a_chunk4CC, int a_seq);

	// 便捷版：自己取序号再查（读盘期用）。
	// ⚠️ 若 `a_plugin`（归属）≠ 正在读的 esp ⇒ 走**带来源**的键（见 LookupFrom）。
	// ⚠️ `a_loaded` = 引擎本次读进来的原值 —— 单值防盖戳护栏用它判断
	//    “这其实是多值字段/过时单值条目”（见 .cpp 的 LookupChunk 注释）。
	[[nodiscard]] std::string_view LookupChunk(const RE::TESFile* a_file, std::string_view a_plugin,
		std::uint32_t a_rawID, std::uint32_t a_chunk4CC, std::string_view a_loaded);

	// 显式指定**来源 esp** 查表（显示层用：那里拿不到“正在读的 TESFile”，
	// 只能用 `TESForm::sourceFiles.back()` = 最后写入者）。语义与 LookupChunk 的覆盖分支一致：
	// 键是 `归属|raw|CHUNK|@来源`，同一条官方记录被多个 mod 覆盖时只有胜出那份的译文能用。
	[[nodiscard]] std::string_view LookupFrom(std::string_view a_owner, std::string_view a_source,
		std::uint32_t a_rawID, std::uint32_t a_chunk4CC, int a_seq = 0);

	Stats Snapshot();

	// 性能计数：热路径预筛的效果。
	//   calls  = Lookup 被调用的总次数
	//   passed = 通过预筛（真的建键查了表）的次数
	// 前者应该远大于后者（全游戏十几个插件有译文）；若两者接近 ⇒ 预筛失效。
	struct Perf
	{
		std::uint64_t calls{ 0 };
		std::uint64_t passed{ 0 };
	};
	[[nodiscard]] Perf PerfSnapshot();

	// 单值防盖戳护栏跳过的回写次数（见 .cpp 的 LookupChunk）。
	// >0 说明遇到了「yaml 把多值字段当单值」的过时条目（已止血）。
	[[nodiscard]] std::uint64_t SuppressedCount();
}
