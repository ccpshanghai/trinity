// Copyright © 2026 CCP ehf.

#pragma once

#include "ALResult.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

// Vulkan reports success and failure by sign, not by a single value: every VkResult >= 0
// is a success code and every negative one is an error. VK_SUCCESS is only the "nothing
// worth mentioning happened" case; VK_SUBOPTIMAL_KHR (+1000001003), VK_TIMEOUT (+2),
// VK_NOT_READY (+1) and VK_INCOMPLETE (+5) are all successes too.
//
// This used to be a switch that recognised 0 and mapped everything else to E_FAIL, which
// turned four Vulkan successes into AL failures. The one that matters is
// VK_SUBOPTIMAL_KHR: it is what vkQueuePresentKHR returns when the frame *was* presented
// but the swapchain no longer matches the surface -- routine on every window resize, and
// routine on Android from the first frame because the surface transform is not identity.
// Callers wrapping it in CR_RETURN_HR were bailing out of a call that had worked.
//
// S_FALSE is the HRESULT for exactly this: succeeded, but not unconditionally. FAILED()
// is false for it, so CR_RETURN_HR carries on.
inline ALResult Vk2Al( VkResult result )
{
	if( result == VK_SUCCESS )
	{
		return S_OK;
	}
	if( result > 0 )
	{
		return S_FALSE;
	}
	return E_FAIL;
}

#endif