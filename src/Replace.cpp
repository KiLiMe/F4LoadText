#include "PCH.h"

#include "Replace.h"

#include "BaseText.h"
#include "Config.h"
#include "Chunks.h"
#include "StrUtil.h"
#include "Translation.h"

#include <array>
#include <mutex>
#include <unordered_map>

#include "RE/T/TESForm.h"
#include "RE/T/TESFullName.h"

namespace
{
	// 约定 2 的正确落实：**直接写底层的 BSFixedStringCS**（走字符串池）。
	//
	// ⚠️ 不要用 BGSLocalizedString::operator= —— 它在「当前值带 <ID= 前缀」时会临时拼一个 vector
	//    然后把那个 vector 的 view 存进去，函数一返回就悬空。实测终端菜单项被写成 `58 58 00`("XX")/乱码。
	//    出处：FallHook 的 RuntimeTextStringAssign::AssignPlainLocalized 也是这么绕开的。
	void AssignText(RE::BGSLocalizedString& a_target, std::string_view a_text)
	{
		auto& raw = reinterpret_cast<RE::BSFixedStringCS&>(a_target);
		raw = a_text;   // 用 view 重载（前两轮实测不崩的版本）
	}

	// ⚠️ g_stats 的字段是**普通整数、非原子** —— 靠“读盘钩子单线程”这条实测结论（2026-10-18，
	//    17.6 万次调用/1 线程，见 docs/项目总览.md §8）保证不丢计数。
	//    若将来引擎改成并行读盘，这里要换 atomic。
	Replace::Stats    g_stats;
	std::atomic<bool> g_on{ false };
}

namespace
{
	// 本文件「自有记录」的运行时 ID：轻插件 → 0xFE + 小块索引；其余 → 编译索引
	std::uint32_t OwnRuntimeID(const RE::TESFile* a_file, std::uint32_t a_rawID)
	{
		if (a_file->IsLight()) {
			return 0xFE000000u |
			       ((static_cast<std::uint32_t>(a_file->smallFileCompileIndex) & 0xFFFu) << 12) |
			       (a_rawID & 0xFFFu);
		}
		return (static_cast<std::uint32_t>(a_file->compileIndex) << 24) | (a_rawID & 0xFFFFFFu);
	}

	// 按**运行时编译索引**反查插件名（诊断用）。
	// ⚠️ 不能只建一次：第一次调用发生在读盘早期，那时表里只有前几个插件；
	//    表会一直长到全部插件登记完 ⇒ 长度变了就重建。
	std::string_view ByCompileIndex(std::uint32_t a_index)
	{
		static std::vector<std::string_view> byCompile;
		static std::size_t                   built = ~0ull;

		const auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return {};
		}
		const auto files = dh->compiledFileCollection.files.size();
		if (files != built) {
			byCompile.assign(files, {});
			for (const auto* file : dh->compiledFileCollection.files) {
				if (file && file->compileIndex < byCompile.size()) {
					byCompile[file->compileIndex] = file->filename;
				}
			}
			built = files;
		}
		return a_index < byCompile.size() ? byCompile[a_index] : std::string_view{};
	}

	// 这个插件名是否在本文件的主文件表（`masterPtrs`）里？
	// 用途：③ 的双重校验 —— 反查出的拥有者**必须**是本文件声明的 master，
	// 否则宁可拒绝（防止“反查出的插件恰好也同名但并非拥有者”这类误判）。
	// ⚠️ 比的是 `masterCount` 个槽位，**不是** `masterCount` 下标 → 不能用 `masterPtrs[hi]`。
	// ⚠️ 比较用 `Str::IEquals`（零分配）：旧写法 `_stricmp(..., std::string{ a_plugin }.c_str())`
	//    在**循环体内**，每条覆盖记录要分配 N 次（N = 主文件数）。
	bool IsMasterOf(const RE::TESFile* a_file, std::string_view a_plugin)
	{
		if (!a_file || !a_file->masterPtrs || a_plugin.empty()) {
			return false;
		}
		for (std::uint32_t i = 0; i < a_file->masterCount; ++i) {
			const auto* m = a_file->masterPtrs[i];
			if (m && m->filename && Str::IEquals(m->filename, a_plugin)) {
				return true;
			}
		}
		return false;
	}

	// 若这条记录被「关注记录」指名 ⇒ 返回 true，并给出**两个候选归属**（诊断用）：
	//   a_owner = 按**高位反查编译索引**得到的插件（即“Base ID 的插件部分”）
	//   a_raw   = 插件内原始 ID（低位）
	// 判定失败时用得上：那时正常路径给不出归属，只能靠反查。
	bool FocusOwnerOf(const RE::TESFile* a_file, std::uint32_t a_formID, std::string_view& a_owner,
		std::uint32_t& a_raw, std::string_view& a_byMaster)
	{
		a_owner = {};
		a_raw = 0;
		a_byMaster = {};
		if (!a_file) {
			return false;
		}
		const auto hi = a_formID >> 24;
		a_raw = hi == 0xFEu ? (a_formID & 0xFFFu) : (a_formID & 0xFFFFFFu);
		a_owner = ByCompileIndex(hi);
		if (hi < a_file->masterCount && a_file->masterPtrs && a_file->masterPtrs[hi]) {
			a_byMaster = a_file->masterPtrs[hi]->filename;
		}

		// 命中判据（两种都可能：反查结果 / 本文件主文件表）
		const auto focus = [&](std::string_view a_plugin) {
			return !a_plugin.empty() && Config::IsFocus(a_plugin, a_raw);
		};
		return focus(a_file->filename) || focus(a_owner) || focus(a_byMaster);
	}

	// 归属判定失败的诊断：去重、最多 48 条（读盘期只跑一次，够定位问题就行）
	void LogIdentityReject(const RE::TESFile* a_file, std::uint32_t a_formID, std::uint32_t a_hi)
	{
		// 关注记录：**命中则无条件打**（不去重、不限额）—— 这正是“想知道为何没写回”的场景。
		// 判定失败时随行带上“按高位反查出的 Base ID”，否则只有个裸的 formID。
		{
			std::string_view owner;
			std::string_view byMaster;
			std::uint32_t    raw = 0;
			if (FocusOwnerOf(a_file, a_formID, owner, raw, byMaster)) {
				LOG_ADV_W("focus(unknown-owner): file={} record={:#010X} hi=0x{:02X} masterCount={} compileIndex={}",
					a_file ? a_file->filename : "?", a_formID, a_hi,
					a_file ? a_file->masterCount : 0, a_file ? a_file->compileIndex : 0);
				LOG_ADV_W("focus(unknown-owner): ^ BaseID={}:{:06X}; by-hi={}; by masterPtrs[{}]={}",
					owner.empty() ? "?" : owner.data(), raw,
					owner.empty() ? "? (hi not in compile index table)" : owner.data(),
					a_hi, byMaster.empty() ? "none" : byMaster.data());
				return;
			}
		}

		static std::mutex                  lock;
		static std::array<std::string, 48> seen{};
		static std::size_t                 count = 0;

		char key[80]{};
		std::snprintf(key, sizeof(key), "%s|%08X", a_file ? a_file->filename : "?", a_formID);

		std::scoped_lock l{ lock };
		for (std::size_t i = 0; i < count; ++i) {
			if (seen[i] == key) {
				return;
			}
		}
		if (count >= seen.size()) {
			return;
		}
		seen[count++] = key;
		// 身份判定失败：这是“我们判不出归属”的诊断，不是插件故障 ⇒ 高级类（[W] 级别保留）。
		// 去重：同一（文件,记录）只打一次，上限 48 条。
		LOG_ADV_W("owner-unresolved: file={} record={:#010X} hi=0x{:02X} masterCount={} compileIndex={} -> skipped, not written",
			a_file ? a_file->filename : "?", a_formID, a_hi,
			a_file ? a_file->masterCount : 0, a_file ? a_file->compileIndex : 0);
	}
}

bool Replace::PluginAndRawIDOf(const RE::TESFile* a_file, std::uint32_t a_formID,
	std::string_view& a_plugin, std::uint32_t& a_rawID)
{
	a_plugin = {};
	a_rawID = 0;
	if (!a_file) {
		return false;
	}

	const auto hi = a_formID >> 24;
	const auto raw = hi == 0xFEu ? (a_formID & 0xFFFu) : (a_formID & 0xFFFFFFu);

	// ① 自有记录的三种写法：轻插件已重映射 / 作者索引 = 主文件数 / 运行时编译索引
	if (hi == 0xFEu || hi == a_file->masterCount || hi == a_file->compileIndex) {
		a_plugin = a_file->filename;
		a_rawID = raw;
		return true;
	}
	// ② 引擎已按**本文件的空间**把这条表单建好了 → 自有（覆盖记录不会出现这条）
	if (RE::TESForm::GetFormByID(OwnRuntimeID(a_file, raw))) {
		a_plugin = a_file->filename;
		a_rawID = raw;
		return true;
	}
	// ③ 覆盖：高位 = **运行时编译索引** → 反查拥有者插件，再校验它确实在本文件的主文件表里。
	//
	// 🔑 为什么不能用 `masterPtrs[hi]`（这是 2026-10-07 修掉的 bug）：
	//    `hi` 是**全局编译索引**（作者建插件时的 load order 序号），
	//    而 `masterPtrs[i]` 是**本文件 masters 列表里的第 i 项**（列表已被 xEdit 压缩过）。
	//    对普通 esp 两者碰巧一致（所以一直没暴露），对 ESL 就不一致了：
	//      实测 `Dank_ArmorOverrides.esp` 覆盖 DLCNukaWorld 的记录写成 `06029C10`：
	//        高位 = 0x06 = DLCNukaWorld 的**编译索引**（官方 7 esm 里的第 6 个）✓
	//        而这个 esp 的 masters 只列了 4 个（masterCount=4）⇒ `6 < 4` 为假 ⇒
	//        旧代码落到 ④ “身份判定失败”⇒ **ESL 的覆盖记录全部写不回去** ✗
	//    ⇒ 改成“按运行时编译索引反查 + 必须在本文件 masters 里”双重校验，
	//      宁可漏（拒绝），也不错改。
	{
		std::string_view owner;
		std::uint32_t    focusRaw = 0;
		std::string_view byMaster;
		const bool       focused = FocusOwnerOf(a_file, a_formID, owner, focusRaw, byMaster);
		if (!owner.empty() && IsMasterOf(a_file, owner) && RE::TESForm::GetFormByID(a_formID)) {
			a_plugin = owner;
			a_rawID = raw;
			return true;
		}
		// 反查不中 / 不在本文件 masters 里 → 不写，但把“反查到了但被挡下”也打给关注记录。
		// ⚠️ 复用上面那一次 FocusOwnerOf 的结果（旧版这里又调了一遍：白算一次反查 + 又一次 GetFormByID）。
		if (focused) {
			LOG_ADV_W("focus(unknown-owner): (3) reverse-lookup miss: owner={} (in this file's masters={}); GetFormByID({:#010X})={} -> still rejected",
				owner.empty() ? "?" : owner.data(), IsMasterOf(a_file, owner) ? "yes" : "no",
				a_formID, RE::TESForm::GetFormByID(a_formID) ? "yes" : "no");
		}
	}
	// ④ 其它：不可信 → 一个字都不写（名单是节点 3 的命门）
	LogIdentityReject(a_file, a_formID, hi);
	return false;
}

void Replace::Init()
{
	g_on.store(Config::Get().enable, std::memory_order_relaxed);
	if (Config::Get().enable) {
		// 信息类：用 INFO —— 这是**正常启动**的一行。
		// 原先写成 WARN 是“重写为固定文本”时代的遗留（那确实是破坏性的：
		// 会把命中的类别值统一覆盖成同一个串），那个功能已删。
		LOG_INFO("replace: enabled -- when a whitelisted record is overridden by a plugin outside the whitelist, the override is dropped and the base text is kept");
	}
}

bool Replace::Enabled()
{
	return g_on.load(std::memory_order_relaxed);
}

Replace::Stats Replace::Snapshot()
{
	return g_stats;
}

bool Replace::IsListed(std::string_view a_plugin, std::uint32_t a_rawID)
{
	return Enabled() && Config::ShouldRewrite(a_plugin, a_rawID);
}

namespace
{
	// 「名单内插件自己读进来的文本」缓存：key = 归属插件|rawID|chunk4CC|**序号**
	struct OwnText
	{
		std::string   plugin;
		std::uint32_t rawID{ 0 };
		std::string   text;
	};
	std::unordered_map<std::string, OwnText> g_ownText;
	std::mutex                                   g_ownTextLock;

	// 当前 chunk 的 4CC（记忆的键要用它区分同一记录里的不同字段）
	std::uint32_t ChunkID(const RE::TESFile* a_file)
	{
		return a_file ? static_cast<std::uint32_t>(a_file->currentchunkID.underlying()) : 0u;
	}

	// ── 序号（seq）与「按读取文件分开计数」──────────────────────────────
	//
	// 记忆的键 = `(归属插件, 低位ID, chunk 4CC, **同 chunk 内序号**)`。
	// 序号 = **本次读取**中这个 chunk 4CC 第几次出现（单值字段也可能是多次，见下）。
	//
	// ⚠️ 计数必须**按读取文件分开**（键里带读取文件名）：
	//   基表那次（如 Fallout4.esm 读 R）与覆盖那次（ModX.esp 读 R）要各自从 0 开始，
	//   否则覆盖那次的序号会从 N 接着涨，与基表侧的序号对不上 ⇒ 全部回落失败。
	//   （同一条记录的基表版与覆盖版**不是**相邻读取的，但这种分开计数更稳。）
	//
	// ⚠️⚠️ **不要把 seq 当“数组下标”用**（2026-10-07 踩过，已撤回）：
	//   实测 `FULL` 在一条记录上被引擎重复读取 **17 次**（`区块普查` 数据：FULL 共 75634 次
	//   调用 / 47 种签名）⇒ seq 与“第几个数组元素”没有对应关系。
	//   曾按“seq ≥ 基表条数就不写”加了一道护栏，结果**误伤大量非数组字段**（FULL/ITXT/WNAM…）。
	//   正确认识：基表那次与覆盖那次是**同一个引擎、同一套读取顺序** ⇒ 序号天然一致 ✓，
	//   不需要长度校验。（那条“长度不等就不碰”是 xEdit 离线脚本的招 —— 它看不到引擎顺序，
	//   而我们能。）
	std::string                            g_curSeqKey;
	std::unordered_map<std::uint32_t, int> g_chunkSeq;

	/// 取本次读取的序号；返回 -1 = 超限（视为不可信，让调用方放弃）。
	int NextSeq(const RE::TESFile* a_file, std::uint32_t a_chunkID, std::string_view a_owner, std::uint32_t a_rawID)
	{
		char key[512]{};
		std::snprintf(key, sizeof(key), "%s|%.*s|%08X", a_file ? a_file->filename : "?",
			static_cast<int>(a_owner.size()), a_owner.data(), a_rawID);
		if (g_curSeqKey != key) {
			g_curSeqKey = key;
			g_chunkSeq.clear();
		}
		auto& seq = g_chunkSeq[a_chunkID];
		if (seq >= Chunk::kMaxSeqPerChunk) {
			return -1;
		}
		return seq++;
	}

	std::string OwnKey(std::uint32_t a_chunkID, std::string_view a_plugin, std::uint32_t a_rawID, int a_seq)
	{
		char buf[400]{};
		std::snprintf(buf, sizeof(buf), "%.*s|%08X|%08X|%d", static_cast<int>(a_plugin.size()), a_plugin.data(),
			a_rawID, a_chunkID, a_seq);
		return std::string{ buf };
	}
}

void Replace::RememberOwnText(std::uint32_t a_chunkID, std::string_view a_text, std::string_view a_plugin,
	std::uint32_t a_rawID, const RE::TESFile* a_file)
{
	// ⚠️ **序号必须先取、无条件取** —— 即使文本为空。
	//
	// 2026-10-07 实测踩过的坑（INNR 的 WNAM 大批错位）：
	//   `RememberOwnText`（基表侧）原先先判空 `if (a_text.empty()) return;`，
	//   而 `RememberedOwnText`（覆盖侧）**总是** NextSeq ⇒ 两边的序号不再一一对应。
	//   只要基表某个 WNAM 是空的（实测存在：`dn_CommonGun` 的 ruleset[2] 有 2 个、[4] 有 1 个），
	//   从那个位置起整列**左移一位** ⇒ `Scoped` 拿到「长枪管」、`Short` 拿到「冰晶」。
	//   ⇒ 两边必须“看得到同一条就占同一个号”，哪怕它没内容。
	const auto seq = NextSeq(a_file, a_chunkID, a_plugin, a_rawID);
	if (seq < 0 || a_text.empty()) {
		return;   // 号已经占了；空文本不存储（但序号要保留）
	}
	auto             key = OwnKey(a_chunkID, a_plugin, a_rawID, seq);
	std::scoped_lock l{ g_ownTextLock };
	g_ownText.insert_or_assign(std::move(key),
		OwnText{ std::string{ a_plugin }, a_rawID, std::string{ a_text } });
}

std::string_view Replace::RememberedOwnText(std::uint32_t a_chunkID, std::string_view a_plugin,
	std::uint32_t a_rawID, const RE::TESFile* a_file, int* a_seqOut)
{
	const auto seq = NextSeq(a_file, a_chunkID, a_plugin, a_rawID);
	if (a_seqOut) {
		*a_seqOut = seq;
	}
	if (seq < 0) {
		return {};
	}
	std::scoped_lock l{ g_ownTextLock };
	const auto       it = g_ownText.find(OwnKey(a_chunkID, a_plugin, a_rawID, seq));
	if (it == g_ownText.end()) {
		return {};
	}
	// 地址会跨记录复用 → 归属与低位 ID 都必须对上，否则当没记住（避免张冠李戴 ✗）
	if (it->second.rawID != a_rawID || !Str::IEquals(it->second.plugin, a_plugin)) {
		return {};
	}
	return it->second.text;
}

// 把可能含换行的文本转成**单行**字面量：\r\n → "\\r" / "\\n"，制表符 → "\\t"。
//
// 为什么需要：终端机正文（TERM）动辄几百字符、自带换行，直接送 REX::WARN 会把**一条**审计行
// 撕成几十个屏幕行 —— 实测一轮 255 KB 里只有 358 行是真日志行，其余全是续行，
// 既不好统计也看不清条数（项目纪律：“日志更细”≠“日志更多”）。
[[nodiscard]] std::string OneLine(std::string_view a_text)
{
	std::string out;
	out.reserve(a_text.size() + 8);
	for (const char ch : a_text) {
		switch (ch) {
		case '\r':
			out += "\\r";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			out += ch;
			break;
		}
	}
	return out;
}

// 逐条审计（仅**真改动**时打：a_read != a_kept）。上限由 ini 的 [调试] 审计上限 控制（0 = 关）。
//
// 为什么要这道门槛：大部分“忽略”是**空操作** —— 本地化插件的覆盖记录在盘上只是 4 字节 stringID，
// 引擎解析出来就是中文，和我们写回的 keep 逐字节相同。实测一轮 22312 条里绝大部分如此，
// 全部打出来会把真正的改动（比如 1955 条英文→中文）淹掉。
void Replace::LogAudit(const char* a_leg, const RE::TESFile* a_file, std::string_view a_owner,
	std::uint32_t a_rawID, std::string_view a_read, std::string_view a_kept)
{
	const auto& cfg = Config::Get();

	// 签名与 formID 的**唯一来源** = 正在读的那条记录（见 Replace.h 的同名注释）。
	// 拿不到文件就没什么可报的（调用点都在已确认 a_file 非空的路径上）。
	if (!a_file) {
		return;
	}
	const auto formCode = a_file->currentform.form;
	const auto formID = a_file->currentform.formID;

	// 关注记录：**命中则一路打全**（不受“真改动”门槛、上限、签名白名单限制）——
	// 它的用途就是“不管发生了什么，先把这条记录的每一步打出来”。
	const bool focused = Config::IsFocus(a_owner, a_rawID) || Config::IsFocus(a_file->filename, a_rawID);

	// 真改动：读入的那份与写回的那份不同。
	// ⚠️ 门槛**必须带 `!focused`**：否则「关注记录」在“读入 == 写回”（空操作）时也打不出来，
	//    与 §4.1「有没有改动都打」矛盾。（2026-10 修：原先这行前还有一次无条件的早退，
	//    把 focused 绕过整个吃掉了。）
	if (a_read == a_kept && !focused) {
		return;
	}

	const auto limit = cfg.auditLimit;
	if (limit == 0 && !focused) {
		return;
	}

	char sig[5]{ static_cast<char>(formCode), static_cast<char>(formCode >> 8),
		static_cast<char>(formCode >> 16), static_cast<char>(formCode >> 24), 0 };

	// 签名白名单（[调试] 审计签名）：空 = 全打；非空则只打列到的（不分大小写）。
	if (!cfg.auditSignatures.empty() && !focused) {
		std::string here{ sig };
		for (auto& c : here) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		bool listed = false;
		for (const auto& want : cfg.auditSignatures) {
			if (want == "*" || want == here) {
				listed = true;
				break;
			}
		}
		if (!listed) {
			return;
		}
	}

	static std::mutex  lock;
	static std::size_t count = 0;

	// 关注记录不占额度（它本来就是不受限制的）
	if (!focused) {
		std::scoped_lock l{ lock };
		if (count >= limit) {
			return;
		}
		++count;
	}
	REX::INFO("write[{}]{}: read={} {} {:#010X} offset={:#X} text='{}'  ->  owner={} {} {:#08X} base='{}'  ->  wrote '{}'",
		a_leg, focused ? " focus" : "", a_file->filename, sig, formID,
		static_cast<std::uint64_t>(a_file->fileoffset), OneLine(a_read),
		a_owner, sig, static_cast<std::uint64_t>(a_rawID), OneLine(a_kept), OneLine(a_kept));
}

bool Replace::HandleFullNameField(RE::TESFullName& a_field, const RE::TESFile* a_file, std::uint32_t a_formCode,
	std::string_view a_plugin, std::uint32_t a_rawID, std::string_view a_previous, std::string_view a_loaded,
	bool a_ownerRead, bool a_readerListed)
{
	// 关注记录：先把“到了哪一步”打出来（尤其后面被名单/归属挡下的情况）。
	const bool focused = Config::IsFocus(a_plugin, a_rawID) || Config::IsFocus(a_file ? a_file->filename : "", a_rawID);
	if (focused) {
		LOG_ADV("focus(FULL): reader={} owner={}{} record={:#010X} raw={:#08X} listed={} reader-listed={} read-in='{}'",
			a_file ? a_file->filename : "?",
			a_plugin.empty() ? "?" : a_plugin.data(), a_ownerRead ? " (is itself)" : " (override)",
			a_file ? a_file->currentform.formID : 0, static_cast<std::uint64_t>(a_rawID),
			IsListed(a_plugin, a_rawID), a_readerListed, OneLine(a_loaded));
	}

	// ── yaml 译文（`[汉化]`）——**优先级高于基底**（用户 2026-10-08 裁定）
	//
	// ⚠️ 必须放在名单判断**之前**：名单里是官方 master，而 yaml 管的是 mod 自己的记录
	//    （以及 mod **覆盖官方**的记录）—— 它们根本不在名单里，若按名单先筛就会被拦得一条不剩。
	// ⚠️ 不再限定 `a_ownerRead`（2026-10-09 改）：
	//    -- `a_plugin` 是**归属插件名**（可能是自己，也可能是被覆盖的官方 master）；
	//       两种情况 Translation 的键都能对上（覆盖条目的键 = master 名，靠 master 反查得到）✓
	//    -- `!ownerRead` 且命中译文 ⇒ 先写 yaml 译文（yaml > 基底 > 引擎原值）
	//    -- 未命中 ⇒ 照旧往下走「保基底」逻辑（一个字不少）
	// ⚠️ 必须在 `RememberOwnText` **之前**返回：写了译文就不该再把英文记进“基底备料”。
	if (Translation::Ready() && !a_plugin.empty()) {
		const auto dest = Translation::LookupChunk(a_file, a_plugin, a_rawID, Chunk::kFull, a_loaded);
		if (!dest.empty()) {
			AssignText(a_field.fullName, dest);
			++g_stats.fieldTranslated;
			LogAudit("Translation", a_file, a_plugin, a_rawID, a_loaded, dest);
			return true;
		}
	}

	if (!IsListed(a_plugin, a_rawID) && !a_readerListed) {
		if (focused) {
			LOG_ADV("focus(FULL): ^ blocked (neither owner nor reader is in the whitelist) -> nothing is written");
		}
		return false;   // 归属和读取者都不在名单：走引擎原逻辑
	}
	if (a_readerListed) {
		// 读取者（正在读它的那个插件）自己在名单里 → **采用它读进来的文本**，什么都不写。
		// 同时记住，供之后被“名单外覆盖”时回落。
		RememberOwnText(ChunkID(a_file), a_loaded, a_plugin, a_rawID, a_file);
		return true;
	}
	if (a_ownerRead) {
		RememberOwnText(ChunkID(a_file), a_loaded, a_plugin, a_rawID, a_file);   // 记住基表那份
	}
	if (!a_ownerRead) {
		int        seq = -1;
		const auto remembered = RememberedOwnText(ChunkID(a_file), a_plugin, a_rawID, a_file, &seq);
		// 忽略文本传入：写回"基表那份"（读前非空就用读前，否则用记住的 —— 压缩记录读前往往是空的）
		const auto keep = !a_previous.empty() ? a_previous : remembered;
		// ⚠️ 这里曾有“数组对齐护栏”（seq ≥ 基表条数就不写）—— 已撤回，因为它误伤大量非数组字段：
		//   实测 `FULL` 在一条记录上被重复读取 17 次（见 NextSeq 上的注释），
		//   seq 不是“数组下标”。基表那次与覆盖那次是同一引擎、同一顺序 ⇒ 序号天然一致 ✓。
		if (!keep.empty()) {
			AssignText(a_field.fullName, keep);
			++g_stats.fieldIgnored;
			LogAudit("FULL", a_file, a_plugin, a_rawID, a_loaded, keep);
		}
	}
	return true;
}

bool Replace::HandleLocalizedField(RE::BGSLocalizedString& a_field, const RE::TESFile* a_file, std::uint32_t a_formCode,
	std::string_view a_plugin, std::uint32_t a_rawID, std::string_view a_previous, std::string_view a_loaded,
	bool a_ownerRead, bool a_readerListed)
{
	// 关注记录：先把“到了哪一步”打出来（尤其后面被名单/归属挡下的情况）。
	const bool focused = Config::IsFocus(a_plugin, a_rawID) || Config::IsFocus(a_file ? a_file->filename : "", a_rawID);
	if (focused) {
		LOG_ADV("focus(String): reader={} owner={}{} record={:#010X} raw={:#08X} listed={} reader-listed={} read-in='{}'",
			a_file ? a_file->filename : "?",
			a_plugin.empty() ? "?" : a_plugin.data(), a_ownerRead ? " (is itself)" : " (override)",
			a_file ? a_file->currentform.formID : 0, static_cast<std::uint64_t>(a_rawID),
			IsListed(a_plugin, a_rawID), a_readerListed, OneLine(a_loaded));
	}

	// ── yaml 译文（`[汉化]`）——优先级高于基底（用户 2026-10-08 裁定）
	//
	// ⚠️ 必须放在名单判断**之前**：名单里装的是官方 master，而 yaml 管的是 mod 自己的记录
	//    （以及 mod **覆盖官方**的记录）—— 它们根本不在名单里，若按名单先筛就会被拦得一条不剩。
	// ⚠️ 不再限定 `a_ownerRead`（2026-10-09 改，理由同 FullName 腿）：
	//    `!ownerRead` 且命中译文 ⇒ 先写 yaml 译文（yaml > 基底 > 引擎原值）。
	// ⚠️ 必须在 `RememberOwnText` **之前**返回：写了译文就不该再把英文记进“基底备料”。
	if (Translation::Ready() && !a_plugin.empty()) {
		const auto dest = Translation::LookupChunk(a_file, a_plugin, a_rawID, ChunkID(a_file), a_loaded);
		if (!dest.empty()) {
			AssignText(a_field, dest);
			++g_stats.fieldTranslated;
			LogAudit("Translation", a_file, a_plugin, a_rawID, a_loaded, dest);
			return true;
		}
	}

	if (!IsListed(a_plugin, a_rawID) && !a_readerListed) {
		if (focused) {
			LOG_ADV("focus(String): ^ blocked (neither owner nor reader is in the whitelist) -> nothing is written");
		}
		return false;
	}
	if (a_readerListed) {
		RememberOwnText(ChunkID(a_file), a_loaded, a_plugin, a_rawID, a_file);   // 读取者在名单 → 采用它的文本
		return true;
	}
	if (a_ownerRead) {
		RememberOwnText(ChunkID(a_file), a_loaded, a_plugin, a_rawID, a_file);
	}
	if (!a_ownerRead) {
		// ⚠️ **`*` 不写**（2026-10-09 修，实测踩过 INNR 拼装名）。
		//
		// `*` 是 **INNR 的兜底占位符**（Ruleset 里最后一项：所有关键词都匹配不上时用它，
		// 输出记录的基础名）。它不是文本，但也**不是我们能定位的“第几项”**：
		// 引擎会**分多遍**读同一条记录（第 1 遍完整 0..N，之后还会只读需要的某一项），
		// 而 `RememberedOwnText` 靠“读取序号”对齐基表那份 ⇒ 第二遍的 seq 从 0 重来
		// ⇒ 会取到**基表第 0 项**写进去 ✗
		// 实测：INNR `dn_CommonArmor` 的 `*` 被写成第 1 项「支撑」(Bolstering)
		// ⇒ 护甲拼装名变成「超轻 聚合物 支撑」而不是「…战斗护甲」✗
		//
		// ⇒ 占位符一律不碰（不引入错译，最多是一处不汉化）。
		//    ⚠️ **限 INNR 记录**（`a_formCode`）—— 不排除别的记录类型里存在“有价值的 `*`”
		//       （真出现了也不至于误伤）。
		//    （注意：不是所有 INNR 的兜底项都是 `*`（`dn_PowerArmor` 的是“躯干”、
		//      `dn_CommonGun` 的 `*` 在第 4 组内），但 `*` 作为 INNR 的词根值几乎必然是占位符。）
		if (a_formCode == Chunk::kInnr && a_loaded == "*") {
			return true;
		}
		int        seq = -1;
		const auto remembered = RememberedOwnText(ChunkID(a_file), a_plugin, a_rawID, a_file, &seq);
		std::string keep{ !a_previous.empty() ? a_previous : remembered };
		// ⚠️ 这里曾有“数组对齐护栏”—— 已撤回（同 FullName 腿），它误伤非数组字段。
		// ⚠️ 2026-10-07 实测：这里同步扫插件文件会让**加载界面卡死** ✗（315 MB 的 esm、
		//    逐表头读取上百万次）→ 暂时关闭，等改成 mmap/分块扫描再开（见 kEnableBaseText）。
		// ⚠️ 2026-10-07 又一次实测停用（两个硬伤，别再直接打开）：
		//   ① 缓存粒度是 (插件,记录ID,chunk) ⇒ **每条失败记录都重扫一遍基表 esm（315 MB）** ✗
		//      → 上千条记录 ⇒ 加载界面卡死 ✗
		//   ② 在"覆盖记录读取"的时机解析基表的 DL 字符串 ID，拿回来的是引擎的
		//      **'LOOKUP FAILED!'** 占位 ✗ —— 这个解析依赖"正在读那个文件"的上下文，
		//      只有基表文件自己加载时才能成功 ✗
		//   正确做法（下次）：在**基表文件自己的读取阶段**就把它的 INFO→文本 收进一张表
		//      （一次遍历 ✓，不按记录重扫 ✗），覆盖记录再来查表 ✓。
		constexpr bool kEnableBaseText = true;   // 一次遍历建索引 + 查松散字符串表（约 1~2 秒一次 ✓）
		if (kEnableBaseText && keep.empty() && a_formCode == Chunk::kInfo) {
			// 引擎对有些记录**只解析胜出的覆盖**，基表那份从未进内存 →
			//   我们自己：一次遍历基表插件建索引 → 取这条记录该 chunk 的 4 字节（字符串表 id）
			//   → 查 <游戏>\Data\Strings\<基名>_<语言>.ILSTRINGS（游戏本来就在用，内容即简体中文 ✓）
			//   → 内联文本则直接用 ✓
			keep = std::string{ BaseText::Get(a_plugin, a_rawID, ChunkID(a_file), "INFO") };
			if (!keep.empty()) {
				++g_stats.fieldFromBase;   // 统计：靠"自取基表文本"救回来的条数
			}
		}
		// 字幕详情（曾有个 ``INFO 忽略: …`` 逐条行，2026-10-07 已删）：
		//   它当初是为排查“约 1600 条字幕”加的（看 `读前='' 记住='' 读入='English' → 写回'中文'`），
		//   那个问题已解决，而它实测一轮 6799 行且**包含了大量“没写回”的条目**，
		//   体积与信息比都远不如 LogAudit（后者只报真改动，且有上限/签名可选）。
		//   要看“哪些 INFO 被放过了”就临时把这段加回来（keep 为空时也打）。
		if (!keep.empty()) {
			AssignText(a_field, keep);
			++g_stats.fieldIgnored;
			LogAudit("String", a_file, a_plugin, a_rawID, a_loaded, keep);
		}
	}
	return true;
}
