// Copyright © 2026 CCP ehf.

#include "TesingUtils.h"
#include "EffectCompilerDX11.h"
#include "EffectCompilerVulkan.h"
#include "StringTable.h"
#include "trinityal/vulkan/Tr2ShaderBindingABIVulkan.h"
#include "trinityal/vulkan/SpirvReflectVulkan.h"

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

// A local cbuffer with both matrix packings. D3D stores a column_major matrix column by column
// and DXC's record row by row as a RowMajor SPIR-V matrix of the transposed shape; the bytes are
// the same, and the layout check has to see that scalar for scalar, not only as a set of offsets.
const char* RT_MATRIX_LOCAL_CBUFFER_EFFECT = R"(
RWTexture2D<float4> Output;
cbuffer LocalData { float4x3 ColumnMajor; row_major float3x4 RowMajor; float4 Tail; };

[shader( "raygeneration" )]
[globalinput( "Output" )]
void RayGen()
{
	float4 v = float4( DispatchRaysIndex().xyz, 1 );
	Output[DispatchRaysIndex().xy] = float4( mul( v, ColumnMajor ), 1 ) + float4( mul( RowMajor, v ), 0 ) + Tail;
}

technique T0 { library L0 { RayGenShader = compile lib_6_3 RayGen(); payloadsize = 4; } }
)";

// A column_major float3x3 is 44 bytes in a D3D cbuffer (its last column is not padded) and 48 in
// a record: the float after it moves. The refusal names the first scalar that moved.
const char* RT_MATRIX_TAIL_LOCAL_CBUFFER_EFFECT = R"(
RWTexture2D<float4> Output;
cbuffer LocalData { float3x3 Rotation; float After; };

[shader( "raygeneration" )]
[globalinput( "Output" )]
void RayGen()
{
	Output[DispatchRaysIndex().xy] = float4( mul( float3( DispatchRaysIndex().xyz ), Rotation ), After );
}

technique T0 { library L0 { RayGenShader = compile lib_6_3 RayGen(); payloadsize = 4; } }
)";

// A tbuffer the library names in its globalinput: an SRV like any other global, never a shader
// record (DX12's LOCAL_INPUT counts cbuffers only).
const char* RT_TBUFFER_EFFECT = R"(
RWTexture2D<float4> Output;
tbuffer Lookup : register( t2 ) { float4 Entries[4]; };
cbuffer LocalData { float4 Tint; };

[shader( "raygeneration" )]
[globalinput( "Output;Lookup" )]
void RayGen()
{
	Output[DispatchRaysIndex().xy] = Entries[DispatchRaysIndex().x & 3] * Tint;
}

technique T0 { library L0 { RayGenShader = compile lib_6_3 RayGen(); payloadsize = 4; } }
)";

// The shape of every V5 hull's `technique RtShadow`: a library of hit group shaders only, beside
// the effect's ordinary passes. This one reads two local cbuffers, which Vulkan cannot give a
// library, so its SPIR-V compile is refused.
const char* RT_HIT_GROUP_LIBRARY_REFUSED_EFFECT = R"(
cbuffer LocalA { float4 Tint; };
cbuffer LocalB { float4 Shade; };

struct Payload { float visibility; };
struct Attributes { float2 uv; };

[shader( "closesthit" )]
void ClosestHit( inout Payload payload, Attributes attrib )
{
	payload.visibility = Tint.x + Shade.x + attrib.uv.x;
}

float4 MainPS( float2 uv : TEXCOORD0 ) : SV_Target
{
	return float4( uv, 0, 1 );
}

technique T0 { pass P0 { PixelShader = compile ps_5_0 MainPS(); } }
technique RtShadow { library Refused { ClosestHitShader = compile lib_6_3 ClosestHit(); payloadsize = 4; } }
)";

// The same shape, refused by DXC itself (a write to a read-only texture).
const char* RT_HIT_GROUP_LIBRARY_DXC_ERROR_EFFECT = R"(
Texture2D<float4> Input;

struct Payload { float visibility; };
struct Attributes { float2 uv; };

[shader( "closesthit" )]
[globalinput( "Input" )]
void ClosestHit( inout Payload payload, Attributes attrib )
{
	Input[uint2( 0, 0 )] = 1;
	payload.visibility = attrib.uv.x;
}

float4 MainPS( float2 uv : TEXCOORD0 ) : SV_Target
{
	return float4( uv, 0, 1 );
}

technique T0 { pass P0 { PixelShader = compile ps_5_0 MainPS(); } }
technique RtShadow { library DxcError { ClosestHitShader = compile lib_6_3 ClosestHit(); payloadsize = 4; } }
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

using SpirvReflectVulkan::SpirvReflection;

// The module, through the reader the AL and the compiler's record check use.
SpirvReflection Reflect( const void* bytes, size_t size )
{
	SpirvReflection reflection;
	EXPECT_TRUE( SpirvReflectVulkan::ReflectSpirv( bytes, size, reflection ) ) << "not a SPIR-V module the shared reader can read";
	return reflection;
}

SpirvReflection ReflectStage( const StageInput& stage )
{
	return Reflect( g_stringTable.GetString( stage.shaderDataStr ), stage.shaderSize );
}

SpirvReflection ReflectLibrary( const Library& library )
{
	return Reflect( g_stringTable.GetString( library.shaderDataStr ), library.shaderSize );
}

// Entry point name -> execution model.
std::map<std::string, uint32_t> EntryPoints( const SpirvReflection& reflection )
{
	std::map<std::string, uint32_t> result;
	for( auto& entryPoint : reflection.entryPoints )
	{
		result[entryPoint.name] = entryPoint.executionModel;
	}
	return result;
}

// Variable name -> storage class.
std::map<std::string, uint32_t> StorageClasses( const SpirvReflection& reflection )
{
	std::map<std::string, uint32_t> result;
	for( auto& variable : reflection.variables )
	{
		result[reflection.NameOf( variable.id )] = variable.storageClass;
	}
	return result;
}

// Every (set, binding) a variable is decorated with.
std::set<std::pair<uint32_t, uint32_t>> BoundSlots( const SpirvReflection& reflection )
{
	std::set<std::pair<uint32_t, uint32_t>> result;
	for( auto& binding : reflection.bindings )
	{
		result.insert( { binding.set, binding.binding } );
	}
	return result;
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
	auto present = BoundSlots( ReflectStage( stage ) );

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

	auto reflection = ReflectLibrary( library );
	auto entryPoints = EntryPoints( reflection );
	ASSERT_EQ( entryPoints.size(), 2u );
	EXPECT_EQ( entryPoints["RayGen"], SPIRV_EXECUTION_MODEL_RAY_GENERATION );
	EXPECT_EQ( entryPoints["Miss"], SPIRV_EXECUTION_MODEL_MISS );
	EXPECT_TRUE( reflection.capabilities.count( SPIRV_CAPABILITY_RAY_TRACING ) );
	// A device with ray queries and no ray tracing pipeline must not be asked for them.
	EXPECT_FALSE( reflection.capabilities.count( SPIRV_CAPABILITY_RAY_QUERY ) );

	// The exports are the entry points, by their HLSL names; the reflection is the DXIL's.
	ASSERT_EQ( library.exports.size(), 2u );
	EXPECT_STREQ( g_stringTable.GetString( library.exports[0].name ), "RayGen" );
	EXPECT_STREQ( g_stringTable.GetString( library.exports[1].name ), "Miss" );
	EXPECT_EQ( library.payloadSize, 4u );

	// Global inputs are bound through the engine's COMPUTE_SHADER slots.
	using namespace Tr2VulkanBindingABI;
	auto present = BoundSlots( reflection );
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
	auto storageClasses = StorageClasses( ReflectLibrary( library ) );

	ASSERT_TRUE( storageClasses.count( "LocalData" ) );
	EXPECT_EQ( storageClasses["LocalData"], SPIRV_STORAGE_CLASS_SHADER_RECORD_BUFFER );
	ASSERT_TRUE( storageClasses.count( "PerFrame" ) );
	EXPECT_NE( storageClasses["PerFrame"], SPIRV_STORAGE_CLASS_SHADER_RECORD_BUFFER );

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
	// Weights[1] is at 16 in the constant buffer and at 4 in the record: the message names the
	// first scalar that moved. (CompileMessageQueue prints a given text once per process, so
	// every expectation on this message lives in this one test.)
	ExpectRefused( RT_PACKED_LOCAL_CBUFFER_EFFECT,
				   { "'RayGen'", "'LocalData'", "laid out differently", "First difference: scalar 1 ('Weights') is at byte 16", "at byte 4 (4 bytes) in the shader record" } );
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

TEST( VulkanCompiler, BothMatrixPackingsInALocalCBufferAreTheSameBytesAsARecord )
{
	EffectData data = CompileSpirv( RT_MATRIX_LOCAL_CBUFFER_EFFECT );
	ASSERT_EQ( data.techniques.size(), 1u );
	ASSERT_EQ( data.techniques[0].libraries.size(), 1u );
	auto storageClasses = StorageClasses( ReflectLibrary( data.techniques[0].libraries[0] ) );
	ASSERT_TRUE( storageClasses.count( "LocalData" ) );
	EXPECT_EQ( storageClasses["LocalData"], SPIRV_STORAGE_CLASS_SHADER_RECORD_BUFFER );
}

TEST( VulkanCompiler, AMatrixWhosePaddingDiffersInARecordIsRefusedByName )
{
	// Rotation's nine scalars agree; After is at 44 in the constant buffer and at 48 in the record.
	ExpectRefused( RT_MATRIX_TAIL_LOCAL_CBUFFER_EFFECT, { "'LocalData'", "laid out differently", "scalar 9 ('After') is at byte 44", "at byte 48" } );
}

TEST( VulkanCompiler, ATbufferIsNeverTheShaderRecord )
{
	EffectData data = CompileSpirv( RT_TBUFFER_EFFECT );
	ASSERT_EQ( data.techniques[0].libraries.size(), 1u );
	auto& library = data.techniques[0].libraries[0];
	auto storageClasses = StorageClasses( ReflectLibrary( library ) );

	ASSERT_TRUE( storageClasses.count( "LocalData" ) );
	EXPECT_EQ( storageClasses["LocalData"], SPIRV_STORAGE_CLASS_SHADER_RECORD_BUFFER );
	for( auto& variable : storageClasses )
	{
		if( variable.first != "LocalData" )
		{
			EXPECT_NE( variable.second, SPIRV_STORAGE_CLASS_SHADER_RECORD_BUFFER ) << variable.first;
		}
	}
	// One shader record attribute, on the cbuffer; the tbuffer stays a tbuffer.
	const std::string attribute = "[[vk::shader_record_ext]]";
	size_t first = library.source.find( attribute );
	ASSERT_NE( first, std::string::npos );
	EXPECT_EQ( library.source.find( attribute, first + 1 ), std::string::npos );
	EXPECT_NE( library.source.find( "tbuffer" ), std::string::npos );
	// The reflection agrees: the one local input is LocalData's constant buffer.
	ASSERT_EQ( library.localInputs.registerInputs.size(), 1u );
	EXPECT_EQ( int( library.localInputs.registerInputs[0].registerType.Packed() ), int( RT_CONSTANT_BUFFER ) );
}

namespace
{
// Compiles `src`, which must succeed with its RtShadow technique's library dropped and a line
// saying so; returns the compiler's output. Each caller's library has its own name, because
// CompileMessageQueue prints a given text once per process.
std::string ExpectHitGroupLibraryDropped( const char* src, const char* libraryName )
{
	testing::internal::CaptureStdout();
	EffectCompilerDX11 compiler;
	EXPECT_TRUE( compiler.Create() );
	EffectData data;
	bool compiled = compiler.CompileEffect( src + 1, strlen( src ), {}, data, { "6_0", true, false, true }, nullptr );
	g_messages.Flush();
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_TRUE( compiled ) << output;
	EXPECT_EQ( data.techniques.size(), 2u ) << output;
	for( auto& technique : data.techniques )
	{
		EXPECT_TRUE( technique.libraries.empty() ) << output;
	}
	EXPECT_NE( output.find( std::string( "ray tracing library '" ) + libraryName + "' dropped" ), std::string::npos ) << output;
	return output;
}
}

TEST( VulkanCompiler, AHitGroupLibraryTheRecordRulesRefuseIsDroppedNotTheEffect )
{
	std::string output = ExpectHitGroupLibraryDropped( RT_HIT_GROUP_LIBRARY_REFUSED_EFFECT, "RtShadow/Refused" );
	EXPECT_NE( output.find( "one local signature" ), std::string::npos ) << output;
}

TEST( VulkanCompiler, AHitGroupLibraryDxcRefusesIsDroppedNotTheEffect )
{
	std::string output = ExpectHitGroupLibraryDropped( RT_HIT_GROUP_LIBRARY_DXC_ERROR_EFFECT, "RtShadow/DxcError" );
	EXPECT_NE( output.find( "cannot assign to return value" ), std::string::npos ) << output;
}

TEST( VulkanCompiler, ARayGenLibraryThatFailsStillFailsTheEffect )
{
	// The effects that trace index their library unchecked; only hit group libraries are dropped.
	testing::internal::CaptureStdout();
	bool compiled = TryCompileSpirv( RT_DXC_ERROR_EFFECT );
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_FALSE( compiled );
	EXPECT_EQ( output.find( "dropped" ), std::string::npos ) << output;
}

// SpirvReflectVulkan.h: the one SPIR-V reader the AL, the compiler's record check and these tests
// share. Its modules come from DXC through the compiler, as the AL gets them.
namespace
{
// A record with an array of float4s (ArrayStride 16 on both sides) and a float after it.
const char* RT_ARRAY_LOCAL_CBUFFER_EFFECT = R"(
RWTexture2D<float4> Output;
cbuffer LocalData { float4 Rows[3]; float Last; };

[shader( "raygeneration" )]
[globalinput( "Output" )]
void RayGen()
{
	Output[DispatchRaysIndex().xy] = Rows[DispatchRaysIndex().x % 3] * Last;
}

technique T0 { library L0 { RayGenShader = compile lib_6_3 RayGen(); payloadsize = 4; } }
)";

std::vector<uint32_t> ScalarOffsets( const SpirvReflection::ShaderRecordBlock& record )
{
	std::vector<uint32_t> offsets;
	for( auto& slot : record.scalars )
	{
		offsets.push_back( slot.offset );
	}
	return offsets;
}
}

TEST( SpirvReflectVulkan, ALibrarysEntryPointsAccelerationStructureAndRecord )
{
	EffectData data = CompileSpirv( RT_LIBRARY_EFFECT );
	ASSERT_EQ( data.techniques[0].libraries.size(), 1u );
	auto reflection = ReflectLibrary( data.techniques[0].libraries[0] );

	ASSERT_EQ( reflection.entryPoints.size(), 2u );
	auto entryPoints = EntryPoints( reflection );
	EXPECT_EQ( entryPoints["RayGen"], SPIRV_EXECUTION_MODEL_RAY_GENERATION );
	EXPECT_EQ( entryPoints["Miss"], SPIRV_EXECUTION_MODEL_MISS );

	// Exactly one acceleration structure binding, and it is Scene: the reflection calls it an
	// SRV_BUFFER, the module knows better.
	std::vector<std::string> accelerationStructures;
	for( auto& binding : reflection.bindings )
	{
		if( binding.accelerationStructure )
		{
			accelerationStructures.push_back( reflection.NameOf( binding.variable ) );
			EXPECT_EQ( binding.set, uint32_t( Tr2VulkanBindingABI::DESCRIPTOR_SET_RESOURCES ) );
		}
	}
	ASSERT_EQ( accelerationStructures.size(), 1u );
	EXPECT_EQ( accelerationStructures[0], "Scene" );

	// One record, LocalData { float4 Tint; float4x4 Transform; float4 Weights[2]; }: 112 bytes,
	// 28 scalars. The column_major matrix is walked by HLSL row: its first row is one scalar from
	// each of the four 16-byte columns.
	ASSERT_EQ( reflection.shaderRecords.size(), 1u );
	auto& record = reflection.shaderRecords[0];
	EXPECT_EQ( reflection.NameOf( record.variable ), "LocalData" );
	EXPECT_TRUE( record.laidOut );
	EXPECT_EQ( record.size, 112u );
	EXPECT_EQ( reflection.ShaderRecordSize(), 112u );
	auto offsets = ScalarOffsets( record );
	ASSERT_EQ( offsets.size(), 28u );
	EXPECT_EQ( std::vector<uint32_t>( offsets.begin(), offsets.begin() + 9 ), ( std::vector<uint32_t>{ 0, 4, 8, 12, 16, 32, 48, 64, 20 } ) );
	EXPECT_EQ( offsets[20], 80u ); // Weights[0]
	EXPECT_EQ( offsets[24], 96u ); // Weights[1]: ArrayStride 16
	for( auto& slot : record.scalars )
	{
		EXPECT_EQ( slot.size, 4u );
	}
	// Which entry point reads it: the interface lists the record's variable.
	for( auto& entryPoint : reflection.entryPoints )
	{
		const bool readsRecord = std::find( entryPoint.interfaceIds.begin(), entryPoint.interfaceIds.end(), record.variable ) != entryPoint.interfaceIds.end();
		EXPECT_EQ( readsRecord, entryPoint.name == "RayGen" ) << entryPoint.name;
	}
}

TEST( SpirvReflectVulkan, BothMatrixPackingsAreWalkedByHlslRow )
{
	// float4x3 ColumnMajor (three 16-byte columns), row_major float3x4 RowMajor (three 16-byte
	// rows) at 48, float4 Tail at 96.
	EffectData data = CompileSpirv( RT_MATRIX_LOCAL_CBUFFER_EFFECT );
	auto reflection = ReflectLibrary( data.techniques[0].libraries[0] );
	ASSERT_EQ( reflection.shaderRecords.size(), 1u );
	auto& record = reflection.shaderRecords[0];
	EXPECT_TRUE( record.laidOut );
	EXPECT_EQ( record.size, 112u );
	auto offsets = ScalarOffsets( record );
	ASSERT_EQ( offsets.size(), 28u );
	EXPECT_EQ( std::vector<uint32_t>( offsets.begin(), offsets.begin() + 4 ), ( std::vector<uint32_t>{ 0, 16, 32, 4 } ) );
	EXPECT_EQ( std::vector<uint32_t>( offsets.begin() + 12, offsets.begin() + 17 ), ( std::vector<uint32_t>{ 48, 52, 56, 60, 64 } ) );
	EXPECT_EQ( offsets[24], 96u );
}

TEST( SpirvReflectVulkan, AnArrayInARecordUsesItsArrayStride )
{
	EffectData data = CompileSpirv( RT_ARRAY_LOCAL_CBUFFER_EFFECT );
	auto reflection = ReflectLibrary( data.techniques[0].libraries[0] );
	ASSERT_EQ( reflection.shaderRecords.size(), 1u );
	auto& record = reflection.shaderRecords[0];
	EXPECT_TRUE( record.laidOut );
	EXPECT_EQ( record.size, 52u );
	auto offsets = ScalarOffsets( record );
	ASSERT_EQ( offsets.size(), 13u );
	EXPECT_EQ( offsets[4], 16u );
	EXPECT_EQ( offsets[8], 32u );
	EXPECT_EQ( offsets[12], 48u );
}

TEST( SpirvReflectVulkan, APassShaderHasOneEntryPointPerStageAndNoRecord )
{
	EffectData data = CompileSpirv( SIMPLE_EFFECT );
	auto& stages = data.techniques[0].passes[0].stages;
	ASSERT_EQ( stages.size(), 2u );
	auto vertex = ReflectStage( stages[0] );
	auto pixel = ReflectStage( stages[1] );
	ASSERT_EQ( vertex.entryPoints.size(), 1u );
	ASSERT_EQ( pixel.entryPoints.size(), 1u );
	EXPECT_EQ( vertex.entryPoints[0].executionModel, 0u ); // Vertex
	EXPECT_EQ( pixel.entryPoints[0].executionModel, 4u ); // Fragment
	// Named after the HLSL function as the compiler's patching left it (_new_symbol_NNNN), which
	// is the name the AL hands vkCreateGraphicsPipelines.
	EXPECT_FALSE( vertex.entryPoints[0].name.empty() );
	EXPECT_FALSE( pixel.entryPoints[0].name.empty() );
	EXPECT_TRUE( vertex.shaderRecords.empty() );
	EXPECT_EQ( vertex.ShaderRecordSize(), 0u );
	EXPECT_FALSE( pixel.bindings.empty() );
	for( auto& binding : pixel.bindings )
	{
		EXPECT_FALSE( binding.accelerationStructure );
	}
}

TEST( SpirvReflectVulkan, AModuleItCannotReadIsRefused )
{
	EffectData data = CompileSpirv( SIMPLE_EFFECT );
	auto& stage = data.techniques[0].passes[0].stages[1];
	const char* bytes = g_stringTable.GetString( stage.shaderDataStr );
	std::vector<uint32_t> words( stage.shaderSize / 4 );
	memcpy( words.data(), bytes, words.size() * 4 );
	SpirvReflection reflection;

	EXPECT_FALSE( SpirvReflectVulkan::ReflectSpirv( nullptr, 64, reflection ) );
	EXPECT_FALSE( SpirvReflectVulkan::ReflectSpirv( words.data(), 5 * 4, reflection ) ) << "a header and nothing else";
	EXPECT_FALSE( SpirvReflectVulkan::ReflectSpirv( words.data(), words.size() * 4 - 2, reflection ) ) << "not a whole number of words";

	auto wrongMagic = words;
	wrongMagic[0] = 0x03022307u; // byte-reversed
	EXPECT_FALSE( SpirvReflectVulkan::ReflectSpirv( wrongMagic.data(), wrongMagic.size() * 4, reflection ) );

	// An instruction that claims to run past the end: refused, with what came before kept.
	ASSERT_GT( words.size(), 6u );
	auto truncated = words;
	const size_t last = truncated.size() - 1; // OpFunctionEnd, one word
	truncated[last] = ( truncated[last] & 0xffffu ) | ( 0x7fffu << 16 );
	EXPECT_FALSE( SpirvReflectVulkan::ReflectSpirv( truncated.data(), truncated.size() * 4, reflection ) );
	auto zeroLength = words;
	zeroLength[5] = 0;
	EXPECT_FALSE( SpirvReflectVulkan::ReflectSpirv( zeroLength.data(), zeroLength.size() * 4, reflection ) );
	EXPECT_TRUE( reflection.entryPoints.empty() );

	EXPECT_TRUE( SpirvReflectVulkan::ReflectSpirv( words.data(), words.size() * 4, reflection ) );
	EXPECT_EQ( reflection.entryPoints.size(), 1u );
}

// Shader records whose types a corrupt module could make cost without end: a vector of 2^32 - 1
// floats, an array of 2^32 - 1 empty structs, and structs of 64 structs of 64 ... (64^6 visits).
// Each is read, quickly, as a record that cannot be laid out.
TEST( SpirvReflectVulkan, ARecordTypeTooLargeToWalkIsNotLaidOut )
{
	const uint32_t SHADER_RECORD_BUFFER = 5343;
	auto op = []( uint32_t opcode, std::vector<uint32_t> operands ) {
		operands.insert( operands.begin(), ( uint32_t( operands.size() + 1 ) << 16 ) | opcode );
		return operands;
	};
	std::vector<uint32_t> words = { 0x07230203u, 0x00010600u, 0, 1000, 0 };
	auto add = [&words]( const std::vector<uint32_t>& instruction ) { words.insert( words.end(), instruction.begin(), instruction.end() ); };
	auto record = [&]( uint32_t pointer, uint32_t variable, uint32_t type ) {
		add( op( 32, { pointer, SHADER_RECORD_BUFFER, type } ) ); // OpTypePointer
		add( op( 59, { pointer, variable, SHADER_RECORD_BUFFER } ) ); // OpVariable
	};

	add( op( 22, { 1, 32 } ) ); // OpTypeFloat %1 32
	add( op( 23, { 2, 1, 0xffffffffu } ) ); // OpTypeVector %2 %1 2^32-1
	add( op( 30, { 3, 2 } ) ); // OpTypeStruct %3 { %2 }
	record( 4, 5, 3 );

	add( op( 21, { 10, 32, 0 } ) ); // OpTypeInt %10 32 unsigned
	add( op( 43, { 10, 11, 0xffffffffu } ) ); // OpConstant %11 2^32-1
	add( op( 30, { 12 } ) ); // OpTypeStruct %12 {}
	add( op( 28, { 13, 12, 11 } ) ); // OpTypeArray %13 %12[%11]
	add( op( 71, { 13, 6, 16 } ) ); // OpDecorate %13 ArrayStride 16
	add( op( 30, { 14, 13 } ) ); // OpTypeStruct %14 { %13 }
	record( 15, 16, 14 );

	uint32_t inner = 12; // the empty struct
	for( uint32_t level = 0; level < 6; ++level )
	{
		std::vector<uint32_t> operands( 65, inner ); // OpTypeStruct %id { inner x 64 }
		operands[0] = 20 + level;
		add( op( 30, operands ) );
		inner = operands[0];
	}
	add( op( 30, { 30, inner } ) ); // OpTypeStruct %30 { the nest }
	record( 31, 32, 30 );

	SpirvReflection reflection;
	EXPECT_TRUE( SpirvReflectVulkan::ReflectSpirv( words.data(), words.size() * 4, reflection ) );
	ASSERT_EQ( reflection.shaderRecords.size(), 3u );
	for( auto& block : reflection.shaderRecords )
	{
		EXPECT_FALSE( block.laidOut ) << reflection.NameOf( block.variable );
		EXPECT_LE( block.scalars.size(), size_t( 1 ) << 16 );
	}
}
