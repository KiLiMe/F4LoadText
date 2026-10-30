#pragma once

// 统一编译开关
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif

// CommonLibF4 自己的预编译头（REL / REX / RE 的 ID 与 RTTI 表都在里面）。
// 少了它，RE 的头会因为没有 <span> / <cstdint> / "..."sv 而编不过 —— 上游 xmake
// 也是 set_pcxxheader("include/F4SE/Impl/PCH.h")，这里对齐。
#include "F4SE/Impl/PCH.h"

// ⚠️ 不要 include <Windows.h>：
//    a) REX/W32/BASE.h 里有硬检查，Windows 先到就直接 #error；
//    b) Windows 的 MAX_PATH 宏会破坏 RE::TESFile 里的 REX::W32::MAX_PATH（零长度数组）。
//    需要 Win32 就用 REX::W32:: 那套绑定（见 Config.cpp / main.cpp）。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// 只补用到的 RE 头，不全量拉 RE/Fallout.h
#include "RE/B/BGSLocalizedString.h"
#include "RE/B/BSStringT.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESDescription.h"
#include "RE/T/TESFile.h"
#include "RE/T/TESFileArray.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESFormUtil.h"
#include "RE/T/TESFullName.h"

#include "F4SE/F4SE.h"

