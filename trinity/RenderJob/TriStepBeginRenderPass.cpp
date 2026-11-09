// Copyright © 2026 CCP ehf.

#include "StdAfx.h"
#include "TriStepBeginRenderPass.h"


TriStepBeginRenderPass::TriStepBeginRenderPass( IRoot* lockobj )
{
}

TriStepResult TriStepBeginRenderPass::Execute( Be::Time realTime, Be::Time simTime, Tr2RenderContext& renderContext )
{
	// Logged rather than swallowed: the only way this fails is an invalid context, and the
	// symptom downstream would be a hosted UI that draws nothing with nothing to read.
	HRESULT hr = renderContext.BeginRenderPass();
	if( !SUCCEEDED( hr ) )
	{
		CCP_LOGERR( "BeginRenderPass failed" );
	}
	return RS_OK;
}
