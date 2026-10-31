#include "PCH.h"

#include "DictionaryCache.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

#include "StrUtil.h"
#include "Paths.h"

// ─────────────────────────────────────────────────────────────────────────────
// 译文字典的**二进制缓存**（`dictionary.cache`）。
//
// 从 Translation.cpp 抽出来的（2026-10-22）：这个格式这两天改了 3 版（v5→v6→v7），
// 而它跟“查表/建键/判定”毫无关系 —— 独立成文件后，改格式只动这一个文件 + 一个版本号。
// ─────────────────────────────────────────────────────────────────────────────

namespace DictionaryCache
{
	// ─────────────────────────────────────────────────────────────────────
	// 二进制缓存（`dictionary.cache`）
	//
	// 为什么要它：yaml-cpp 解析 5 MB yaml 实测 **≈ 950 ms**（Release、UFO4P 一个占 490 ms），
	// 而它在 `F4SEPluginLoad` 里同步跑 ⇒ 直接拖慢游戏启动。缓存把这一步降到“读文件 + 挪指针”量级。
	//
	// 做法（不引入 FlatBuffers，省掉一个外部生成器）：
	//   * 首次：解析 yaml 建表后，把表**紧凑序列化**写 dictionary.cache（含 yaml 清单）
	//   * 后续：只读缓存头的 yaml 清单（name+size+mtime）比对，全等就直接加载缓存
	//
	// ⚠️ 两个硬约束：
	//   ① **缓存只是加速手段**，任何异常（缺失/损坏/版本不符/yaml 变了）一律静默重建，
	//      绝不能因为缓存问题导致译文不加载。
	//   ② 缓存是**产物**，不入库（deploy.py 只清 `*.y*ml`，不会动它）。
	//
	// ⚠️ MO2（USVFS）下的落盘位置（实测，别当故障）：
	//    `汉化目录` 对游戏进程是**虚拟路径**（`<游戏>\Data\F4SE\Plugins\F4LoadText\translations`，
	//    日志里 config 行也印证 ini 走的就是虚拟路径），而 USVFS 会把**写操作**重定向到
	//    `MO2\overwrite\f4se\plugins\F4LoadText\translations\`。**读**也走同一虚拟路径
	//    ⇒ 读写一致、缓存命中正常 ✓。⇒ 用户已在 MO2 里看到这个文件是**预期**的。
	// ─────────────────────────────────────────────────────────────────────

	constexpr char kCacheMagic[4]{ 'F', '4', 'L', 'T' };
	// ⚠️ **改了建表逻辑就必须 +1**（否则旧缓存会被错误复用 —— 缓存里存的是“当时算好的表”，
	//    而指纹只看 yaml，看不见代码变了）。
	//   v1 → v2（2026-10-09）：FULL / WNAM 改为强制单值（修 INNR 变体循环错位）
	//   v2 → v3（2026-10-09）：FULL / WNAM 的**多变体组整组丢弃**（v2 修得不够，
	//                          单值会让所有变体都被写成基础名 —— 见入库处那段长注释）
	//   v3 → v4（2026-10-09）：只丢 FULL（`WNAM` 是真数组，实测一个 INNR 带 72~175 个词根）；
	//                          判据改用 `indexMax`（不再看跨文件的条目数）
	//   v4 → v5（2026-10-09）：覆盖记录的键带**来源 esp**（`|@来源`）—— 同一官方记录被多个
	//                          mod 覆盖时，译文必须按“引擎实际加载/胜出的那份”选
	//   v5 → v6（2026-10-22）：缓存里**一并存下建表统计**（`Translation::Stats`）——
	//                          以前缓存命中时那 6 个计数全是 0，日志看起来像“一条都没跳过”
	//                          ⇒ 日志在说谎（违反项目纪律）。改格式必须 +1。
	//   v6 → v7（2026-10-22）：**丢弃不可信的 INNR `WNAM` 词表组**（`index_max ≤ 1` 或
	//                          所有条目 index 相同）—— 建表逻辑变了，旧缓存必须作废，
	//                          否则被丢掉的条目会从缓存里“复活”（实测后果：武器名
	//                          「尸鬼猎手 尸鬼猎手」/ 词被盖成 `*`）。
	constexpr std::uint32_t kCacheVersion = 7;

	struct YamlStamp   // 一个 yaml 的“指纹”（校验用：名字 + 大小 + 修改时间）
	{
		std::string   name;
		std::uint64_t size{ 0 };
		std::int64_t  mtime{ 0 };
	};

	// 把目录里所有 yaml 按**文件名排序**取指纹（必须与 Load() 的加载顺序同一口径）。
	std::vector<YamlStamp> StampYamls(const std::filesystem::path& a_dir)
	{
		std::vector<YamlStamp> out;
		std::error_code        ec;
		for (const auto& e : std::filesystem::directory_iterator{ a_dir, ec }) {
			if (!e.is_regular_file()) {
				continue;
			}
			const auto ext = Str::LowerAscii(e.path().extension().string());
			if (ext != ".yaml" && ext != ".yml") {
				continue;
			}
			YamlStamp s;
			s.name = e.path().filename().string();
			s.size = static_cast<std::uint64_t>(e.file_size(ec));
			// file_time_type → 系统时钟秒数（转失败置 0 ⇒ 永远不命中，最安全）
			const auto ft = e.last_write_time(ec);
			if (!ec) {
				const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
				s.mtime = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
			}
			out.push_back(std::move(s));
		}
		std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
		return out;
	}

	void PutU32(std::string& b, std::uint32_t v) { b.append(reinterpret_cast<const char*>(&v), 4); }
	void PutU64(std::string& b, std::uint64_t v) { b.append(reinterpret_cast<const char*>(&v), 8); }
	void PutStr(std::string& b, std::string_view s)
	{
		PutU32(b, static_cast<std::uint32_t>(s.size()));
		b.append(s);
	}

	struct Reader
	{
		std::string_view data;
		std::size_t      pos{ 0 };
		bool             ok{ true };

		std::uint32_t U32()
		{
			if (pos + 4 > data.size()) { ok = false; return 0; }
			std::uint32_t v;
			std::memcpy(&v, data.data() + pos, 4);
			pos += 4;
			return v;
		}
		std::uint64_t U64()
		{
			if (pos + 8 > data.size()) { ok = false; return 0; }
			std::uint64_t v;
			std::memcpy(&v, data.data() + pos, 8);
			pos += 8;
			return v;
		}
		std::string_view Str()
		{
			const auto n = U32();
			if (!ok || pos + n > data.size()) { ok = false; return {}; }
			const auto v = data.substr(pos, n);
			pos += n;
			return v;
		}
	};

	// 写缓存（失败只告警，不影响已建好的表）。
	void Save(const std::filesystem::path& a_dir, const Translation::Stats& a_stats,
		const std::unordered_map<std::string, std::string>&              a_single,
		const std::unordered_map<std::string, std::vector<std::string>>& a_multi)
	{
		std::string body;
		body.reserve(1 << 20);
		PutU32(body, static_cast<std::uint32_t>(a_single.size()));
		for (const auto& [k, v] : a_single) {
			PutStr(body, k);
			PutStr(body, v);
		}
		PutU32(body, static_cast<std::uint32_t>(a_multi.size()));
		for (const auto& [k, v] : a_multi) {
			PutStr(body, k);
			PutU32(body, static_cast<std::uint32_t>(v.size()));
			for (const auto& s : v) {
				PutStr(body, s);
			}
		}

		std::string out;
		out.reserve(body.size() + 256);
		out.append(kCacheMagic, 4);
		PutU32(out, kCacheVersion);
		// 指纹由本文件自己取（调用方不需要知道 `YamlStamp` 这个内部类型）
		const auto stamps = StampYamls(a_dir);
		PutU32(out, static_cast<std::uint32_t>(stamps.size()));
		for (const auto& s : stamps) {
			PutStr(out, s.name);
			PutU64(out, s.size);
			PutU64(out, static_cast<std::uint64_t>(s.mtime));
		}
		// v6：建表统计（6 个 u32）—— 只为让“缓存命中”时的日志和解析路径一样可信。
		//   `files` 不入缓存（= 上面的 stamp 个数，读的时候直接用 n）。
		PutU32(out, a_stats.skippedHeader);
		PutU32(out, a_stats.skippedEmpty);
		PutU32(out, a_stats.skippedVariants);
		PutU32(out, a_stats.skippedForeign);
		PutU32(out, a_stats.duplicates);
		PutU32(out, 0);   // 保留位（以后加计数不用再 +1 版本）
		out += body;

		// 原子写：先写 .tmp 再改名（避免下次读到写了一半的缓存）
		//
		// ⚠️ MO2（USVFS）下这个写入会被重定向：`a_dir` 对游戏进程来说是
		//    `<游戏>\Data\F4SE\Plugins\F4LoadText\translations`（虚拟路径），
		//    USVFS 把写操作落到 MO2 的 **overwrite\f4se\plugins\F4LoadText\translations\**。
		//    读也走同一虚拟路径 ⇒ **读写一致**，缓存命中正常（已实测）。
		//    在 MO2 里看到 overwrite 多了个 dictionary.cache 是**预期现象**，不是故障。
		const auto tmp = a_dir / "dictionary.cache.tmp";
		const auto dst = a_dir / "dictionary.cache";
		{
			std::ofstream f{ tmp, std::ios::binary | std::ios::trunc };
			if (!f || !f.write(out.data(), static_cast<std::streamsize>(out.size()))) {
				REX::WARN("translation: cache write failed ({}) -- will be rebuilt next time", Paths::ForLog(tmp));
				return;
			}
		}
		std::error_code ec;
		std::filesystem::rename(tmp, dst, ec);
		if (ec) {
			std::filesystem::remove(dst, ec);
			std::filesystem::rename(tmp, dst, ec);
		}
		if (ec) {
			REX::WARN("translation: cache rename failed ({}) -- will be rebuilt next time: {}", Paths::ForLog(dst), ec.message());
			std::filesystem::remove(tmp, ec);
			return;
		}
		REX::INFO("translation: cache written {} ({} KB; under MO2 this lands in overwrite)", dst.filename().string(), out.size() / 1024);
	}

	// 试读缓存：指纹不符 / 损坏 / 版本不符 → 返回 false（走 yaml 重建）。
	bool TryLoad(const std::filesystem::path& a_dir, Translation::Stats& a_stats,
		std::unordered_map<std::string, std::string>&              a_single,
		std::unordered_map<std::string, std::vector<std::string>>& a_multi)
	{
		std::ifstream f{ a_dir / "dictionary.cache", std::ios::binary | std::ios::ate };
		if (!f) {
			return false;
		}
		const auto size = static_cast<std::size_t>(f.tellg());
		if (size < 16) {
			return false;
		}
		// ⚠️ 同上（EspMastersFrom）：缓存是**产物**，读到个超大/指错的文件只该“静默重建”，
		//    不该拿一个无上限的 `std::string(size)` 去换一次 bad_alloc。
		//    （`tellg()` 失败会返回 -1 ⇒ 转成 size_t 是个天文数字，也在这里被挡下。）
		constexpr std::size_t kMaxCacheBytes = 64u << 20;
		if (size > kMaxCacheBytes) {
			REX::WARN("translation: dictionary.cache size abnormal ({} bytes, limit {}) -- ignoring it and rebuilding", size, kMaxCacheBytes);
			return false;
		}
		std::string buf(size, '\0');
		f.seekg(0, std::ios::beg);
		f.read(buf.data(), static_cast<std::streamsize>(size));
		if (!f) {
			return false;
		}

		if (std::memcmp(buf.data(), kCacheMagic, 4) != 0) {
			return false;
		}
		Reader r{ buf, 4 };
		if (r.U32() != kCacheVersion) {
			return false;
		}
		// 指纹比对：与**当前磁盘上的 yaml** 全等才用（任一文件增删改 ⇒ 重建）
		const auto stamps = StampYamls(a_dir);
		const auto n      = r.U32();
		if (!r.ok || n != stamps.size()) {
			return false;
		}
		for (const auto& s : stamps) {
			const auto name = r.Str();
			const auto sz   = r.U64();
			const auto mt   = r.U64();
			if (!r.ok || name != s.name || sz != s.size || static_cast<std::uint64_t>(s.mtime) != mt) {
				return false;
			}
		}

		// v6：建表统计（写的时候在指纹之后）—— 不读就等于让“缓存命中”的日志说谎。
		a_stats = {};
		a_stats.files          = static_cast<std::uint32_t>(stamps.size());
		a_stats.skippedHeader  = r.U32();
		a_stats.skippedEmpty   = r.U32();
		a_stats.skippedVariants = r.U32();
		a_stats.skippedForeign = r.U32();
		a_stats.duplicates     = r.U32();
		(void)r.U32();   // 保留位
		if (!r.ok) {
			return false;
		}

		// 数据区
		a_single.clear();
		a_multi.clear();
		const auto ns = r.U32();
		if (!r.ok) {
			return false;
		}
		a_single.reserve(ns);
		for (std::uint32_t i = 0; i < ns; ++i) {
			const auto k = r.Str();
			const auto v = r.Str();
			if (!r.ok) {
				return false;
			}
			a_single.emplace(std::string{ k }, std::string{ v });
		}
		const auto nm = r.U32();
		if (!r.ok) {
			return false;
		}
		a_multi.reserve(nm);
		for (std::uint32_t i = 0; i < nm; ++i) {
			const auto k = r.Str();
			const auto c = r.U32();
			if (!r.ok) {
				return false;
			}
			std::vector<std::string> v;
			v.reserve(c);
			for (std::uint32_t j = 0; j < c; ++j) {
				v.emplace_back(r.Str());
			}
			if (!r.ok) {
				return false;
			}
			a_multi.emplace(std::string{ k }, std::move(v));
		}
		return r.ok;
	}
}
