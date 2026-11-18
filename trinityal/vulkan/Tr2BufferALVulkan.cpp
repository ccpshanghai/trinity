// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2BufferALVulkan.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "UtilitiesVulkan.h"
#include "../ALLog.h"
#include <algorithm>


namespace TrinityALImpl
{
	namespace
	{
		// Slots a renaming buffer starts with; doubled whenever the next slot is still in
		// flight, capped so a runaway caller cannot take the device. Tr2Blitter's quad
		// settles at 32 on the S23 (ten maps a frame, three frames in flight).
		const uint32_t INITIAL_RENAME_SLOTS = 4;
		const uint32_t MAX_RENAME_SLOTS = 4096;
	}

	Tr2BufferAL::Tr2BufferAL()
		:m_buffer( VK_NULL_HANDLE ),
		m_memory( VK_NULL_HANDLE ),
		m_bufferView( VK_NULL_HANDLE ),
		m_owner( nullptr ),
		m_mapped( nullptr ),
		m_stagingBuffer( VK_NULL_HANDLE ),
		m_stagingOffset( 0 ),
		m_renames( false ),
		m_slotCount( 1 ),
		m_slot( 0 ),
		m_slotStride( 0 ),
		m_usage( 0 ),
		m_memoryProperty( VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT )
	{

	}

	Tr2BufferAL::~Tr2BufferAL()
	{
		Destroy();
	}

	ALResult Tr2BufferAL::Create(
		const Tr2BufferDescriptionAL& desc,
		const void* initialData,
		Tr2PrimaryRenderContextAL& renderContext )
	{
		Destroy();

		if( desc.count == 0 )
		{
			return E_INVALIDARG;
		}

		if( !renderContext.IsValid() )
		{
			return E_INVALIDCALL;
		}

		bool isImmutable = !HasFlag( desc.cpuUsage, Tr2CpuUsage::WRITE ) && !HasFlag( desc.gpuUsage, Tr2GpuUsage::UNORDERED_ACCESS );
		if( isImmutable && !initialData )
		{
			return E_INVALIDARG;
		}

		// WRITE_OFTEN, not WRITE. Both the dx12 and the metal backend reject exactly
		// READ | WRITE_OFTEN here -- a buffer that is read back *and* rewritten every
		// frame has no single memory home on those APIs -- and this port dropped the
		// _OFTEN. READ | WRITE on its own is ordinary host-visible memory, which is what
		// the branch below gives it, and what MapForReading, MapForWriting and
		// UpdateBuffer all already handle.
		//
		// The cost of the missing four letters: Tr2SuballocatedBuffer asks for exactly
		// READ | WRITE, so g_sharedBuffer -- the one buffer every TriGeometryRes vertex
		// and index allocation comes out of -- could never be created on Vulkan.
		// CreateLodFromCMFMesh then returned false without a word, so an EVE hull loaded
		// its .cmf, its .red and its eight .dds and drew nothing at all. The only trace
		// in a 20-second log was one line about six quads.
		if( HasFlag( desc.cpuUsage, Tr2CpuUsage::READ ) && HasFlag( desc.cpuUsage, Tr2CpuUsage::WRITE_OFTEN ) )
		{
			return E_INVALIDARG;
		}

		auto stride = desc.stride;
		if( desc.format != Tr2RenderContextEnum::PIXEL_FORMAT_UNKNOWN )
		{
			stride = GetBytesPerPixel( desc.format );
		}

		if( HasFlag( desc.gpuUsage, Tr2GpuUsage::INDEX_BUFFER ) && stride != 2 && stride != 4 )
		{
			return E_INVALIDARG;
		}

		auto size = desc.count * stride;

		// TRANSFER_DST unconditionally: UpdateBuffer accepts any buffer regardless of
		// cpuUsage (the dx12 backend imposes no cpuUsage requirement there), and for a
		// device-local buffer the only way to service that is a staging copy into it.
		// Deciding the bit from initialData/WRITE here left exactly those buffers
		// without it. The bit is always legal on a VkBuffer and costs nothing.
		VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if( HasFlag( desc.gpuUsage, Tr2GpuUsage::INDEX_BUFFER ) )
		{
			usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		}
		if( HasFlag( desc.gpuUsage, Tr2GpuUsage::VERTEX_BUFFER ) )
		{
			usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		}
		if( HasFlag( desc.gpuUsage, Tr2GpuUsage::DRAW_INDIRECT_ARGS ) )
		{
			usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		}
		const bool isTypedBuffer = desc.format != Tr2RenderContextEnum::PIXEL_FORMAT_UNKNOWN;
		if( HasFlag( desc.gpuUsage, Tr2GpuUsage::SHADER_RESOURCE ) )
		{
			usage |= isTypedBuffer ? VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		}
		if( HasFlag( desc.gpuUsage, Tr2GpuUsage::UNORDERED_ACCESS ) )
		{
			usage |= isTypedBuffer ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		}

		// CreateBuffer takes VkMemoryPropertyFlagBits, not VkMemoryPropertyFlags, so a
		// combined mask needs the explicit conversion -- C++ will not convert an integer
		// to an enumeration implicitly. Tr2ConstantBufferALVulkan.cpp does the same.
		//
		// HOST_COHERENT is requested alongside HOST_VISIBLE deliberately: it is what lets
		// every write path below skip vkFlushMappedMemoryRanges, and flush ranges are the
		// one thing here with a nonCoherentAtomSize alignment rule that a caller-supplied
		// offset/size cannot be trusted to satisfy.
		const bool isHostVisible = HasFlag( desc.cpuUsage, Tr2CpuUsage::READ ) || HasFlag( desc.cpuUsage, Tr2CpuUsage::WRITE );
		const VkMemoryPropertyFlagBits memoryProperty = isHostVisible
			? VkMemoryPropertyFlagBits( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT )
			: VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

		// A synchronized, host-written buffer that is only ever a vertex or index buffer
		// RENAMES on MapForWriting: the map takes a fresh slot of a small ring, the way
		// Tr2ConstantBufferAL::Lock does, and the draw re-binds at the new offset. No copy,
		// no barrier, no render pass closed -- which is what the copy path costs, ten times a
		// frame for Tr2Blitter's screen quad, and a quarter of shippreview's frame rate on the
		// S23 (docs/spikes/artifacts/m6-android-flicker-20260914.txt). Buffers a descriptor
		// can name (SHADER_RESOURCE, UNORDERED_ACCESS, DRAW_INDIRECT_ARGS, typed) keep the
		// copy path, because a written descriptor set would not follow a moved buffer; so do
		// readable ones, whose MapForReading addresses the buffer at 0.
		const bool renames = HasFlag( desc.cpuUsage, Tr2CpuUsage::WRITE )
			&& !HasFlag( desc.cpuUsage, Tr2CpuUsage::READ )
			&& !NonSynchronizedVulkan( desc )
			&& !isTypedBuffer
			&& !HasFlag( desc.gpuUsage, Tr2GpuUsage::SHADER_RESOURCE )
			&& !HasFlag( desc.gpuUsage, Tr2GpuUsage::UNORDERED_ACCESS )
			&& !HasFlag( desc.gpuUsage, Tr2GpuUsage::DRAW_INDIRECT_ARGS )
			&& ( HasFlag( desc.gpuUsage, Tr2GpuUsage::VERTEX_BUFFER ) || HasFlag( desc.gpuUsage, Tr2GpuUsage::INDEX_BUFFER ) );
		// 16-byte slots keep every slot start legal for an index buffer of either stride.
		const VkDeviceSize slotStride = renames ? ( VkDeviceSize( size ) + 15 ) & ~VkDeviceSize( 15 ) : VkDeviceSize( size );
		const uint32_t slotCount = renames ? INITIAL_RENAME_SLOTS : 1;

		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		FORWARD_HR( CreateBuffer( buffer, memory, size_t( slotStride * slotCount ), usage, memoryProperty, renderContext ) );
		ON_BLOCK_EXIT( [&] {
			
			if( buffer ) renderContext.DestroyLaterVulkan( buffer, &vkDestroyBuffer );
			if( memory ) renderContext.DestroyLaterVulkan( memory, &vkFreeMemory );
		} );

		if( initialData )
		{
			if( isHostVisible )
			{
				// No flush: this allocation is HOST_COHERENT by construction above.
				void* mapped = nullptr;
				CR_RETURN_HR( Vk2Al( vkMapMemory( renderContext.m_device, memory, 0, size, 0, &mapped ) ) );
				memcpy( mapped, initialData, size );
				vkUnmapMemory( renderContext.m_device, memory );
			}
			else
			{
			VkBuffer stagingBuffer = VK_NULL_HANDLE;
			VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
			FORWARD_HR( CreateBuffer( stagingBuffer, stagingMemory, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, renderContext ) );

			ON_BLOCK_EXIT( [&] {
				if( stagingBuffer ) renderContext.DestroyLaterVulkan( stagingBuffer, &vkDestroyBuffer );
				if( stagingMemory ) renderContext.DestroyLaterVulkan( stagingMemory, &vkFreeMemory );
			} );


			void *mapped;
			// VK_WHOLE_SIZE rather than size, so the mapping ends where the allocation ends.
			// See the note on the identical flush in Update.
			CR_RETURN_HR( Vk2Al( vkMapMemory( renderContext.m_device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped ) ) );
			memcpy( mapped, initialData, size );

			VkMappedMemoryRange flushRange = {
				VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
				nullptr,
				stagingMemory,
				0,
				VK_WHOLE_SIZE
			};
			vkFlushMappedMemoryRanges( renderContext.m_device, 1, &flushRange );
			vkUnmapMemory( renderContext.m_device, stagingMemory );

			VkBufferCopy copyInfo = { 0, 0, size };
			// Transfer commands and pipeline barriers are illegal inside a render pass
			// instance. Nothing had a render target bound before PushRenderTarget existed, so
			// no pass was ever open here and this was unreachable; the moment it became
			// reachable it was VUID-vkCmdCopyBuffer-renderpass and
			// VUID-vkCmdPipelineBarrier-None-07889. The pass is reopened lazily by SetPass.
			renderContext.EndRenderPassVulkan();
			vkCmdCopyBuffer( renderContext.m_commandBuffer, stagingBuffer, buffer, 1, &copyInfo );

			VkBufferMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			// Vertex and index input, and the indirect-argument read: a buffer created
			// with INDIRECT_BUFFER usage is consumed by vkCmdDrawIndirect at a stage the
			// old VERTEX_INPUT-only barrier never covered. The 1.0 barrier had the same
			// gap; synchronization2's finer stages are what made the validator name it.
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer = buffer;
			barrier.offset = 0;
			barrier.size = VK_WHOLE_SIZE;
			VkDependencyInfo dependencyInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dependencyInfo.bufferMemoryBarrierCount = 1;
			dependencyInfo.pBufferMemoryBarriers = &barrier;
			renderContext.m_vkCmdPipelineBarrier2( renderContext.m_commandBuffer, &dependencyInfo );
			}
		}

		VkBufferView bufferView = VK_NULL_HANDLE;
		if( isTypedBuffer && ( HasFlag( desc.gpuUsage, Tr2GpuUsage::SHADER_RESOURCE ) || HasFlag( desc.gpuUsage, Tr2GpuUsage::UNORDERED_ACCESS ) ) )
		{
			VkBufferViewCreateInfo viewInfo = {
				VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
				nullptr,
				0,
				buffer,
				GetVulkanFormat( desc.format ),
				0,
				VK_WHOLE_SIZE
			};
			CR_RETURN_HR( Vk2Al( vkCreateBufferView( renderContext.m_device, &viewInfo, nullptr, &bufferView ) ) );
		}

		m_buffer = buffer;
		buffer = VK_NULL_HANDLE;
		m_memory = memory;
		memory = VK_NULL_HANDLE;
		m_bufferView = bufferView;
		bufferView = VK_NULL_HANDLE;

		m_owner = &renderContext;
		m_desc = desc;
		m_usage = usage;
		m_memoryProperty = memoryProperty;
		m_renames = renames;
		m_slotStride = slotStride;
		m_slotCount = slotCount;
		m_slot = 0;
		m_slotFrame.assign( slotCount, 0 );
		if( initialData )
		{
			// Slot 0 holds the initial contents and is this frame's, so a map in the same
			// frame does not hand it straight back out.
			m_slotFrame[0] = renderContext.GetRecordingFrameNumber();
		}

		return S_OK;
	}

	// A bigger ring, empty. The old allocation may still be read by a frame in flight --
	// including a draw recorded earlier in THIS frame against the old handle -- so
	// DestroyLater holds it until that frame's fence says otherwise. Contents are not
	// carried over: a rename ring is DISCARD memory, nothing in it outlives its map.
	ALResult Tr2BufferAL::AllocateSlotsVulkan( uint32_t slotCount )
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		FORWARD_HR( CreateBuffer( buffer, memory, size_t( m_slotStride * slotCount ), m_usage, m_memoryProperty, *m_owner ) );
		if( m_buffer )
		{
			m_owner->DestroyLaterVulkan( m_buffer, &vkDestroyBuffer );
		}
		if( m_memory )
		{
			m_owner->DestroyLaterVulkan( m_memory, &vkFreeMemory );
		}
		m_buffer = buffer;
		m_memory = memory;
		m_slotCount = slotCount;
		m_slot = 0;
		m_slotFrame.assign( slotCount, 0 );
		return S_OK;
	}

	void Tr2BufferAL::Destroy()
	{
		if( m_buffer )
		{
			// Freeing implicitly unmaps, but m_mapped has to be cleared with it or a
			// reused Tr2BufferAL would carry a stale "still mapped" into its next Create.
			// A synchronized map that was never unmapped points into the frame's upload
			// ring, which is not this buffer's to unmap; an in-place one is.
			if( m_mapped && m_stagingBuffer == VK_NULL_HANDLE )
			{
				vkUnmapMemory( m_owner->m_device, m_memory );
			}
			m_mapped = nullptr;
			m_stagingBuffer = VK_NULL_HANDLE;
			m_stagingOffset = 0;

			if( m_bufferView )
			{
				m_owner->DestroyLaterVulkan( m_bufferView, &vkDestroyBufferView );
				m_bufferView = VK_NULL_HANDLE;
			}

			m_owner->DestroyLaterVulkan( m_buffer, &vkDestroyBuffer );
			m_buffer = VK_NULL_HANDLE;

			m_owner->DestroyLaterVulkan( m_memory, &vkFreeMemory );
			m_memory = VK_NULL_HANDLE;

			m_owner = nullptr;
			m_desc = Tr2BufferDescriptionAL();
			m_renames = false;
			m_slotCount = 1;
			m_slot = 0;
			m_slotStride = 0;
			m_slotFrame.clear();
		}
	}

	bool Tr2BufferAL::IsValid() const
	{
		return m_buffer != VK_NULL_HANDLE;
	}

	Tr2ALMemoryType Tr2BufferAL::GetMemoryClass() const
	{
		return AL_MEMORY_VIDEO;
	}

	const Tr2BufferDescriptionAL& Tr2BufferAL::GetDesc() const
	{
		return m_desc;
	}

	// The buffer's own extent in bytes, by the same stride rule Create uses.
	static uint32_t ByteSize( const Tr2BufferDescriptionAL& desc )
	{
		auto stride = desc.stride;
		if( desc.format != Tr2RenderContextEnum::PIXEL_FORMAT_UNKNOWN )
		{
			stride = GetBytesPerPixel( desc.format );
		}
		return desc.count * stride;
	}

	// Two things are true of every map path below and neither is obvious.
	//
	// The device comes from m_owner, never from the caller's context. Tr2RenderContextAL
	// -- the base class these overrides take by reference -- has no m_device member at
	// all; only Tr2PrimaryRenderContextAL does. The memory belongs to the context that
	// created the buffer regardless, which is why dx12 also routes through *m_owner.
	//
	// m_mapped, not IsValid(), is what guards the unmaps. The AL's own tests call
	// UnmapForReading immediately after a MapForReading they expect to fail --
	// Buffer.LockingNonReadableBufferForReadingFails and its writing twin -- and
	// vkUnmapMemory on memory that is not currently mapped is undefined behaviour, not a
	// tolerated no-op. Keying off IsValid() sent both of those straight into it, and in
	// the reading case the allocation is DEVICE_LOCAL and was never mappable to begin
	// with. m_mapped also rejects a second Map while one is outstanding.
	ALResult Tr2BufferAL::MapForReading( const void*& data, Tr2RenderContextAL& renderContext )
	{
		return MapForReading( data, 0, ByteSize( m_desc ), renderContext );
	}
	ALResult Tr2BufferAL::MapForReading( const void*& data, uint32_t offset, uint32_t size, Tr2RenderContextAL& renderContext )
	{
		if( !IsValid() )
		{
			return E_INVALIDCALL;
		}
		if( !HasFlag( m_desc.cpuUsage, Tr2CpuUsage::READ ) )
		{
			return E_INVALIDCALL;
		}
		if( size == 0 || uint64_t( offset ) + size > ByteSize( m_desc ) )
		{
			return E_INVALIDARG;
		}
		if( m_mapped )
		{
			return E_INVALIDCALL;
		}

		// The GPU may still owe us this buffer's contents -- a compute shader that wrote
		// it through a UAV, or Create's own staging copy, is only recorded, not run.
		// Make those writes available to the host, then submit and wait. Without the
		// wait the map returns memory the GPU has not touched yet, which is what made
		// every Compute test read back zeros while reporting S_OK.
		VkBufferMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
		barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
		barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.buffer = m_buffer;
		barrier.offset = 0;
		barrier.size = VK_WHOLE_SIZE;
		VkDependencyInfo dependencyInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		dependencyInfo.bufferMemoryBarrierCount = 1;
		dependencyInfo.pBufferMemoryBarriers = &barrier;
		// Outside the pass for the same reason as the staging copies above; the
		// FlushAndSyncVulkan below would close it anyway, but not before this barrier.
		m_owner->EndRenderPassVulkan();
		m_owner->m_vkCmdPipelineBarrier2( m_owner->m_commandBuffer, &dependencyInfo );

		FORWARD_HR( m_owner->FlushAndSyncVulkan() );

		// No vkInvalidateMappedMemoryRanges: the allocation is HOST_COHERENT.
		void* mapped = nullptr;
		CR_RETURN_HR( Vk2Al( vkMapMemory( m_owner->m_device, m_memory, offset, size, 0, &mapped ) ) );
		m_mapped = mapped;
		data = mapped;
		return S_OK;
	}
	void Tr2BufferAL::UnmapForReading( Tr2RenderContextAL& renderContext )
	{
		if( m_mapped )
		{
			vkUnmapMemory( m_owner->m_device, m_memory );
			m_mapped = nullptr;
		}
	}
	// The engine's MapForWriting contract is D3D11's MAP_WRITE_DISCARD unless the buffer was
	// created NON_SYNCRONIZED_WRITE: the caller writes the whole buffer and the GPU work
	// already recorded against the old contents is not disturbed. dx12 spells that
	// Tr2LockType::SYNCHRONIZED; metal hands out a staging MTLBuffer and blits it into the
	// real one at unmap. Only the three ring buffers (Tr2DynamicRingBuffer, Tr2RingBuffer,
	// EveInstancedMeshManager) ask for NON_SYNCRONIZED_WRITE, and they manage their own
	// in-flight regions.
	//
	// This port mapped m_memory in place for every buffer. With three frames in flight,
	// that let the CPU overwrite a vertex buffer the GPU was still reading -- and
	// Tr2Blitter writes its one four-vertex screen quad once per blit, a dozen times a
	// frame, so whichever quad was in the buffer when the GPU fetched the final upscale's
	// vertices is the quad it drew. About one frame in 250 on the S23 Ultra fetched a quad
	// that was not the full-screen one, drew nothing over the cleared back buffer, and the
	// screen flickered: the scene gone for a frame, the HUD (its own per-frame buffers) in
	// place. Not a pass-to-pass barrier, not the HUD, not the post-process chain -- the
	// no-HUD and no-post-process runs flicker at the same rate
	// (docs/spikes/artifacts/m6-android-flicker-20260914.txt).
	//
	// TRINITY_BUFFER_MAP_SYNC=0 restores the in-place map for the A/B, and should go once
	// the numbers are accepted.
	bool Tr2BufferAL::NonSynchronizedVulkan( const Tr2BufferDescriptionAL& desc )
	{
		static const bool synchronizedMaps = []{
			const char* value = getenv( "TRINITY_BUFFER_MAP_SYNC" );
			return !( value && value[0] == '0' );
		}();
		return !synchronizedMaps || HasFlag( desc.cpuUsage, Tr2CpuUsage::NON_SYNCRONIZED_WRITE );
	}

	bool Tr2BufferAL::IsNonSynchronizedVulkan() const
	{
		return NonSynchronizedVulkan( m_desc );
	}

	// The copy from an upload allocation into this buffer, ordered against everything the
	// command buffer has already recorded and everything it records next. The barrier in
	// front is the write-after-read: a draw recorded earlier -- this frame or the last --
	// still reads the old contents, and a copy has no ordering against it without one; an
	// execution dependency is all a WAR needs, so no source access. The barrier behind is
	// Create's and UpdateBuffer's: the copy's write made visible to every consumer a buffer
	// created here can have, the shader stages included because SHADER_RESOURCE and
	// UNORDERED_ACCESS buffers come through the same path.
	void Tr2BufferAL::RecordStagingCopyVulkan( VkBuffer staging, VkDeviceSize stagingOffset, uint32_t offset, uint32_t size )
	{
		// Transfer commands and barriers are illegal inside a render pass instance; the
		// pass is reopened lazily by SetPass, as for every other copy in this file. On a
		// tiler that restart is a store and a reload of the open target, so the draw log
		// counts how often a copy lands while a pass is open.
		if( DrawLogEnabledVulkan() )
		{
			NoteSyncMapVulkan( size, m_owner->IsRenderingActiveVulkan() );
		}
		m_owner->EndRenderPassVulkan();

		const VkPipelineStageFlags2 consumerStages =
			VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
			VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

		VkBufferMemoryBarrier2 before = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
		before.srcStageMask = consumerStages;
		before.srcAccessMask = 0;
		before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer = m_buffer;
		before.offset = offset;
		before.size = size;
		VkDependencyInfo beforeInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		beforeInfo.bufferMemoryBarrierCount = 1;
		beforeInfo.pBufferMemoryBarriers = &before;
		m_owner->m_vkCmdPipelineBarrier2( m_owner->m_commandBuffer, &beforeInfo );

		VkBufferCopy copyInfo = { stagingOffset, offset, size };
		vkCmdCopyBuffer( m_owner->m_commandBuffer, staging, m_buffer, 1, &copyInfo );

		VkBufferMemoryBarrier2 after = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
		after.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		after.dstStageMask = consumerStages;
		after.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
			VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;
		after.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		after.buffer = m_buffer;
		after.offset = offset;
		after.size = size;
		VkDependencyInfo afterInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		afterInfo.bufferMemoryBarrierCount = 1;
		afterInfo.pBufferMemoryBarriers = &after;
		m_owner->m_vkCmdPipelineBarrier2( m_owner->m_commandBuffer, &afterInfo );
	}

	ALResult Tr2BufferAL::MapForWriting( void*& data, Tr2RenderContextAL& renderContext )
	{
		if( !IsValid() )
		{
			return E_INVALIDCALL;
		}
		if( !HasFlag( m_desc.cpuUsage, Tr2CpuUsage::WRITE ) )
		{
			return E_INVALIDCALL;
		}
		if( m_mapped )
		{
			return E_INVALIDCALL;
		}

		if( IsNonSynchronizedVulkan() )
		{
			// The caller promised to write only what nothing in flight is reading.
			void* mapped = nullptr;
			CR_RETURN_HR( Vk2Al( vkMapMemory( m_owner->m_device, m_memory, 0, VK_WHOLE_SIZE, 0, &mapped ) ) );
			m_mapped = mapped;
			data = mapped;
			return S_OK;
		}

		if( m_renames )
		{
			// A fresh slot: the next one round the ring if the frame that took it has
			// rendered, a bigger ring otherwise. Tr2ConstantBufferAL::Lock, exactly.
			uint32_t next = ( m_slot + 1 ) % m_slotCount;
			const uint64_t rendered = m_owner->GetRenderedFrameNumber();
			if( m_slotFrame[next] != 0 && m_slotFrame[next] > rendered )
			{
				if( m_slotCount < MAX_RENAME_SLOTS )
				{
					FORWARD_HR( AllocateSlotsVulkan( std::min( m_slotCount * 2, MAX_RENAME_SLOTS ) ) );
					next = 0;
				}
				else
				{
					static bool logged = false;
					if( !logged )
					{
						logged = true;
						CCP_AL_LOGERR( "Tr2BufferAL: %u rename slots of %u bytes are all in flight; reusing one, and a draw may read the wrong vertices", m_slotCount, ByteSize( m_desc ) );
					}
				}
			}
			m_slot = next;
			m_slotFrame[m_slot] = m_owner->GetRecordingFrameNumber();
			void* mapped = nullptr;
			CR_RETURN_HR( Vk2Al( vkMapMemory( m_owner->m_device, m_memory, GetOffsetVulkan(), ByteSize( m_desc ), 0, &mapped ) ) );
			m_mapped = mapped;
			data = mapped;
			return S_OK;
		}

		// A range of the frame's upload ring for this write; UnmapForWriting copies it into
		// the buffer at the command buffer's current position. Its contents start undefined,
		// which is what DISCARD promises. The ring is HOST_COHERENT and stays mapped, so
		// there is nothing to flush or unmap.
		VkBuffer staging = VK_NULL_HANDLE;
		VkDeviceSize stagingOffset = 0;
		void* mapped = nullptr;
		FORWARD_HR( m_owner->AllocateUploadVulkan( ByteSize( m_desc ), staging, stagingOffset, mapped ) );
		m_stagingBuffer = staging;
		m_stagingOffset = stagingOffset;
		m_mapped = mapped;
		data = mapped;
		return S_OK;
	}
	void Tr2BufferAL::UnmapForWriting( Tr2RenderContextAL& renderContext )
	{
		if( !m_mapped )
		{
			return;
		}
		if( m_stagingBuffer == VK_NULL_HANDLE )
		{
			// No vkFlushMappedMemoryRanges: the allocation is HOST_COHERENT. Flushing a
			// caller-shaped range here would also have to satisfy nonCoherentAtomSize, which
			// an arbitrary offset/size does not.
			vkUnmapMemory( m_owner->m_device, m_memory );
			m_mapped = nullptr;
			return;
		}

		m_mapped = nullptr;
		RecordStagingCopyVulkan( m_stagingBuffer, m_stagingOffset, 0, ByteSize( m_desc ) );
		// The ring range is the frame's; it is reused once the frame's fence has passed.
		m_stagingBuffer = VK_NULL_HANDLE;
		m_stagingOffset = 0;
	}

	ALResult Tr2BufferAL::UpdateBuffer( uint32_t offset, uint32_t size, const void* data, Tr2RenderContextAL & renderContext )
	{
		if( !IsValid() )
		{
			return E_INVALIDCALL;
		}
		const uint32_t byteSize = ByteSize( m_desc );
		if( size == 0 )
		{
			// dx12's convention, matched deliberately: zero means the whole buffer.
			size = byteSize;
		}
		if( uint64_t( offset ) + size > byteSize )
		{
			return E_INVALIDARG;
		}
		if( m_mapped )
		{
			return E_INVALIDCALL;
		}

		// In place only for a buffer whose caller manages its own in-flight regions. A
		// synchronized host-visible buffer takes the staging copy below like a device-local
		// one: the copy is ordered in the command buffer, the map would not be. See
		// MapForWriting.
		const bool hostVisible = HasFlag( m_desc.cpuUsage, Tr2CpuUsage::READ ) || HasFlag( m_desc.cpuUsage, Tr2CpuUsage::WRITE );
		if( hostVisible && IsNonSynchronizedVulkan() )
		{
			void* mapped = nullptr;
			CR_RETURN_HR( Vk2Al( vkMapMemory( m_owner->m_device, m_memory, offset, size, 0, &mapped ) ) );
			memcpy( mapped, data, size );
			vkUnmapMemory( m_owner->m_device, m_memory );
			return S_OK;
		}

		// Everything else is a copy from the frame's upload ring: device-local memory
		// cannot be mapped at all, and a synchronized host-visible buffer must not be
		// written under a draw still reading it. The copy is ordered in the command buffer;
		// a map would not be.
		VkBuffer staging = VK_NULL_HANDLE;
		VkDeviceSize stagingOffset = 0;
		void* mapped = nullptr;
		FORWARD_HR( m_owner->AllocateUploadVulkan( size, staging, stagingOffset, mapped ) );
		memcpy( mapped, data, size );
		RecordStagingCopyVulkan( staging, stagingOffset, uint32_t( GetOffsetVulkan() ) + offset, size );
		return S_OK;
	}

	uint32_t Tr2BufferAL::GetSrvIndexInHeap() const
	{
		return 0xffffffff;
	}

	uint32_t Tr2BufferAL::GetUavIndexInHeap() const
	{
		return 0xffffffff;
	}

	void Tr2BufferAL::Describe( Tr2DeviceResourceDescriptionAL& description ) const
	{
		description["type"] = "Tr2BufferAL";
		description["size"] = std::to_string( GetDesc().count * GetDesc().stride );
	}

	ALResult Tr2BufferAL::SetName( const char* )
	{
		return E_NOTIMPL;
	}
}


#endif