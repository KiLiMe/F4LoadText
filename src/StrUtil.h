#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

// ── 零分配 / 一处实现的小工具（**热路径专用**）────────────────────────────
//
// 为什么要单独拎出来：下面这些调用都发生在「每条记录的每个字段」上（实测一轮 17.6 万次），
// 而旧写法是 `_stricmp(a_file->filename, std::string{ plugin }.c_str())` ——
// 每次比较都**堆分配一个 std::string**；`Replace::IsMasterOf` 那处还在**循环体内**
// （每条覆盖记录分配 N 次，N = 该文件的主文件数）。
namespace Str
{
	// ASCII 大小写不敏感比较（非 ASCII 字节按原样比 —— 插件名/路径都是 ASCII，够用）。
	[[nodiscard]] inline bool IEquals(std::string_view a_lhs, std::string_view a_rhs)
	{
		if (a_lhs.size() != a_rhs.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a_lhs.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(a_lhs[i])) !=
				std::tolower(static_cast<unsigned char>(a_rhs[i]))) {
				return false;
			}
		}
		return true;
	}

	// ASCII 小写化（返回新串）。用在**建键**与扩展名比较上 —— 不要求快，要求一处实现。
	[[nodiscard]] inline std::string LowerAscii(std::string_view a_s)
	{
		std::string out{ a_s };
		for (auto& c : out) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return out;
	}
}