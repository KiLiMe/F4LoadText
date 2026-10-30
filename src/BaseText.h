#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 从**基表插件**里取一条记录的某个文本 chunk，并解析成最终文本。//
// 为什么需要它：有些被覆盖的记录，引擎**只解析胜出的覆盖**，基表那份文本从未进内存
//（实测 INFO 0x000CE9F6：两条读取路径都只有 mod 的英文，Fallout4.esm 一次都没读）。
// 这时"记住基表文本"无从记起，只能由我们自己把基表那份读出来。
//
// 解析方式：
//   · chunk 是内联文本（非本地化插件）→ 直接就是文本
//   · chunk 是 4 字节 DL 字符串 ID（本地化插件）→ 用引擎自己的字符串表
//     （BGSLocalizedStringDL::GetText）解析 —— 它带的就是玩家当前那份表（含汉化）
//
// 读不到就返回空（调用方什么都不写）。
namespace BaseText
{
	// a_displayLayer = true：调用方是**显示层**（不在读盘上下文）。
	//   那时 4 字节字符串 id → 文本 这一步**优先交给引擎**
	//   （`BGSLocalizedStringDL{id}.GetText(TESFile&)`）：语言、MO2 覆盖优先级、
	//   松文件 vs BA2、三种表的格式差异 —— 全是引擎已经做对的事，不该我们重造。
	//   ⚠️ **读盘期绝不能这么干**：实测拿回来是引擎的 `LOOKUP FAILED!` 占位
	//   （这个解析依赖“正在读那个文件”的上下文，见 Replace.cpp 的实测注释）
	//   ⇒ 读盘期保持自解字符串表。
	[[nodiscard]] std::string_view Get(std::string_view a_plugin, std::uint32_t a_formID, std::uint32_t a_chunk4CC,
		std::string_view a_leg = {}, bool a_displayLayer = false);

	// 取chunk 的**原始 4 字节**（本地化字符串表 id），**不查表**。
	// 显示层用：拿到 id 后交给引擎自己的 `BGSLocalizedStringDL::GetText(TESFile&)` 解析 ——
	// 它带的是玩家当前那份表（含汉化），所以**不需要**知道表文件在哪（MO2 虚拟目录也能对）。
	//   a_outId = 返回的 id（成功时）
	// 返回 false = 该记录没有这个 chunk，或它不是 4 字节 id。
	[[nodiscard]] bool GetLocalizedID(std::string_view a_plugin, std::uint32_t a_formID, std::uint32_t a_chunk4CC,
		std::uint32_t& a_outId);

	// 取chunk 的原始字节（不解析）。返回 false = 没这条记录/ 没这个 chunk。
	bool GetRawChunk(std::string_view a_plugin, std::uint32_t a_formID, std::uint32_t a_chunk4CC,
		std::vector<std::uint8_t>& a_out);
}
