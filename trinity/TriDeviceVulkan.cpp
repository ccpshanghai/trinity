// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if ( TRINITY_PLATFORM == TRINITY_VULKAN )

#include "TriDevice.h"

#include "RenderJob/Tr2RenderJobs.h"

using namespace Tr2RenderContextEnum;

CCP_STATS_DECLARED_ELSEWHERE( presentTime );


// This is the frame pump, and despite living in a per-platform translation unit there is
// nothing Vulkan-specific in it -- every call below is declared unconditionally in
// TriDevice.h, and Present() dispatches into the AL. It is modelled on TriDeviceMetal.cpp
// rather than TriDevice12.cpp: dx12's version carries DXGI device-lost and fullscreen
// handling that has no analogue here, while metal is the other modern explicit backend
// and its MarkFrameEvent calls line up with what the Vulkan AL already implements.
void TriDevice::HandleRenderTick( Be::Time realTime, Be::Time simTime )
{
	USE_MAIN_THREAD_RENDER_CONTEXT();

	if( !renderContext.IsValid() )
	{
		return;
	}

	if( ShouldSkipFrame() )
	{
		Throttle();
		return;
	}

	if( m_upscalingChanged )
	{
		CCP_LOGNOTICE( "Resetting device - Upscaler changed" );
		CreateUpscalingTechnique( mAdapter );
		ResetDevice();
		return;
	}

	if( mDeviceLost )
	{
		return;
	}

	renderContext.MarkFrameEvent( Tr2RenderContextEnum::FRAME_EVENT_UPDATE_STARTED );

	if( m_renderJobs )
	{
		m_renderJobs->RunUpdate( realTime, simTime );
	}
	renderContext.MarkFrameEvent( Tr2RenderContextEnum::FRAME_EVENT_UPDATE_FINISHED );

	m_postUpdateCallbacks->Update();

	{
		renderContext.MarkFrameEvent( Tr2RenderContextEnum::FRAME_EVENT_PRESENT_STARTED );
		CCP_STATS_SCOPED_TIME( presentTime );
		CR_RETURN( renderContext.Present() );
		renderContext.MarkFrameEvent( Tr2RenderContextEnum::FRAME_EVENT_PRESENT_FINISHED );
	}

	// A minimised Win32 window has a zero-sized surface, so no swapchain exists and Present
	// began no frame (IsPresentationSuspendedVulkan). Rendering now would record into the
	// previous, already submitted command buffer against a back buffer that is gone, and
	// every TriStepClear logged "Clear failed" once per frame. Skip the rendering, as
	// ShouldSkipFrame does; the update above (render jobs' update, post-update callbacks)
	// has run, and so has everything else in the device's tick. Present tries again next
	// frame and the first frame after the restore renders as usual.
	if( renderContext.IsPresentationSuspendedVulkan() )
	{
		return;
	}

	renderContext.MarkFrameEvent( Tr2RenderContextEnum::FRAME_EVENT_RENDERING_STARTED );
	if( !Render() )
	{
		CCP_LOGERR( "Failed to render a frame" );
	}
	renderContext.MarkFrameEvent( Tr2RenderContextEnum::FRAME_EVENT_RENDERING_FINISHED );
}

// -- Smaller helpers to enable big methods like TriDevice::Render to be mostly API neutral.

// Do we have a valid device pointer? Lower level question than "do we have a valid renderContext",
// so first question may be true while second one is still false.
bool TriDevice::DeviceExists()
{
	USE_MAIN_THREAD_RENDER_CONTEXT();
	return renderContext.IsValid();
}

// --------------------------------------------------------------------------------------
// Description:
//   A chance for device to respond to application window being activated/deactivated.
//   Not implemented for DX9 - we are happy with how windows behaves in DX9.
// Arguments:
//   activated - true if application was activated; false otherwise
// --------------------------------------------------------------------------------------
void TriDevice::ApplicationActivated( ApplicationActivation )
{
}

#endif
