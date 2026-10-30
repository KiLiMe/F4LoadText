# ─────────────────────────────────────────────────────────────────────────────
# F4Text（展示名；工程/目标/产物名仍为 F4LoadText）· 读盘期文本拦截
#
# 依赖只有两样，都就地取，不联网：
#   * CommonLibF4       —— 同级工作区目录（或 extern/CommonLibF4），直接编源码
#   * spdlog / minhook  —— 仓内已解包的 vcpkg 树（离线可用，不触发 vcpkg 下载）
#
#   cmake --preset default
#   cmake --build build --config Release
# 产物：build/Release/F4LoadText.dll（tools/deploy.py 认这个位置）
#
# 一条命令：uv run python tools/deploy.py（编译 + 部署；-h 看全部参数）
# ─────────────────────────────────────────────────────────────────────────────
cmake_minimum_required(VERSION 3.21)

project(F4LoadText LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")  # /MD，与游戏进程一致

# ── CommonLibF4 定位 ─────────────────────────────────────────────────────────
set(COMMONLIBF4_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibF4" CACHE PATH "CommonLibF4 源码目录")
if (NOT EXISTS "${COMMONLIBF4_DIR}/include/RE/Fallout.h")
	set(_fallback "${CMAKE_CURRENT_SOURCE_DIR}/../CommonLibF4")
	if (EXISTS "${_fallback}/include/RE/Fallout.h")
		message(STATUS "没有 extern/CommonLibF4，改用同级目录：${_fallback}")
		set(COMMONLIBF4_DIR "${_fallback}")
	else()
		message(FATAL_ERROR "找不到 CommonLibF4：${COMMONLIBF4_DIR}")
	endif()
endif()

# ── 依赖定位：先用仓内已解包的 vcpkg 树（离线/不重拉包）────────────────────
# ⚠️ yaml-cpp 也在这里（2026-10-08 加，`[汉化]` 读 translations\*.yaml 用）：
#    它由 `vcpkg install --triplet x64-windows-static-md` 装进这棵树，
#    但要**手动拷 lib/include/share**（vcpkg 的 manifest 模式装到了仓根，不是 build\）。
set(F4LT_DEP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/build/vcpkg_installed/x64-windows-static-md"
	CACHE PATH "已解包的 vcpkg 依赖根（spdlog / minhook / yaml-cpp）")
if (IS_DIRECTORY "${F4LT_DEP_ROOT}")
	list(PREPEND CMAKE_PREFIX_PATH "${F4LT_DEP_ROOT}")
endif()

find_package(spdlog REQUIRED CONFIG)
find_package(minhook REQUIRED CONFIG)
find_package(yaml-cpp REQUIRED CONFIG)

# ── commonlib-shared（自带 CMakeLists）──────────────────────────────────────
set(COMMONLIB_INI OFF CACHE BOOL "" FORCE)
set(COMMONLIB_JSON OFF CACHE BOOL "" FORCE)
set(COMMONLIB_TOML OFF CACHE BOOL "" FORCE)
set(COMMONLIB_XBYAK OFF CACHE BOOL "" FORCE)
set(COMMONLIB_RANDOM OFF CACHE BOOL "" FORCE)
add_subdirectory(
	"${COMMONLIBF4_DIR}/lib/commonlib-shared"
	"${CMAKE_BINARY_DIR}/commonlib-shared"
	EXCLUDE_FROM_ALL
)

# ── commonlibf4 本体：上游现在只发 xmake，这里直接编源码 ────────────────────
file(GLOB_RECURSE CLF4_SOURCES CONFIGURE_DEPENDS "${COMMONLIBF4_DIR}/src/*.cpp")
add_library(commonlibf4 STATIC ${CLF4_SOURCES})
target_include_directories(commonlibf4 PUBLIC "${COMMONLIBF4_DIR}/include")
target_compile_features(commonlibf4 PUBLIC cxx_std_23)
target_link_libraries(commonlibf4 PUBLIC commonlib-shared)
# 上游 xmake 用 set_pcxxheader("include/F4SE/Impl/PCH.h")，这里必须对齐：
# 它的头文件不自带 <cstdint>/<span>/sv 字面量，没有 PCH 编不过。
target_precompile_headers(commonlibf4 PRIVATE "${COMMONLIBF4_DIR}/include/F4SE/Impl/PCH.h")

# ── 插件本体 ────────────────────────────────────────────────────────────────
add_library(F4LoadText SHARED
	src/BaseText.cpp
	src/Config.cpp
	src/DictionaryCache.cpp
	src/Hooks.cpp
	src/Replace.cpp
	src/Translation.cpp
	src/main.cpp
)

target_include_directories(F4LoadText PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(F4LoadText PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WINDLL)

if(MSVC)
	target_compile_options(F4LoadText PRIVATE
		/utf-8 /Zc:preprocessor /permissive- /bigobj
		/wd4200 /wd4201 /wd4324
		/external:W0
	)
endif()

target_precompile_headers(F4LoadText PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/PCH.h")

target_link_libraries(F4LoadText PRIVATE
	commonlibf4
	commonlib-shared
	minhook::minhook
	yaml-cpp::yaml-cpp
)

set_target_properties(F4LoadText PROPERTIES
	OUTPUT_NAME "F4LoadText"
	PREFIX ""
	SUFFIX ".dll"
)

