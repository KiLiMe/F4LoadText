#pragma once

#include <cstdint>

namespace Hooks
{
	struct Status
	{
		const char*   name{ "" };
		std::uint64_t id{ 0 };
		void*         target{ nullptr };
		void*         detour{ nullptr };
		void**        original{ nullptr };
		bool          disabled{ false };
		bool          installed{ false };
		bool          enabled{ false };
		int           code{ 0 };
	};

	void Install();
	// 第 4 条腿（显示层）：TESDescription::GetDescription(2193019) 的 DESC 回写。
	// **必须在 kGameDataReady 调用**（先建“组件地址 → formID”映射，再装钩子）——
	// 见 src/Hooks.cpp 里 InstallDescriptionHook 的注释。重复调用是安全的（已装则直接返回）。
	void InstallDescriptionHook();
	void Report();
	// 建“组件地址 → formID”映射（显示层钩子专用，只读）。
	// InstallDescriptionHook() 自己会调，单拿出来是为了能单独验证一次。
	bool BuildDescriptionMap();
	// 查某个 TESDescription 组件属于哪条记录（映射未建时返回 false）。
	bool FormIDOfDescription(const void* a_description, std::uint32_t& a_formID);
}
