#pragma once

#include <cstdint>

#include "RE/B/BGSLocalizedString.h"

#include <string>
#include <string_view>

namespace RE
{
	class TESFile;
}

// 节点 2：按类别把文本统一替换（默认关闭，靠 F4LoadText.ini 打开）。
namespace Replace
{
	struct Stats
	{
		std::uint64_t fieldIgnored{ 0 };   // 「忽略文本传入」数（保住基底文本）
		std::uint64_t fieldFromBase{ 0 };  // 其中靠"自取基表文本"救回来的（引擎没读基表那份）
		std::uint64_t fieldTranslated{ 0 };  // 靠 yaml 译文写回的（`[汉化]`，见 Translation.h）
		// 显示层（GetText）的计数**不在这里**：它在 `kGameDataReady` 时必然为 0（界面还没开），
		// 由 `Replace::HandleInlineText` 自己按数量级打点（1/10/100/1000/10000）。
	};

	void             Init();                                  // Config::Load 之后调用
	bool             Enabled();
	// 读盘期 FULL 钩子用：只有 TESFullName*，分类靠「正在读的那条记录的签名」
	// ── 加载器钩子专用：忽略文本传入 ──────────────────────────────────
	// 名单外的文本一律走引擎原逻辑（返回 false 表示不碰）。
	// 命中名单时：写回「基表那份」（a_previous，或记住的那份）。
	// a_previous = 调原函数**之前**的字段值；a_ownerRead = 这次读的就是归属插件自己（不是覆盖）。
	// a_plugin = 归属（拥有者）插件；a_readerListed = **正在读它的那个插件**在不在名单里。
	// 规则：读取者在名单里 → 采用它读进来的文本；否则按归属判定（忽略传入）。
	[[nodiscard]] bool HandleFullNameField(RE::TESFullName& a_field, const RE::TESFile* a_file,
		std::uint32_t a_formCode, std::string_view a_plugin, std::uint32_t a_rawID, std::string_view a_previous,
		std::string_view a_loaded, bool a_ownerRead, bool a_readerListed);
	[[nodiscard]] bool HandleLocalizedField(RE::BGSLocalizedString& a_field, const RE::TESFile* a_file,
		std::uint32_t a_formCode, std::string_view a_plugin, std::uint32_t a_rawID, std::string_view a_previous,
		std::string_view a_loaded, bool a_ownerRead, bool a_readerListed);

	// ── 显示层（第二条，2026-11-08）：非本地化插件的**内联**字符串 ──────────────
	// 为什么必须在这里：localized=false 的插件，读盘期**一个字节都不读**这些字段
	//   （实测：UFO4P 的 QUST CNAM 在 Load/Load2 里一次都不出现）。
	//   BGSLocalizedStringDL::id 里存的是**文件内偏移**，界面来取时才由
	//   BGSLocalizedStringDL::GetText(TESFile&) 现场按偏移读原文 ⇒ 没有“字段”可写。
	// ⚠️ **不要试图从 `BGSLocalizedStringDL` 拿“第几条”**（2026-11-08 实测：本 hook 上它的 id 恒为 0）。
	//   多值字段（QUST 的 15 条 CNAM）靠**引擎当前 chunk 长度**（`TESFile::actualChunkSize`）
	//   去比基表里各条 chunk 的长度来定序号（见 `Replace.cpp` 的 `ChunkEntriesOf`）。
	// ⚠️ 只改**这一次调用返回的缓冲**（ScrapStringBuffer），记录本身不动。
	// ⚠️ 新串必须从**同一个 scrap heap** 分配（那个缓冲的析构器会把 ptr 还回堆）。
	[[nodiscard]] bool HandleInlineText(RE::BGSLocalizedStrings::ScrapStringBuffer& a_out, const RE::TESFile* a_file);

	// 显示层（GetText）：**我们自己**（`BaseText::ResolveViaEngine`）也会调 GetText。
	// 那种调用不在“正在读某条记录”的上下文里 —— `TESFile::currentform / currentchunkID`
	// 还是上一次读盘留下的**陈旧值**，拿它建键可能命中别的记录的译文（写错文本）。
	// ⇒ 自己调 GetText 时置上，`HandleInlineText` 看到就直接返回。
	void SetSuppressInline(bool a_on);

	// 逐条审计（文本日志）：仅**真改动**时打（a_read != a_kept）。
	// 上限由 ini 的 [调试] 审计上限 控制（0 = 关，默认 300）。
	//   [读它的插件] 签名 formID 偏移 实际文本  →  [归属插件] 基表/译文  →  回写为 …
	//
	// ⚠️ 不再收 `a_read` / `a_written` / `a_formCode`（2026-10 收敛）：
	//   * 三个调用点原本都传同一个值（`dest, dest` / `keep, keep`）⇒ 打出来永远是
	//     “文本='X' → 回写为 'X'”，白占两个参数；真正写进去的就是 a_kept。
	//   * `a_formCode` 与 `a_file->currentform.form` 是**同一件事的两份来源**，一旦不同源
	//     就会打出“签名 formID 对不上”的假线索 —— 日志的可信度是项目硬纪律，所以只留一份：
	//     **签名与 formID 一律从 a_file 取**。
	void LogAudit(const char* a_leg, const RE::TESFile* a_file, std::string_view a_owner,
		std::uint32_t a_rawID, std::string_view a_read, std::string_view a_kept);

	// 记住「名单内插件自己读进来的文本」：覆盖记录（尤其压缩记录）读进来时字段往往已被清空，
	// 这时要写回的不是"读前那份"（空的 ✗），而是**基表那份**。
	// 记忆的键 = **(归属插件, 低位ID, chunk 4CC, 同 chunk 内序号)** ——
	//   为什么不用字段地址 ✗：引擎解析子记录用的是**会复用的临时对象**，
	//   同一记录里不同字段的地址会撞车（实测：ARMO 的 "Default"/"Light" 被塞进了 FULL 的中文名 ✗）。
	//   序号必须**按读取文件分开计**（否则基表与覆盖两次读取会串号）✓
	//
	// a_seq = 调用方**已经占好的号**（-1 = 本函数自己取）。
	//   ⚠️ 为什么需要它：一次钩子调用里可能有“命中译文就提前返回”的分支，
	//   如果那个分支不占号，后面几句的 seq 就整体**左移一位** ⇒ 第 2 句拿到第 1 句的基表文本
	//   （= 2026-10-31 “字幕总是同一句”的第二条成因）。⇒ 序号必须在**函数入口**取一次。
	void RememberOwnText(std::uint32_t a_chunkID, std::string_view a_text, std::string_view a_plugin,
		std::uint32_t a_rawID, const RE::TESFile* a_file, int a_seq = -1);
	// 取回“基表那份”。
	//   a_file   = **正在读的那个文件**（序号按它分开计，必传）
	//   a_seqOut = 出参：本次读取的序号（-1 = 超限）。
	//      ⚠️ seq **不是“数组下标”**：它是“这个 chunk 在本次读取里第几次出现”，
	//      而引擎会把同一个单值字段（实测 `FULL`）重复读很多次。别拿它做数组长度校验。
	[[nodiscard]] std::string_view RememberedOwnText(std::uint32_t a_chunkID, std::string_view a_plugin,
		std::uint32_t a_rawID, const RE::TESFile* a_file, int* a_seqOut = nullptr, int a_seq = -1);

	// 这条记录是否在名单里（不看类别值，只看 [名单]）
	[[nodiscard]] bool IsListed(std::string_view a_plugin, std::uint32_t a_rawID);
	Stats            Snapshot();

	// 归属判定（读盘期专用）：按**本文件自己的主文件表**判，不要拿记录头里的高位去查全局编译索引表 ——
	// 那个高位是「作者当年的索引」，会跟运行时索引撞车。
	//   实测（2026-10-07，xEdit 复核）：IntegratedAutomatron.esp（5 个主文件）的自有记录在盘上是
	//   0x05000B51，游戏里编译索引 5 = DLCworkshop02.esm → 被当成「官方记录的覆盖」而误改。
	// 返回 false 表示归属不可信（此时一个字都不写）。
	[[nodiscard]] bool PluginAndRawIDOf(const RE::TESFile* a_file, std::uint32_t a_formID,
		std::string_view& a_plugin, std::uint32_t& a_rawID);
}
