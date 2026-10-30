#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// ini 的**纯解析**部分：只吃字符串，不碰引擎 / REX / 文件系统。
//
// 为什么单独拎出来：这几个函数是 [名单] / [审计签名] / [关注记录] 的唯一入口，
// 而它们的失败方式全是**静默**的（中文逗号不认、十六进制写错、大小写…），
// 排查起来极贵。挪到这里之后 `tools\selftest.py` 能把它们**单独编一遍**跑表驱动
// 用例（不需要游戏、不需要 CommonLib），见该脚本头部说明。
//
// ⚠️ 本头文件**不得**引入 REX / RE / Windows 依赖 —— 否则 selftest 编不过。
//    需要打日志的调用方（Config.cpp）自己打：这里只**回传原因**（`a_error`）。
// ─────────────────────────────────────────────────────────────────────────────

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "StrUtil.h"

namespace ConfigParse
{
	inline std::string Trim(std::string_view a_text)
	{
		const auto begin = a_text.find_first_not_of(" \t\r\n");
		if (begin == std::string_view::npos) {
			return {};
		}
		const auto end = a_text.find_last_not_of(" \t\r\n");
		return std::string{ a_text.substr(begin, end - begin + 1) };
	}

	inline std::string Lower(std::string_view a_text)
	{
		std::string out{ a_text };
		for (auto& c : out) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return out;
	}

	inline bool Truthy(std::string_view a_text)
	{
		const auto v = Lower(Trim(a_text));
		return v == "1" || v == "true" || v == "on" || v == "yes";
	}

	// 全角分隔符（UTF-8）：`，` = EF BC 8C、`、` = E3 80 81。
	//
	// ⚠️ 为什么非得认它们：ini 是**人手写的**，中文输入法下敲出来的就是 `，`。
	//    只认 ASCII 逗号时 `Fallout4.esm，DLCCoast.esm` 会被当成**一个**插件名
	//    ⇒ 永不匹配、且一声不响（[名单] 静默失效，而名单是节点 3 的命门）。
	inline bool IsWideSeparator(std::string_view a_text, std::size_t a_pos, std::size_t& a_len)
	{
		a_len = 0;
		if (a_pos + 3 > a_text.size()) {
			return false;
		}
		const auto b0 = static_cast<unsigned char>(a_text[a_pos]);
		const auto b1 = static_cast<unsigned char>(a_text[a_pos + 1]);
		const auto b2 = static_cast<unsigned char>(a_text[a_pos + 2]);
		if (b0 == 0xEF && b1 == 0xBC && b2 == 0x8C) {   // ，
			a_len = 3;
			return true;
		}
		if (b0 == 0xE3 && b1 == 0x80 && b2 == 0x81) {   // 、
			a_len = 3;
			return true;
		}
		return false;
	}

	inline std::vector<std::string> SplitComma(std::string_view a_text)
	{
		std::vector<std::string> out;
		std::string              current;
		const auto               flush = [&]() {
			if (auto v = Trim(current); !v.empty()) {
				out.push_back(std::move(v));
			}
			current.clear();
		};
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			const char c = a_text[i];
			if (c == ',') {
				flush();
				continue;
			}
			std::size_t wide = 0;
			if (IsWideSeparator(a_text, i, wide)) {
				flush();
				i += wide - 1;   // 跳过剩下两个字节（for 的 ++i 再推一位）
				continue;
			}
			current.push_back(c);
		}
		flush();
		return out;
	}

	// ── 节点 3 名单的一条 ──────────────────────────────────────────────────
	// 条目形态（都可带 * 通配）：
	//   SomeMod.esp              插件级 —— 该插件的所有记录
	//   SomeMod.esp|0012AB       记录级 —— 插件内原始 ID（十六进制）
	//   *|000800                 任意插件里的这条记录
	//   插件名不分大小写
	struct Entry
	{
		std::string   plugin;             // **已小写**（匹配时零分配比较）
		std::uint32_t rawID{ 0 };
		bool          pluginWild{ false };
		bool          idWild{ true };     // 没写 |ID 就是记录级通配
		bool          valid{ false };
	};

	// 解析一条。失败时 `valid=false`，原因写进 `a_error`
	// （调用方自己决定怎么报 —— 这里不能引 REX，见文件头）。
	inline Entry ParseEntry(std::string_view a_text, std::string* a_error = nullptr)
	{
		Entry            entry;
		const auto       bar = a_text.find('|');
		std::string_view plugin = bar == std::string_view::npos ? a_text : a_text.substr(0, bar);
		std::string_view idPart = bar == std::string_view::npos ? std::string_view{} : a_text.substr(bar + 1);
		const auto       pluginTrimmed = Trim(plugin);
		const auto       idTrimmed = Trim(idPart);
		if (pluginTrimmed.empty()) {
			if (a_error) {
				*a_error = "插件名是空的";
			}
			return entry;
		}
		entry.plugin = Lower(pluginTrimmed);
		entry.pluginWild = pluginTrimmed == "*";
		if (idTrimmed.empty() || idTrimmed == "*") {
			entry.idWild = true;
		} else {
			entry.idWild = false;
			std::uint32_t parsed = 0;
			const auto    result = std::from_chars(idTrimmed.data(), idTrimmed.data() + idTrimmed.size(), parsed, 16);
			if (result.ec != std::errc{} || result.ptr != idTrimmed.data() + idTrimmed.size()) {
				if (a_error) {
					*a_error = "ID 不是十六进制";
				}
				return entry;
			}
			entry.rawID = parsed;
		}
		entry.valid = true;
		return entry;
	}

	inline bool Match(const Entry& a_entry, std::string_view a_plugin, std::uint32_t a_rawID)
	{
		if (!a_entry.valid) {
			return false;
		}
		if (!a_entry.pluginWild) {
			if (!Str::IEquals(a_entry.plugin, a_plugin)) {
				return false;
			}
		}
		return a_entry.idWild || a_entry.rawID == a_rawID;
	}

	inline bool AnyMatch(const std::vector<Entry>& a_entries, std::string_view a_plugin, std::uint32_t a_rawID)
	{
		for (const auto& entry : a_entries) {
			if (Match(entry, a_plugin, a_rawID)) {
				return true;
			}
		}
		return false;
	}
}