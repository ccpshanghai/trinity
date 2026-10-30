// Copyright © 2026 CCP ehf.

#pragma once

// Vulkan ray tracing libraries: the shader record a group's local constant buffer becomes, checked
// against the D3D constant buffer the engine fills. Two halves, kept apart:
//
// - the layout comparison (CompareShaderRecordLayout and the D3D packing helpers it uses), which
//   knows nothing of the engine: does this record hold every scalar of this cbuffer at the same
//   byte, scalar for scalar;
// - the engine's policy (CheckShaderRecords): one local cbuffer per library, and a record that
//   fits in what every Vulkan device takes, with the layout comparison applied to each record.
//
// Used by EffectCompilerDX11::CompileLibrarySpirv; declared here so the tests can call the pieces
// directly. The SPIR-V side comes from trinityal/vulkan/SpirvReflectVulkan.h, the reader the AL
// sizes the same record with (vulkan-ray-tracing spec, section 3.3). The D3D reflection types come
// from stdafx.h, the precompiled header every shader compiler source starts with.

#include "trinityal/vulkan/SpirvReflectVulkan.h"

#include <string>
#include <vector>

struct Library;

namespace RtShaderRecordLayout
{
using SpirvReflectVulkan::SpirvReflection;
using SpirvReflectVulkan::SpirvScalarSlot;

// Vulkan's guaranteed minimum for maxShaderGroupStride, and its required shaderGroupHandleSize:
// the largest shader binding table record any conformant device takes is a 32-byte group handle
// followed by 4064 bytes of record data.
constexpr uint32_t VULKAN_SHADER_GROUP_HANDLE_SIZE = 32;
constexpr uint32_t VULKAN_MIN_MAX_SHADER_GROUP_STRIDE = 4096;

// D3D constant buffer packing, from the DXIL reflection. 64-bit integers are not modelled: a
// cbuffer holding one fails the layout comparison and is reported, never mis-copied.
uint32_t D3DElementSize( ID3D12ShaderReflectionType* type );
uint32_t D3DTotalSize( ID3D12ShaderReflectionType* type );

// Appends every scalar of `type` at `base` in SpirvReflection's record order -- declaration order,
// array elements outer, a matrix by HLSL row then column whatever its packing -- so the two lists
// compare index by index. Every array element starts a new 16-byte row; so does every struct and
// every matrix column (row, for row_major).
void D3DFlatten( ID3D12ShaderReflectionType* type, uint32_t base, std::vector<SpirvScalarSlot>& slots );

// The DXIL reflection of the cbuffer of that name in any of the library's functions, or null.
ID3D12ShaderReflectionConstantBuffer* FindLibraryCBuffer( ID3D12LibraryReflection* reflection, const std::string& name );

// Whether `record` holds every scalar of `cbuffer` at the same offset and width, in the same
// order. Equal offset sets are not enough: two members swapping places would pass a sorted
// comparison and copy each into the other. When they differ, `difference` says where (the first
// differing scalar and the top-level variable it belongs to, or the two scalar counts, or that
// the record could not be laid out).
bool CompareShaderRecordLayout( const SpirvReflection::ShaderRecordBlock& record, ID3D12ShaderReflectionConstantBuffer* cbuffer, std::string& difference );

// The contract a Vulkan ray tracing library's local cbuffers keep with the AL: the shader record
// after a group's handle is a byte copy of the library's one local constant buffer, as the
// engine filled it in D3D packing. Checked on the compiled module, because that is where
// "referenced by an entry point" and "laid out as" are decided:
//
// - One local cbuffer per library, not per entry point. The engine hands the AL one local
//   signature for the whole library (Library::localInputs, shared by every export the library
//   adds), and the AL lays out one inline record per group from it: the record is the bytes of
//   the one constant buffer that signature names, so Tr2RtPipelineStateAL refuses a signature
//   with two. A library whose entry points each read a different local cbuffer would compile
//   and then switch ray tracing off at run time. (Within one entry point DXC would accept two
//   as well, every ShaderRecordBufferKHR block starting at record offset 0, aliasing the same
//   bytes.)
// - Group handle + cbuffer fits VULKAN_MIN_MAX_SHADER_GROUP_STRIDE.
// - Same layout (CompareShaderRecordLayout). DXC lays a shader record out as a storage buffer
//   (std430: an array of floats has a 4-byte stride), not as a constant buffer (16), and no
//   -fvk-use-*-layout switch gives the D3D cbuffer rule to a shader record, so a cbuffer whose
//   two layouts differ is refused by name.
//
// Messages start with \memory(0), which CompileMessageQueue replaces with the effect's path.
bool CheckShaderRecords( const SpirvReflection& module, ID3D12LibraryReflection* reflection, const Library& library, const std::string& libraryName );
}
