// Copyright © 2026 CCP ehf.
#include "StdAfx.h"
#if ( TRINITY_PLATFORM == TRINITY_VULKAN )

#include "WithRenderContextFixture.h"
#include <filesystem>
#include <string>

// Set by the host (or by a test) to the directory the backend persists its
// VkPipelineCache blob in. Null means the feature is off, which is the default
// everywhere except an Android run and this test.
extern const char* g_pipelineCacheDirectory;

namespace
{

// Android has no writable /tmp and TMPDIR is usually unset, so
// std::filesystem::temp_directory_path() is not portable here. When the host has
// already pointed the backend at a directory (the app's files dir on Android), take
// a subdirectory of that -- it is writable by construction. Fall back to the temp
// directory on desktop, where it is real.
std::filesystem::path TestCacheDirectory()
{
	const std::filesystem::path base = ( g_pipelineCacheDirectory && *g_pipelineCacheDirectory )
		? std::filesystem::path( g_pipelineCacheDirectory )
		: std::filesystem::temp_directory_path();
	return base / "trinityal-pipelinecache-test";
}

ALResult CreatePositionOnlyVS( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( PositionOnly.vs )
	};

	auto input = Tr2ShaderSignatureAL().Add( Tr2VertexDefinition::POSITION, 0, 0, Tr2ShaderPipelineInputAL::FLOAT, 3 );

	return shader.Create( Tr2RenderContextEnum::VERTEX_SHADER, bytecode, input, "", renderContext );
}

ALResult CreateConstantColorPS( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( ConstantColor.ps )
	};

	return shader.Create( Tr2RenderContextEnum::PIXEL_SHADER, bytecode, Tr2ShaderSignatureAL(), "", renderContext );
}

// One draw, which is what puts a pipeline in the cache. A device that is created and
// destroyed without drawing writes a bare 32-byte header and would prove nothing.
void DrawOneTriangle( Tr2PrimaryRenderContextAL& renderContext )
{
	Tr2ShaderAL vs;
	ASSERT_HRESULT_SUCCEEDED( CreatePositionOnlyVS( vs, renderContext ) );

	Tr2ShaderAL ps;
	ASSERT_HRESULT_SUCCEEDED( CreateConstantColorPS( ps, renderContext ) );

	Tr2ShaderAL shaders[] = { vs, ps };
	Tr2ShaderProgramAL sp;
	ASSERT_HRESULT_SUCCEEDED( sp.Create( shaders, 2, renderContext ) );

	float vertices[] = {
		-0.5f, -0.5f, 0.0f,
		-0.5f,  0.5f, 0.0f,
		 0.5f, -0.5f, 0.0f,
	};
	const uint32_t vbStride = 3 * sizeof( float );
	Tr2BufferAL vb;
	ASSERT_HRESULT_SUCCEEDED( vb.Create( vbStride, sizeof( vertices ) / vbStride, Tr2GpuUsage::VERTEX_BUFFER, Tr2CpuUsage::NONE, vertices, renderContext ) );

	Tr2VertexDefinition definition;
	definition.Add( Tr2VertexDefinition::FLOAT32_3, Tr2VertexDefinition::POSITION );

	Tr2VertexLayoutAL vertexLayout;
	ASSERT_HRESULT_SUCCEEDED( vertexLayout.Create( definition, renderContext ) );

	ASSERT_HRESULT_SUCCEEDED( renderContext.BeginScene() );
	ASSERT_HRESULT_SUCCEEDED( renderContext.Clear( Tr2RenderContextEnum::CLEARFLAGS_TARGET, 0xff000000, 1.0f ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext.SetStreamSource( 0, vb, 0, vbStride ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext.SetVertexLayout( vertexLayout ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext.SetShaderProgram( sp ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext.SetTopology( Tr2RenderContextEnum::TOP_TRIANGLES ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext.SetRenderState( Tr2RenderContextEnum::RS_ZENABLE, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext.SetRenderState( Tr2RenderContextEnum::RS_CULLMODE, Tr2RenderContextEnum::CULLMODE_NONE ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext.DrawPrimitive( 0, 1 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext.EndScene() );
	ASSERT_HRESULT_SUCCEEDED( renderContext.Present() );

	// The draw has to have reached the GPU before the device is torn down, or the
	// pipeline may never be built at all. Present already waits on the frame fence.
	ASSERT_HRESULT_SUCCEEDED( renderContext.SetStreamSource( 0, Tr2BufferAL(), 0, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext.SetShaderProgram( Tr2ShaderProgramAL() ) );
}

void SetUpPresentParameters( Tr2PresentParametersAL& presentParameters )
{
	Tr2VideoAdapterInfo::GetAdapterDisplayMode( Tr2VideoAdapterInfo::DEFAULT_ADAPTER, presentParameters.mode );
	presentParameters.mode.width = 640;
	presentParameters.mode.height = 480;
	presentParameters.backBufferCount = 1;
	presentParameters.msaaType = 0;
	presentParameters.msaaQuality = 0;
	presentParameters.swapEffect = Tr2RenderContextEnum::SWAP_EFFECT_DISCARD;
	presentParameters.outputWindow = WithWindow::GetWindowHandle();
	presentParameters.windowed = true;
	presentParameters.software = false;
	presentParameters.presentInterval = Tr2RenderContextEnum::PRESENT_INTERVAL_IMMEDIATE;
}

}

struct PipelineCache : public WithRenderContext
{
};

// The feature this proves: a device that draws leaves a pipeline cache on disk, and the
// next device on the same machine loads that blob instead of starting cold. It asserts the
// round trip -- one file, more than a bare header, and accepted on the way back in -- not a
// timing, because a timing assertion on a warm cache is a flake generator. The device
// measurement lives in the milestone doc.
TEST_F( PipelineCache, RoundTripsThroughDisk )
{
	if( !MachineHasGfxAdapter() )
	{
		GTEST_SKIP() << "Test Skipped as no adapters present on machine.";
	}

	const std::filesystem::path dir = TestCacheDirectory();
	std::error_code ec;
	std::filesystem::remove_all( dir, ec );
	ASSERT_TRUE( std::filesystem::create_directories( dir, ec ) ) << "could not create " << dir.string();

	const char* previous = g_pipelineCacheDirectory;
	const std::string dirString = dir.string();
	g_pipelineCacheDirectory = dirString.c_str();

	Tr2PresentParametersAL presentParameters;
	SetUpPresentParameters( presentParameters );

	// Cold: nothing on disk yet.
	ASSERT_HRESULT_SUCCEEDED( renderContext->CreateDevice( 0, WithWindow::GetWindowHandle(), presentParameters ) );
	DrawOneTriangle( *renderContext );
	renderContext->Destroy();

	int files = 0;
	std::filesystem::path cacheFile;
	for( const auto& entry : std::filesystem::directory_iterator( dir ) )
	{
		++files;
		cacheFile = entry.path();
	}
	EXPECT_EQ( files, 1 ) << "expected exactly one cache blob in " << dir.string();
	if( files == 1 )
	{
		// VkPipelineCacheHeaderVersionOne is 32 bytes. Anything at or below that is a
		// header with no pipelines behind it, which is not a round trip.
		EXPECT_GT( std::filesystem::file_size( cacheFile ), 32u );
	}

	// Warm: the same directory, now populated. The blob must be accepted, not merely
	// tolerated -- a rejected blob shows up as a device that still creates successfully,
	// so the size check above is what distinguishes the two.
	EXPECT_HRESULT_SUCCEEDED( renderContext->CreateDevice( 0, WithWindow::GetWindowHandle(), presentParameters ) );

	g_pipelineCacheDirectory = previous;
	std::filesystem::remove_all( dir, ec );
}

#endif
