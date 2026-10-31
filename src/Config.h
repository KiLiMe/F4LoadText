#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 节点 2 的配置：<插件目录>\F4LoadText.ini
// 没有 ini / 读不到 = 替换关闭。
namespace Config
{
	// 日志分成**三类**，各自独立开关（原先是两个旋钮 + 一个死的 `日志级别`，已规范化）：
	//   ① 信息（info）：启动 / 名单 / hook 报告 / 收尾汇总，约 10 行。
	//   ② 审计（audit）：真改动的逐条回写；条数上限 + 签名可筛。
	//   ③ 高级（advanced）：诊断 —— 指名追踪 + “判定不出归属”的告警。
	struct ReplaceConfig
	{
		bool        enable{ false };
		// ① 信息
		bool        info{ true };
		// ② 审计：逐条回写（LogAudit）的行数上限，**0 = 关**（默认关）。
		std::uint32_t auditLimit{ 0 };
		// 审计行只对哪些**记录签名**打。逗号分隔，不分大小写；**空 = 全部**，`*` = 同上。
		//   例：审计签名 = TERM, ARMO, MGEF
		std::vector<std::string> auditSignatures;

		// ③ 审计：**文本子串**过滤（逗号分隔，任一命中即打）。空 = 不过滤。
		//
		// 为什么需要它：签名过滤（`INFO`）只能把范围收到「对话记录」，但 UFO4P 这类补丁
		// 一次性覆盖上万条 INFO，200 条上限几秒就打满，想找的那一句永远在墙后面
		// （2026-10-31 实测：`write[String] 188 + write[Translation] 12 = 200 = AuditLimit`，
		// 日志正好被截断，差点据此得出“没写过”的错误结论）。按文本过滤后，没命中的
		// 一条都不占额度，上限就不再是问题。
		std::vector<std::string> auditText;
		// ③ 高级：诊断总闸（默认 false，与其它键一样显式写在 ini 里）。关着时“关注(…)”与
		//    “身份判定失败”**都不打**。
		bool advanced{ false };
		// 指名追踪（在 `高级` 开着的前提下）：命中的记录**一路打全**，且**不占**审计上限、
		// 不受“真改动”门槛与签名白名单限制。
		// 条目形态与 [名单] 相同：`插件名|插件内ID`（十六进制）；只写插件名 = 该插件全部记录。
		//   例：关注记录 = BallisticWeaveNoRailroadAndForNukaWorld.esp|029C10
		std::vector<std::string> focusRecords;
		// 节点 3 名单：**空 = 什么都不改**（README：白名单之外一律走引擎原逻辑）；
		// 要"全改"就写 Entries = *。条目形态见 docs/节点3-名单.md
		std::vector<std::string> whitelist;
		std::vector<std::string> blacklist;

		// ── [汉化] 译文字典（读本仓 translations\*.yaml；源自 F4TCHook，已入库）──
		// 开启后，插件**自有记录**的字段会用 yaml 里那条 `dest` 写回（见 Translation.h）。
		bool        translationEnabled{ true };
		// 目录：相对路径 → 相对 DLL 所在目录（MO2 下就是 mod 文件夹里那份）；
		// 部署时把本仓 translations\*.yaml 拷到这里。
		std::string translationDir{ "F4LoadText\\translations" };
		// 二进制缓存（`dictionary.cache`）：yaml-cpp 解析 5 MB 实测 ≈950 ms，
		// 缓存后降到读文件量级（≈10 ms）。yaml 增删改会自动失效重建。
		// 关掉 = 每次都解析 yaml（调试/排查缓存问题时用）。
		bool translationCache{ true };

		// 异步日志：把 CommonLib 建立的**同步** logger 换成 `spdlog::async_logger`
		// （写日志只入队、落盘由后台线程做）⇒ 即便开了审计/信息，也不会阻塞调用线程、影响帧数。
		// 默认开。关掉回同步（排查异步相关问题/丢日志时用）。
		//   ⚠️ 只影响**本 DLL** 的 spdlog 全局 logger，不动其它 F4SE 插件。
		bool asyncLog{ true };
	};

	void                 Load();
	const ReplaceConfig& Get();

	const std::string& Path();

	// 节点 3：这条记录该不该改写。a_plugin = 插件文件名（不含路径），a_rawID = 插件内原始 ID（低位）
	bool ShouldRewrite(std::string_view a_plugin, std::uint32_t a_rawID);

	// ③ 高级：诊断总闸（`高级`）—— 日志宏（LOG_ADV / LOG_ADV_W）直接读 `Get().advanced`，
	//    不需要专门的取值函数。
	// 诊断（`关注记录`）：这条记录是否被指名追踪（命中就一路打全，不占额度）
	bool IsFocus(std::string_view a_plugin, std::uint32_t a_rawID);
}

// ── 日志三类（与 ReplaceConfig 里那三个开关一一对应）──────────────────────
//
// 用宏而不是函数：`REX::INFO` 是 fmt 风格变参，封成函数就丢掉了编译期格式检查。
// 依赖：调用处已包含 PCH.h（它把 REX/LOG.h 带进来）。
//
//   LOG_INFO   信息：启动 / 名单 / hook 报告 / 收尾汇总（默认开，约 10 行）
//   LOG_ADV    高级：诊断信息（默认关）
//   LOG_ADV_W  高级：诊断告警（默认关；把“我们查不到”这类异常降到这里）
//
// ⚠️ 审计（逐条回写）**不走这三个宏**（它自己跑过滤），但**受「信息」总闸控制**：
//    信息 = false 时一条审计行都不写（`Replace::LogAudit` 第一句就返回）。
// ⚠️ 真故障（配置读不到 / hook 失败 / MH_Init 失败）**不走宏**，始终 REX::ERROR。
#define LOG_INFO(...)  do { if (::Config::Get().info)     { REX::INFO(__VA_ARGS__); } } while (0)
#define LOG_ADV(...)   do { if (::Config::Get().advanced) { REX::INFO(__VA_ARGS__); } } while (0)
#define LOG_ADV_W(...) do { if (::Config::Get().advanced) { REX::WARN(__VA_ARGS__); } } while (0)
