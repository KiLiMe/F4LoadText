#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// ── yaml 的 `path` → 候选文件路径（**纯逻辑，无引擎/无文件系统依赖**）───────
//
// 为什么单独拎出来：这段决定「能不能读到 esp 的 masters」，而 masters 又决定
// **覆盖记录的译文键**（`src/Translation.cpp` 里 `ownIdx == 0` 会走「全部当自有」
// 的宽容分支）。判错了不会报错，只会**静默查不到译文** —— 典型的静默失效，
// 必须能用表驱动用例钉住，所以它不能依赖引擎（`Paths::DataDir()` 要游戏路径）。
//
// 调用方（`Translation.cpp` 的 `ResolveEspPath`）只负责拿这两个基准目录来喂它：
//   * <游戏>\Data      —— MO2 的 USVFS 把 mod 的文件映射进来，这是**发布后的正路**
//   * <mod>\F4SE\Plugins —— 兼容「把 esp 跟 DLL 放一起」的本地调试
namespace EspPath
{
	// 候选的三种来源（顺序即优先级，见 Candidates()）
	enum class Kind
	{
		AsGiven,   // 原样：绝对路径 / 相对游戏 CWD 的路径（老 yaml、本地导出）
		InData,    // <Data>\<文件名>      ← 发布后的正路
		BesideDll, // <dllDir>\<文件名>    ← 本地调试兜底
	};

	struct Candidate
	{
		Kind        kind{ Kind::AsGiven };
		std::string path;
	};

	// 取路径的文件名部分（同时认 `\` 与 `/`；末尾的分隔符先剥掉）。
	//
	// ⚠️ 不能直接用 std::filesystem::path::filename()：这里要能被自检单独编，
	//    而本仓的自检（tools\selftest\cases.cpp）刻意不引任何第三方/系统头。
	[[nodiscard]] inline std::string_view FileName(std::string_view a_path)
	{
		while (!a_path.empty() && (a_path.back() == '\\' || a_path.back() == '/')) {
			a_path.remove_suffix(1);
		}
		if (const auto pos = a_path.find_last_of("\\/"); pos != std::string_view::npos) {
			a_path.remove_prefix(pos + 1);
		}
		return a_path;
	}

	// 生成候选列表（按优先级）。
	//
	// `a_dataDir` / `a_dllDir` 为空 = 该基准不可用，跳过对应候选。
	// 返回空 = `a_path` 里连文件名都没有（yaml 的 `path:` 写空了）。
	//
	// ⚠️ 顺序是**功能性的**，不是风格问题：
	//    老 yaml 里的绝对路径必须能命中（否则本地导出的工作流当场失效）；
	//    而只写文件名的 yaml 必须靠 InData —— 少了它，覆盖记录的译文全丢。
	[[nodiscard]] inline std::string Join(std::string_view a_dir, std::string_view a_name)
	{
		if (a_dir.empty()) {
			return {};
		}
		std::string out{ a_dir };
		const char  last = out.back();
		if (last != '\\' && last != '/') {
			out.push_back('\\');
		}
		out.append(a_name);
		return out;
	}

	[[nodiscard]] inline std::size_t Candidates(std::string_view a_path, std::string_view a_dataDir,
		std::string_view a_dllDir, Candidate (&a_out)[3])
	{
		std::size_t n = 0;
		if (a_path.empty()) {
			return 0;
		}
		a_out[n++] = Candidate{ Kind::AsGiven, std::string{ a_path } };

		const auto name = FileName(a_path);
		if (name.empty()) {
			return n;
		}
		if (!a_dataDir.empty()) {
			a_out[n++] = Candidate{ Kind::InData, Join(a_dataDir, name) };
		}
		if (!a_dllDir.empty()) {
			a_out[n++] = Candidate{ Kind::BesideDll, Join(a_dllDir, name) };
		}
		return n;
	}
}
