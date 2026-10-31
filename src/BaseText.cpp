#include "PCH.h"

#include "BaseText.h"

#include "Config.h"
#include "Paths.h"   // LOG_INFO（建索引的量体重日志走 [调试] 信息 开关）

#include "REX/W32/KERNEL32.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// 为什么需要它：有些被覆盖的记录，引擎**只解析胜出的覆盖**，基表那份文本从未进内存
//（实测 INFO 0x000CE9F6：两条读取路径都只有 mod ✓，Fallout4.esm 一次都没读 ✗）。
// 这时"记住基表文本"无从记起，只能我们自己把基表那份读出来。
//
// 做法（2026-10-07 实测定稿；2026-10-22 改成“只记位置、chunk 按需读”）：
//   ① 对基表插件做**一次**遍历（4 MB 顺序缓冲 ✓），**只记**「formID → 记录体偏移/长度」
//      （12 B/记录；实测 Fallout4.esm ≈ 17 MB、百毫秒级）
//      ⚠️ 绝不能按记录重扫：315 MB 的 esm × 上千条 ⇒ 加载界面卡死 ✗（踩过）
//      ⚠️ 也**不要**再回到“把每个 ≤64 字节的 chunk 都拷进内存”：实测 593 万个条目 ⇒ 622 MB ✗
//         （2026-10-22 日志：`BaseText: 索引 …`；四个 DLC 加起 861 MB、建索引 1.6 秒）
//      真要文本时：二分找记录 → `fseek` 读那一条（≤4096 字节）→ 在记录体里现扫 chunk ✓
//   ② 记录的 chunk 是 4 字节 → 那是**字符串表 id**
//   ③ 查**松散字符串表** `<游戏>\Data\Strings\<插件基名>_<语言>.ILSTRINGS`
//      （表布局：count@0 → 条目@8 `(id, offset)`（id 升序）→ blob = 8+count*8 → 串 = u32 长度 + 字节 ✓）
//   ④ 拿到文本 → 调用方写回字段 ✓
//
// 本文件**不装任何钩子**，只是个查表工具（懒加载：某条记录其真要取文本时才扫那一个插件）。
// 两个调用方：
//   * 读盘期：INFO 字幕兜底（引擎没解析基表那份时）—— Replace.cpp
//   * 显示层：DESC 取中文 —— Hooks.cpp 的 getDescriptionThunk
//     ⇒ “不碰显示层”在这里不成立：**逻辑**不碰，但**被显示层调用** ✓
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	std::mutex g_lock;

	struct PluginIndex;   // 定义见下面（凑近 RecordEntry 的注释）
	PluginIndex* EnsureIndex(const std::string& a_plugin);

	std::uint32_t U32(const std::uint8_t* a_p)
	{
		return static_cast<std::uint32_t>(a_p[0]) | (static_cast<std::uint32_t>(a_p[1]) << 8) |
		       (static_cast<std::uint32_t>(a_p[2]) << 16) | (static_cast<std::uint32_t>(a_p[3]) << 24);
	}

	// ── 顺序读取器（只向前）：4 MB 缓冲 ──
	// 逐 24 字节 fread/fseek 会让加载卡死（315 MB、上百万条记录 ✗）；整个 GRUP 走法是前向的 ✓
	class ForwardReader
	{
	public:
		explicit ForwardReader(std::FILE* a_file) : _file(a_file) { _buf.resize(kBufSize); }
		bool Read(void* a_out, std::size_t a_n)
		{
			auto* out = static_cast<std::uint8_t*>(a_out);
			while (a_n) {
				if (_pos == _len && !Refill()) return false;
				const auto take = std::min(a_n, _len - _pos);
				std::memcpy(out, _buf.data() + _pos, take);
				_pos += take; out += take; a_n -= take;
			}
			return true;
		}
		bool Skip(std::uint64_t a_n)
		{
			while (a_n) {
				if (_pos == _len && !Refill()) return false;
				const auto take = std::min<std::uint64_t>(a_n, _len - _pos);
				_pos += static_cast<std::size_t>(take); a_n -= take;
			}
			return true;
		}
		[[nodiscard]] std::uint64_t Pos() const { return _base + _pos; }
	private:
		bool Refill()
		{
			_base += _pos;
			const auto left = _len - _pos;
			if (left) std::memmove(_buf.data(), _buf.data() + _pos, left);
			_pos = 0;
			const auto got = std::fread(_buf.data() + left, 1, _buf.size() - left, _file);
			_len = left + got;
			return got > 0;
		}
		static constexpr std::size_t kBufSize = 4u << 20;
		std::FILE*                _file{ nullptr };
		std::vector<std::uint8_t> _buf;
		std::size_t               _pos{ 0 }, _len{ 0 };
		std::uint64_t             _base{ 0 };
	};

	// ── ① 每插件的索引：formID → { chunk 4CC → 原始字节 } ──
	// ── ① 每插件索引：**只记「记录在文件里的位置」**，chunk 按需读 ──────────
	//
	// ⚠️ 为什么不把 chunk 拷进内存了（2026-10-22 用真实日志定的案）：
	//    旧结构 `map<formID, vector<ChunkBytes>>` 把**每个 ≤64 字节的 chunk 深拷贝一份**，
	//    实测 Fallout4.esm 进表 149.2 万条记录 / **593 万** 个 chunk 条目，而 payload 只有 60 MB
	//    ⇒ 容器 + 「每条一个独立堆块」把它放大到 **约 622 MB**；4 个 DLC 加起 **约 861 MB**，
	//    建索引 **1.6 秒**，全部发生在**进世界那一刻**（日志：`BaseText: 索引 …`）。
	//    而我们只问**极少数几个 chunk**（`DESC` / INFO 的 `NAM1` …）⇒ 99% 的索引是白建的。
	//    ⇒ 改成「formID → 记录体偏移」的**扁平排序数组**（12 B/记录 ≈ 17 MB），
	//      真要文本时 `fseek` 读那一条记录、在记录体里现扫 chunk。
	//      代价 = 每次**首次**请求多一次 seek + 读（≤4096 字节）；外层 `Get` 另有文本级缓存，
	//      实测请求数极少（DESC 一轮几百次）⇒ 可忽略。
	struct RecordEntry
	{
		// ⚠️ **插件内原始 ID**（高位已去掉），不是文件里原样的 formID。
		//    自有记录的存储高位 = **本插件的编译索引**（`Fallout4.esm`=0x00、
		//    `DLCCoast.esm`=0x01、`cc*.esl`=0x01，实测），而所有调用方传进来的都是
		//    “去高位”的原始 ID ⇒ 不掩高位就**永远查不到**（只有 Fallout4.esm 碰巧高位=0）。
		//    2026-10-22 修：这正好解释了为什么每一轮日志都是 `自取基表=0`。
		std::uint32_t key{ 0 };
		std::uint32_t offset{ 0 };   // 记录体（24 字节记录头之后）的**绝对**文件偏移
		std::uint32_t size{ 0 };     // 记录体长度
	};
	static_assert(sizeof(RecordEntry) == 12, "RecordEntry 要紧凑：这张表就是内存占用本身");

	struct PluginIndex
	{
		std::vector<RecordEntry> records;   // 按 (formID, offset) 升序 —— 二分用
	};

	// ⚠️ 值用 `unique_ptr`：`unordered_map` 扩容会让**元素地址失效**，而下面会把 `PluginIndex*`
	//    带出 EnsureIndex 再用（同一把 g_lock 内不会有并发插入，但别依赖这个前提）。
	//    有键 = 已经试过（打不开时 `records` 为空，这也是一种结论，**不重试**）。
	std::unordered_map<std::string, std::unique_ptr<PluginIndex>> g_index;

	// 在索引里找「插件内原始 ID = a_raw」的**所有**候选（同一条原始 ID 可能既有本插件自己
	// 定义的记录、又有它覆盖 master 的记录，两者低位会撞车）⇒ 返回区间，由调用方逐个试。
	// 注意：`records` 按 (key, offset) 升序 ⇒ 先出现的（文件里靠前的）先试。
	std::pair<const RecordEntry*, const RecordEntry*> FindCandidates(const PluginIndex& a_index,
		std::uint32_t a_raw)
	{
		const auto first = std::lower_bound(a_index.records.begin(), a_index.records.end(), a_raw,
			[](const RecordEntry& a_e, std::uint32_t a_id) { return a_e.key < a_id; });
		const auto last = std::upper_bound(first, a_index.records.end(), a_raw,
			[](std::uint32_t a_id, const RecordEntry& a_e) { return a_id < a_e.key; });
		const auto* b = a_index.records.empty() ? nullptr : &*first;
		const auto* e = a_index.records.empty() ? nullptr : &*last;
		return { b, e };
	}

	// **按需**读一条记录，在记录体里找**第 `a_occurrence` 个**匹配的 chunk（0 = 第一个）。
	// 口径与旧索引完全一致：只认 `csz ≤ 64`（内联文本/4 字节字符串表 id 都在这个量级）。
	//
	// ⚠️ **为什么需要序号**（2026-10-31 实测）：一条 INFO 可以带**多句台词**（多个 `NAM1`，
	//    每句一条字幕）。以前只取第一个 ⇒ 后面几句全部被写成第一句：
	//    `read=… text='要解开需要把每一片钥匙…' -> base='圣三角是一组加密系统…' -> wrote '圣三角是一组加密系统…'`
	//    （同一记录 0X0013A412 连续三条，base 完全相同）⇒ 玩家看到「说了新台词，字幕还是上一句」。
	//    序号口径与 `Replace` 记忆表的 `seq` 同源（同一引擎、同一读取顺序）。
	bool ReadChunk(std::string_view a_plugin, const RecordEntry& a_record, std::uint32_t a_chunk4CC,
		std::size_t a_occurrence, std::vector<std::uint8_t>& a_out)
	{
		const auto path = Paths::Plugin(a_plugin);
		if (path.empty()) {
			return false;
		}
		std::FILE* f = std::fopen(path.c_str(), "rb");
		if (!f) {
			return false;
		}
		bool                      ok = false;
		std::vector<std::uint8_t> data(a_record.size);
		if (std::fseek(f, static_cast<long>(a_record.offset), SEEK_SET) == 0 &&
			std::fread(data.data(), 1, data.size(), f) == data.size()) {
			std::size_t o      = 0;
			std::size_t a_seen = 0;
			while (o + 6 <= data.size()) {
				const auto csig = U32(data.data() + o);
				const auto csz  = static_cast<std::uint32_t>(data[o + 4] | (data[o + 5] << 8));
				o += 6;
				if (o + csz > data.size()) {
					break;   // 截断的记录体：当作没有这个 chunk
				}
				if (csig == a_chunk4CC && csz <= 64 && a_seen++ >= a_occurrence) {
					a_out.assign(data.begin() + static_cast<std::ptrdiff_t>(o),
						data.begin() + static_cast<std::ptrdiff_t>(o + csz));
					ok = true;
					break;
				}
				o += csz;
			}
		}
		std::fclose(f);
		return ok;
	}

	// 读盘取 chunk 原始字节（公共路径，`Get` / `GetLocalizedID` / `GetRawChunk` 都走它）。
	// `a_occurrence`：同一记录里第几个同名 chunk（INFO 的多句台词 = 多个 `NAM1`）。
	bool GetRawChunkImpl(std::string_view a_plugin, std::uint32_t a_formID, std::uint32_t a_chunk4CC,
		std::vector<std::uint8_t>& a_out, std::size_t a_occurrence = 0)
	{
		const auto* idx = EnsureIndex(std::string{ a_plugin });
		if (!idx) {
			return false;
		}
		const auto [b, e] = FindCandidates(*idx, a_formID);
		for (const auto* it = b; it != e; ++it) {
			if (ReadChunk(a_plugin, *it, a_chunk4CC, a_occurrence, a_out)) {
				return true;
			}
		}
		return false;
	}

	// 一次遍历：只记「记录 → 位置」。**不解析 chunk、不拷贝字节**（理由见 RecordEntry 上面的注释）
	bool BuildIndex(const std::string& a_path, const std::string& a_plugin, PluginIndex& a_index,
		std::size_t& a_scanned)
	{
		const auto t0 = std::chrono::steady_clock::now();

		a_scanned = 0;
		a_index.records.clear();

		std::FILE* f = std::fopen(a_path.c_str(), "rb");
		if (!f) {
			return false;
		}
		// 预留：记录平均 ~200 字节（实测 Fallout4.esm：315 MB / 155 万条）
		// ⚠️ 封顶 200 万条（24 MB），别让一个異常大的文件把预留本身变成内存事故。
		{
			std::error_code ec;
			const auto      bytes = std::filesystem::file_size(a_path, ec);
			if (!ec && bytes > 0) {
				a_index.records.reserve(
					static_cast<std::size_t>(std::min<std::uintmax_t>(bytes / 200, 2u << 20)));
			}
		}

		ForwardReader r{ f };
		std::uint8_t  head[24]{};
		// TES4 头：先取 flags 判**轻插件（ESL）**—— `0x200` 位。ESL 的插件内原始 ID **只有 12 位**
		// （与 `Replace::PluginAndRawIDOf` / `Hooks::BaseRefOf` 同口径）。
		std::uint32_t tes4Flags = 0;
		const auto    readTes4   = [&](bool a_ok) {
			if (!a_ok || std::memcmp(head, "TES4", 4) != 0) {
				return false;
			}
			tes4Flags = U32(head + 8);
			return true;
		};
		const bool    tes4Ok = readTes4(r.Read(head, 24));
		const bool    light  = (tes4Flags & 0x00000200u) != 0;   // ⚠️ 必须在 readTes4 之后算
		if (tes4Ok && r.Skip(U32(head + 4))) {
			// 顶层是隐式无界作用域：GRUP 用栈记终点；栈空只表示回到顶层，必须继续读下一个顶层组 ✓
			std::vector<std::uint64_t> stack;
			while (true) {
				// ⚠️ 出栈检查必须在**读表头之前**（顺序读取器不能后退 24 字节 ✗）
				while (!stack.empty() && r.Pos() >= stack.back()) {
					const auto end = stack.back();
					stack.pop_back();
					if (!r.Skip(end - r.Pos())) goto done;
				}
				if (!r.Read(head, 24)) break;
				const auto size = U32(head + 4);
				if (std::memcmp(head, "GRUP", 4) == 0) {
					stack.push_back((r.Pos() - 24) + size);
					continue;
				}
				++a_scanned;
				const auto bodyPos = r.Pos();   // 记录体起点（24 字节记录头之后）
				const auto flags   = U32(head + 8);
				if (flags & 0x00040000u) {                 // 压缩记录：跳过（不冒 zlib 的险）
					if (!r.Skip(size)) break;
					continue;
				}
				// 只留小记录（大记录没有我们要的文本 chunk ✓）；偏移必须塞得进 u32
				if (size <= 4096 && bodyPos <= 0xFFFFFFFFull) {
					// ⚠️ **必须掩掉存储高位**：文件里自有记录的 formID 高位 = 本插件的编译索引
					// （实测 `Fallout4.esm`=0x00、`DLCCoast.esm`=0x01、`cc*.esl`=0x01），
					// 而所有调用方传进来的是“去高位”的插件内原始 ID ⇒ 不掩就永远查不到。
					const auto stored = U32(head + 12);
					a_index.records.push_back(RecordEntry{ light ? (stored & 0xFFFu) : (stored & 0xFFFFFFu),
						static_cast<std::uint32_t>(bodyPos), size });
				}
				if (!r.Skip(size)) break;
			}
		}
	done:
		std::fclose(f);

		// 二分要求有序。**同一条原始 ID 允许保留多个候选**（本插件自有记录 + 它覆盖 master 的
		// 记录）⇒ 不再按 ID 去重；排序键带上 offset（文件里靠前的先，逐个试）。
		std::sort(a_index.records.begin(), a_index.records.end(),
			[](const RecordEntry& a, const RecordEntry& b) {
				return a.key != b.key ? a.key < b.key : a.offset < b.offset;
			});

		{
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
								std::chrono::steady_clock::now() - t0)
								.count();
			const auto bytes = a_index.records.size() * sizeof(RecordEntry);
			LOG_INFO("BaseText: indexed {} -- scanned={} indexed={}  index~{} KB  took={} ms (chunks read on demand)",
				a_plugin, a_scanned, a_index.records.size(), bytes / 1024, ms);
		}
		return true;
	}

	PluginIndex* EnsureIndex(const std::string& a_plugin)
	{
		if (const auto it = g_index.find(a_plugin); it != g_index.end()) {
			return it->second.get();   // 有键 = 已试过（打不开时 records 为空 —— 那也是一种结论，别重试）
		}
		auto        owned   = std::make_unique<PluginIndex>();
		const auto  path    = Paths::Plugin(a_plugin);
		std::size_t scanned = 0;
		if (!path.empty()) {
			(void)BuildIndex(path, a_plugin, *owned, scanned);
		}
		auto* raw = owned.get();
		g_index.emplace(a_plugin, std::move(owned));
		return raw;
	}

	// ── ③ 松散字符串表：id → 文本（布局见文件头注释）──
	std::unordered_map<std::string, std::vector<std::pair<std::uint32_t, std::uint32_t>>> g_tables;   // 表路径 → (id, offset) 列表
	std::unordered_map<std::string, std::vector<std::uint8_t>>                            g_tableRaw;
	std::unordered_map<std::string, bool>                                                  g_tableTried;

	// 取一张松散字符串表里的一条文本。
	//
	// ⚠️ **三种表有两种条目格式**（2026-10-22 用真实文件实测，别按“统一格式”改）：
	//   `.ILSTRINGS` / `.DLSTRINGS` → **u32 长度前缀 + 字节**
	//       实测 `FALLOUT4_CN.DLSTRINGS` / `.ILSTRINGS` 按这种解才正常，按 NUL 解是垃圾
	//   `.STRINGS`                  → **NUL 结尾**（没有长度前缀）
	//       实测 `FALLOUT4_CN.STRINGS` 与 `ccBGSFO4116-HeavyFlamer_{en,cn}.STRINGS` 都是这种
	// 以前一律按 u32 长度解 ⇒ 凡是要查 `.STRINGS` 的**静默返回空**（不报错、日志上也看不出）。
	std::string LookupTable(const std::string& a_tablePath, std::uint32_t a_id, bool a_nullTerminated)
	{
		if (!g_tableTried.contains(a_tablePath)) {
			g_tableTried[a_tablePath] = true;
			std::FILE* f = std::fopen(a_tablePath.c_str(), "rb");
			if (f) {
				std::fseek(f, 0, SEEK_END);
				const auto sz = static_cast<std::size_t>(std::ftell(f));
				std::fseek(f, 0, SEEK_SET);
				// ⚠️ 同上（EspMastersFrom / TryLoadCache）：`ftell` 失败返回 -1 ⇒ 转成 size_t 是天文字数字，
				//    `vector<uint8_t> raw(sz)` 会直接抛 bad_alloc，而本函数在 `BaseText::Get` 的
				//    持锁路径上、不在任何 try/catch 里。给个上限，超了就当“这张表没有”
				//    （调用方本来就把空当成“没取到”，会继续试下一种表）。
				// ⚠️ `sz == 8` 是**合法的空表**（只有 count+dataSize 头、count=0）——
				//    实测 `ccBGSFO4046-TesCan_en.ILSTRINGS` 就是 8 字节。当异常报会变成假警报。
				constexpr std::size_t kMaxTableBytes = 256u << 20;
				if (sz < 8 || sz > kMaxTableBytes) {
					REX::WARN("BaseText: abnormal string-table size, skipped: {} ({} bytes)", a_tablePath, sz);
					std::fclose(f);
					return {};
				}
				std::vector<std::uint8_t> raw(sz);
				if (std::fread(raw.data(), 1, sz, f) == sz) {
					const auto cnt = U32(raw.data());
					if (cnt > 0 && 8 + static_cast<std::size_t>(cnt) * 8 <= sz) {
						auto& tbl = g_tables[a_tablePath];
						tbl.reserve(cnt);
						for (std::uint32_t k = 0; k < cnt; ++k) {
							const auto id  = U32(raw.data() + 8 + k * 8);
							const auto off = U32(raw.data() + 8 + k * 8 + 4);
							tbl.emplace_back(id, off);
						}
						g_tableRaw[a_tablePath] = std::move(raw);
					}
				}
				std::fclose(f);
			}
		}
		const auto it = g_tables.find(a_tablePath);
		if (it == g_tables.end()) {
			return {};
		}
		const auto& tbl = it->second;
		const auto& raw = g_tableRaw[a_tablePath];
		const auto  blob = 8 + static_cast<std::size_t>(tbl.size()) * 8;
		// 二分（id 升序 ✓）
		std::size_t lo = 0, hi = tbl.size();
		while (lo < hi) {
			const auto mid = (lo + hi) / 2;
			if (tbl[mid].first < a_id) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		if (lo >= tbl.size() || tbl[lo].first != a_id) {
			return {};
		}
		const auto o = blob + tbl[lo].second;
		if (o >= raw.size()) {
			return {};
		}
		if (a_nullTerminated) {
			// `.STRINGS`：NUL 结尾。严格边界 —— 找不到结尾 NUL、或长得离谱，就当“没有”，
			// 宁可不翻译，也不能把半截字节写进 DESC（那是**可见**的文字损坏）。
			// ⚠️ 扫描窗口用**整张表剩余**（不取 4096）：实测 FALLOUT4_CN.STRINGS 里有
			//    2 条**超过 4 KB** 的长文本，用 4096 窗口会把它们误判成“没有 NUL”。
			//    “没 NUL”靠 `end == p + maxScan` 兜底，“长度离谱”靠 8192 字符上限兜底。
			const auto  maxScan = raw.size() - o;
			const auto* p       = reinterpret_cast<const char*>(raw.data() + o);
			const auto* end     = std::find(p, p + maxScan, '\0');
			if (end == p + maxScan) {
				return {};
			}
			const auto n = static_cast<std::size_t>(end - p);
			if (n > 8192) {
				return {};
			}
			return std::string{ p, n };
		}
		if (o + 4 > raw.size()) {
			return {};
		}
		const auto len = U32(raw.data() + o);
		// ⚠️ 上限 8192：实测 `FALLOUT4_CN.DLSTRINGS` 里有 **5016 / 7084** 字节的长对话行，
		//    卡 4096 会把它们当“坏条目”静默丢掉（和上面 `.STRINGS` 那个坑同一类）。
		if (len == 0 || len > 8192 || o + 4 + len > raw.size()) {
			return {};
		}
		const auto* p = reinterpret_cast<const char*>(raw.data() + o + 4);
		const auto  n = len - (raw[o + 4 + len - 1] == 0 ? 1 : 0);
		return std::string{ p, n };
	}

	// 插件基名（去扩展名）+ 表名后缀尝试顺序
	std::string BaseName(std::string_view a_plugin)
	{
		std::string s{ a_plugin };
		if (const auto dot = s.find_last_of('.'); dot != std::string::npos) {
			s.erase(dot);
		}
		return s;
	}

	// ── 让**引擎自己**把 4 字节字符串 id 解成文本 ────────────────────────
	//
	// 为什么不自己解表（2026-10-22）：自己解就要替引擎操心语言（`sLanguage`）、
	// MO2 覆盖优先级、松文件 vs BA2、三种表的格式差异 —— 这些已经踩了 4 个 bug。
	// 引擎的 `BGSLocalizedStringDL::GetText(TESFile&)` 干的就是这件事，
	// 拿的是它**已载入**的那份表（含汉化 + 胜出的覆盖）。
	//
	// ⚠️ **只能在显示层用**：读盘期对“另一个文件”的 id 调它，实测拿回来是
	//   `LOOKUP FAILED!` 占位（见 Replace.cpp 的实测注释）。
	// 返回 false = 引擎这条路过不去（拿不到 TESFile）⇒ 调用方退回自解表。
	bool ResolveViaEngine(std::string_view a_plugin, std::uint32_t a_id, std::string& a_out)
	{
		a_out.clear();
		auto* dh = RE::TESDataHandler::GetSingleton();   // ⚠️ 不能 const：LookupModByName 是非 const 成员
		if (!dh) {
			return false;
		}
		const auto* file = dh->LookupModByName(a_plugin);
		if (!file) {
			return false;
		}
		RE::BGSLocalizedStringDL dl{};
		dl.id = a_id;
		// ⚠️ `ScrapStringBuffer` 是**临时池**（RAII）：不能拷贝，必须在同一个作用域里用完。
		//    绑到 `const&&` 可把临时对象的生命周期延到本块结束。
		const auto&& buf = dl.GetText(*const_cast<RE::TESFile*>(file));
		const auto*  p   = buf.GetString();
		if (!p) {
			return true;   // 引擎答“没有文本” ⇒ 这是权威答案
		}
		a_out = p;
		// 引擎找不到时的占位串：当“没取到”，绝不能写进 DESC。
		if (a_out == "LOOKUP FAILED!") {
			a_out.clear();
		}
		return true;
	}
}

std::string_view BaseText::Get(std::string_view a_plugin, std::uint32_t a_formID, std::uint32_t a_chunk4CC,
	std::string_view a_leg, bool a_displayLayer, std::size_t a_occurrence)
{
	static std::unordered_map<std::string, std::string> cache;   // 缓存在锁内访问 ✓
	// ⚠️ 缓存键必须带 `a_occurrence` —— 否则同一条 INFO 的第 2/3 句台词会拿到第 1 句的结果
	//    （这正是 2026-10-31 “字幕总是同一句”的根因之一）。
	const auto key = std::format("{:.{}}|{:08X}|{:08X}|{}", a_plugin.data(), static_cast<int>(a_plugin.size()),
		a_formID, a_chunk4CC, a_occurrence);

	std::scoped_lock l{ g_lock };
	if (const auto it = cache.find(key); it != cache.end()) {
		return it->second;
	}

	std::string text;
	{
		// 按需读：二分找记录 → 读那一条记录 → 在记录体里找这个 chunk
		// （以前是预先建好的“记录 → 全部 chunk”索引，622 MB / 1.2 秒，见 RecordEntry 注释）
		std::vector<std::uint8_t> bytes;
		if (GetRawChunkImpl(a_plugin, a_formID, a_chunk4CC, bytes, a_occurrence)) {
			if (bytes.size() == 4) {
				const auto id = U32(bytes.data());
				// ① 显示层：**优先让引擎解**（语言/MO2 覆盖/BA2/表格式都是它的事）。
				//    读盘期不走这条（会拿到 `LOOKUP FAILED!` 占位，见 ResolveViaEngine 的注释）。
				bool resolved = false;
				if (a_displayLayer) {
					resolved = ResolveViaEngine(a_plugin, id, text);
					if (!resolved) {
						// 极罕见（插件名对不上 / 数据句柄未就绪）—— 退路是自解字符串表，
						// 但这是“该走引擎却没走成”，值得一行告警。
						LOG_ADV("BaseText: plugin not found in engine -- plugin={} (falling back to self-parsed string table)", a_plugin);
					}
				}
				// ② 自解字符串表：读盘期的主路 + 显示层“拿不到 TESFile”时的兜底。
				if (!resolved) {
					// 松散表：优先 ILSTRINGS，其次 DLSTRINGS / STRINGS（语言 en → cn）
					const auto base = BaseName(a_plugin);
					for (const auto* kind : { "ILSTRINGS", "DLSTRINGS", "STRINGS" }) {
						// 只有 `.STRINGS` 是 NUL 结尾（见 LookupTable 的注释）
						const bool nulTerminated = std::strcmp(kind, "STRINGS") == 0;
						for (const auto* lang : { "en", "cn" }) {
							char name[256]{};
							std::snprintf(name, sizeof(name), "Strings\\%s_%s.%s", base.c_str(), lang, kind);
							const auto p = Paths::DataDir() / name;
							if (auto t = LookupTable(p.string(), id, nulTerminated); !t.empty()) {
								text = std::move(t);
								break;
							}
						}
						if (!text.empty()) {
							break;
						}
					}
				}
			} else if (!bytes.empty()) {
				const auto* p   = reinterpret_cast<const char*>(bytes.data());
				const auto  end = std::find(p, p + bytes.size(), '\0');
				text.assign(p, static_cast<std::size_t>(end - p));
			}
		}
	}

	if (text.empty()) {
		// ⚠️ 归 **[调试] 高级**：这是“我们查过但没取到”。
		//    今天挖出的几条 bug（`.STRINGS` 格式、记录没这个 chunk、记录 >4096 字节、
		//    字符串表里没这个 id）表现**全都是静默返回空** —— 没这一行就只能靠猜。
		//    查表本身有缓存 ⇒ 每个 (插件, 记录, 字段) 最多一行。
		char chunkName[5]{ static_cast<char>(a_chunk4CC), static_cast<char>(a_chunk4CC >> 8),
			static_cast<char>(a_chunk4CC >> 16), static_cast<char>(a_chunk4CC >> 24), 0 };
		LOG_ADV("BaseText: not obtained -- plugin={} record={:08X} chunk={} purpose={}",
			a_plugin, a_formID, chunkName, a_leg.empty() ? std::string_view{ "?" } : a_leg);
	}

	cache.emplace(key, text);
	(void)a_leg;
	return cache.at(key);
}

bool BaseText::GetRawChunk(std::string_view a_plugin, std::uint32_t a_formID, std::uint32_t a_chunk4CC,
	std::vector<std::uint8_t>& a_out)
{
	std::scoped_lock l{ g_lock };
	return GetRawChunkImpl(a_plugin, a_formID, a_chunk4CC, a_out);
}

bool BaseText::GetLocalizedID(std::string_view a_plugin, std::uint32_t a_formID, std::uint32_t a_chunk4CC,
	std::uint32_t& a_outId)
{
	std::vector<std::uint8_t> bytes;
	{
		std::scoped_lock l{ g_lock };
		if (!GetRawChunkImpl(a_plugin, a_formID, a_chunk4CC, bytes)) {
			return false;
		}
	}
	// 只有 **4 字节**才是本地化字符串表 id（内联文本不是 ⇒ 交回false）
	if (bytes.size() != 4) {
		return false;
	}
	a_outId = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
		(static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
	return true;
}
