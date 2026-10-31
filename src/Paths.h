#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "REX/W32/KERNEL32.h"

// ── 路径：**全项目唯一一份** ────────────────────────────────────────────────
//
// 为什么要它：以前「本 DLL 在哪 / 游戏 Data 在哪 / 插件文件在哪」有 **5 份实现、
// 两套 API**（A 与 W 混用，还有一份手写的 `__declspec(dllimport)` 声明）。
// 代价已经付过：`Config::PluginDir` 漏了长度校验，而 `Translation::PluginDir`
// 有 —— 同一个名字，两种行为（2026-10-22 才补齐）。
//
// ⚠️ 两个目录的语义不同，别混：
//   * `DllDir()`  —— 物理路径（MO2 下 = mod 文件夹）。**只**用于“DLL 旁边还有什么”。
//   * `DataDir()` —— `<游戏 exe>\Data`，MO2 下是**虚拟目录**（USVFS 把 mod 的文件映射进来）。
//     读插件/字符串表必须用它（用 DllDir 会报 mod 目录，读不到游戏文件）。
namespace Paths
{
	namespace detail
	{
		inline std::filesystem::path ModuleDir(REX::W32::HMODULE a_module)
		{
			wchar_t    buf[REX::W32::MAX_PATH]{};
			const auto len = REX::W32::GetModuleFileNameW(a_module, buf, REX::W32::MAX_PATH);
			// ⚠️ 必须校验 `len`（失败时构造空串 ⇒ 路径退化成**相对路径**，按 CWD 找文件）
			if (!len || len >= REX::W32::MAX_PATH) {
				return {};
			}
			return std::filesystem::path{ std::wstring{ buf, len } }.parent_path();
		}
	}

	// 本 DLL 所在目录（物理路径）
	inline const std::filesystem::path& DllDir()
	{
		static const std::filesystem::path cached = detail::ModuleDir(REX::W32::GetCurrentModule());
		return cached;
	}

	// <游戏 exe 目录>\Data（MO2 下 = 虚拟目录）
	inline const std::filesystem::path& DataDir()
	{
		static const std::filesystem::path cached = [] {
			const auto exeDir = detail::ModuleDir(nullptr);
			return exeDir.empty() ? std::filesystem::path{} : exeDir / "Data";
		}();
		return cached;
	}

	// <Data>\F4SE\Plugins —— ini 的**主**候选目录（与 ImeControl 同一约定）
	inline std::filesystem::path GamePluginsDir()
	{
		const auto& dir = DataDir();
		return dir.empty() ? std::filesystem::path{} : dir / "F4SE" / "Plugins";
	}

	// 插件文件路径：<Data>\<插件名>（读 esp/esl 用，走 MO2 虚拟目录）
	inline std::string Plugin(std::string_view a_plugin)
	{
		const auto& dir = DataDir();
		if (dir.empty() || a_plugin.empty()) {
			return {};
		}
		return (dir / std::filesystem::path{ std::string{ a_plugin } }).string();
	}

	// 日志/报告里**给人看**的路径：尽量脱敏成相对形式。
	//   <游戏 exe>\Data\F4SE\Plugins\F4LoadText.ini  →  Data\F4SE\Plugins\F4LoadText.ini
	//   别的盘/别的目录（MO2 的 mod 文件夹、临时目录…）  →  只留文件名
	//
	// ⚠️ 为什么：日志会被整段贴进 bug 报告 / issue，绝对路径带**用户名与安装布局**
	//    （`F:\Games\SteamLibrary\…`、`C:\Users\<名字>\…`），对诊断零价值。
	//   用 `lexically_relative`（纯字符串，不碰磁盘、不看文件存不存在）。
	inline std::string ForLog(const std::filesystem::path& a_path)
	{
		if (a_path.empty()) {
			return "?";
		}
		const auto rel = a_path.lexically_relative(DataDir());
		if (!rel.empty() && rel.native().rfind(L"..", 0) != 0) {
			return (std::filesystem::path{ "Data" } / rel).string();
		}
		return a_path.filename().string();
	}
}
