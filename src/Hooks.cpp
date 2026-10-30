#include "PCH.h"

#include "Hooks.h"

#include "BaseText.h"
#include "Config.h"
#include "Replace.h"
#include "Chunks.h"
#include "StrUtil.h"
#include "Translation.h"

#include <shared_mutex>

#include <MinHook.h>

// MinHook.h 会拉进 Windows.h：它把 ERROR / MAX_PATH 定义成宏，
// 而 CommonLib 的 REX::ERROR 与 RE::TESFile 里的 REX::W32::MAX_PATH 都要用原名字。
// CommonLib 的头此时已经全部解析完（PCH 在前），这里撤掉宏没有副作用。
#undef ERROR
#undef MAX_PATH

// 共 **4 个钩子**，每个都是「引擎读这段文本 / 界面来取这段文本」的**当时**改 ——
// 没有任何“启动时先扫一遍建字典、之后查表”的动作（那套本项目**没有**）。
// 没有 vtable / Scaleform / UI 钩子。
//
// 三条**读盘期腿**（都在 F4SEPluginLoad 装；判归属 → 名单内则忽略覆盖、写回基底文本）：
//   TESFullName::LoadFullNameChunk（2193215） —— 引擎读「逐字段 FULL」时
//   BGSLocalizedStringDL::Load（2194235）   —— 引擎读任一本地化字符串字段时（本地化 + 内联两分支）
//   BGSLocalizedStringDL::Load2（2194240）  —— 同上，INFO:NAM1（字幕正文）走这条
//
// 一条**显示层腿**（DESC 专用；**唯一**的显示层例外，用户 2026-10-08 已裁定）：
//   TESDescription::GetDescription（2193019） —— **界面来取这条描述**时换出参（BSString&）
//     ⚠️ 它不在 F4SEPluginLoad 装：a_form 形参实测恒为 nullptr，必须先靠全表单扫出的
//        「组件地址 → formID」反查表才能知道“这是哪条记录” ⇒ 等 kGameDataReady
//        （见 InstallDescriptionHook 与 docs/DESC-描述文本.md）。
//     它不是 UI/布局钩子：改的是「这条记录自己的描述文本」，只是引擎延迟解析。
//
// 另外两处“扫一遍”都**不是字典**，且都不产出文本：
//   buildDescriptionMap()      —— 组件地址 → formID（身份反查，kGameDataReady 一次）
//   BaseText::EnsureIndex()    —— 插件文件 → chunk 原始字节索引（**懒加载**，按需扫那一个插件）
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	using LoadFullNameChunk_t = void (*)(RE::TESFullName*, RE::TESFile*);
	using LoadLocString_t     = void (*)(RE::BGSLocalizedString*, RE::TESFile*);
	// 显示层：void TESDescription::GetDescription(BSString& a_out, const TESForm* a_form)
	// 签名来自 CommonLibF4 `RE/T/TESDescription.h`（非反汇编猜测）。

	using GetDescription_t = void (*)(RE::TESDescription*, RE::BSString&, const RE::TESForm*);

	LoadFullNameChunk_t g_chunkOriginal{ nullptr };
	LoadLocString_t     g_loadLocStringOriginal{ nullptr };
	LoadLocString_t     g_loadLocString2Original{ nullptr };
	GetDescription_t    g_getDescriptionOriginal{ nullptr };

	Hooks::Status g_status[4];

	// ── “TESDescription* → formID” 映射 ─────────────────────────────
	//
	// 为什么需要：`GetDescription` 的 `a_form` 参数实测 **100% 为 nullptr**（522/522），
	// 拿不到 formID ⇒ 无法判归属 ⇒ 没法决定要不要改。
	//
	// 为什么不能用指针算术反推：`TESDescription` 在 form 内的**偏移随类而变**
	//   （BGSPerk 0x30 / BGSMod 0x30 / TESObjectWEAP **0x170**）⇒ 只能建表。
	//   但 `TESForm::As<TESDescription>()` 是 **编译期 static_cast**（CommonLibF4 的
	//   TESFormUtil.h 里一个 switch + `if constexpr`），运行时偏移由编译器决定 ⇒ 可用。
	// ⚠️ 建表后只读 ⇒ 用 shared_mutex（唯一写=buildDescriptionMap 一次；读=每次 GetDescription）。
	//    单线程下其实无竞争，但这是更精确的表达，且不排除将来并发。
	std::shared_mutex                                                              g_mapLock;
	std::unordered_map<std::uint64_t, std::uint32_t>                               g_descToFormID;
	std::atomic<std::uint32_t>                                                     g_mapScanned{ 0 };
	std::atomic<std::uint32_t>                                                     g_mapMatched{ 0 };
	std::atomic<bool>                                                              g_mapBuilt{ false };

static bool buildDescriptionMap()
{
	const auto* handler = RE::TESDataHandler::GetSingleton();
	if (!handler) {
		return false;
	}
	std::unordered_map<std::uint64_t, std::uint32_t> map;
	std::uint32_t                                  scanned = 0;
	for (const auto& bucket : handler->formArrays) {
		for (auto* form : bucket) {
			if (!form) {
				continue;
			}
			++scanned;
			if (auto* desc = form->As<RE::TESDescription>()) {
				map.emplace(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(desc)),
					static_cast<std::uint32_t>(form->formID));
			}
		}
	}
	{
		std::unique_lock l{ g_mapLock };
		g_descToFormID.swap(map);
	}
	g_mapScanned.store(scanned, std::memory_order_relaxed);
	g_mapMatched.store(static_cast<std::uint32_t>(g_descToFormID.size()), std::memory_order_relaxed);
	g_mapBuilt.store(true, std::memory_order_relaxed);
	LOG_INFO("DESC map: scanned-forms={}  with-TESDescription={}",
		g_mapScanned.load(std::memory_order_relaxed), g_mapMatched.load(std::memory_order_relaxed));
	return true;
}

// 查映射的内部实现。
static bool formIDOfDescription(const void* a_description, std::uint32_t& a_formID)
{
	if (!a_description || !g_mapBuilt.load(std::memory_order_relaxed)) {
		return false;
	}
	std::shared_lock l{ g_mapLock };
	const auto        it = g_descToFormID.find(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_description)));
	if (it == g_descToFormID.end()) {
		return false;
	}
	a_formID = it->second;
	return true;
}

	// ── DESC 探针（只读）────────────────────────────────────────────
	//
	// 背景：`DESC` 在内存里是 `BGSLocalizedStringDL`（只有一个 u32 id，延迟解析），
	// 所以**读盘期写不进去**（字段里没有文本）。唯一可写的地方是
	// `TESDescription::GetDescription` 的输出参数。
	//
	// 本次只**观察**，不改任何东西（项目硬规则：先确认再动手）。
	//
	// ⚠️ 去重 + 计数：界面会反复调（本项目实测逐条日志能把磁盘写爆）。
	std::mutex                                                                     g_descLock;
	// 不同 this 指针 → 见过几次（只为去重，不打印）
	std::unordered_map<std::uint64_t, std::uint64_t>                               g_descSeen;
	std::atomic_uint64_t                                                           g_descCalls{ 0 };
	// 实际改写 a_out 的次数（小步验证的关键指标：确认“真能改”而不只是“记了日志”）
	std::atomic_uint64_t                                                           g_descFixed{ 0 };
	// 探针已打的行数（去重之后）。实测一轮 509 条会把日志撑到 200 KB 且没有额外信息。
	std::size_t                                                                    g_descProbeLines{ 0 };

	constexpr std::size_t   kDescProbeLimit{ 200 };

	// 一条记录的「基表插件 + 插件内原始 ID」。
	//   * 基表 = `sourceFiles.front()` —— **不是** back()：back 只是“最后写入者”。
	//     原版记录被 CFM 覆盖后 formID 高位仍属 Fallout4.esm，而 back() 是 CFM（实测）。
	//   * rawID 位宽按**归属文件种类**定：普通 esp 24 位 / ESL 12 位
	//     （误用 & 0xFFF 会把 00148334 算成 000334 —— 交接文档记的那 159 条误判）。
	struct BaseRef
	{
		std::string_view name{};     // 基表插件（= sourceFiles.front()，DESC 回写用）
		std::string_view winner{};   // 最后写入者（= sourceFiles.back()，覆盖记录选译文用）
		std::uint32_t    raw{ 0 };
		bool             ok{ false };
	};

	BaseRef BaseRefOf(const RE::TESForm* a_form)
	{
		BaseRef r;
		if (!a_form || !a_form->sourceFiles.array) {
			return r;
		}
		const auto& files = *a_form->sourceFiles.array;
		if (files.empty()) {
			return r;
		}
		const auto* f = files.front();
		if (!f) {
			return r;
		}
		r.name = f->GetFilename();
		// 最后写入者 = 胜出的覆盖者（可能 == name）。DESC 是显示层，拿不到“正在读的那个
		// TESFile”，只能靠它选译文：同一条官方记录被多个 mod 覆盖时，只有胜出那份的译文可用。
		if (const auto* back = files.back()) {
			r.winner = back->GetFilename();
		}
		const auto id = static_cast<std::uint32_t>(a_form->formID);
		if (f->IsLight()) {
			r.raw = id & 0xFFFu;
			r.ok  = ((id >> 12) & 0xFFFu) == f->smallFileCompileIndex && (id >> 24) == 0xFEu;
		} else {
			r.raw = id & 0xFFFFFFu;
			r.ok  = (id >> 24) == f->compileIndex;
		}
		return r;
	}

	// 审计签名（记录 4CC）白名单：空 = 全部，`*` 同义。
	//
	// ⚠️ 它筛的是**记录签名**（OMOD / PERK / ARMO…），不是字段 4CC。
	//    所以**写不出** `DESC`（DESC 是 chunk，不是记录签名）—— 想“只看 DESC”的组合是：
	//      审计上限 = 0（关掉 `回写[…]` 那些行） + 审计签名 = OMOD（DESC 探针只打 OMOD）
	//    口径与 `Replace::LogAudit` 完全一致（都转小写、`*` 通配）。
	bool SignatureWanted(std::uint32_t a_formCode)
	{
		const auto& want = Config::Get().auditSignatures;
		if (want.empty()) {
			return true;   // 空 = 全部（与 LogAudit 同义）
		}
		char sig[5]{ static_cast<char>(a_formCode), static_cast<char>(a_formCode >> 8),
			static_cast<char>(a_formCode >> 16), static_cast<char>(a_formCode >> 24), 0 };
		std::string here{ sig };
		for (auto& c : here) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		for (const auto& w : want) {
			if (w == "*" || w == here) {
				return true;
			}
		}
		return false;
	}

	void getDescriptionThunk(RE::TESDescription* a_description, RE::BSString& a_out, const RE::TESForm* a_form)
	{
		if (g_getDescriptionOriginal) {
			g_getDescriptionOriginal(a_description, a_out, a_form);   // 先调原函数（约定 1）
		}

		g_descCalls.fetch_add(1, std::memory_order_relaxed);

		// ① 先用形参 a_form（非空时直接用，它就是那条记录）
		// ② a_form 实测 100% 为 nullptr ⇒ 靠**映射表**反查（唯一出路）
		//    （F4TCHook 的 DescriptionHook 撞的是同一个坑：`form=00000000 type=-1`）
		const RE::TESForm* form = a_form;
		if (!form) {
			std::uint32_t mappedID = 0;
			if (formIDOfDescription(a_description, mappedID)) {
				form = RE::TESForm::GetFormByID(mappedID);
			}
		}

		const auto        base       = BaseRefOf(form);
		const bool        listed     = form && base.ok && Replace::IsListed(base.name, base.raw);
		// ⚠️ 必须**拷贝**：引擎原文的指针在 `a_out.Set(...)` 后就可能指到被释放/另一个缓冲。
		//    （探针与比较都从这份拷贝读。）
		const std::string original{ a_out.c_str() ? a_out.c_str() : "" };

		// ── yaml 译文（`[汉化]`）—— 优先级高于基底（与读盘期两条腿同一规则）
		//
		// DESC 的译文就住在这里：`descriptionText` 是延迟解析的 4 字节 id，
		// 读盘期钩子拿不到文本 ⇒ 只能等界面来取的那一刻换掉出参。
		// yaml 里的 MESG/COBJ/OMOD… 的 `DESC` 条目就是给这一处用的。
		// ⚠️ 不限 `名单`：yaml 是“插件自己的记录”那批（含未列入名单的 mod），
		//    所以这里**不能用 listed 做前提**。
		bool byYaml = false;
		if (form && base.ok && base.raw != 0 && Translation::Ready()) {
			// ⚠️ 覆盖记录（胜出的写入者 != 基表）必须带**来源 esp** 查表：
			//    同一条官方记录可能被多个 mod 覆盖，只有胜出那份的译文才与引擎一致。
			//    显示层拿不到“正在读的 TESFile”，所以用 `sourceFiles.back()` = 最后写入者。
			const auto dest = (!base.winner.empty() && base.winner != base.name)
							  ? Translation::LookupFrom(base.name, base.winner, base.raw, Chunk::kDesc, 0)
							  : Translation::Lookup(base.name, base.raw, Chunk::kDesc, 0);
			if (!dest.empty() && original != dest) {
				a_out.Set(dest.data(), static_cast<std::uint32_t>(dest.size() + 1));
				g_descFixed.fetch_add(1, std::memory_order_relaxed);
				byYaml = true;   // 后面不再做“保基底”（但探针照打）
			}
		}

		// 记录签名（探针用）
		const auto sig = form ? static_cast<std::uint32_t>(form->GetFormCode()) : 0u;
		char       text[5]{ static_cast<char>(sig), static_cast<char>(sig >> 8), static_cast<char>(sig >> 16),
			static_cast<char>(sig >> 24), 0 };
		const bool decent = std::all_of(text, text + 4, [](char c) {
			const auto u = static_cast<unsigned char>(c);
			return u >= 0x0A && u <= 0x7E;
		});

		// ⭐ 回写：**名单内记录**（基表那份）的 DESC 换成基表中文。
		//
		// 不依赖 `高级` 开关 —— 这是功能本身，不是探针（照 F4TCHook：它的
		// DescriptionHook 也是无条件装、无条件写）。
		//
		// 为什么必须自己取文本：`descriptionText` 是 `BGSLocalizedStringDL`（只有一个 u32 id，
		// 延迟解析），读盘期根本不解析基表那份，所以“记住基表文本”无从谈起。
		// 取法 = 本项目已有的 `BaseText`：一次遍历基表插件建索引 + 查松散 `.ILSTRINGS`
		//（INFO 字幕就是靠它打通的 ✓）。
		//
		// ⚠️ 首次调用会扫一遍基表插件（约 1~2 秒）。本机实际不会卡在界面上：
		//    读盘期的 INFO 字幕走的就是同一个 `BaseText`，索引（按插件缓存）早就建好了 ✓。
		std::string kept{};
		bool        wrote = false;
		if (listed && !byYaml) {
			// ⚠️ 拷成 std::string：`BaseText::Get` 返回的是它内部 static map 里元素的 view，
			//    另一线程再 emplace 一次就会 rehash ⇒ 那个 view 会悬空 ✗。
			kept = std::string{ BaseText::Get(base.name, base.raw, Chunk::kDesc, "DESC", /*a_displayLayer=*/true) };
			if (!kept.empty()) {
				// 只在**真的不同**时写（本地化插件的覆盖读出来本来就可能已是中文 ⇒ 空操作）。
				if (original != kept) {
					a_out.Set(kept.c_str(), static_cast<std::uint32_t>(kept.size() + 1));
					g_descFixed.fetch_add(1, std::memory_order_relaxed);
					wrote = true;
				}
			}
		}

		// ── 探针（只读）────────────────────────────────────────
		//
		// 照 F4TCHook 的教训：探针**不能被闸门吞掉**（它曾因日志等级误判出“零调用”）。
		//   ① **名单内且带 DESC 的记录无条件打**（这是我们真正关心的那一小撮）；
		//   ② 其余（名单外 / 归属不明）才需要 `高级`。
		// 另外三道闸：
		//   * **审计签名**：只打指定的记录签名（空 = 全部）。例 `审计签名 = OMOD`
		//     ⇒ 哔哔小子特长表那种刷屏（PERK 193 条）就没机会吃光额度 ✓
		//   * **去重 + 上限**：同一组件只在第一次打（实测一轮 509 条会把日志撑到 200 KB，
		//     且第 2..N 条没有新信息）。上限是**本探针自己的**，与 `审计上限` 无关 ——
		//     这样才能“审计上限=0（关掉回写行）+ DESC 照打”地组合 ✓
		//
		// ⚠️ 本行是 `LOG_INFO` ⇒ **归 ①信息**（`信息` 键开/关），不是 ②审计。
		//    但**筛选**用 ②的 `审计签名`（SignatureWanted）—— 唯一一处跨类设计，ini 里有说明。
		{
			const bool want = wrote || byYaml || listed || (Config::Get().advanced && Replace::Enabled());
			if (want && SignatureWanted(sig)) {
				std::scoped_lock l{ g_descLock };
				if (g_descSeen.emplace(reinterpret_cast<std::uint64_t>(a_description), 1).second &&
					g_descProbeLines < kDescProbeLimit) {
					++g_descProbeLines;
					// 回写结果**并进这一行**（不再单独用 `高级` 门控）—— 日常配置下也能验证是否生效。
					LOG_INFO("DESC probe: type={:<4} form={:08X} editor=\"{}\" base={} raw={:06X} ok={} listed={} mapped={} wrote={} engine-text=\"{}\" base-text=\"{}\"",
						decent ? text : "?",
						form ? form->formID : 0u,
						form ? (form->GetFormEditorID() ? form->GetFormEditorID() : "") : "",
						base.name.empty() ? "-" : base.name.data(), base.raw, base.ok ? "yes" : "no",
						listed ? "yes" : "no", form ? "yes" : "no",
						wrote ? "✓" : (kept.empty() ? "no base text" : "unchanged"),
						original.c_str(), kept.c_str());
				}
			}
		}
	}

	// 2194235 与 2194240 共用（两者同构：arg1 = 目标字段，arg2 = TESFile*）
	void HandleLoadedLocString(RE::BGSLocalizedString* a_dst, RE::TESFile* a_file, std::string_view a_previous)
	{
		if (!a_dst || !a_file || !Replace::Enabled()) {
			return;
		}
		// 身份：正在读的那条记录（file->currentform）→ 拥有者插件 + 插件内原始 ID
		// ⚠️ 必须按「本文件自己的主文件表」判（见 Replace::PluginAndRawIDOf 的注释）
		const auto&      form = a_file->currentform;
		std::string_view plugin;
		std::uint32_t    rawID = 0;
		if (!Replace::PluginAndRawIDOf(a_file, form.formID, plugin, rawID)) {
			return;   // 归属不可信 → 一个字都不写
		}
		const bool ownerRead = Str::IEquals(plugin, a_file->filename);
		// ⚠️ `a_dst->c_str()` 是**字符串池里的活指针**，不是拷贝：下游的 AssignText
		//    会把它重新赋值 ⇒ 旧 pool 条目被释放 ⇒ 再读这个指针就是**被复用的内存** ✗。
		//    实测症状（2026-10-08）：日志里文本末尾少一个字节（“冷冻舱”的 舱 = E8 88 B1 只写了 E8），
		//    产生非法 UTF-8 —— **看着像数据写错了，其实只有日志被污染**（写进字段的值是完整的）。
		//    就在这个边界拷一次；下游所有写入点都不用再管。
		const std::string loaded{ a_dst->c_str() ? a_dst->c_str() : "" };
		(void)Replace::HandleLocalizedField(*a_dst, a_file, form.form, plugin, rawID, a_previous,
			loaded, ownerRead, Replace::IsListed(a_file->filename, rawID));
	}

	void LoadLocString_Detour(RE::BGSLocalizedString* a_dst, RE::TESFile* a_file)
	{
		const std::string previous{ a_dst && a_dst->c_str() ? a_dst->c_str() : "" };
		if (g_loadLocStringOriginal) {
			g_loadLocStringOriginal(a_dst, a_file);   // 约定 1：先调原函数
		}
		HandleLoadedLocString(a_dst, a_file, previous);
	}

	// ID 2194240：INFO 的 NAM1（字幕正文）等字段的读取器（反汇编确认同构于 2194235）
	void LoadLocString2_Detour(RE::BGSLocalizedString* a_dst, RE::TESFile* a_file)
	{
		const std::string previous{ a_dst && a_dst->c_str() ? a_dst->c_str() : "" };
		if (g_loadLocString2Original) {
			g_loadLocString2Original(a_dst, a_file);
		}
		HandleLoadedLocString(a_dst, a_file, previous);
	}

	void LoadFullNameChunk_Detour(RE::TESFullName* a_fullName, RE::TESFile* a_file)
	{
		if (!g_chunkOriginal) {
			return;
		}
		// 快照：调原函数**之前**的值（"忽略文本传入"要写回它）
		const std::string previous{ a_fullName ? a_fullName->fullName.c_str() : "" };

		g_chunkOriginal(a_fullName, a_file);   // 约定 1：先调原函数，改的是它读完后的值

		if (!a_fullName || !a_file) {
			return;
		}
		const auto&      form = a_file->currentform;   // 正在读的这条记录（签名 + 记录 ID 都在这）
		std::string_view pluginName;
		std::uint32_t    rawID = 0;
		if (!Replace::PluginAndRawIDOf(a_file, form.formID, pluginName, rawID)) {
			pluginName = {};   // 归属不可信：后面的名单判定必然拒绝
		}
		(void)Replace::HandleFullNameField(*a_fullName, a_file, form.form, pluginName, rawID, previous,
			// ⚠️ 必须拷：这是**池里的活指针**，下游 AssignText 会把它重新赋值 ⇒ 悬空
			//    （同 HandleLoadedLocString 处的注释，实测症状 = 日志里少一个字节）。
			std::string{ a_fullName->fullName.c_str() ? a_fullName->fullName.c_str() : "" },
			Str::IEquals(pluginName, a_file->filename),
			Replace::IsListed(a_file->filename, rawID));
	}

	void Try(Hooks::Status& a_status)
	{
		if (a_status.disabled || !a_status.target || !a_status.detour || !a_status.original) {
			return;   // 空槽位：target/detour/original 是 nullptr，装了也是 MH_ERROR_NOT_EXECUTABLE
		}
		a_status.code = static_cast<int>(::MH_CreateHook(a_status.target, a_status.detour, a_status.original));
		a_status.installed = (a_status.code == MH_OK);
		if (!a_status.installed) {
			return;
		}
		a_status.code = static_cast<int>(::MH_EnableHook(a_status.target));
		a_status.enabled = (a_status.code == MH_OK);
	}
}

void Hooks::Install()
{
	const auto init = ::MH_Initialize();
	if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
		REX::ERROR("hooks: MH_Initialize failed ({})", static_cast<int>(init));
		return;
	}

	g_status[0] = Status{
		.name = "TESFullName::LoadFullNameChunk",
		.id = 2193215,
		.target = reinterpret_cast<void*>(REL::ID{ 2193215 }.address()),
		.detour = reinterpret_cast<void*>(&LoadFullNameChunk_Detour),
		.original = reinterpret_cast<void**>(&g_chunkOriginal),
	};
	g_status[1] = Status{
		.name = "BGSLocalizedStringDL::Load(2194235)",
		.id = 2194235,
		.target = reinterpret_cast<void*>(REL::ID{ 2194235 }.address()),
		.detour = reinterpret_cast<void*>(&LoadLocString_Detour),
		.original = reinterpret_cast<void**>(&g_loadLocStringOriginal),
	};
	g_status[2] = Status{
		.name = "BGSLocalizedStringDL::Load2(2194240) subtitle NAM1",
		.id = 2194240,
		.target = reinterpret_cast<void*>(REL::ID{ 2194240 }.address()),
		.detour = reinterpret_cast<void*>(&LoadLocString2_Detour),
		.original = reinterpret_cast<void**>(&g_loadLocString2Original),
	};
	// 第 4 条腿（显示层）：**不在这里装** —— 它依赖全表单映射（见 InstallDescriptionHook）。
	//   `Try()` 在名/目标为空时会跳过，所以先把描述符填好、等 kGameDataReady 再装。
	g_status[3] = Status{
		.name = "TESDescription::GetDescription(2193019) [display layer · DESC writeback]",
		.id = 2193019,
		.target = reinterpret_cast<void*>(REL::ID{ 2193019 }.address()),
		.detour = reinterpret_cast<void*>(&getDescriptionThunk),
		.original = reinterpret_cast<void**>(&g_getDescriptionOriginal),
	};

	for (auto& s : g_status) {
		if (!s.name || s.id == 2193019) {
			continue;   // 空位 / 留给 kGameDataReady 的显示层腿
		}
		Try(s);
	}
}

void Hooks::Report()
{
	for (const auto& s : g_status) {
		if (!s.name || !s.target) {
			continue;   // 空槽位
		}
		if (!s.installed && s.id == 2193019) {
			// 第 4 条腿是“延后到 kGameDataReady 才装”的（它靠全表单映射）——
			// 这里必须单独放行，否则 F4SEPluginLoad 那次 Report 会把它误报成 `hook 失败` ✗。
			LOG_INFO("hook defer  : {} (ID {}) -- installed at kGameDataReady", s.name, s.id);
			continue;
		}
		if (s.disabled) {
			LOG_INFO("hook off    : {} (disabled in src/Hooks.cpp)", s.name);
		} else if (s.enabled) {
			LOG_INFO("hook ok     : {} (ID {}) @ {:X}", s.name, s.id, reinterpret_cast<std::uintptr_t>(s.target));
		} else if (s.installed) {
			// “半装”= CreateHook 成了但 EnableHook 没成 ⇒ 行为可疑，保持告警。
			REX::WARN("hook partial: {} (ID {}) code={}", s.name, s.id, s.code);
		} else {
			// 真故障：钩子没装上，插件基本失效 ⇒ 永远是 ERROR。
			REX::ERROR("hook failed : {} (ID {}) code={}", s.name, s.id, s.code);
		}
	}
}

// DESC 映射：转发到匿名 namespace 里的内部实现。
bool Hooks::BuildDescriptionMap()
{
	return buildDescriptionMap();
}

// 第 4 条腿（显示层）：**kGameDataReady** 时调用 —— 先建“组件地址 → formID”映射，再装钩子。
//
// 为什么要晚装（照 F4TCHook：`kGameDataReady` 里 BuildMap() → Install()）：
//   ① 映射依赖**全表单已就绪**（`formArrays` 要填满），F4SEPluginLoad 时还是空的；
//   ② 显示层函数在读数期无人调，晚装没有窗口期损失。
// 为什么默认就算（不再挂在 `高级` 下）：没有映射就没法从 `TESDescription*` 反推 formID
//   （`a_form` 实测恒为 nullptr），钩子就是死的 —— 而它现在承担**功能**，不再只是探针。
void Hooks::InstallDescriptionHook()
{
	if (g_status[3].enabled) {
		return;   // 已装（收档事件会来多次）
	}
	(void)buildDescriptionMap();
	Try(g_status[3]);
}

bool Hooks::FormIDOfDescription(const void* a_description, std::uint32_t& a_formID)
{
	return formIDOfDescription(a_description, a_formID);
}
