// Copyright © 2026 CCP ehf.

#pragma once
#ifndef Tr2ResourceBindingsVulkan_h_
#define Tr2ResourceBindingsVulkan_h_

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include <cstdint>
#include <vector>

#include "../ALResult.h"
#include "../Tr2RenderContextEnum.h"
#include "../include/Tr2BufferAL.h"
#include "../include/Tr2RegisterMapAL.h"
#include "../include/Tr2SamplerStateAL.h"
#include "../include/Tr2TextureAL.h"

class Tr2PrimaryRenderContextAL;

namespace TrinityALImpl
{
class Tr2ShaderProgramAL;
}

// -------------------------------------------------------------
// Description:
//   The Vulkan side of upstream #90's direct binding model: shader resource, unordered
//   access and sampler bindings, stored in the slots of the shader program that was set
//   before them (the register map), and written into a fresh descriptor set 1 when a draw
//   or dispatch commits them. Same contract as Tr2ResourceBindingsDx11 and
//   Tr2ResourceBindingsMetal, which this follows line for line where it can:
//
//   - SetProgram drops everything set for the previous program.
//   - A commit seals the batch; the next Set* starts a new one and clears every slot, so
//     the engine sets the full binding for each draw, as on DX11.
//   - Unbound 2D texture and sampler slots read the primary context's dummies (black,
//     like the old default resource set). Any other unbound slot is left unwritten and
//     logged once: there is no honest 1x1 stand-in for a buffer or a UAV.
//
//   What is Vulkan's own:
//
//   - Descriptor sets are never rewritten once a command buffer may have bound them
//     (vkUpdateDescriptorSets on a bound set invalidates the command buffer). Each commit
//     that changes something allocates a new set from the pools of the frame being
//     recorded; a frame's pools are reset only once GetRenderedFrameNumber says that
//     frame is done. A set is reused only within the frame that allocated it.
//   - Images have layouts. The layout every bound image needs (SHADER_READ_ONLY_OPTIMAL,
//     DEPTH_STENCIL_READ_ONLY_OPTIMAL for a depth SRV, GENERAL for a storage image) is
//     known as soon as the bindings are, and barriers cannot be recorded inside a
//     rendering scope -- so the render context asks NeedsTransition before it opens one
//     and calls TransitionImages ahead of the attachments, exactly where the resource
//     set's transitions used to run.
// -------------------------------------------------------------
class Tr2ResourceBindings
{
public:
	Tr2ResourceBindings();
	~Tr2ResourceBindings();

	/** Select the program the following bindings are for; drops everything set for the previous one */
	void SetProgram( const TrinityALImpl::Tr2ShaderProgramAL* program ) throw();

	const TrinityALImpl::Tr2ShaderProgramAL* GetProgram() const
	{
		return m_program;
	}

	ALResult SetSrv( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex, const Tr2BufferAL& buffer ) throw();
	ALResult SetSrv( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex, const Tr2TextureAL& texture, Tr2RenderContextEnum::ColorSpace colorSpace ) throw();
	ALResult SetUav( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex, const Tr2BufferAL& buffer ) throw();
	ALResult SetUav( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex, const Tr2TextureAL& texture, uint32_t mip ) throw();
	ALResult SetSampler( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex, const Tr2SamplerStateAL& sampler ) throw();

	/** Drop everything that was set; the next commit writes a new set */
	ALResult Reset() throw();

	/** Drop everything, including the program and the last committed set */
	void Discard() throw();

	/** True if a bound image is not in the layout its descriptor will claim */
	bool NeedsTransition() const;

	/** Move every bound image into the layout its descriptor will claim. Outside a rendering scope only. */
	void TransitionImages( VkCommandBuffer commandBuffer );

	/** Write (if anything changed) and bind descriptor set 1 for the current program */
	ALResult Commit( Tr2PrimaryRenderContextAL& owner, VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint ) throw();

	/** Hand every descriptor pool to the owner's deferred destruction */
	void Release( Tr2PrimaryRenderContextAL& owner ) throw();

	// What the direct model costs on the CPU, for M2's measurement: descriptor sets written,
	// descriptors written into them, commits that reused the frame's last set, and the time
	// spent allocating and writing. Read and cleared by whoever is measuring.
	struct Stats
	{
		uint64_t setsWritten = 0;
		uint64_t descriptorsWritten = 0;
		uint64_t setsReused = 0;
		uint64_t writeNanoseconds = 0;
	};
	Stats TakeStats() throw();

private:
	struct Resource
	{
		enum Type
		{
			NONE,
			BUFFER,
			TEXTURE,
		};

		Tr2TextureAL texture;
		Tr2BufferAL buffer;
		Type type = NONE;
		Tr2RenderContextEnum::ColorSpace colorSpace = Tr2RenderContextEnum::COLOR_SPACE_LINEAR;
		uint32_t mip = 0;
	};

	struct Sampler
	{
		Tr2SamplerStateAL sampler;
		bool set = false;
	};

	// One frame's descriptor pools. More pools are added when one runs out; the whole group
	// is reset and reused once its frame has retired.
	struct PoolGroup
	{
		std::vector<VkDescriptorPool> pools;
		size_t current = 0;
		uint64_t frame = 0;
	};

	void BeginBatch() throw();
	void Clear() throw();
	Resource* GetSrvSlot( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex ) throw();
	Resource* GetUavSlot( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex ) throw();
	Sampler* GetSamplerSlot( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex ) throw();
	const Resource* FindSrv( uint32_t stage, uint32_t registerIndex ) const throw();
	const Resource* FindUav( uint32_t stage, uint32_t registerIndex ) const throw();
	const Sampler* FindSampler( uint32_t stage, uint32_t registerIndex ) const throw();

	// The layout a texture bound in this slot has to be in when the descriptor is used.
	static VkImageLayout RequiredLayout( const Resource& resource, bool storage );

	ALResult AllocateSet( Tr2PrimaryRenderContextAL& owner, VkDescriptorSetLayout layout, VkDescriptorSet& set ) throw();
	ALResult WriteSet( Tr2PrimaryRenderContextAL& owner, VkDescriptorSet set ) throw();

	Resource m_srvs[Tr2RenderContextEnum::SHADER_TYPE_COUNT * Tr2RegisterMapAL::MAX_RESOURCES_IN_STAGE];
	Resource m_uavs[Tr2RenderContextEnum::SHADER_TYPE_COUNT * Tr2RegisterMapAL::MAX_RESOURCES_IN_STAGE];
	Sampler m_samplers[Tr2RenderContextEnum::SHADER_TYPE_COUNT * Tr2RegisterMapAL::MAX_RESOURCES_IN_STAGE];

	const TrinityALImpl::Tr2ShaderProgramAL* m_program;
	bool m_committed;
	bool m_sealed;

	// The set the last commit wrote, the layout it was allocated against, and the frame
	// whose pools it came from: it may be bound again only while that frame is recording.
	VkDescriptorSet m_set;
	VkDescriptorSetLayout m_setLayout;
	uint64_t m_setFrame;

	std::vector<PoolGroup> m_poolGroups;
	size_t m_activeGroup;

	Stats m_stats;
};

#endif

#endif
