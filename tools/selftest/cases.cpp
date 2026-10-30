// tools/selftest/cases.cpp
//
// 表驱动自检：只覆盖**纯函数**（不碰引擎、不需要游戏、不需要 CommonLib）。
// 由 tools\selftest.py 编译并运行。
//
// ⚠️ 加用例就加在这一个文件里，别在脚本里拼字符串。
// ⚠️ 故意用最朴素的 printf（不引测试框架）—— 这个仓的依赖都是手工解包的，
//    多一个第三方头就多一处要维护。

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "ConfigParse.h"
#include "EspPathResolve.h"
#include "StrUtil.h"

namespace
{
	int g_pass = 0;
	int g_fail = 0;

	void Ok(bool a_cond, std::string_view a_what)
	{
		if (a_cond) {
			++g_pass;
		} else {
			++g_fail;
			std::printf("FAIL  %.*s\n", static_cast<int>(a_what.size()), a_what.data());
		}
	}

	// SplitComma 的用例：输入 + 期望的分段
	void Split(std::string_view a_in, std::vector<std::string> a_want)
	{
		const auto got = ConfigParse::SplitComma(a_in);
		bool       ok  = got.size() == a_want.size();
		if (ok) {
			for (std::size_t i = 0; i < got.size(); ++i) {
				ok = ok && got[i] == a_want[i];
			}
		}
		if (!ok) {
			std::printf("      split <%.*s> -> %zu:", static_cast<int>(a_in.size()), a_in.data(), got.size());
			for (const auto& s : got) {
				std::printf(" <%s>", s.c_str());
			}
			std::printf("\n");
		}
		Ok(ok, a_in);
	}

	void TestTrimAndTruthy()
	{
		Ok(ConfigParse::Trim("  x  ") == "x", "Trim 去两端空白");
		Ok(ConfigParse::Trim("") == "", "Trim 空串");
		Ok(ConfigParse::Trim("\t\n x \r\n") == "x", "Trim 各种空白");
		Ok(ConfigParse::Trim("中文 ") == "中文", "Trim 中文");

		for (const auto* v : { "1", "true", "TRUE", "on", "yes", " Yes " }) {
			Ok(ConfigParse::Truthy(v), std::string{ "Truthy 应为真: " } + v);
		}
		for (const auto* v : { "0", "", "off", "false", "2", "no" }) {
			Ok(!ConfigParse::Truthy(v), std::string{ "Truthy 应为假: " } + v);
		}
	}

	// ⚠️ 全角分隔符这几条是**真事故**（中文输入法敲的名单会静默失效），别删。
	void TestSplitComma()
	{
		Split("", {});
		Split("A.esp", { "A.esp" });
		Split("A.esp,B.esp", { "A.esp", "B.esp" });
		Split("A.esp，B.esp", { "A.esp", "B.esp" });          // 全角逗号 U+FF0C
		Split("A.esp、B.esp", { "A.esp", "B.esp" });          // 顿号 U+3001
		Split(" A.esp , B.esp , ", { "A.esp", "B.esp" });
		Split("A.esp，", { "A.esp" });
		Split("中文.esp，B.esp", { "中文.esp", "B.esp" });
		Split("A.esp|0012AB,B.esp|*", { "A.esp|0012AB", "B.esp|*" });
		Split(",,,", {});
	}

	void TestParseEntry()
	{
		{
			std::string err;
			const auto  e = ConfigParse::ParseEntry("Fallout4.ESM", &err);
			Ok(err.empty() && e.valid, "ParseEntry 插件级 有效");
			Ok(e.plugin == "fallout4.esm", "ParseEntry 插件名存小写");
			Ok(!e.pluginWild && e.idWild, "ParseEntry 插件级 = 记录通配");
		}
		{
			std::string err;
			const auto  e = ConfigParse::ParseEntry("Mod.esp|0012AB", &err);
			Ok(err.empty() && e.valid && !e.idWild, "ParseEntry 记录级 有效");
			Ok(e.rawID == 0x12AB, "ParseEntry 记录级 rawID");
		}
		{
			const auto e = ConfigParse::ParseEntry("*|000800");
			Ok(e.valid && e.pluginWild && e.rawID == 0x800, "ParseEntry 插件通配");
		}
		{
			const auto e = ConfigParse::ParseEntry(" Mod.esp | 0a ");
			Ok(e.valid && e.rawID == 0x0A && e.plugin == "mod.esp", "ParseEntry 两端空格 + 小写十六进制");
		}
		{
			const auto e = ConfigParse::ParseEntry("Mod.esp|");
			Ok(e.valid && e.idWild, "ParseEntry | 后为空 = 记录通配");
		}
		{
			std::string err;
			const auto  e = ConfigParse::ParseEntry("Bad.esp|XYZ", &err);
			Ok(!e.valid && !err.empty(), "ParseEntry 非十六进制 ID 要报错");
		}
		{
			std::string err;
			const auto  e = ConfigParse::ParseEntry("|0012AB", &err);
			Ok(!e.valid && !err.empty(), "ParseEntry 空插件名要报错");
		}
	}

	void TestMatch()
	{
		const auto rec = ConfigParse::ParseEntry("Fallout4.esm|0012AB");
		Ok(ConfigParse::Match(rec, "fallout4.esm", 0x12AB), "Match 大小写不敏感 + ID 命中");
		Ok(!ConfigParse::Match(rec, "fallout4.esm", 0x12AC), "Match ID 不同");
		Ok(!ConfigParse::Match(rec, "other.esp", 0x12AB), "Match 插件不同");

		const auto any = ConfigParse::ParseEntry("*|000800");
		Ok(ConfigParse::Match(any, "anything.esp", 0x800), "Match 插件通配命中");
		Ok(!ConfigParse::Match(any, "anything.esp", 0x801), "Match 插件通配但 ID 不同");

		const auto pluginOnly = ConfigParse::ParseEntry("Mod.esp");
		Ok(ConfigParse::Match(pluginOnly, "MOD.ESP", 0xDEAD), "Match 插件级 = 任意记录");

		const auto bad = ConfigParse::ParseEntry("Bad.esp|XYZ");
		Ok(!ConfigParse::Match(bad, "Bad.esp", 0), "Match 无效条目不匹配");

		const std::vector<ConfigParse::Entry> list{ rec, any };
		Ok(ConfigParse::AnyMatch(list, "x.esp", 0x800), "AnyMatch 命中第二条");
		Ok(!ConfigParse::AnyMatch(list, "x.esp", 0x801), "AnyMatch 都不中");
		Ok(!ConfigParse::AnyMatch({}, "x.esp", 0x800), "AnyMatch 空名单");
	}

	// EspPath::FileName / Candidates —— yaml 的 `path` → 候选文件路径。
	//
	// 为何要钉针：`path` 只写文件名时，如果没生成 <Data>\<文件名> 这个候选，
	// `EspMastersFrom` 会读不到 masters ⇒ `ownIdx == 0` ⇒ 走「全部当自有」的
	// 宽容分支 ⇒ **覆盖记录的译文键少一段 `|@来源`，静默查不到**。
	// 实测数据：community fixes merged.yaml 的 1,300+ 条覆盖记录全靠它。
	void TestEspPath()
	{
		// FileName：反斜杠、正斜杠、末尾分隔符、无分隔符
		Ok(EspPath::FileName("community fixes merged.esp") == "community fixes merged.esp",
			"FileName 裸文件名");
		Ok(EspPath::FileName("F:\\MODs_dir\\Fallout 4\\mods\\X\\a.esp") == "a.esp",
			"FileName 反斜杠绝对路径");
		Ok(EspPath::FileName("E:/Games/Fallout 4/Data/a.esp") == "a.esp", "FileName 正斜杠绝对路径");
		Ok(EspPath::FileName("dir\\") == "dir", "FileName 末尾反斜杠先剥掉");
		Ok(EspPath::FileName("") == "", "FileName 空串");

		// Join：目录末尾有没有分隔符都对
		Ok(EspPath::Join("C:\\Data", "a.esp") == "C:\\Data\\a.esp", "Join 无反斜杠补一个");
		Ok(EspPath::Join("C:\\Data\\", "a.esp") == "C:\\Data\\a.esp", "Join 已有反斜杠不重复");
		Ok(EspPath::Join("", "a.esp").empty(), "Join 空目录 ⇒ 空");

		// Candidates：三个基准全给 ⇒ 三条，顺序 = AsGiven / InData / BesideDll
		{
			EspPath::Candidate c[3];
			const auto         n = EspPath::Candidates("unofficial fallout 4 patch.esp", "G:\\Data", "G:\\P", c);
			Ok(n == 3, "Candidates 全基准 ⇒ 3 条");
			Ok(c[0].kind == EspPath::Kind::AsGiven && c[0].path == "unofficial fallout 4 patch.esp",
				"Candidates[0] = 原样");
			Ok(c[1].kind == EspPath::Kind::InData && c[1].path == "G:\\Data\\unofficial fallout 4 patch.esp",
				"Candidates[1] = <Data>\\<文件名>（发布后的正路）");
			Ok(c[2].kind == EspPath::Kind::BesideDll && c[2].path == "G:\\P\\unofficial fallout 4 patch.esp",
				"Candidates[2] = DLL 旁");
		}

		// 老 yaml 的绝对路径：原样一条要能打头阵（本地导出流程不能坏）
		{
			EspPath::Candidate c[3];
			const auto         n = EspPath::Candidates("F:\\MODs_dir\\M\\a.esp", "G:\\Data", "G:\\P", c);
			Ok(n == 3, "Candidates 绝对路径也给 3 条");
			Ok(c[0].path == "F:\\MODs_dir\\M\\a.esp", "Candidates[0] 保留绝对路径原样");
			Ok(c[1].path == "G:\\Data\\a.esp", "Candidates[1] 剥成文件名放 Data 下");
		}

		// 基准缺失 ⇒ 跳过对应候选（不能生成 "\\a.esp" 这种相对到盘根的路径）
		{
			EspPath::Candidate c[3];
			Ok(EspPath::Candidates("a.esp", "", "", c) == 1, "Candidates 无基准 ⇒ 只剩原样");
			Ok(EspPath::Candidates("a.esp", "G:\\Data", "", c) == 2, "Candidates 只有 Data ⇒ 2 条");
		}

		// 空 path ⇒ 一个候选都不产生（调用方据此走宽容分支）
		{
			EspPath::Candidate c[3];
			Ok(EspPath::Candidates("", "G:\\Data", "G:\\P", c) == 0, "Candidates 空 path ⇒ 0 条");
		}
	}

	// Str::IEquals 必须与 _stricmp 语义一致（它就是替换 _stricmp 的）
	void TestIEquals()
	{
		const std::vector<std::string> names{
			"Fallout4.esm", "fallout4.ESM", "FALLOUT4.ESM", "DLCNukaWorld.esm",
			"ccBGSFO4046-TesCan.esl", "", "a", "A", "Diablo IV - blah.esp",
			"diablo iv - BLAH.ESP", "中文.esp", "中文.Esp", "Fallout4.esm ",
			" Fallout4.esm", "Fallout4.esmX"
		};
		for (const auto& a : names) {
			for (const auto& b : names) {
				const bool mine = Str::IEquals(std::string_view{ a }, std::string_view{ b });
				const bool ref  = _stricmp(a.c_str(), b.c_str()) == 0;
				Ok(mine == ref, std::string{ "IEquals vs _stricmp: " } + a + " / " + b);
			}
		}
	}
}

int main()
{
	TestTrimAndTruthy();
	TestSplitComma();
	TestParseEntry();
	TestMatch();
	TestEspPath();
	TestIEquals();
	std::printf("cases: pass=%d fail=%d\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}