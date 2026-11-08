#include "PCH.h"

#include "Translation.h"

#include "Chunks.h"
#include "Config.h"
#include "DictionaryCache.h"
#include "EspPathResolve.h"
#include "Paths.h"
#include "StrUtil.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "RE/T/TESFile.h"
#include "REX/W32/KERNEL32.h"

// ─────────────────────────────────────────────────────────────────────────────
// 读本仓 translations\*.yaml，建「身份键 → 译文」索引（源自 F4TCHook，2026-10-09 搬入）。
//
// 为什么不用 FlatBuffers：那条路要跑 build_flatbuffer_dict.py（几分钟）+ 重启，
// 而这里直接读文本 —— 改完 yaml 重开游戏就生效。代价是启动时解析一次（实测很快）。
//
// ⚠️ yaml 是**人手编辑**的，解析必须宽容：任何一个文件/条目坏了都不能让整个索引挂掉。
//
// ── 序号策略（这里有个容易踩的坑，所以做了「单值/多值」分离）────────────────
//   yaml 的 `index` = 该字段在这条记录里第几次出现（如 MESG 的 ITXT 有 0..11）。
//   但**单值字段**绝不能靠序号匹配：实测引擎会把同一个 `FULL` 在一条记录上
//   重复读取多次（见 Replace.cpp 的 NextSeq 注释），若严格要求 seq==0，
//   第 2..N 次就匹配不上、字段会被引擎自己的值再盖回去。
//   ⇒ 单值字段：键里**不带序号**（任何一次读取都命中，写的是同一个 dest，幂等 ✓）
//      多值字段：键里**带序号**（否则 ITXT 的 12 个按钮会全被写成同一个）
//   判据：yaml 的 `index_max`（导出脚本填的「该字段共几条」）> 1，或同组出现多个不同 index。
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	std::mutex g_lock;
	// 单值字段：key = plugin|raw|CHUNK        （不带序号）
	std::unordered_map<std::string, std::string> g_single;
	// 多值字段：key = plugin|raw|CHUNK  →  dest 按 yaml 的 index 排列的列表
	//
	// 为什么用列表而不是 `<key>|<seq>` 的散键：
	//   实测引擎会把**同一个字段**在一条记录上重复读取多次（见 Replace.cpp 的 NextSeq 注释：
	//   FULL 被读过 17 次）。多值字段（如 MESG 的 12 个 ITXT）很可能也一样，
	//   那时 seq 会跑到 12、13…… 用散键就对不上了。
	//   存成列表 + 取模回绕 ⇒ 重复读取也能落到正确的那一项 ✓
	std::unordered_map<std::string, std::vector<std::string>> g_multi;
	std::atomic<bool>                                          g_ready{ false };
	Translation::Stats                                          g_stats;

	// ── 热路径预筛（性能关键，别删）─────────────────────────────
	//
	// Lookup 会在**每条记录的每个字段**上被调用（实测引擎一轮读数百万个字段），
	// 而全游戏只有十几个插件有译文。若不做预筛，每次都要
	// `std::format` 建键（堆分配）+ 全局锁 —— 实测量级下会给加载加好几秒。
	//
	// 这里只存 **(插件名哈希, 字段 4CC)** 这一对（插件名取 **FNV-1a 32 位**、小写）：
	// 查表就是一次无分配的字符遍历 + 一个哈希集查询（≈20ns）。
	//
	// ⚠️ 2026-10-22 从“只看插件名”改成“插件名 + 字段” —— 旧版**已经失效**：
	//    日志实测 `查表 176707 次，通过预筛 169731 次（96%）`（文档里记的是 3%）。
	//    原因：`[汉化]` 的 yaml 现在覆盖 22 个插件（含四个官方 esm），而 `a_plugin` 是
	//    **归属插件** —— 读官方记录时必然命中 ⇒ 预筛什么都挡不住，17 万次全走重路径
	//    （`std::format` 建键 + 4 次小写转换 + 全局锁）。
	//    加上字段这一维就准了：查 `FULL` 时不因为“这个插件有 `DESC` 译文”而放行。
	//
	// 碰撞概率：即使撞了也只是多走一次真查（会 miss）—— 绝不会把译文写到错的地方。
	std::unordered_set<std::uint64_t> g_pluginChunk;   // (插件名 FNV-1a) << 32 | chunk 4CC
	std::size_t                       g_pluginCount{ 0 };   // 日志用：不同插件名个数
	std::atomic_uint64_t              g_lookupCalls{ 0 };
	std::atomic_uint64_t              g_lookupPassed{ 0 };   // 通过预筛的（真查了的）
	std::atomic_uint64_t              g_suppressed{ 0 };     // 单值防盖戳护栏跳过的次数（见 LookupChunk）

	// FNV-1a 32（对 ASCII 小写化）；无分配、无分支预测失败。
	std::uint32_t Fnv1a(std::string_view a_s)
	{
		std::uint32_t h = 2166136261u;
		for (const unsigned char raw : a_s) {
			auto c = raw;
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<unsigned char>(c + 32);
			}
			h ^= c;
			h *= 16777619u;
		}
		return h;
	}

	std::uint64_t PrefilterKey(std::string_view a_plugin, std::uint32_t a_chunk4CC)
	{
		return (static_cast<std::uint64_t>(Fnv1a(a_plugin)) << 32) | a_chunk4CC;
	}

	bool HasTranslation(std::string_view a_plugin, std::uint32_t a_chunk4CC)
	{
		return g_pluginChunk.contains(PrefilterKey(a_plugin, a_chunk4CC));
	}

	// 零分配的大小写不敏感比较：统一用 `Str::IEquals`（见 StrUtil.h）

	// 读盘期的序号计数器（**独立于 Replace 那一套**，绝不共用/互相影响）。
	//
	// ⚠️ `thread_local`：保留为**防御性**设计。
	//    原注释说“读盘会并行读多个插件”，但 2026-10-18 实测**否定了这一点**：
	//    加线程探针跑一轮，17.6 万次钩子调用（`Handle*Field`，含 Replace::NextSeq）
	//    全部落在**同一个线程**（见 docs/项目总览.md §8）。
	//    既然实测单线程，共用一份也不会串号；thread_local 只是多花一点 TLS，
	//    留着以防将来引擎版本真的并行（那时它兜底，代价可忽略）。
	// ⚠️ key 用**固定缓冲 + 长度对比**，不用 std::string —— 它在热路径上（见 NextSeq）。
	thread_local char                                   g_curKey[384]{};
	thread_local std::size_t                            g_curKeyLen = 0;
	thread_local std::unordered_map<std::uint32_t, int> g_seq;
	// 单值防盖戳护栏：单值键 → 本次加载里第一次读到的原值。
	// 若同键第 2 次读到**不同**原值 ⇒ 说明它其实是多值字段（老 yaml 只导了 index0）⇒ 停写。
	//
	// ⚠️ **不能随记录切换清空**（2026-10-22 修）：本 map 的键是 `BaseKey`（含 rawID +
	//    chunk4CC + 来源），**本来就不会跨记录串**；而引擎读同一个 INNR 的 WNAM 是**不连续**的
	//    （中间夹着别的记录）⇒ 每次回到它就清空一次 ⇒ “第二次读到不同值就停”形同虚设，
	//    每个 slot 都能被盖一次。实测后果：`ccBGSFO4116` 的 WNAM[8]（值 `*`，意思是“此处用
	//    记录自己的名字”）被盖成 `尸鬼猎手` ⇒ 武器名变成「尸鬼猎手 尸鬼猎手」。
	//    ⇒ 一旦触发就**长期生效**（只在本 DLL 生命周期内）。
	thread_local std::unordered_map<std::string, std::string> g_singleLoaded;

	std::uint32_t ChunkOf(std::string_view a_name)
	{
		std::uint32_t v = 0;
		for (std::size_t i = 0; i < 4 && i < a_name.size(); ++i) {
			v |= static_cast<std::uint32_t>(static_cast<unsigned char>(a_name[i])) << (8 * i);
		}
		return v;
	}

	// 键 = `插件名|低位ID|字段4CC`；**覆盖记录**额外接一段 `|@来源esp`：
	//
	// 为什么覆盖记录要带来源：同一官方记录可能被**多个 mod** 覆盖（实测 CFM 与 UFO4P
	// 有 38 处同键不同译），而引擎**只会加载胜出的那一份 esp** ⇒ 我们只能用
	// **那份 esp 的译文**，否则会出现“引擎给的值是 A 的、我们写回 B 的”错配。
	// ⇒ 键带来源 esp，查表时要求 `来源 == 正在读的那份 esp`（由 MO2 加载顺序决定谁胜）。
	//
	// 自有记录不带（`owner` 就是来源，恒相等）—— 也避免把现有键全部改长。
	std::string BaseKey(std::string_view a_plugin, std::uint32_t a_rawID, std::uint32_t a_chunk4CC,
		std::string_view a_source = {})
	{
		if (a_source.empty()) {
			return std::format("{}|{:06X}|{}", Str::LowerAscii(a_plugin), a_rawID, Chunk::ChunkName(a_chunk4CC));
		}
		return std::format("{}|{:06X}|{}|@{}", Str::LowerAscii(a_plugin), a_rawID, Chunk::ChunkName(a_chunk4CC),
			Str::LowerAscii(a_source));
	}


	// 把 yaml 里的 `path` 变成真正能打开的文件路径。
	//
	// yaml 是**可分发的数据**（发布包里跟着 DLL 一起发），所以里面的 `path` 只写
	// **插件文件名**，不带任何本机目录 —— 早期导出的 yaml 里是导出机的绝对路径
	// （`X:\MO2\mods\...`），既泄露本机目录结构，换台机器又必然指空。
	//
	// 于是按「先具体、后通用」依次尝试：
	//   ① 原样（绝对路径 / 相对于游戏 CWD 的路径）—— 兼容老 yaml 与本地导出场景；
	//   ② `<Data>\<文件名>` —— **发布后的正路**：MO2 的 USVFS 把 mod 的文件映射进
	//      Data，所以哪怕 esp 其实在别的 mod 目录里，这里也读得到（见 Paths.h 的
	//      DllDir/DataDir 语义说明）。
	//   ③ DLL 旁边（`<mod>\F4SE\Plugins\<文件名>`）—— 兼容“把 esp 跟 DLL 放一起”的本地调试。
	//
	// ⚠️ 返回空 = 一个都打不开；调用方据此走宽容分支（全部当自有记录）。
	//    这正是「删掉 path / 只写文件名」的老实现会踩的静默失效（`ownIdx == 0`
	//    ⇒ 覆盖记录的译文全查不到），所以这里必须回退到 Data 目录。
	std::filesystem::path ResolveEspPath(const std::string& a_path)
	{
		if (a_path.empty()) {
			return {};
		}
		// 候选顺序 = EspPath::Candidates（纯函数，tools\selftest 有表驱动用例）
		EspPath::Candidate cand[3];
		const auto         n = EspPath::Candidates(a_path, Paths::DataDir().string(), Paths::DllDir().string(), cand);
		std::error_code    ec;
		for (std::size_t i = 0; i < n; ++i) {
			if (cand[i].path.empty()) {
				continue;
			}
			const std::filesystem::path p{ cand[i].path };
			if (std::filesystem::is_regular_file(p, ec)) {
				return p;
			}
		}
		return {};
	}

	// 读 esp 的 TES4 头，取出它的 **MAST 列表**（master 们，按索引顺序）。
	//
	// 这个列表同时干两件事：
	//   ① `masters.size()` = **本文件自有记录的索引**（自有记录的高位恰好等于 masters 数，
	//      因为它排在自己的 masters 之后）；
	//   ② `masters[hi]` = **覆盖记录（高位 < masters 数）的归属插件名** ——
	//      运行时 `Replace::PluginAndRawIDOf` 判出来的归属就是这个名字，所以译文键必须用它。
	//
	// 读不到（esp 路径不存在 → mod 被卸了）返回空 ⇒ 调用方走宽容分支：
	// 全部当自有（宁可多写自己人的译文，也不要去碰官方记录）。
	std::vector<std::string> EspMastersFrom(const std::string& a_path)
	{
		std::vector<std::string> masters;
		const auto               resolved = ResolveEspPath(a_path);
		if (resolved.empty()) {
			return masters;
		}
		std::ifstream in{ resolved, std::ios::binary };
		if (!in) {
			return masters;
		}
		char head[24]{};
		in.read(head, 24);
		if (in.gcount() != 24 || std::memcmp(head, "TES4", 4) != 0) {
			return masters;
		}
		const auto size = static_cast<std::uint32_t>(static_cast<unsigned char>(head[4])) |
			(static_cast<std::uint32_t>(static_cast<unsigned char>(head[5])) << 8) |
			(static_cast<std::uint32_t>(static_cast<unsigned char>(head[6])) << 16) |
			(static_cast<std::uint32_t>(static_cast<unsigned char>(head[7])) << 24);

		// ⚠️ `size` 是**文件里的字节**，不是我们算出来的：yaml 的 `path` 指错/指到个改过扩展名的
		//    大文件时，这里会直接 `vector<char>(size)` 要到几个 GB 并抛 bad_alloc ——
		//    而本函数**在逐条 yaml 的 try/catch 之外**，又跑在 `F4SEPluginLoad` 里
		//    ⇒ 一抛就是启动崩溃（译文本该是“可有可无的加速项”，不该能崩游戏）。
		//    TES4 头的实测大小是几百字节，8 MB 已经很宽松。
		constexpr std::uint32_t kMaxTes4Bytes = 8u << 20;
		if (size == 0 || size > kMaxTes4Bytes) {
			REX::WARN("translation: abnormal TES4 header length for {} ({} bytes) -- not reading masters (treating as self-owned records)",
				a_path, size);
			return masters;
		}

		try {
			std::vector<char> body(size);
			in.read(body.data(), static_cast<std::streamsize>(size));
			const auto got = static_cast<std::uint32_t>(in.gcount());

			std::uint32_t pos = 0;
			while (pos + 6 <= got) {
				const auto sub = static_cast<std::uint32_t>(static_cast<unsigned char>(body[pos + 4])) |
					(static_cast<std::uint32_t>(static_cast<unsigned char>(body[pos + 5])) << 8);
				if (std::memcmp(body.data() + pos, "MAST", 4) == 0 && pos + 6 + sub <= got) {
					masters.emplace_back(body.data() + pos + 6, sub);   // 含结尾 NUL，比较时统一忽略
				}
				pos += 6 + sub;
			}
		} catch (const std::exception& e) {
			// 分配/读取失败 = 拿不到 masters ⇒ 走调用方的“宽容分支”（全部当自有记录）
			REX::WARN("translation: failed to read TES4 header of {}: {} (treating as self-owned records)", a_path, e.what());
			masters.clear();
		}
		return masters;
	}

	// 一条待入库的候选
	struct Candidate
	{
		std::string base;      // plugin|raw|CHUNK
		std::uint32_t chunk{ 0 };   // 字段 4CC（小端）—— 判“单值/多值”要用
		std::uint32_t record{ 0 };  // 记录签名 4CC（如 INNR）—— 数据卫生规则要用
		std::uint32_t index{ 0 };
		std::uint32_t indexMax{ 0 };
		std::string   dest;
	};
}

std::uint32_t Translation::Load()
{
	g_stats = {};
	g_ready.store(false, std::memory_order_relaxed);

	const auto& cfg = Config::Get();
	if (!cfg.translationEnabled || cfg.translationDir.empty()) {
		return 0;
	}

	std::filesystem::path dir{ cfg.translationDir };
	if (dir.is_relative()) {
		dir = Paths::DllDir() / dir;   // MO2 下 = mod 文件夹里那份
	}
	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec)) {
		REX::WARN("translation: directory not found: {} (translation feature skipped)", Paths::ForLog(dir));
		return 0;
	}

	const auto t_start = std::chrono::steady_clock::now();

	// 把建好的表“发布”：swap 进全局 + 建预筛哈希 + 统计 + 日志。
	// 抽成 lambda：**缓存命中路径**与 yaml 解析路径都要走这一套。
	const auto publish = [&](std::unordered_map<std::string, std::string>&              a_single,
							 std::unordered_map<std::string, std::vector<std::string>>& a_multi,
							 Translation::Stats&                                         a_stats,
							 std::size_t                                                 a_bytesRead,
							 const char*                                                 a_src) {
		// 预筛集合：**(插件名哈希, chunk 4CC)** 对（从刚建好的两张表里取，不留额外状态）
		// 键的形状两形都有：`插件|000ABC|FULL` 与 `插件|000ABC|FULL|@来源.esp`
		//   ⇒ 第 1 个 `|` 之前是插件名，第 2 个 `|` 之后就是那 4 个字节的 chunk
		{
			std::unordered_set<std::uint64_t> pairs;
			std::unordered_set<std::uint32_t>  plugins;
			const auto                        add = [&](std::string_view key) {
				const auto b1 = key.find('|');
				if (b1 == std::string_view::npos) {
					return;
				}
				const auto b2 = key.find('|', b1 + 1);
				if (b2 == std::string_view::npos || b2 + 5 > key.size()) {
					return;
				}
				const auto    hash = Fnv1a(key.substr(0, b1));
				std::uint32_t chunk = 0;
				std::memcpy(&chunk, key.data() + b2 + 1, 4);   // 与 ChunkOf() 同一字节序
				plugins.insert(hash);
				pairs.insert((static_cast<std::uint64_t>(hash) << 32) | chunk);
			};
			for (const auto& kv : a_single) {
				add(kv.first);
			}
			for (const auto& kv : a_multi) {
				add(kv.first);
			}
			std::scoped_lock l{ g_lock };
			g_single.swap(a_single);
			g_multi.swap(a_multi);
			g_pluginChunk.swap(pairs);
			g_pluginCount = plugins.size();
		}
		a_stats.entries = static_cast<std::uint32_t>(g_single.size());
		for (const auto& [_, v] : g_multi) {
			for (const auto& s : v) {
				if (!s.empty()) {
					++a_stats.entries;
				}
			}
		}
		g_stats = a_stats;
		g_ready.store(true, std::memory_order_relaxed);

		LOG_INFO("translation: single={} multi={} (source={}; files={}; skipped header={} empty={} variant-group={} foreign={}; duplicate-key={}) plugins={} / (plugin,chunk)={} groups, prefilted by name+chunk",
			g_single.size(), g_multi.size(), a_src, a_stats.files, a_stats.skippedHeader,
			a_stats.skippedEmpty, a_stats.skippedVariants, a_stats.skippedForeign, a_stats.duplicates,
			g_pluginCount, g_pluginChunk.size());
	};

	// ── ① 先试二进制缓存（命中则直接把 yaml 解析的 ≈950 ms 省掉）──
	if (cfg.translationCache) {
		std::unordered_map<std::string, std::string>              cSingle;
		std::unordered_map<std::string, std::vector<std::string>> cMulti;
		// ⚠️ 统计（`files` 与 5 个跳过计数）也一起从缓存里恢复 ——
		//    否则缓存命中时日志里会打 `跳过 头=0 无译文=0 变体组=0 …`，看起来像
		//    “yaml 变干净了”，其实是没统计（项目纪律：日志必须可信）。
		Translation::Stats cs{};
		if (DictionaryCache::TryLoad(dir, cs, cSingle, cMulti)) {
			publish(cSingle, cMulti, cs, 0, "缓存");
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
								std::chrono::steady_clock::now() - t_start)
								.count();
			LOG_INFO("translation: perf cache hit, load took={} ms  entries={}", ms, cs.entries);
			return cs.entries;
		}
	}

	std::vector<Candidate> cand;
	Translation::Stats     stats;
	std::size_t            bytes_read = 0;

	// ⚠️ 按**文件名排序**后再加载：不同 yaml 可能给同一个键（如两个 mod 都覆盖
	//    `Fallout4.esm` 的同一字段）不同译文 —— 目录遍历顺序是文件系统定的、不可控，
	//    不排序的话“哪个胜”会随机器变化（且难复现）。
	//    排序后 = 字典序**靠后**的文件胜（`insert_or_assign` 后写覆盖先写）。
	std::vector<std::filesystem::directory_entry> files;
	for (const auto& entry : std::filesystem::directory_iterator{ dir, ec }) {
		if (entry.is_regular_file()) {
			files.push_back(entry);
		}
	}
	std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
		return a.path().filename().string() < b.path().filename().string();
	});

	// 同一个 esp 出现两份 yaml = 「换新版译文忘了删旧文件」。两份**都加载**（文件名靠后的
	// 逐键覆盖），这里只记名字、打一行 WARN —— **运行时不做丢弃/排序决策**：它不知道 esp 的
	// 实际版本，选“哪份对”要的信息只有离线工具才有（`tools\deploy.py` 的 lint 按
	// `generated` 点名该删哪份）。判重复用 pass 2 已经解析出来的 `esp`，零额外 I/O。
	std::unordered_set<std::string> seenPlugins;   // 小写 esp

	for (const auto& entry : files) {
		if (!entry.is_regular_file()) {
			continue;
		}
		const auto ext = Str::LowerAscii(entry.path().extension().string());
		if (ext != ".yaml" && ext != ".yml") {
			continue;
		}

		YAML::Node root;
		try {
			root = YAML::LoadFile(entry.path().string());
		} catch (const std::exception& e) {
			REX::WARN("translation: failed to parse {}: {} (file skipped)", entry.path().filename().string(), e.what());
			continue;
		}
		std::error_code szec;
		bytes_read += static_cast<std::size_t>(std::filesystem::file_size(entry.path(), szec));
		if (!root.IsMap()) {
			continue;
		}
		const auto pluginNode = root["esp"];
		if (!pluginNode || !pluginNode.IsScalar()) {
			continue;
		}
		const auto  plugin  = pluginNode.as<std::string>();
		if (!plugin.empty() && !seenPlugins.insert(Str::LowerAscii(plugin)).second) {
			REX::WARN("translation: '{}' is declared by more than one yaml (both loaded; delete the stale one)", plugin);
		}
		const bool  isLight = root["is_light"] && root["is_light"].as<bool>(false);
		// `path` 约定为**插件文件名**（发布包里的 yaml 不带本机目录）；
		// 老 yaml 里的绝对路径也认（EspMastersFrom 内部会依次回退到 <Data>\、DLL 旁）。
		// 完全没有 `path` 时退到 `esp` 字段 —— 它本来就写着文件名。
		std::string path = root["path"] ? root["path"].as<std::string>("") : std::string{};
		if (path.empty() && !plugin.empty()) {
			path = plugin;
		}
		// masters 列表：[0..n) = 覆盖记录的归属插件名；n = 「自有记录」的高位。
		// （空 = esp 读不到 → 宽容分支，全部当自有）
		const auto  masters = path.empty() ? std::vector<std::string>{} : EspMastersFrom(path);
		const auto  ownIdx  = static_cast<std::uint32_t>(masters.size());
		const auto  entries = root["entries"];
		if (!entries || !entries.IsSequence()) {
			continue;
		}
		++stats.files;

		for (const auto& e : entries) {
			try {
				if (!e.IsMap()) {
					continue;
				}
				if (e["is_header"] && e["is_header"].as<bool>(false)) {
					++stats.skippedHeader;   // TES4 头（作者名/版本史）：不是游戏内文本
					continue;
				}
				const auto dest = e["dest"] ? e["dest"].as<std::string>("") : std::string{};
				const auto text = e["text"] ? e["text"].as<std::string>("") : std::string{};
				if (dest.empty() || dest == text) {
					++stats.skippedEmpty;   // 没译文 / 与原文相同
					continue;
				}
				const auto fieldName = e["field"] ? e["field"].as<std::string>("") : std::string{};
				if (fieldName.size() != 4) {
					continue;
				}
				// 记录签名 4CC（如 "INNR"）—— 下面入库时的数据卫生规则要用（见候选循环）。
				const auto typeName = e["type"] ? e["type"].as<std::string>("") : std::string{};
				const auto formID = e["form_id"] ? e["form_id"].as<std::uint32_t>(0) : 0u;
				// 归属插件名：
				//   * 高位 == masters 数（或 masters 未知）⇒ 本文件自有记录 ⇒ 用 yaml 顶上的 `esp`
				//   * 高位 < masters 数 ⇒ **覆盖记录** ⇒ 归属 = masters[hi]（如 Fallout4.esm）
				//     ⚠️ 运行时 `Replace::PluginAndRawIDOf` 判出来的归属就是这个名字，
				//        键必须用它才能命中（旧版这里直接 `continue` 丢掉，导致覆盖记录永远查不到译文）。
				//   * 高位 > masters 数或反查不到 ⇒ 不可信，丢弃
				std::string owner = plugin;
				const auto  hi    = (formID >> 24) & 0xFFu;
				if (ownIdx != 0 && hi != ownIdx) {
					if (hi >= ownIdx) {
						++stats.skippedForeign;   // 高位超出本文件 masters（stale / 手改错）
						continue;
					}
					owner = masters[hi];
					while (!owner.empty() && (owner.back() == '\0' || owner.back() == ' ')) {
						owner.pop_back();
					}
					if (owner.empty()) {
						++stats.skippedForeign;
						continue;
					}
				}
				const auto rawID = isLight ? (formID & 0xFFFu) : (formID & 0xFFFFFFu);
				const auto chunk = ChunkOf(fieldName);
				//   覆盖记录（`owner != plugin`）带上来源 esp：同一条官方记录可能被多个 mod
				//   覆盖，只有“正在读的那份 esp”的译文才有意义（引擎只会加载胜出的那份）。
				// ⚠️ **大小写不敏感**比：运行时 `LookupChunk` 判 isOverride 用的就是 `Str::IEquals`
				//    （2026-10-22 统一）。两边的“是不是覆盖”必须是同一个结论，否则键会多/少
				//    一段 `|@来源` ⇒ 译文静默查不到。
				const bool isOverride = !Str::IEquals(owner, plugin);
				cand.push_back(Candidate{
					.base = isOverride ? BaseKey(owner, rawID, chunk, plugin) : BaseKey(owner, rawID, chunk),
					.chunk = chunk,
					.record = typeName.size() == 4 ? ChunkOf(typeName) : 0u,
					.index = e["index"] ? e["index"].as<std::uint32_t>(0) : 0u,
					.indexMax = e["index_max"] ? e["index_max"].as<std::uint32_t>(0) : 0u,
					.dest = dest,
				});
			} catch (const std::exception& e2) {
				REX::WARN("translation: one entry failed to parse: {} (skipped)", e2.what());
			}
		}
	}

	// ── 判单值/多值，然后入库 ──
	// 多值 = index_max > 1（导出脚本填的「该字段共几条」），或同组出现多个不同 index。
	//
	// ⚠️ **`FULL` 的多变体组：整组丢弃**（2026-10-09 修，实测两轮踩过）。
	//
	// 背景：ARMO / WEAP 记录可以带**多个** FULL（INNR 变体：基础名 + `Default` + `Light` + …），
	// 而引擎是**按需逐次重启**它（读 N 次填 N 个变体），**没有稳定的可写位置**：
	//   * 按多值存（index=seq）⇒ 与引擎读取顺序对不上 ⇒ **循环错位** ✗
	//     （实测：文本='避难所科技保安护甲'→写'避难所科技保安护甲'；文本='预设'→写'默认' ✗）
	//   * 按单值存（任何 seq 都命中）⇒ 全部变体都被写成基础名 ✗
	//     （实测：文本='预设'/'轻型'/'中型' … 均被写成'合成人护甲胸甲' ✗）
	//   * 用 `chunkoffset` 区分 ⇒ **也不可靠**（探针实测同一个 chunkoffset 连续对应多个值：
	//     0X7 依次对应'默认'/'轻型'/'中型'；且本地化 esp 的 chunkoffset 恒为 0X4 ✗）
	//
	// ⇒ **这些变体名本就由引擎/汉化 esp 自己给（探针里 read 值已经是中文），我们一写就错。**
	//    单 FULL 的记录（绝大多数普通物品）不受影响，正常回写 ✓。
	//
	// ⚠️ `WNAM` **不要**跟着丢：它在 INNR 记录上是**真数组**（实测一个 INNR 带 72~175 个
	//    WNAM 词根，引擎按下标读）⇒ 走下面的多值路径才对。
	//    （其他多 index 字段 —— ITXT/UNAM/NNAM/CNAM/BTXT —— 同理，都是真数组。）
	std::unordered_map<std::string, std::uint32_t> groupMax;        // base → index_max
	std::unordered_map<std::string, std::uint32_t> groupDistinct;   // base → 不同 index 个数
	{
		std::unordered_map<std::string, std::unordered_set<std::uint32_t>> seen;
		for (const auto& c : cand) {
			auto& m = groupMax[c.base];
			m = std::max(m, c.indexMax);
			seen[c.base].insert(c.index);
		}
		for (const auto& [base, set] : seen) {
			groupDistinct[base] = static_cast<std::uint32_t>(set.size());
		}
	}

	std::unordered_map<std::string, std::string>              single;
	std::unordered_map<std::string, std::vector<std::string>> multi;
	for (auto& c : cand) {
		// ── INNR 的 `WNAM` 词表：**只接受自洽的多值组**（2026-10-22 实测定案）──────
		//
		// `WNAM` 是 INNR 的**命名词数组**：实测 `ccBGSFO4116` 的 INNR 有 **9** 个、
		// `dn_CommonGun` 有 **175** 个。而 F4TCHook 时代导出的 CC yaml 把它们导成了
		// `index: 0 / index_max: 1`（只 1 条）⇒ 被判成**单值** ⇒ 那一个词会被盖到
		// **每一次** WNAM 读取上（单值防盖戳护栏只能挡住第一次之后）。
		//   实测后果：`ccBGSFO4116` 的 WNAM[8]（值 `*`，意思是“此处用记录自己的名字”）
		//   被写成 `尸鬼猎手` ⇒ 武器名变成「尸鬼猎手 尸鬼猎手」（应为「尸鬼猎手 喷火器」）。
		//
		// 另一类同样不可信：`ccOTMFO4001-Remnants` 的 74 条**全挤在 index 0**
		//   （`index_max: 74`）⇒ `slot[0]` 被连续覆盖 74 次、最后一条（`*`）胜出
		//   ⇒ 实测把 `支撑` 盖成了 `*`。
		//
		// ⇒ 两种一律**整组丢弃**（宁可那几个词保持引擎/汉化 esp 自己的译法）：
		//   ① `index_max ≤ 1`：单词表不存在
		//   ② 所有条目 index 相同（`groupDistinct ≤ 1`）：与 `index_max` 自相矛盾
		// ⚠️ 只针对 `WNAM`；`UNAM` 是**单个**分隔符字符串（真实值如 `+`），单值是对的。
		if (c.record == Chunk::kInnr && c.chunk == Chunk::kWnam) {
			if (groupMax[c.base] <= 1 || groupDistinct[c.base] <= 1) {
				++stats.skippedVariants;   // 与 FULL 变体组同口径：整组丢弃
				continue;
			}
		}

		// FULL：只有“整条记录只有一个 FULL”时才写（多变体 INNR 组整组丢弃，理由见上）。
		// ⚠️ 判据用 `indexMax`（= 导出脚本填的「该字段共几条」，即文件里真的有几个此 chunk），
		//    不能用 `groupCount`（同键条目数）—— 后者会把**跨 yaml 文件**的重复条目也算进来，
		//    导致普通单变体记录被误丢。（实测：只用 groupCount 会多丢 90 组。）
		if (c.chunk == Chunk::kFull) {
			if (groupMax[c.base] > 1) {
				++stats.skippedVariants;   // INNR 变体组：交给引擎/汉化 esp 自己
				continue;
			}
			single.insert_or_assign(c.base, std::move(c.dest));
			continue;
		}
		const bool isMulti = groupMax[c.base] > 1 || groupDistinct[c.base] > 1;
		if (!isMulti) {
			single.insert_or_assign(c.base, std::move(c.dest));
			continue;
		}
		// 列表长度取 **index_max**（该字段共几条），而不是“最大已填 index + 1” ——
		// 后者会让多值字段的取模回绕错位（如 index_max=12 但只填了 0/3/7 ⇒ 长度 8，
		// 引擎第 8 次读取就会被回绕到 slot 0）。取 index_max 才能让 seq 与 slot 一一对应。
		auto& slot = multi[c.base];
		const auto want = std::max<std::size_t>(static_cast<std::size_t>(c.index) + 1,
			static_cast<std::size_t>(groupMax[c.base]));
		if (slot.size() < want) {
			slot.resize(want);
		}
		if (!slot[c.index].empty()) {
			++stats.duplicates;
		}
		slot[c.index] = std::move(c.dest);
	}

	// ── ② 写缓存（下次启动就能走快路径）；失败不影响本次 ──
	if (cfg.translationCache) {
		DictionaryCache::Save(dir, stats, single, multi);
	}

	publish(single, multi, stats, bytes_read, "yaml");

	// ── 性能行（启动开销 / 内存占用；优化前后对比就看它）──
	{
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
							std::chrono::steady_clock::now() - t_start)
							.count();
		// 键/译文都住在两张 unordered_map 里；估算 = 键字节 + 译文字节 + 容器开销（节点 ~48B/条）
		std::size_t keyBytes = 0;
		std::size_t valBytes = 0;
		std::size_t count    = 0;
		for (const auto& [k, v] : g_single) {
			keyBytes += k.size();
			valBytes += v.size();
			++count;
		}
		for (const auto& [k, v] : g_multi) {
			keyBytes += k.size();
			for (const auto& s : v) {
				valBytes += s.size() + 8;
			}
			++count;
		}
		const auto approx = keyBytes + valBytes + count * 48;
		LOG_INFO("translation: perf parse took={} ms  read={} KB  entries={}  est. memory={} KB",
			ms, bytes_read / 1024, stats.entries, approx / 1024);
	}
	return stats.entries;
}

bool Translation::Ready()
{
	return g_ready.load(std::memory_order_relaxed);
}

int Translation::NextSeq(const RE::TESFile* a_file, std::uint32_t a_chunk4CC, std::string_view a_plugin,
	std::uint32_t a_rawID)
{
	// 换记录（或换读取文件）→ 计数清零。与 Replace::NextSeq 同一套重置规则，但**各算各的**。
	//
	// ⚠️ 这里手写拼接而不用 snprintf：热路径上，snprintf 解析格式串比拼接贵好几倍。
	//    上限 384 = 插件名（实测最长 40 余字） + '|' + 插件名 + '|' + 8 位 hex。
	char        key[384]{};
	std::size_t n = 0;
	if (a_file) {
		for (const char* fn = a_file->filename; *fn && n < sizeof(key) - 24; ++fn) {
			key[n++] = *fn;
		}
	}
	key[n++] = '|';
	{
		const auto pn = std::min<std::size_t>(a_plugin.size(), 200);
		std::memcpy(key + n, a_plugin.data(), pn);
		n += pn;
	}
	key[n++] = '|';
	{
		static constexpr char hex[] = "0123456789ABCDEF";
		for (int shift = 28; shift >= 0; shift -= 4) {
			key[n++] = hex[(a_rawID >> shift) & 0xFu];
		}
	}
	key[n] = '\0';

	if (g_curKeyLen != n || std::memcmp(g_curKey, key, n) != 0) {
		std::memcpy(g_curKey, key, n + 1);
		g_curKeyLen = n;
		g_seq.clear();
		// ⚠️ **不要在这里 `g_singleLoaded.clear()`** —— 理由见它的声明处（护栏必须跨记录长期生效）。
	}
	auto& seq = g_seq[a_chunk4CC];
	if (seq >= Chunk::kMaxSeqPerChunk) {
		return -1;
	}
	return seq++;
}

bool Translation::HasChunk(std::string_view a_plugin, std::uint32_t a_chunk4CC)
{
	return g_ready.load(std::memory_order_relaxed) && HasTranslation(a_plugin, a_chunk4CC);
}

std::string_view Translation::Lookup(std::string_view a_plugin, std::uint32_t a_rawID,
	std::uint32_t a_chunk4CC, int a_seq)
{
	if (!g_ready.load(std::memory_order_relaxed)) {
		return {};
	}
	// ── 热路径预筛（性能关键，别删）──────────────────────────────────
	// 每次查表前先看“这个插件有没有译文”：一次 FNV-1a + 一次哈希集查询，
	// **无堆分配、无锁**。全游戏只有十几个插件有译文 → 砍掉 99.9% 的调用，
	// 保住了下面那段 std::format + 全局锁。
	g_lookupCalls.fetch_add(1, std::memory_order_relaxed);
	if (!HasTranslation(a_plugin, a_chunk4CC)) {
		return {};
	}
	g_lookupPassed.fetch_add(1, std::memory_order_relaxed);

	const auto base = BaseKey(a_plugin, a_rawID, a_chunk4CC);
	std::scoped_lock l{ g_lock };
	if (const auto it = g_multi.find(base); it != g_multi.end() && !it->second.empty()) {
		// 取模回绕：引擎会重复读同一字段（seq 会超出列表长度），回绕到正确的那一项 ✓
		const auto idx = static_cast<std::size_t>(a_seq < 0 ? 0 : a_seq) % it->second.size();
		return it->second[idx];   // 可能是空（yaml 里缺这个 index）⇒ 调用方视为未命中
	}
	const auto it = g_single.find(base);
	return it == g_single.end() ? std::string_view{} : std::string_view{ it->second };
}

Translation::Perf Translation::PerfSnapshot()
{
	return Perf{ g_lookupCalls.load(std::memory_order_relaxed),
		g_lookupPassed.load(std::memory_order_relaxed) };
}

std::string_view Translation::LookupFrom(std::string_view a_owner, std::string_view a_source,
	std::uint32_t a_rawID, std::uint32_t a_chunk4CC, int a_seq)
{
	if (!g_ready.load(std::memory_order_relaxed) || a_owner.empty() || a_source.empty()) {
		return {};
	}
	g_lookupCalls.fetch_add(1, std::memory_order_relaxed);
	if (!HasTranslation(a_owner, a_chunk4CC)) {   // 预筛看的是**归属插件名**（与 Lookup 一致）
		return {};
	}
	g_lookupPassed.fetch_add(1, std::memory_order_relaxed);
	const auto base = BaseKey(a_owner, a_rawID, a_chunk4CC, a_source);
	std::scoped_lock l{ g_lock };
	if (const auto it = g_multi.find(base); it != g_multi.end() && !it->second.empty()) {
		const auto idx = static_cast<std::size_t>(a_seq < 0 ? 0 : a_seq) % it->second.size();
		return it->second[idx];
	}
	const auto it = g_single.find(base);
	return it == g_single.end() ? std::string_view{} : std::string_view{ it->second };
}

std::string_view Translation::LookupChunk(const RE::TESFile* a_file, std::string_view a_plugin,
	std::uint32_t a_rawID, std::uint32_t a_chunk4CC, std::string_view a_loaded)
{
	const auto seq = NextSeq(a_file, a_chunk4CC, a_plugin, a_rawID);
	if (!g_ready.load(std::memory_order_relaxed) || a_plugin.empty()) {
		return {};
	}
	// 热路径预筛（与 Lookup / LookupFrom 同一道；这里内联以免多算一次计数）。
	g_lookupCalls.fetch_add(1, std::memory_order_relaxed);
	if (!HasTranslation(a_plugin, a_chunk4CC)) {
		return {};
	}
	g_lookupPassed.fetch_add(1, std::memory_order_relaxed);

	// 覆盖记录（归属插件 != 正在读的 esp）⇒ 键必须带来源 esp（见 BaseKey 注释）：
	// 同一条官方记录可能被多个 mod 覆盖，**只有正在读的那份 esp 的译文才有意义**。
	const auto reader = a_file && a_file->filename ? std::string_view{ a_file->filename } : std::string_view{};
	const bool isOverride = !reader.empty() && !Str::IEquals(a_plugin, reader);
	const auto base = isOverride
	                      ? BaseKey(a_plugin, a_rawID, a_chunk4CC, reader)
	                      : BaseKey(a_plugin, a_rawID, a_chunk4CC);

	std::scoped_lock l{ g_lock };

	// 多值字段：取模回绕（yaml 正确导出的那些走这里，不受护栏影响）。
	if (const auto it = g_multi.find(base); it != g_multi.end() && !it->second.empty()) {
		const auto idx = static_cast<std::size_t>(seq < 0 ? 0 : seq) % it->second.size();
		return it->second[idx];
	}

	const auto it = g_single.find(base);
	if (it == g_single.end()) {
		return {};
	}

	// ── 单值防盖戳护栏（2026-10-18 加）──────────────────────────────────
	//
	// 背景（实测定案）：老 F4TCHook 的 CC yaml 对**多值字段**（TERM 的 ITXT/UNAM/BTXT、
	//   OMOD/ARMO 的多变体 FULL…）只导了一条 `index:0 / index_max:1` ⇒ 被判为**单值**
	//   ⇒ 同一个 dest 被盖到该字段的**每一次**读取。实测一轮 300 条回写里 275 条是这种盖戳：
	//     `手指虎/护腕/液压/瞄准…` 122 个不同值全 → `炼狱`。
	//
	// 判据：单值键在**同一条记录的同一次加载**里若读到**第 2 个不同原值**（`a_loaded`）
	//   ⇒ 它根本不是单值字段 ⇒ **停写**，把引擎自己的值留给后面的出现。
	//   · 真·单值字段：引擎重读同一子记录，`a_loaded` 每次相同 ⇒ 不受影响 ✓
	//   · 过时单值条目：第 2 次读到不同值 ⇒ 停写，只保留 index0 那一次（与“正确多值结构只填 index0”等价）✓
	// ⚠️ 不能改用 `seq` 做判据 —— 真·单值字段也会被引擎重复读多次（FULL 实测 17 次），
	//    用 seq 会把它们误伤（§5 第 18 条的前车之鉴）。这里用**值**，不用序号。
	if (const auto fit = g_singleLoaded.find(base); fit == g_singleLoaded.end()) {
		g_singleLoaded.emplace(base, std::string{ a_loaded });
	} else if (fit->second != a_loaded) {
		g_suppressed.fetch_add(1, std::memory_order_relaxed);
		return {};
	}
	return it->second;
}

Translation::Stats Translation::Snapshot()
{
	return g_stats;
}

std::uint64_t Translation::SuppressedCount()
{
	return g_suppressed.load(std::memory_order_relaxed);
}
