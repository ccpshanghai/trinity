// Copyright © 2026 CCP ehf.

#include "TesingUtils.h"
#include "EffectCompilerDX11.h"
#include "EffectCompilerVulkan.h"
#include "StringTable.h"
#include "trinityal/vulkan/Tr2ShaderBindingABIVulkan.h"

#include <algorithm>
#include <map>
#include <set>

extern StringTable g_stringTable;

#if _WIN32

namespace
{
// The smallest effect that exercises a cbuffer, a texture and a sampler in both stages.
const char* SIMPLE_EFFECT = R"(
float4x4 WorldViewProj;
Texture2D DiffuseMap;
SamplerState DiffuseSampler { Filter = MIN_MAG_MIP_LINEAR; };

struct VS_IN { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VS_OUT MainVS( VS_IN input )
{
	VS_OUT output;
	output.pos = mul( input.pos, WorldViewProj );
	output.uv = input.uv;
	return output;
}

float4 MainPS( VS_OUT input ) : SV_Target
{
	return DiffuseMap.Sample( DiffuseSampler, input.uv );
}

technique T0
{
	pass P0
	{
		VertexShader = compile vs_5_0 MainVS();
		PixelShader = compile ps_5_0 MainPS();
	}
}
)";

// One resource array, which AssignRegisters puts on a space of its own -- space1 for
// the first array, which is already the set the ABI wants. The baseline for the pair
// below: the rewrite must leave this alone rather than refuse it.
const char* ONE_RESOURCE_ARRAY_EFFECT = R"(
Texture2D DiffuseMaps[4];
SamplerState Samp { Filter = MIN_MAG_MIP_LINEAR; };

float4 MainPS( float2 uv : TEXCOORD0 ) : SV_Target
{
	return DiffuseMaps[0].Sample( Samp, uv );
}

technique T0 { pass P0 { PixelShader = compile ps_5_0 MainPS(); } }
)";

// Two resource arrays used by one stage. The space counter is seeded with 0, so the
// second array lands on space2 -- and space is the descriptor set, of which the ABI
// header has exactly two. Nothing downstream can express set 2: -fvk-t-shift's
// trailing "1" stops matching, so the shift silently does not apply and the AL builds
// set 1 while the module says set 2.
const char* TWO_RESOURCE_ARRAYS_EFFECT = R"(
Texture2D DiffuseMaps[4];
Texture2D NormalMaps[4];
SamplerState Samp { Filter = MIN_MAG_MIP_LINEAR; };

float4 MainPS( float2 uv : TEXCOORD0 ) : SV_Target
{
	return DiffuseMaps[0].Sample( Samp, uv ) + NormalMaps[0].Sample( Samp, uv );
}

technique T0 { pass P0 { PixelShader = compile ps_5_0 MainPS(); } }
)";

EffectData CompileSpirv( const char* src )
{
	EffectCompilerDX11 compiler;
	[&] { ASSERT_TRUE( compiler.Create() ); }();
	EffectData data;
	bool compiled = compiler.CompileEffect( src + 1, strlen( src ), {},
		data, { "6_0", true, false, true }, nullptr );
	g_messages.Flush();
	[compiled] { ASSERT_TRUE( compiled ); }();
	return data;
}

// CompileSpirv asserts success. This one reports it, for the cases where refusing to
// compile is the correct answer.
bool TryCompileSpirv( const char* src )
{
	EffectCompilerDX11 compiler;
	[&] { ASSERT_TRUE( compiler.Create() ); }();
	EffectData data;
	bool compiled = compiler.CompileEffect( src + 1, strlen( src ), {},
		data, { "6_0", true, false, true }, nullptr );
	g_messages.Flush();
	return compiled;
}

EffectData CompileDx11Reference( const char* src )
{
	EffectCompilerDX11 compiler;
	[&] { ASSERT_TRUE( compiler.Create() ); }();
	EffectData data;
	bool compiled = compiler.CompileEffect( src + 1, strlen( src ), {},
		data, { "5_1", true, false }, nullptr );
	g_messages.Flush();
	[compiled] { ASSERT_TRUE( compiled ); }();
	return data;
}

const uint32_t* ShaderWords( const StageInput& stage, size_t& wordCount )
{
	const char* bytes = g_stringTable.GetString( stage.shaderDataStr );
	wordCount = stage.shaderSize / 4;
	return reinterpret_cast<const uint32_t*>( bytes );
}

struct Decoration
{
	uint32_t set = ~0u;
	uint32_t binding = ~0u;
};

std::map<uint32_t, Decoration> ScanDecorations( const uint32_t* words, size_t wordCount )
{
	std::map<uint32_t, Decoration> byId;
	size_t i = 5;
	while( i < wordCount )
	{
		uint32_t opcode = words[i] & 0xFFFF;
		uint32_t length = words[i] >> 16;
		if( opcode == 71 && length == 4 )
		{
			if( words[i + 2] == 33 )
			{
				byId[words[i + 1]].binding = words[i + 3];
			}
			if( words[i + 2] == 34 )
			{
				byId[words[i + 1]].set = words[i + 3];
			}
		}
		i += std::max<size_t>( length, 1u );
	}
	return byId;
}

// Every register input a stage declares must appear in the module on the set and
// binding the ABI header computes for it. Nothing else in the pipeline can recover
// from a mismatch here, because both sides derive their numbers independently.
void ExpectStageObeysTheAbi( const StageInput& stage )
{
	using namespace Tr2VulkanBindingABI;

	uint32_t shaderType = uint32_t( stage.type );
	size_t wordCount = 0;
	const uint32_t* words = ShaderWords( stage, wordCount );
	auto decorations = ScanDecorations( words, wordCount );

	std::set<std::pair<uint32_t, uint32_t>> present;
	for( auto& d : decorations )
	{
		if( d.second.set != ~0u && d.second.binding != ~0u )
		{
			present.insert( { d.second.set, d.second.binding } );
		}
	}

	for( auto& ri : stage.registerInputs )
	{
		uint32_t expectedSet = DescriptorSetIndex( uint32_t( ri.registerType.Packed() ) );
		uint32_t expectedBinding = BindingNumber( uint32_t( ri.registerType.Packed() ), ri.registerIndex, shaderType );
		EXPECT_TRUE( present.count( { expectedSet, expectedBinding } ) )
			<< "register type " << int( ri.registerType.Packed() ) << " index " << ri.registerIndex
			<< " declared space " << int( ri.registerSpace )
			<< " expected set " << expectedSet << " binding " << expectedBinding;
	}
}
}

TEST( VulkanCompiler, EmitsSpirvForEveryStage )
{
	EffectData data = CompileSpirv( SIMPLE_EFFECT );
	ASSERT_EQ( data.techniques.size(), 1u );
	ASSERT_EQ( data.techniques[0].passes.size(), 1u );
	auto& stages = data.techniques[0].passes[0].stages;
	ASSERT_EQ( stages.size(), 2u );
	for( auto& stage : stages )
	{
		size_t wordCount = 0;
		const uint32_t* words = ShaderWords( stage, wordCount );
		ASSERT_GE( wordCount, 5u );
		EXPECT_EQ( words[0], 0x07230203u );      // SPIR-V magic
		EXPECT_EQ( words[1], 0x00010600u );      // SPIR-V 1.6 = vulkan1.3
	}
}

TEST( VulkanCompiler, ReflectionSurvivesTheBackendSwap )
{
	EffectData data = CompileSpirv( SIMPLE_EFFECT );
	EffectData dx11Data = CompileDx11Reference( SIMPLE_EFFECT );
	auto& ps = data.techniques[0].passes[0].stages[1];
	auto& dx11Ps = dx11Data.techniques[0].passes[0].stages[1];
	EXPECT_EQ( ps.type, PIXEL_STAGE );
	// The DX11 pass path produces the same counts for this source when static samplers are not collected.
	EXPECT_EQ( ps.registerInputs.size(), 2u );
	EXPECT_EQ( ps.registerInputs.size(), dx11Ps.registerInputs.size() );
	EXPECT_EQ( ps.textures.size(), 1u );
	EXPECT_EQ( ps.textures.size(), dx11Ps.textures.size() );
	EXPECT_EQ( ps.samplers.size(), 1u );
	EXPECT_EQ( ps.samplers.size(), dx11Ps.samplers.size() );
	ASSERT_EQ( ps.constants.size(), 0u ); // WorldViewProj is vs-only
	auto& vs = data.techniques[0].passes[0].stages[0];
	auto& dx11Vs = dx11Data.techniques[0].passes[0].stages[0];
	ASSERT_EQ( vs.constants.size(), 1u );
	EXPECT_EQ( vs.constants.size(), dx11Vs.constants.size() );
	EXPECT_STREQ( g_stringTable.GetString( vs.constants[0].name ), "WorldViewProj" );
	EXPECT_EQ( vs.pipelineInputs.size(), 2u ); // POSITION + TEXCOORD
	EXPECT_EQ( vs.pipelineInputs.size(), dx11Vs.pipelineInputs.size() );
}

TEST( VulkanCompiler, BindingsObeyTheABIHeader )
{
	EffectData data = CompileSpirv( SIMPLE_EFFECT );
	for( auto& stage : data.techniques[0].passes[0].stages )
	{
		ExpectStageObeysTheAbi( stage );
	}
}

TEST( VulkanCompiler, TheVulkanCompilerSetsThePlatformDefine )
{
	const char* src = R"(
float4 MainPS() : SV_Target
{
#if PLATFORM == 20
	return float4( 0, 0, 0, 0 );
#else
#error wrong platform
#endif
}

technique T0 { pass P0 { PixelShader = compile ps_5_0 MainPS(); } }
)";
	EffectData data = Compile<EffectCompilerVulkan>( src );
	auto& ps = data.techniques[0].passes[0].stages[0];
	size_t wordCount = 0;
	const uint32_t* words = ShaderWords( ps, wordCount );
	EXPECT_EQ( words[0], 0x07230203u );
}

TEST( VulkanCompiler, AResourceArrayLandsInTheResourceDescriptorSet )
{
	EffectData data = CompileSpirv( ONE_RESOURCE_ARRAY_EFFECT );
	auto& stages = data.techniques[0].passes[0].stages;
	ASSERT_EQ( stages.size(), 1u );
	auto& ps = stages[0];
	// AssignRegisters puts the array on a space of its own, which for the first array
	// is space1 -- already the resource set. It has to be left there, not refused.
	ASSERT_EQ( ps.registerInputs.size(), 2u );
	for( auto& ri : ps.registerInputs )
	{
		EXPECT_EQ( int( ri.registerSpace ), int( Tr2VulkanBindingABI::DESCRIPTOR_SET_RESOURCES ) )
			<< "register type " << int( ri.registerType.Packed() ) << " index " << ri.registerIndex;
	}
	ExpectStageObeysTheAbi( ps );
}

TEST( VulkanCompiler, ASecondResourceArrayIsRefusedRatherThanMisbound )
{
	// The second array is assigned space2, which the ABI has no descriptor set for.
	// It used to pass straight through: -fvk-t-shift names space 1 and nothing else,
	// so the shift did not apply and the module declared set 2 while the AL built
	// set 1 -- a wrong binding with no diagnostic on any side.
	testing::internal::CaptureStdout();
	bool compiled = TryCompileSpirv( TWO_RESOURCE_ARRAYS_EFFECT );
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_FALSE( compiled );
	EXPECT_NE( output.find( "NormalMaps" ), std::string::npos ) << output;
	EXPECT_NE( output.find( "register space 2" ), std::string::npos ) << output;
}

#endif
