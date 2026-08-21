// Copyright © 2026 CCP ehf.

#include "TesingUtils.h"
#include "EffectCompilerDX11.h"
#include "StringTable.h"

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

#endif
