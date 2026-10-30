#pragma once

#include <cstdint>
#include <string>

// ── 4CC / 记录签名 / 序号上限：**全项目唯一一份** ──────────────────────────
//
// 为什么单独拎出来：同一个值以前有两个名字（`kFullChunk4CC` / `kFull`、
// `kInnrSignature` / `kInnrSig`、`kMaxSeqPerChunk` ×2）⇒ **改一处忘一处**。
// 而 4CC 的字节序在本项目踩过不止一次（多字符字面量 `'DESC'` 那次 509/509 全失败）。
//
// ⚠️ 这些值都是**小端**拼出来的：文件里的字节序是 'F','U','L','L'，
//    读成 U32 = 'F' | 'U'<<8 | 'L'<<16 | 'L'<<24。**别用多字符字面量**。
//    反向（U32 → 可读名）用 `Chunk::ChunkName()`。
namespace Chunk
{
	// 字段 4CC
	inline constexpr std::uint32_t kFull = 0x4C4C5546u;   // "FULL"
	inline constexpr std::uint32_t kWnam = 0x4D414E57u;   // "WNAM"
	inline constexpr std::uint32_t kDesc = 0x43534544u;   // "DESC"

	// 记录签名 4CC
	inline constexpr std::uint32_t kInnr = 0x524E4E49u;   // "INNR"
	inline constexpr std::uint32_t kInfo = 0x4F464E49u;   // "INFO"

	// 「同一个 chunk 在本次读取里第几次出现」的上限（异常数据兜底）。
	// `Replace` 与 `Translation` 各有一套计数器，但**上限必须同一个数**。
	inline constexpr int kMaxSeqPerChunk = 512;

	// U32 → 4 字符可读名（用于日志与建键）。
	[[nodiscard]] inline std::string ChunkName(std::uint32_t a_4cc)
	{
		char s[5]{ static_cast<char>(a_4cc), static_cast<char>(a_4cc >> 8),
			static_cast<char>(a_4cc >> 16), static_cast<char>(a_4cc >> 24), 0 };
		return std::string{ s };
	}
}
