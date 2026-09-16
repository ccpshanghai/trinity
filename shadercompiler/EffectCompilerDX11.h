// Copyright © 2023 CCP ehf.

#pragma once

// Built on every platform since 2026-09-08, for its SPIR-V branch: EffectCompilerVulkan
// is a shim over this class. Its DX11/DX12 branches stay Windows-only, guarded inside
// the .cpp -- see CompileEffect's first lines. Every member below is a dxc or
// d3dcommon type, both of which macOS has through the directx-dxc port.
#include "EffectCompilerBase.h"


class EffectCompilerDX11 : public EffectCompilerBase
{
public:
	bool Create() override;
	bool CompileEffect( const char* source, size_t sourceLength, const std::vector<Macro>& defines, EffectData& result, class IWorkQueue* workQueue ) override;

	struct CompileOptions
	{
		const char* minShaderVersion; // minimal shader version (5_0 by default)
		bool addSpaces; // add space declarations to shader resources (dx12)
		bool useStaticSamplers; // dx12
		bool spirv = false; // vulkan: pass shaders through DXC -spirv; reflection from a DXIL sibling compile
	};
	bool CompileEffect( const char* source, size_t sourceLength, const std::vector<Macro>& defines, EffectData& result, const CompileOptions& compileOptions, class IWorkQueue* workQueue );

private:
	struct SyncData
	{
		bool compiled = false;
		std::condition_variable conditionVariable;
		std::mutex mutex;
		CComPtr<ID3D10Blob> passResource;
		CComPtr<IDxcBlob> libraryResource;
		CComPtr<IDxcBlob> libraryReflection;
		CComPtr<IDxcBlob> passSpirv; // -spirv object
		CComPtr<IDxcBlob> passSpirvReflection; // DXC_OUT_REFLECTION from the DXIL sibling compile
	};

	std::unordered_map<std::string, std::shared_ptr<SyncData>> m_compiled;
	std::mutex m_compiledCS;
	std::mutex m_pdbCS;
	CComPtr<IDxcUtils> m_dxilUtils;
};
