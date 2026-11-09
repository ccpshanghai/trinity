// Copyright © 2023 CCP ehf.

#pragma once

#if ( TRINITY_PLATFORM == TRINITY_METAL )

#include "../include/Tr2TextureAL.h"
#include "../Tr2HalHelperStructures.h"
#include "../Tr2MemoryCounterAL.h"
#include "MetalContext.h"

namespace TrinityALImpl
{
class Tr2TextureAL : public Tr2DeviceResourceAL<Tr2TextureAL>
{
public:
	Tr2TextureAL();
	~Tr2TextureAL();

	ALResult Create( const Tr2BitmapDimensions& desc, const Tr2MsaaDesc& msaa, Tr2GpuUsage::Type gpuUsage, Tr2CpuUsage::Type cpuUsage, Tr2SubresourceData* initialData, Tr2PrimaryRenderContextAL& renderContext );
	ALResult OpenShared( uintptr_t handle, Tr2GpuUsage::Type gpuUsage, Tr2PrimaryRenderContextAL& renderContext );
	void Destroy();

	bool IsValid() const;
	Tr2ALMemoryType GetMemoryClass() const;
	const Tr2BitmapDimensions& GetDesc() const;
	const Tr2MsaaDesc& GetMsaaDesc() const;
	Tr2GpuUsage::Type GetGpuUsage() const;
	Tr2CpuUsage::Type GetCpuUsage() const;

	ALResult MapForReading( const Tr2TextureSubresource& region, const void*& data, uint32_t& pitch, Tr2RenderContextAL& renderContext )
	{
		return MapForReading( region, true, data, pitch, renderContext );
	}
	ALResult MapForReading( const Tr2TextureSubresource& region, bool synchronize, const void*& data, uint32_t& pitch, Tr2RenderContextAL& renderContext );
	void UnmapForReading( Tr2RenderContextAL& renderContext );
	ALResult MapForWriting( const Tr2TextureSubresource& region, void*& data, uint32_t& pitch, Tr2RenderContextAL& renderContext );
	void UnmapForWriting( Tr2RenderContextAL& renderContext );

	ALResult UpdateSubresource( const Tr2TextureSubresource& region, const void* source, uint32_t pitch, uint32_t slicePitch, Tr2RenderContextAL& renderContext );
	ALResult CopySubresourceRegion( const Tr2TextureSubresource& destSubresource, Tr2TextureAL& source, const Tr2TextureSubresource& sourceSubresource, Tr2RenderContextAL& renderContext );
	ALResult GenerateMipMaps( Tr2RenderContextAL& renderContext );
	ALResult Resolve( Tr2TextureAL& destination, Tr2RenderContextAL& renderContext );
	uintptr_t GetSharedHandle() const;
	void Describe( Tr2DeviceResourceDescriptionAL& description ) const;
	const char* GetName() const;
	ALResult SetName( const char* name );

	id<MTLTexture> GetMetalTexture()
	{
		return m_mtlTexture;
	};
	id<MTLTexture> GetSRGBViewMetalTexture();
	id<MTLTexture> GetUAVMetalTexture( uint32_t mipLevel );

	void AssignFromTexture( Tr2TextureAL& backBuffer );

	uint32_t GetSrvIndexInHeap( Tr2RenderContextEnum::ColorSpace colorSpace = Tr2RenderContextEnum::COLOR_SPACE_LINEAR ) const;
	uint32_t GetUavIndexInHeap( uint32_t mip ) const;

	// Debug-only: true once BcDecompress has actually run during this texture's
	// Create() (spec D8). Lets tests tell "GetFormat reports BGRA8" apart from
	// "the decompress path really executed" -- without this, a stub that
	// hardcoded the fallback format without ever calling BcDecompress would
	// still pass the format-only assertion. Reached from tests via
	// TrinityALImpl_GetObject() -- a pre-existing production escape hatch (used
	// elsewhere to reach real backend resources, e.g. GetMetalTexture() below),
	// repurposed here as the channel for a test-only introspection point. No
	// new production-facing API is added to Tr2TextureAL itself.
	bool DebugDecompressedOnCreate() const
	{
		return m_debugDecompressedOnCreate;
	}

	// Debug-only: for each (slice, mip) processed by the decompression branch
	// of Create(), the m_sysMem of the Tr2SubresourceData source it actually
	// consumed, in iteration order. Exists to prove per-slice content isn't
	// corrupted by an indexing mistake (the initialData[mip]-vs-[index] defect
	// D7's widened trigger made reachable for arrays/cubes) -- MapForReading
	// cannot substitute for this because it only ever supports face 0
	// (CCP_ASSERT( region.m_startFace == 0 && region.m_endFace == 1 )).
	const std::vector<const void*>& DebugDecompressedSources() const
	{
		return m_debugDecompressedSources;
	}

private:
	Tr2BitmapDimensions m_desc;
	Tr2MsaaDesc m_msaa;
	Tr2GpuUsage::Type m_gpuUsage;
	Tr2CpuUsage::Type m_cpuUsage;
	Tr2SubresourceData m_dataLayout;

	Tr2TextureSubresource m_mappedRegion;

	struct MappedRange
	{
		uint32_t offset;
		uint32_t size;
		uint64_t frame;
	};

	id<MTLBuffer> m_mtlWriteBuffer;
	std::vector<MappedRange> m_mappedRanges;
	MappedRange m_mappedRange;

	id<MTLBuffer> m_mtlReadBackBuffer;
	id<MTLTexture> m_mtlTexture;
	id<MTLTexture> m_mtlTextureSRGBView;
	uint32_t m_srvHeapIndices[2];
	std::vector<id<MTLTexture>> m_mtlTextureUAV;
	std::vector<uint32_t> m_uavHeapIndices;

	uint64_t m_usedInEncoder;

	MetalContext* m_metalContext;
	std::string m_name;
	Tr2MemoryCounterAL m_memory;
	bool m_wrappedTexture;
	bool m_debugDecompressedOnCreate;
	std::vector<const void*> m_debugDecompressedSources;
	friend class ::Tr2RenderContextAL;
};
}

#endif
