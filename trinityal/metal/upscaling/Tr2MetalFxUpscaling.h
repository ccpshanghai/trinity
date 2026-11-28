// Copyright © 2024 CCP ehf.

#pragma once

#include <TargetConditionals.h>
// MetalFX ships no iOS Simulator framework (present for macOS and real iOS/tvOS
// devices, absent from every *Simulator*.sdk), so this whole technique is compiled
// out there. Tr2UpscalingALMetal.mm's CreateUpscalingTechnique gates its METALFX
// case the same way, and IsAvailable() already made this a runtime "unsupported"
// rather than a guarantee, so nothing here changes what a working platform sees.
#if TRINITY_PLATFORM == TRINITY_METAL && !TARGET_OS_SIMULATOR
#include "include/upscaling/Tr2UpscalingAL.h"
#include <MetalFx/MetalFX.h>

class Tr2MetalFxUpscalingTechnique : public Tr2UpscalingTechniqueAL
{
public:
	Tr2MetalFxUpscalingTechnique( Tr2RenderContextAL& renderContext, Tr2UpscalingAL::Technique technique, Tr2UpscalingAL::Setting setting, bool frameGeneration, uint32_t adapter );
	~Tr2MetalFxUpscalingTechnique();

	virtual bool IsAvailable() const override;
	virtual std::vector<Tr2UpscalingAL::Setting> GetAvailableSettings() const override;
	virtual bool IsTemporal() const override;

private:
	virtual Tr2UpscalingContextAL* CreateContextInstance( Tr2UpscalingAL::UpscalingContextParams params ) override;

	bool m_temporal;
};

class Tr2MetalFxUpscalingContext : public Tr2UpscalingContextAL
{
public:
	Tr2MetalFxUpscalingContext( Tr2UpscalingAL::Setting setting, bool temporal, Tr2UpscalingAL::UpscalingContextParams params );
	~Tr2MetalFxUpscalingContext();

	virtual bool HasSharpening() const override;
	virtual void UpdateJitter() override;
	virtual uint32_t GetDispatchRequirements() const override;

	virtual Tr2UpscalingAL::Result Dispatch( Tr2UpscalingAL::DispatchParameters& dispatchParameters ) override;

private:
	void CreateTemporalScaler();
	void CreateSpatialScaler();

	void SetSiliconUpscalingAmount();
	void SetIntelUpscalingAmount();

	API_AVAILABLE( macos( 13.0 ) )
	id<MTLFXSpatialScaler> m_mfxSpatialScaler;

	bool m_setup;
	bool m_temporal;
	Tr2UpscalingAL::JitterSequence m_jitterSequence;

	Tr2RenderContextAL& m_renderContext;

	struct TemporalScaler
	{
		API_AVAILABLE( macos( 13.0 ) )
		MTLFXTemporalScalerDescriptor* desc;

		API_AVAILABLE( macos( 13.0 ) )
		id<MTLFXTemporalScaler> temporalScaler;

		id<MTLDevice> device;
		std::atomic<bool> canceled = false;
		std::atomic<bool> created = false;
	};

	std::shared_ptr<TemporalScaler> m_temporalScaler;

	friend class Tr2MetalFxUpscalingTechnique;
};
#endif
