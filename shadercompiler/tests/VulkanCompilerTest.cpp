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

// Un-gated on 2026-09-08: the SPIR-V back end is built on macOS now. What stays behind
// _WIN32 is the DX11 half only -- CompileDx11Reference asks EffectCompilerDX11 for a
// non-SPIR-V compile, which needs d3dcompiler.dll and which the macOS build answers with
// "DX11/DX12 compilation needs Windows" by design.

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

// A `static const` array beside ordinary globals -- the shape of 30 EVE effects (tonemapping,
// shadowdepth, downsample, ...). CreateGlobalsCB used to sweep the static into the generated
// `cbuffer Globals` with the rest; DXIL shrugged, DXC's SPIR-V back end warned "cbuffer member
// initializer ignored" and then emitted invalid SPIR-V (OpAccessChain out of bounds, or
// "reached non-composite type"). Item 9's whole failure class, in eleven lines.
const char* STATIC_CONST_ARRAY_EFFECT = R"(
static const float4 Colors[4] = { float4( 1, 0, 0, 1 ), float4( 0, 1, 0, 1 ), float4( 0, 0, 1, 1 ), float4( 1, 1, 1, 1 ) };
float Brightness;
float3 Tint;
float Which;

float4 MainPS( float2 uv : TEXCOORD0 ) : SV_Target
{
	return Colors[int( Which )] * Brightness + float4( Tint, 0 );
}

technique T0 { pass P0 { PixelShader = compile ps_5_0 MainPS(); } }
)";

// Thread-group memory beside ordinary globals -- createhistograms.fx's shape. CreateGlobalsCB used
// to sweep the groupshared array into the generated `cbuffer Globals`; DXIL reserved its bytes in
// the cbuffer and carried on, DXC's SPIR-V back end refused the first write with "cannot store to
// Uniform Blocks", and the dynamic exposure chain had no compute shaders on Vulkan.
const char* GROUPSHARED_BESIDE_GLOBALS_EFFECT = R"(
float MinLuminance = 0.01;
groupshared uint histogram[64];
float MaxLuminance = 10.0;
RWBuffer<uint> Output;

[numthreads( 64, 1, 1 )]
void MainCS( uint3 localIdx : SV_GroupThreadID )
{
	histogram[localIdx.x] = 0;
	GroupMemoryBarrierWithGroupSync();
	InterlockedAdd( histogram[localIdx.x], uint( MinLuminance + MaxLuminance ) );
	GroupMemoryBarrierWithGroupSync();
	Output[localIdx.x] = histogram[localIdx.x];
}

technique T0 { pass P0 { ComputeShader = compile cs_5_0 MainCS(); } }
)";

// Globals with HLSL initialisers, the way every EVE effect declares its tunables. Whether the
// compiled stage carries them is the whole difference between a tonemapper with a curve and
// one that divides zero by zero.
const char* DEFAULTED_GLOBALS_EFFECT = R"(
float Brightness < string Group = "Test"; > = 0.75;
float3 Tint = { 0.25, 0.5, 1.0 };
float Weights[3] = { 2, 4, 8 };
int Mode = 3;

float4 MainPS( float2 uv : TEXCOORD0 ) : SV_Target
{
	return float4( Tint * Brightness, Weights[int( uv.x * 2 )] + Mode );
}

technique T0 { pass P0 { PixelShader = compile ps_5_0 MainPS(); } }
)";

// A ray tracing library in the shape of rtshadows.fx plus rtcalculatefroxels.fx's local cbuffer:
// a ray generation and a miss export, an acceleration structure, a UAV and a cbuffer named in the
// globalinput, and one cbuffer that is not -- which makes it local, the shader record's data.
const char* RT_LIBRARY_EFFECT = R"(
RaytracingAccelerationStructure Scene;
RWTexture2D<float4> Output;
cbuffer PerFrame { float4 SunDirection; };
cbuffer LocalData { float4 Tint; float4x4 Transform; float4 Weights[2]; };

struct Payload { float visibility; };

[shader( "raygeneration" )]
[globalinput( "Scene;Output;PerFrame" )]
void RayGen()
{
	RayDesc ray;
	ray.Origin = mul( float4( 0, 0, 0, 1 ), Transform ).xyz;
	ray.Direction = SunDirection.xyz;
	ray.TMin = 0.0;
	ray.TMax = 1000.0;
	Payload payload;
	payload.visibility = 0;
	TraceRay( Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 0, 1, 0, ray, payload );
	Output[DispatchRaysIndex().xy] = payload.visibility * Tint * Weights[1].x;
}

[shader( "miss" )]
[globalinput( "Scene;Output;PerFrame" )]
void Miss( inout Payload payload )
{
	payload.visibility = 1;
}

technique T0
{
	library L0
	{
		RayGenShader = compile lib_6_3 RayGen();
		MissShader = compile lib_6_3 Miss();
		payloadsize = 4;
	}
}
)";

// The same library with a second local cbuffer read by the same entry point. Vulkan gives a
// library one local signature, whose one constant buffer is every record's data; DXC would put
// both blocks at record offset 0.
const char* RT_TWO_LOCAL_CBUFFERS_EFFECT = R"(
RaytracingAccelerationStructure Scene;
RWTexture2D<float4> Output;
cbuffer LocalA { float4 Tint; };
cbuffer LocalB { float4 Origin; };

struct Payload { float visibility; };

[shader( "raygeneration" )]
[globalinput( "Scene;Output" )]
void RayGen()
{
	RayDesc ray;
	ray.Origin = Origin.xyz;
	ray.Direction = float3( 0, 0, 1 );
	ray.TMin = 0.0;
	ray.TMax = 1000.0;
	Payload payload;
	payload.visibility = 0;
	TraceRay( Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload );
	Output[DispatchRaysIndex().xy] = payload.visibility * Tint;
}

[shader( "miss" )]
[globalinput( "Scene;Output" )]
void Miss( inout Payload payload )
{
	payload.visibility = 1;
}

technique T0
{
	library L0
	{
		RayGenShader = compile lib_6_3 RayGen();
		MissShader = compile lib_6_3 Miss();
		payloadsize = 4;
	}
}
)";

// Two entry points, each reading a different local cbuffer: one each per entry point, but two
// for the library. The AL builds every group's record from the library's one local signature
// and refuses a signature with two constant buffers, so this would compile and then switch ray
// tracing off at run time.
const char* RT_LOCAL_CBUFFER_PER_ENTRY_POINT_EFFECT = R"(
RaytracingAccelerationStructure Scene;
RWTexture2D<float4> Output;
cbuffer RayGenData { float4 Origin; };
cbuffer MissData { float4 Sky; };

struct Payload { float4 colour; };

[shader( "raygeneration" )]
[globalinput( "Scene;Output" )]
void RayGen()
{
	RayDesc ray;
	ray.Origin = Origin.xyz;
	ray.Direction = float3( 0, 0, 1 );
	ray.TMin = 0.0;
	ray.TMax = 1000.0;
	Payload payload;
	payload.colour = 0;
	TraceRay( Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload );
	Output[DispatchRaysIndex().xy] = payload.colour;
}

[shader( "miss" )]
[globalinput( "Scene;Output" )]
void Miss( inout Payload payload )
{
	payload.colour = Sky;
}

technique T0
{
	library L0
	{
		RayGenShader = compile lib_6_3 RayGen();
		MissShader = compile lib_6_3 Miss();
		payloadsize = 16;
	}
}
)";

// A local cbuffer whose array of floats D3D strides at 16 bytes and a shader record at 4: the
// record would not be a byte copy of the constant buffer.
const char* RT_PACKED_LOCAL_CBUFFER_EFFECT = R"(
RWTexture2D<float4> Output;
cbuffer LocalData { float Weights[4]; float4 Tint; };

[shader( "raygeneration" )]
[globalinput( "Output" )]
void RayGen()
{
	Output[DispatchRaysIndex().xy] = Weights[DispatchRaysIndex().x & 3] * Tint;
}

technique T0 { library L0 { RayGenShader = compile lib_6_3 RayGen(); payloadsize = 4; } }
)";

// A local cbuffer that leaves no room for the group handle in a 4096-byte record.
const char* RT_OVERSIZED_LOCAL_CBUFFER_EFFECT = R"(
RWTexture2D<float4> Output;
cbuffer LocalData { float4 Rows[256]; };

[shader( "raygeneration" )]
[globalinput( "Output" )]
void RayGen()
{
	Output[DispatchRaysIndex().xy] = Rows[DispatchRaysIndex().x & 255];
}

technique T0 { library L0 { RayGenShader = compile lib_6_3 RayGen(); payloadsize = 4; } }
)";

// Valid to this compiler's parser, refused by DXC: a write to a read-only texture.
const char* RT_DXC_ERROR_EFFECT = R"(
Texture2D<float4> Input;
RWTexture2D<float4> Output;

[shader( "raygeneration" )]
[globalinput( "Input;Output" )]
void RayGen()
{
	Input[DispatchRaysIndex().xy] = 1;
	Output[DispatchRaysIndex().xy] = 2;
}

technique T0 { library L0 { RayGenShader = compile lib_6_3 RayGen(); payloadsize = 4; } }
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

#if _WIN32
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
#endif

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

// What the ray tracing tests read from a library's SPIR-V module.
struct SpirvLibraryInfo
{
	std::map<std::string, uint32_t> entryPoints; // name -> execution model
	std::set<uint32_t> capabilities;
	std::map<uint32_t, std::string> names;
	std::map<std::string, uint32_t> variableStorageClasses; // variable name -> storage class
	std::map<uint32_t, Decoration> decorations;
};

std::string SpirvString( const uint32_t* words, size_t wordCount )
{
	const char* bytes = reinterpret_cast<const char*>( words );
	return std::string( bytes, strnlen( bytes, wordCount * 4 ) );
}

SpirvLibraryInfo ReadSpirvLibrary( const Library& library )
{
	SpirvLibraryInfo info;
	auto words = reinterpret_cast<const uint32_t*>( g_stringTable.GetString( library.shaderDataStr ) );
	size_t wordCount = library.shaderSize / 4;
	info.decorations = ScanDecorations( words, wordCount );
	std::vector<std::pair<uint32_t, uint32_t>> variables;
	for( size_t i = 5; i < wordCount; )
	{
		uint32_t opcode = words[i] & 0xFFFF;
		uint32_t length = words[i] >> 16;
		if( length == 0 )
		{
			break;
		}
		if( opcode == 17 ) // OpCapability
		{
			info.capabilities.insert( words[i + 1] );
		}
		else if( opcode == 15 ) // OpEntryPoint
		{
			info.entryPoints[SpirvString( words + i + 3, length - 3 )] = words[i + 1];
		}
		else if( opcode == 5 ) // OpName
		{
			info.names[words[i + 1]] = SpirvString( words + i + 2, length - 2 );
		}
		else if( opcode == 59 ) // OpVariable
		{
			variables.push_back( { words[i + 2], words[i + 3] } );
		}
		i += length;
	}
	for( auto& variable : variables )
	{
		info.variableStorageClasses[info.names[variable.first]] = variable.second;
	}
	return info;
}

const uint32_t SPIRV_EXECUTION_MODEL_RAY_GENERATION = 5313;
const uint32_t SPIRV_EXECUTION_MODEL_MISS = 5317;
const uint32_t SPIRV_CAPABILITY_RAY_QUERY = 4472;
const uint32_t SPIRV_CAPABILITY_RAY_TRACING = 4479;
const uint32_t SPIRV_STORAGE_CLASS_SHADER_RECORD_BUFFER = 5343;

// Compiling must fail, and the messages must name every one of `expected`.
void ExpectRefused( const char* src, std::initializer_list<const char*> expected )
{
	testing::internal::CaptureStdout();
	bool compiled = TryCompileSpirv( src );
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_FALSE( compiled );
	for( auto text : expected )
	{
		EXPECT_NE( output.find( text ), std::string::npos ) << "'" << text << "' not in: " << output;
	}
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

// The reflected constant of that name, or null.
const Constant* FindConstant( const StageData& stage, const char* name )
{
	for( auto& constant : stage.constants )
	{
		if( strcmp( g_stringTable.GetString( constant.name ), name ) == 0 )
		{
			return &constant;
		}
	}
	return nullptr;
}

float DefaultAt( const StageData& stage, const Constant& constant, size_t index = 0 )
{
	float value = 0.0f;
	if( constant.offset + ( index + 1 ) * sizeof( float ) > stage.defaultValues.size() )
	{
		ADD_FAILURE() << g_stringTable.GetString( constant.name ) << ": the defaults block ends before this element";
		return value;
	}
	memcpy( &value, &stage.defaultValues[constant.offset + index * sizeof( float )], sizeof( float ) );
	return value;
}

TEST( VulkanCompiler, TheInitialisersOfGlobalsBecomeTheStagesDefaultValues )
{
	EffectData data = CompileSpirv( DEFAULTED_GLOBALS_EFFECT );
	ASSERT_EQ( data.techniques.size(), 1u );
	auto& stage = data.techniques[0].passes[0].stages[0];
	ASSERT_FALSE( stage.defaultValues.empty() ) << "dxc's reflection has no DefaultValue; the AST initialisers must be evaluated instead";

	const Constant* brightness = FindConstant( stage, "Brightness" );
	ASSERT_NE( brightness, nullptr );
	EXPECT_FLOAT_EQ( 0.75f, DefaultAt( stage, *brightness ) );

	const Constant* tint = FindConstant( stage, "Tint" );
	ASSERT_NE( tint, nullptr );
	EXPECT_FLOAT_EQ( 0.25f, DefaultAt( stage, *tint, 0 ) );
	EXPECT_FLOAT_EQ( 0.5f, DefaultAt( stage, *tint, 1 ) );
	EXPECT_FLOAT_EQ( 1.0f, DefaultAt( stage, *tint, 2 ) );

	// An array: D3D lays each element on its own 16-byte row.
	const Constant* weights = FindConstant( stage, "Weights" );
	ASSERT_NE( weights, nullptr );
	EXPECT_FLOAT_EQ( 2.0f, DefaultAt( stage, *weights, 0 ) );
	EXPECT_FLOAT_EQ( 4.0f, DefaultAt( stage, *weights, 4 ) );
	EXPECT_FLOAT_EQ( 8.0f, DefaultAt( stage, *weights, 8 ) );

	const Constant* mode = FindConstant( stage, "Mode" );
	ASSERT_NE( mode, nullptr );
	int32_t modeValue = 0;
	memcpy( &modeValue, &stage.defaultValues[mode->offset], sizeof( modeValue ) );
	EXPECT_EQ( 3, modeValue );
}

TEST( VulkanCompiler, AStaticConstArrayStaysOutOfTheGlobalsCbuffer )
{
	// Compiling at all is the assertion: before the fix this effect was refused with DXC's
	// "generated SPIR-V is invalid". The module is then checked for its magic like the others.
	EffectData data = CompileSpirv( STATIC_CONST_ARRAY_EFFECT );
	ASSERT_EQ( data.techniques.size(), 1u );
	auto& stages = data.techniques[0].passes[0].stages;
	ASSERT_EQ( stages.size(), 1u );
	size_t wordCount = 0;
	const uint32_t* words = ShaderWords( stages[0], wordCount );
	ASSERT_GE( wordCount, 5u );
	EXPECT_EQ( words[0], 0x07230203u );
}

TEST( VulkanCompiler, AGroupSharedArrayStaysOutOfTheGlobalsCbuffer )
{
	// Compiling at all is the assertion, as for the static above: before the fix DXC refused the
	// module with "generated SPIR-V is invalid ... cannot store to Uniform Blocks".
	EffectData data = CompileSpirv( GROUPSHARED_BESIDE_GLOBALS_EFFECT );
	ASSERT_EQ( data.techniques.size(), 1u );
	auto& stages = data.techniques[0].passes[0].stages;
	ASSERT_EQ( stages.size(), 1u );
	size_t wordCount = 0;
	const uint32_t* words = ShaderWords( stages[0], wordCount );
	ASSERT_GE( wordCount, 5u );
	EXPECT_EQ( words[0], 0x07230203u );
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

#if _WIN32
// The only test here that needs a second back end to compare against: it asserts the
// SPIR-V path's reflection agrees with the DX11 path's, and the DX11 path is Windows-only.
// Its Vulkan-side numbers are absolute, not relative, so a macOS run would still be worth
// having -- but not at the price of asserting a fact about a compiler that is not there.
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

TEST( VulkanCompiler, ARayTracingLibraryIsOneSpirvModuleWithAnEntryPointPerExport )
{
	EffectData data = CompileSpirv( RT_LIBRARY_EFFECT );
	ASSERT_EQ( data.techniques.size(), 1u );
	ASSERT_EQ( data.techniques[0].libraries.size(), 1u );
	auto& library = data.techniques[0].libraries[0];
	ASSERT_GE( library.shaderSize, 20u );
	auto words = reinterpret_cast<const uint32_t*>( g_stringTable.GetString( library.shaderDataStr ) );
	EXPECT_EQ( words[0], 0x07230203u ) << "the library bytecode is not SPIR-V (DXIL starts with DXBC)";

	auto info = ReadSpirvLibrary( library );
	ASSERT_EQ( info.entryPoints.size(), 2u );
	EXPECT_EQ( info.entryPoints["RayGen"], SPIRV_EXECUTION_MODEL_RAY_GENERATION );
	EXPECT_EQ( info.entryPoints["Miss"], SPIRV_EXECUTION_MODEL_MISS );
	EXPECT_TRUE( info.capabilities.count( SPIRV_CAPABILITY_RAY_TRACING ) );
	// A device with ray queries and no ray tracing pipeline must not be asked for them.
	EXPECT_FALSE( info.capabilities.count( SPIRV_CAPABILITY_RAY_QUERY ) );

	// The exports are the entry points, by their HLSL names; the reflection is the DXIL's.
	ASSERT_EQ( library.exports.size(), 2u );
	EXPECT_STREQ( g_stringTable.GetString( library.exports[0].name ), "RayGen" );
	EXPECT_STREQ( g_stringTable.GetString( library.exports[1].name ), "Miss" );
	EXPECT_EQ( library.payloadSize, 4u );

	// Global inputs are bound through the engine's COMPUTE_SHADER slots.
	using namespace Tr2VulkanBindingABI;
	std::set<std::pair<uint32_t, uint32_t>> present;
	for( auto& d : info.decorations )
	{
		present.insert( { d.second.set, d.second.binding } );
	}
	ASSERT_FALSE( library.globalInputs.registerInputs.empty() );
	for( auto& ri : library.globalInputs.registerInputs )
	{
		uint32_t expectedSet = DescriptorSetIndex( uint32_t( ri.registerType.Packed() ) );
		uint32_t expectedBinding = BindingNumber( uint32_t( ri.registerType.Packed() ), ri.registerIndex, uint32_t( COMPUTE_STAGE ) );
		EXPECT_TRUE( present.count( { expectedSet, expectedBinding } ) )
			<< "register type " << int( ri.registerType.Packed() ) << " index " << ri.registerIndex
			<< " expected set " << expectedSet << " binding " << expectedBinding;
	}
}

TEST( VulkanCompiler, ALocalCBufferIsTheShaderRecord )
{
	EffectData data = CompileSpirv( RT_LIBRARY_EFFECT );
	auto& library = data.techniques[0].libraries[0];
	auto info = ReadSpirvLibrary( library );

	ASSERT_TRUE( info.variableStorageClasses.count( "LocalData" ) );
	EXPECT_EQ( info.variableStorageClasses["LocalData"], SPIRV_STORAGE_CLASS_SHADER_RECORD_BUFFER );
	ASSERT_TRUE( info.variableStorageClasses.count( "PerFrame" ) );
	EXPECT_NE( info.variableStorageClasses["PerFrame"], SPIRV_STORAGE_CLASS_SHADER_RECORD_BUFFER );

	// The reflection still describes it as the library's local input, as on DX12.
	ASSERT_EQ( library.localInputs.registerInputs.size(), 1u );
	EXPECT_EQ( int( library.localInputs.registerInputs[0].registerType.Packed() ), int( RT_CONSTANT_BUFFER ) );
	EXPECT_NE( library.source.find( "[[vk::shader_record_ext]]" ), std::string::npos );
}

TEST( VulkanCompiler, TwoLocalCBuffersInOneEntryPointAreRefused )
{
	ExpectRefused( RT_TWO_LOCAL_CBUFFERS_EFFECT, { "'T0/L0'", "'RayGen'", "'LocalA'", "'LocalB'", "one local signature" } );
}

TEST( VulkanCompiler, TwoEntryPointsReadingDifferentLocalCBuffersAreRefused )
{
	// The message names the library and every local cbuffer with the entry points that read it;
	// CompileMessageQueue puts the effect's path in front.
	ExpectRefused( RT_LOCAL_CBUFFER_PER_ENTRY_POINT_EFFECT,
				   { "'T0/L0'", "'RayGenData' (entry point 'RayGen')", "'MissData' (entry point 'Miss')", "one local signature" } );
}

TEST( VulkanCompiler, ALocalCBufferPackedDifferentlyAsARecordIsRefused )
{
	ExpectRefused( RT_PACKED_LOCAL_CBUFFER_EFFECT, { "'RayGen'", "'LocalData'", "laid out differently" } );
}

TEST( VulkanCompiler, ALocalCBufferTooLargeForARecordIsRefused )
{
	ExpectRefused( RT_OVERSIZED_LOCAL_CBUFFER_EFFECT, { "'RayGen'", "'LocalData'", "4096 bytes" } );
}

TEST( VulkanCompiler, ADxcErrorInARayTracingLibraryIsReported )
{
	// IDxcCompiler3 answers S_OK with a failed status; the library's DXIL compile used to read
	// that as success and dereference a reflection that was never produced.
	ExpectRefused( RT_DXC_ERROR_EFFECT, { "cannot assign to return value" } );
}
