#include "PCH.h"

#include "REX/W32/USER32.h"

#include "Config.h"
#include "Hooks.h"
#include "Replace.h"
#include "Translation.h"

// 异步日志：把 CommonLib 建立的同步 logger 换成 spdlog 异步 logger。
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/spdlog.h>

#include <cstdlib>

// ─────────────────────────────────────────────────────────────────────────────
// 生命周期：
//   1. F4SEPluginLoad：Init → 读配置 → 挂**3 条读盘腿**（必须在这一步挂，等 kGameDataReady 就晚了）
//   2. kGameDataReady：打收尾汇总 + **建 DESC 身份反查表 → 装第 4 条（显示层）腿**
//      （它必须晚：a_form 恒 nullptr，没表就不知道在给哪条记录写；且当时 formArrays 才填满）
//   3. 版本：**不硬拦**（2026-10-09 改）—— 参见 F4SEPluginLoad 里的说明。
//      全程用地址库（REL::ID），版本映射交给它；F4SE 的 CompatibleVersions +
//      addressIndependence/structureIndependence 元数据把“能不能跑”的关。
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	// ── 把 CommonLib 的**同步** logger 换成**异步** logger（只影响本 DLL）──
	//
	// 动机：`CommonLibF4` 的 `F4SE::Init → API::InitLog` 建的是普通同步 logger
	//   （`basic_file_sink_mt`/`rotating_file_sink_mt` + `flush_on`）—— 写一条就落一次盘，
	//   若在**主线程/渲染线程**上发生就会阻塞帧。换成 async 后：写日志只**入队**，
	//   落盘由后台线程做 ⇒ 无论开不开审计/信息，调用方都不会被 IO 卡住。
	//
	// 做法（不动文件路径/编码，只换调度）：
	//   * 复用现 logger 的 sinks（shared_ptr）⇒ 同一个文件句柄，不重开、不丢句柄；
	//   * `async_overflow_policy::overrun_oldest`：队列满时丢**最旧**的 —— 确保写日志
	//     **永不阻塞调用方**（日志是诊断用，丢了不致命）；
	//   * `flush_on(level)` 仍在，flush 也被异步化（后台线程执行）。
	//   * `atexit(spdlog::shutdown)`：退出时把队列刷干净（否则最后几条可能丢在队列里）。
	//   ⚠️ 每个 F4SE 插件有各自的 spdlog 全局 default_logger ⇒ 本改动**不影响其它插件**。
	void MakeLoggerAsync()
	{
		const auto current = spdlog::default_logger();
		if (!current) {
			return;   // 日志没初始化（本插件不会走到这）
		}
		if (std::dynamic_pointer_cast<spdlog::async_logger>(current)) {
			return;   // 已经是异步（重复调用保护）
		}

		const auto level = current->level();
		const auto sinks = current->sinks();   // 拷贝 shared_ptr 列表 ⇒ 文件句柄由新旧 logger 共享
		if (sinks.empty()) {
			return;
		}

		spdlog::init_thread_pool(8192, 1);   // 队列 8192、一个后台线程（本插件日志量极小）
		auto async = std::make_shared<spdlog::async_logger>("global", sinks.begin(), sinks.end(),
			spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
		async->set_level(level);
		async->flush_on(level);
		spdlog::set_default_logger(std::move(async));

		std::atexit([]() { spdlog::shutdown(); });
	}

	void F4SEAPI MessageHandler(F4SE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case F4SE::MessagingInterface::kGameDataReady:
			if (a_msg->data == nullptr) {
				break;  // data 为 0 时数据还没就绪（读档流程会再来一次）
			}

			if (Replace::Enabled()) {
				const auto r = Replace::Snapshot();
				LOG_INFO("replace: load-time fields  incoming-dropped={} (of which from base={})  translated={}  -- overrides from plugins outside the whitelist are discarded, base text is kept",
					r.fieldIgnored, r.fieldFromBase, r.fieldTranslated);
			}
			// ⚠️ 注意：“覆盖丢弃”对**占位符 `*`** 不写（见 Replace.cpp 的 `!a_ownerRead` 分支）——
			//     实测 `*` 会被错写成基表第 1 项（引擎分多遍读同一条记录，序号对不上）。
			// 译文字典的性能：看预筛效果（passed 应远小于 calls）。
			// 读数解读：如果 calls 上百万而 passed 只有几千 ⇒ 预筛正常；
			//           两者接近 ⇒ 预筛失效（插件名对不上）。
			if (Translation::Ready()) {
				const auto p = Translation::PerfSnapshot();
				LOG_INFO("translation: lookups={}  prefilter-passed={} ({}%)  single-value-guard-skipped={}",
					p.calls, p.passed, p.calls ? (p.passed * 100 / p.calls) : 0, Translation::SuppressedCount());
			}
			// DESC 探针**故意不在这里**逐条打印：GetDescription 是“界面要用才调”的，
			//    kGameDataReady（刚读完档）时界面还没开 —— 逐条行改由
			//    getDescriptionThunk “遇到新组件就即时打一行”（见那里的注释）。
			//
			// 但**这条腿本身要在这里装**（照 F4TCHook：kGameDataReady 里 BuildMap() → Install()）：
			//   ① 它的“组件地址 → formID”映射要全表单就绪（formArrays 填满），F4SEPluginLoad 时是空的；
			//   ② 显示层函数在读盘期无人调，晚装没有窗口期损失。
			if (Replace::Enabled()) {
				Hooks::InstallDescriptionHook();
			}

			break;
		default:
			break;
		}
	}
}

// ⚠️ 版本数据段不是可选项：只导出 F4SEPlugin_Load 的 DLL，F4SE 会直接跳过
//    （f4se.log 里那句「no version data 0 (handle 0)」就是这么来的），
//    插件根本不会被加载、也不会留下任何日志。
F4SE_PLUGIN_VERSION = []() noexcept {
	F4SE::PluginVersionData v{};
	v.PluginVersion(REL::Version{ 0, 3, 2, 0 });
	v.PluginName("F4LoadText");
	v.AuthorName("F4TC");
	// 元数据：声明“靠什么保持跨版本兼容”，而不是“只支持哪个版本号”。
	//   UsesAddressLibrary → kAddressIndependence_AddressLibrary_1_11_137
	//   HasNoStructUse(false) + IsLayoutDependent → kStructureIndependence_1_11_137Layout
	//   ⭐ 结构无关位：再加 OG/NG 的布局位（本插件不直接读引擎结构体字段，只用函数级 AL ID，
	//      声明这些位能让 F4SE 在更多运行时上放行；实际能不能跑由 AL ID 解析决定）。
	v.UsesSigScanning(false);
	v.UsesAddressLibrary(true);
	v.HasNoStructUse(false);
	v.IsLayoutDependent(true);
	v.addressIndependence |= F4SE::PluginVersionData::kAddressIndependence_AddressLibrary_1_10_980;
	v.structureIndependence |= F4SE::PluginVersionData::kStructureIndependence_1_10_980Layout;
	v.CompatibleVersions({ F4SE::RUNTIME_LATEST });
	v.MinimumRequiredXSEVersion(REL::Version{ 0, 7, 9, 0 });
	return v;
}();

F4SEPluginLoad(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se, F4SE::InitInfo{ .log = true, .logName = "F4LoadText" });

	// ⚠️ **不硬拦版本号**（2026-10-09 改；原先是 `game != RUNTIME_1_11_240 ⇒ 弹窗 + 不加载`）。
	//
	// 理由（与 CommonLibF4RD 的官方指导、以及 FloatingSubtitlesF4 的做法一致）：
	//   * 本插件**全程用地址库**（`REL::ID{…}.address()`），版本映射由它负责；
	//   * 精确版本白名单会让一次普通游戏更新直接失效（还弹个框，比日志难看得多）；
	//   * 正确的语义是 **能力失败**、不是 **版本号失败**：
	//     换版本后如果某个 AL ID 解析不了，那是能力不够，应当在解析处报错，而不是在这里猜。
	//   * F4SE 自己的 `CompatibleVersions` + `addressIndependence/structureIndependence`
	//     元数据已经把“能不能跑”的关。（详见 F4SEPlugin_Version 那段。）
	//
	// ⇒ 这里只**记一行版本**（排查时靠它对齐），然后继续。
	const auto game = REX::FModule::GetExecutingModule().GetFileVersion();
	LOG_INFO("F4LoadText loading: game {}, save folder {}",
		game.string(), F4SE::GetSaveFolderName());

	// ⚠️ 必须读配置（Enable / 名单 / 类别都在 ini 里）。
	//    2026-10-07 事故：这里被误删过一次，结果 ini 完全不生效（Enable 一直默认关）。
	Config::Load();

	// 日志：按 ini 把同步 logger 换成异步（默认开）。放在 Config::Load 之后 ⇒
	// 「config: 用 …」那行及以后都是异步的；之前仅「版本/加载」两行是同步（启动一次性，无影响）。
	if (Config::Get().asyncLog) {
		MakeLoggerAsync();
	}

	Replace::Init();

	// 译文字典（`[汉化]`）：读本仓 translations\*.yaml 建索引（源自 F4TCHook，已入库）。
	// ⚠️ 必须在**读盘之前**完成 —— 三条读盘腿会实时查它。
	(void)Translation::Load();

	if (const auto* messaging = F4SE::GetMessagingInterface(); messaging) {
		messaging->RegisterListener(MessageHandler);
	}

	Hooks::Install();
	Hooks::Report();

	return true;
}
