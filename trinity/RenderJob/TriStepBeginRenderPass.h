// Copyright © 2026 CCP ehf.

#pragma once
#ifndef TriStepBeginRenderPass_h_
#define TriStepBeginRenderPass_h_


#include "TriRenderStep.h"

// Opens a render pass on the currently bound targets, so that whatever runs next can record
// draws into it.
//
// Every other step in a job either records a draw itself or changes state; this one exists
// because Metal has no always-recording command list. An MTLRenderCommandEncoder lives only
// between a pass beginning and ending, and the steps that look like they would open one do not:
// Clear ends the current encoder and leaves MTLLoadActionClear on the descriptor for the next
// pass to apply, and SetStdRndStates only touches the effect state manager.
//
// So a job that hands control to something which records its own draws -- TriStepPythonCB with
// a hosted UI in it -- has to ask for the pass, in the job, where a reader can see it. The
// alternative was for GetNativeRenderEncoder() to open one when asked, and that is the thing
// M3 spec 5 forbids: the getters observe, they do not drive.
//
// A no-op on DX11, DX12, Vulkan and the stub, where the command list is always recording.
BLUE_CLASS( TriStepBeginRenderPass ) :
	public TriRenderStep
{
public:
	EXPOSE_TO_BLUE();

	TriStepBeginRenderPass( IRoot* lockobj = 0 );

	//IRenderStep
	TriStepResult Execute( Be::Time realTime, Be::Time simTime, Tr2RenderContext & renderContext );
};

TYPEDEF_BLUECLASS( TriStepBeginRenderPass );

#endif
