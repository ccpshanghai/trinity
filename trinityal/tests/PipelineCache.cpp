// Copyright © 2026 CCP ehf.
#include "StdAfx.h"
#if ( TRINITY_PLATFORM == TRINITY_VULKAN )

#include "WithRenderContextFixture.h"
#include "gtest/gtest-spi.h"
#include <filesystem>
#include <string>

// Set by the host (or by a test) to the directory the backend persists its
// VkPipelineCache blob in. Null means the feature is off, which is the default
// everywhere except an Android run and this test.
extern const char* g_pipelineCacheDirectory;

// Bytes of cache blob the most recent CreateDevice started the device from; 0 for a cold
// start or a rejected one. This is the test's only oracle for "accepted", because
// CreateDevice returns S_OK whether the blob was loaded, was rejected by the header
// check, or was never found -- see the warm leg below.
extern size_t g_pipelineCacheBytesLoaded;

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

// Points the backend at a directory for as long as the scope lives, and restores the
// previous value on every way out of it -- including the way a fatal gtest assertion
// takes, which is a bare `return` from the test body with no trailing statement reached.
//
// The guard owns the string because the global holds a `const char*` into it. With the
// string a plain local of the test and the restore a trailing assignment, a failed ASSERT
// destroys the string while the global still points at its heap buffer, and every later
// CreateDevice and Destroy *in the whole process* does std::string() + fopen() on freed
// memory -- a use-after-free that lands in whatever test runs next. Member destruction
// runs after this destructor body, so the global is restored before the string dies.
class ScopedPipelineCacheDirectory
{
public:
	explicit ScopedPipelineCacheDirectory( const std::filesystem::path& directory )
		: m_directory( directory.string() )
		, m_previous( g_pipelineCacheDirectory )
	{
		g_pipelineCacheDirectory = m_directory.c_str();
	}

	~ScopedPipelineCacheDirectory()
	{
		g_pipelineCacheDirectory = m_previous;
	}

	ScopedPipelineCacheDirectory( const ScopedPipelineCacheDirectory& ) = delete;
	ScopedPipelineCacheDirectory& operator=( const ScopedPipelineCacheDirectory& ) = delete;

private:
	const std::string m_directory;
	const char* const m_previous;
};

// A test body that leaves under a fatal assertion, which is the shape the guard exists
// for and the one the round-trip test cannot check about itself. Takes no arguments
// because EXPECT_FATAL_FAILURE's statement may not name the caller's locals.
void FailFatallyUnderTheDirectoryGuard()
{
	const ScopedPipelineCacheDirectory scopedDirectory( TestCacheDirectory() );
	ASSERT_NE( g_pipelineCacheDirectory, nullptr );
	FAIL() << "deliberate: standing in for the cold CreateDevice failing";
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

	const ScopedPipelineCacheDirectory scopedDirectory( dir );

	Tr2PresentParametersAL presentParameters;
	SetUpPresentParameters( presentParameters );

	// Cold: nothing on disk yet. Asserted, not assumed -- if this leg started warm off a
	// blob a previous run left behind, the warm leg below proves nothing.
	ASSERT_HRESULT_SUCCEEDED( renderContext->CreateDevice( 0, WithWindow::GetWindowHandle(), presentParameters ) );
	EXPECT_EQ( g_pipelineCacheBytesLoaded, 0u ) << "the cold leg started from a blob";
	DrawOneTriangle( *renderContext );
	renderContext->Destroy();

	int files = 0;
	std::filesystem::path cacheFile;
	for( const auto& entry : std::filesystem::directory_iterator( dir ) )
	{
		++files;
		cacheFile = entry.path();
	}
	ASSERT_EQ( files, 1 ) << "expected exactly one cache blob in " << dir.string();

	// VkPipelineCacheHeaderVersionOne is 32 bytes. Anything at or below that is a header
	// with no pipelines behind it, which is not a round trip.
	const uintmax_t coldBytes = std::filesystem::file_size( cacheFile );
	EXPECT_GT( coldBytes, 32u );

	// Warm: the same directory, now populated. CreateDevice's return cannot be the oracle
	// for this leg -- it is S_OK whether the blob was loaded, was discarded by the header
	// check, was never found because the read path built a different filename than the
	// write path, or was handed to a driver that refused it, because the whole cache block
	// is non-fatal by design. So assert what the device actually started from, against the
	// file the cold leg wrote. This is the "accepted" half of spec 8's round trip; the size
	// check above only ever spoke for the cold half.
	EXPECT_HRESULT_SUCCEEDED( renderContext->CreateDevice( 0, WithWindow::GetWindowHandle(), presentParameters ) );
	EXPECT_EQ( (uintmax_t)g_pipelineCacheBytesLoaded, coldBytes )
		<< "the warm device did not start from the " << coldBytes << "-byte blob the cold run wrote";

	// Destroyed while the directory above is still the configured one, so this device's own
	// cache write lands there instead of wherever the host had pointed the backend before.
	renderContext->Destroy();

	std::filesystem::remove_all( dir, ec );
}

// The failure path of the test above, which that test cannot exercise about itself: the
// round trip only reaches its restore when everything succeeds, and the one thing the
// backend reads on every CreateDevice and every Destroy in the process is exactly what
// gets left behind. Needs no adapter -- it never creates a device.
TEST( PipelineCacheDirectory, IsRestoredWhenAFatalAssertionLeavesTheScope )
{
	const char* const before = g_pipelineCacheDirectory;

	EXPECT_FATAL_FAILURE( FailFatallyUnderTheDirectoryGuard(), "deliberate" );

	EXPECT_EQ( g_pipelineCacheDirectory, before )
		<< "the directory every later CreateDevice and Destroy reads was left pointing into a destroyed std::string";
}

#endif
