// Copyright © 2023 CCP ehf.

#pragma once

// Built on every platform since 2026-09-08, for its SPIR-V branch: EffectCompilerVulkan
// is a shim over this class. Its DX11/DX12 branches stay Windows-only, guarded inside
// the .cpp -- see CompileEffect's first lines. Every member below is a dxc or
// d3dcommon type, both of which macOS has through the directx-dxc port.
#include "EffectCompilerBase.h"

#include <functional>

class ParserState;
struct GlobalInputElement;
struct Library;


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
		CComPtr<IDxcBlob> librarySpirv; // -spirv object of a ray tracing library
	};

	// The cache entry for `key`, compiled exactly once across threads: the first caller runs
	// `compile` on the new entry, every other caller waits (telling the work queue it is
	// blocked) until that has finished. The entry is marked compiled and its waiters woken on
	// every way out of `compile`, so a failed compile leaves null outputs behind and never a
	// waiter that sleeps for ever. The caller reads raw pointers out of the entry, which the
	// cache keeps alive for the compiler's life, then hands its reference back with
	// ReleaseCacheEntry.
	std::shared_ptr<SyncData> CompileOnce( const std::string& key, class IWorkQueue* workQueue, const std::function<void( SyncData& )>& compile );
	void ReleaseCacheEntry( std::shared_ptr<SyncData>& entry );

	bool CompileLibrarySpirv( ParserState& state, const std::vector<GlobalInputElement>& globalInputs, ID3D12LibraryReflection* reflection, const std::string& libraryName, Library& library, class IWorkQueue* workQueue );

	std::unordered_map<std::string, std::shared_ptr<SyncData>> m_compiled;
	std::mutex m_compiledCS;
	std::mutex m_pdbCS;
	CComPtr<IDxcUtils> m_dxilUtils;
};
