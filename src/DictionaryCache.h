#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "Translation.h"

// 译文字典的**二进制缓存**（`dictionary.cache`）—— 从 `Translation.cpp` 抽出来的。
//
// 为什么独立：这个格式两天改了 3 版（v5→v6→v7），而它跟“查表 / 建键 / 判定”毫无关系。
// 改格式只动这一个文件 + 一个版本号，不会误碰到核心逻辑。
//
// ⚠️ 两条硬约束（别改坏）：
//   ① **缓存只是加速手段**：缺失 / 损坏 / 版本不符 / yaml 变了 ⇒ 一律静默重建，
//      绝不能因为缓存问题导致译文不加载。
//   ② 写缓存用**原子写**（先 `.tmp` 再 rename），避免下次读到写了一半的文件。
namespace DictionaryCache
{
	// 写（失败只告警，不影响已建好的表）。`a_stats` 里只用到 5 个跳过计数。
	void Save(const std::filesystem::path& a_dir, const Translation::Stats& a_stats,
		const std::unordered_map<std::string, std::string>&              a_single,
		const std::unordered_map<std::string, std::vector<std::string>>& a_multi);

	// 读：指纹（文件名+大小+修改时间）不符 / 损坏 / 版本不符 ⇒ false（调用方重建）。
	// 成功时把 `files` 与 5 个跳过计数一起恢复（否则缓存命中时日志会打全 0）。
	bool TryLoad(const std::filesystem::path& a_dir, Translation::Stats& a_stats,
		std::unordered_map<std::string, std::string>&              a_single,
		std::unordered_map<std::string, std::vector<std::string>>& a_multi);
}
