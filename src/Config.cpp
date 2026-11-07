#include "PCH.h"

#include "Config.h"

#include "ConfigParse.h"
#include "StrUtil.h"

#include <fstream>

#include "Paths.h"
#include "REX/W32/KERNEL32.h"

// 配置文件在哪：**优先 Documents 下那一份**。
//
// 教训（2026-10-07 实测）：mod 目录里那份 ini，游戏进程读出来是空的 ——
// 同一时刻磁盘上是 1003 字节、内容正确，日志却报 Enable=false、替换一条都没生效。
// 原因指向 MO2 的 usvfs：我们对「mod 目录物理路径」的读被虚拟化了。
// 写 Documents 目录（VFS 之外）一直没问题，所以配置也放那儿，并且两个位置都读、都记日志。

namespace
{
	Config::ReplaceConfig g_cfg;
	std::string           g_used;
	std::string           g_usedForLog;   // 同一份路径的“可贴进 bug 报告”形式（相对 Data / 只剩文件名）
	std::string           g_report;   // 逐候选路径的读结果，日志里一行行打出来

	// ── 节点 3 名单 ────────────────────────────────────────────────────────
	// ⚠️ 解析/匹配的实现全部在 `ConfigParse.h`（纯函数、无引擎依赖，可被
	//    `tools\selftest.py` 单独编译并跑表驱动用例）。
	using ConfigParse::AnyMatch;
	using ConfigParse::Entry;
	using ConfigParse::Lower;
	using ConfigParse::Match;
	using ConfigParse::ParseEntry;
	using ConfigParse::SplitComma;
	using ConfigParse::Trim;
	using ConfigParse::Truthy;

	std::vector<Entry>       g_white;
	std::vector<Entry>       g_black;
	std::vector<Entry>       g_focus;   // [调试] 关注记录：诊断用，命中就一路打全
	std::vector<std::string> g_whiteRaw;
	std::vector<std::string> g_blackRaw;
	std::vector<std::string> g_focusRaw;

	std::vector<std::filesystem::path> Candidates()
	{
		std::vector<std::filesystem::path> out;
		if (const auto dir = Paths::GamePluginsDir(); !dir.empty()) {
			out.push_back(dir / "F4LoadText.ini");   // 主：<游戏>\Data\F4SE\Plugins\（= ImeControl 约定）
		}
		out.push_back(Paths::DllDir() / "F4LoadText.ini");   // 兜底：DLL 所在目录（MO2 下其实是同一个文件）
		return out;
	}

	// 返回读到的字节数；内容放进 a_out
	std::size_t ReadAll(const std::filesystem::path& a_path, std::string& a_out)
	{
		a_out.clear();
		std::ifstream in{ a_path, std::ios::binary };
		if (!in) {
			return 0;
		}
		a_out.assign(std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{});
		// ⚠️ 去掉 UTF-8 BOM：Trim 只去空格/制表/换行，带 BOM 时第一段名会变成
		//    "\xEF\xBB\xBF功能" ⇒ [功能] 整段静默失效、Enable 落回默认 false。
		//    用 pwsh 的 Set-Content（默认带 BOM）改一次 ini 就会踩，防一下。
		if (a_out.size() >= 3 && static_cast<unsigned char>(a_out[0]) == 0xEF &&
			static_cast<unsigned char>(a_out[1]) == 0xBB && static_cast<unsigned char>(a_out[2]) == 0xBF) {
			a_out.erase(0, 3);
		}
		return a_out.size();
	}

	std::size_t Parse(const std::string& a_text, Config::ReplaceConfig& a_cfg)
	{
		std::size_t keys = 0;
		std::string section;
		std::string line;
		std::istringstream in{ a_text };
		while (std::getline(in, line)) {
			const auto trimmed = Trim(line);
			if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') {
				continue;
			}
			if (trimmed.front() == '[' && trimmed.back() == ']') {
				section = Lower(std::string_view{ trimmed }.substr(1, trimmed.size() - 2));
				continue;
			}
			// 行内注释：' ; ' 或 '#' 之后一律算注释（与 ImeControl 的解析一致）
			const auto text = Trim(std::string_view{ trimmed }.substr(0, trimmed.find_first_of(";#")));
			if (text.empty()) {
				continue;
			}
			const auto eq = text.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			const auto key = Lower(Trim(std::string_view{ text }.substr(0, eq)));
			const auto val = Trim(std::string_view{ text }.substr(eq + 1));

			// [Whitelist] / [Blacklist]：段里每个「值」都是一个名单条目（逗号分隔，键名随意）
			// 段名中英双认：[名单]==[Whitelist]、[黑名单]==[Blacklist]
			const bool isWhite = section == "whitelist" || section == "名单";
			const bool isBlack = section == "blacklist" || section == "黑名单";
			if (isWhite || isBlack) {
				for (auto& item : SplitComma(val)) {
					(isWhite ? g_whiteRaw : g_blackRaw).push_back(std::move(item));
				}
				++keys;
				continue;
			}

			// 其余段：认键就行
			if (key == "enable") {
				a_cfg.enable = Truthy(val);
				++keys;
			} else if (key == "审计上限" || key == "auditlimit") {
				// ② 审计：逐条回写的行数上限（0 = 不打）。解析失败保留默认。
				std::uint32_t parsed = 0;
				const auto    r = std::from_chars(val.data(), val.data() + val.size(), parsed, 10);
				if (r.ec == std::errc{} && r.ptr == val.data() + val.size()) {
					a_cfg.auditLimit = parsed;
				}
				++keys;
			} else if (key == "审计签名" || key == "auditsignatures") {
				// 逗号分隔的 4CC 列表；空/`*` = 全打（保留旧行为）。
				a_cfg.auditSignatures.clear();
				for (auto& item : SplitComma(val)) {
					a_cfg.auditSignatures.push_back(Lower(item));
				}
				++keys;
			} else if (key == "审计文本" || key == "audittext") {
				// 逗号分隔的子串（CJK 直接按 UTF-8 字节比对，ini 就是 UTF-8）。
				// 匹配对象是「读进来的那份」和「写回去的那份」两边。
				a_cfg.auditText.clear();
				for (auto& item : SplitComma(val)) {
					a_cfg.auditText.push_back(std::move(item));
				}
				++keys;
			} else if (key == "关注记录" || key == "focusrecords") {
				// 指名追踪：条目形态与 [名单] 相同（插件名|ID）；键名随意，值逗号分隔。
				for (auto& item : SplitComma(val)) {
					g_focusRaw.push_back(std::move(item));
				}
				++keys;
			} else if (key == "信息" || key == "info") {
				a_cfg.info = Truthy(val);
				++keys;
			} else if (key == "高级" || key == "advanced") {
				a_cfg.advanced = Truthy(val);
				++keys;
			} else if (key == "汉化" || key == "translation") {
				a_cfg.translationEnabled = Truthy(val);
				++keys;
			} else if (key == "汉化目录" || key == "translationdir") {
				a_cfg.translationDir = std::string{ val };
				++keys;
			} else if (key == "字典缓存" || key == "translationcache") {
				a_cfg.translationCache = Truthy(val);
				++keys;
			} else if (key == "异步日志" || key == "asynclog") {
				a_cfg.asyncLog = Truthy(val);
				++keys;
			}
		}
		(void)section;
		return keys;
	}
}

void Config::Load()
{
	g_report.clear();

	// ⚠️ 只让「第一个解析成功的候选」生效。
	// 踩过的坑：早先每个候选都往同一个全局结构里写，于是后读的那份（后备 ini）
	// 会把前一份的 Enable 覆盖掉 —— 日志还显示成「用 Documents 那份」，完全误导。
	bool adopted = false;
	for (const auto& path : Candidates()) {
		if (adopted) {
			break;   // 采用之后就停：两个候选可能指向同一个文件，重复解析会把名单算两遍
		}
		g_whiteRaw.clear();
		g_blackRaw.clear();
		g_focusRaw.clear();
		std::string             text;
		const auto              bytes = ReadAll(path, text);
		Config::ReplaceConfig   candidate;                       // 先解到临时变量
		const auto              keys = bytes ? Parse(text, candidate) : 0;
		const bool              take = (keys > 0 && !adopted);

		g_report += std::format("\n    {}  exists={} bytes={} 键数={}{}", Paths::ForLog(path),
			std::filesystem::exists(path) ? "Y" : "N", bytes, keys, take ? "   ← 采用" : "");

		if (take) {
			g_cfg = candidate;
			g_used = path.string();
			g_usedForLog = Paths::ForLog(path);   // 日志用：脱敏后的相对路径
			adopted = true;
		}
	}

	if (g_used.empty()) {
		REX::ERROR("config: no usable config file found (everything disabled by default) {}", g_report);
		return;
	}

	// 名单：解析成可匹配的条目（节点 3）
	// ⚠️ 解析失败要**报出来**：条目会以 valid=false 存在，即“永不匹配”——
	//    是典型的静默失效，只靠日志里的条数看不出来。
	const auto parseInto = [](const std::vector<std::string>& a_raw, std::vector<Entry>& a_out, const char* a_which) {
		a_out.clear();
		for (const auto& item : a_raw) {
			std::string err;
			auto        e = ParseEntry(item, &err);
			if (!err.empty()) {
				REX::WARN("{} entry parse failed ({}): {}", a_which, err, item);
			}
			a_out.push_back(std::move(e));
		}
	};
	parseInto(g_whiteRaw, g_white, "[Whitelist]");
	parseInto(g_blackRaw, g_black, "[Blacklist]");
	g_cfg.whitelist = g_whiteRaw;
	g_cfg.blacklist = g_blackRaw;

	// 诊断：指名追踪（[调试] 关注记录）—— 与名单同一套解析/匹配
	parseInto(g_focusRaw, g_focus, "[Debug] FocusRecords");
	g_cfg.focusRecords = g_focusRaw;

	LOG_INFO("whitelist: {} entries  blacklist: {} entries{}  focus: {} entries", g_white.size(), g_black.size(),
		g_white.empty() ? " (whitelist empty -> nothing is replaced; use Entries = * to replace all)" : "", g_focus.size());

	LOG_INFO("config: using {} -> Enable={} Info={} AuditLimit={} AuditSignatures={} AuditText={} Advanced={} FocusRecords={} Translation={} Dir={} Cache={} AsyncLog={}",
		g_usedForLog, g_cfg.enable, g_cfg.info, g_cfg.auditLimit,
		g_cfg.auditSignatures.empty() ? "(all)" : std::format("{} entries", g_cfg.auditSignatures.size()),
		g_cfg.auditText.empty() ? "(any)" : std::format("{} entries", g_cfg.auditText.size()),
		g_cfg.advanced, g_focus.size(), g_cfg.translationEnabled, g_cfg.translationDir, g_cfg.translationCache, g_cfg.asyncLog);
	LOG_INFO("config: tried each candidate: {}", g_report);

	// 「信息」是总闸（见 Config.h 的宏说明与 Replace::LogAudit 的头一条门槛）。
	// 关掉信息却还留着审计上限 / 关注记录 = 典型的**静默失效**，必须喊一声：
	// 这里用 REX::WARN，它不受任何开关限制，一定会出现在日志里。
	if (!g_cfg.info) {
		if (g_cfg.auditLimit > 0) {
			REX::WARN("config: AuditLimit={} but Info=false -> audit lines will NOT be written (audit rides on Info)",
				g_cfg.auditLimit);
		}
		if (!g_focus.empty()) {
			REX::WARN("config: FocusRecords={} entries but Info=false -> those records will NOT be logged", g_focus.size());
		}
	}
}

const Config::ReplaceConfig& Config::Get()
{
	return g_cfg;
}

bool Config::ShouldRewrite(std::string_view a_plugin, std::uint32_t a_rawID)
{
	if (a_plugin.empty()) {
		return false;
	}
	if (AnyMatch(g_black, a_plugin, a_rawID)) {
		return false;   // 黑名单优先
	}
	if (g_white.empty()) {
		// README：「默认走白名单…白名单之外默认黑名单」⇒ 空白名单 = 什么都不改。
		// 要"全改"请显式写 Entries = *（解析器把它当插件通配）。
		return false;
	}
	return AnyMatch(g_white, a_plugin, a_rawID);
}

// 诊断用：是否被「关注记录」指名（[调试] focusrecords）。
// 报错也给（不看 Enable）：它只是日志开关，方便在 Enable=false 的对照实验下一起看。
//
// ⚠️ 两种 ID 形态都认（2026-11-08）：插件内原始 ID（低位）**或**运行时 formID
//    （日志里 `record=` 那一串，如 FE004F9A）。旧版只比 raw —— 按日志抄成运行时 ID
//    的条目会静默永不命中（排“这条记录怎么没被处理”时最坑）。
bool Config::IsFocus(std::string_view a_plugin, std::uint32_t a_rawID, std::uint32_t a_formID)
{
	if (a_plugin.empty() || g_focus.empty()) {
		return false;
	}
	return AnyMatch(g_focus, a_plugin, a_rawID) || (a_formID != 0 && AnyMatch(g_focus, a_plugin, a_formID));
}

const std::string& Config::Path()
{
	return g_used;
}

