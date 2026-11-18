// Copyright © 2000 CCP ehf.

#if !defined( _TriTextureRes_H_ )
#define _TriTextureRes_H_

#include "include/ITriTextureRes.h"
#include "Tr2DeviceResource.h"
#include "ITr2TextureProvider.h"
#include "Tr2AsyncSave.h"
#include "Tr2DepthStencil.h"
#include "Tr2LoadPrepareFence.h"

class Tr2RenderTarget;
class Tr2ImageHandler;
class Tr2TextureLodManager;
struct Tr2TextureLodUpdateRequest;

BLUE_DECLARE( Tr2RenderTarget );
BLUE_DECLARE( Tr2HostBitmap );
BLUE_DECLARE( Tr2TexturePipeline );
BLUE_DECLARE( Tr2ImageRes );


/////////////////////////////////////////////////////////////////////////////////////
// TriTextureRes
/////////////////////////////////////////////////////////////////////////////////////
BLUE_CLASS( TriTextureRes ) :
	public BlueAsyncRes,
	public ITr2TextureProvider,
	public ICacheable,
	public ITriTextureRes,
	public Tr2DeviceResource,
	public Tr2BitmapDimensions,
	public Tr2AsyncSave
{
public:
	using BlueAsyncRes::Lock;
	using BlueAsyncRes::Unlock;

	TriTextureRes();
	~TriTextureRes();

	/////////////////////////////////////////////////////////////////////////////////////
	// ITriTextureRes
	Tr2TextureAL* GetTexture();
	unsigned GetWidth() const
	{
		return Tr2BitmapDimensions::GetWidth();
	}
	unsigned GetHeight() const
	{
		return Tr2BitmapDimensions::GetHeight();
	}
	unsigned GetMipLevelCount() const
	{
		return Tr2BitmapDimensions::GetMipCount();
	}
	uint32_t GetMsaaType() const;
	uint32_t GetMsaaQuality() const;

	Color GetAverageColor() const
	{
		return m_averageColor;
	}
	bool IsValid() const
	{
		return IsGood();
	}
	Tr2RenderContextEnum::TextureType GetType() const
	{
		return Tr2BitmapDimensions::GetType();
	}
	long UpdateSubresource( unsigned left, unsigned top, unsigned right, unsigned bottom, const void* source, unsigned sourcePitch );
	void SetAverageColor( float red, float green, float blue, float alpha ) override;

	const Tr2TextureAL* GetTexture() const;
	OnTextureChangeEvent& OnTextureChange() override;

	bool SetTextureFromRT( Tr2RenderTarget * renderTarget );

	// Make a TriTextureRes that takes a copy of the renderTarget contents and sticks with it (not a live view).
	bool CreateAndCopyFromRenderTarget( Tr2RenderTarget * renderTarget );


	// Make an immutable TriTextureRes that takes a copy of the hostBitmap contents and sticks with it (not a live view).
	bool CreateFromHostBitmap( Tr2HostBitmap * bitmap );
	bool CreateEmptyTexture( uint32_t width, uint32_t height, uint32_t mipCount, Tr2RenderContextEnum::PixelFormat format );
	BlueStdResult CreateFromTexture( TriTextureRes * texture );

	// Create a new TriTextureRes with undefined contents.  It's implied that the usage is dynamic.
	bool Create( uint32_t width,
				 uint32_t height,
				 uint32_t mipCount,
				 Tr2RenderContextEnum::PixelFormat format,
				 Tr2RenderContextEnum::BufferUsageFlags usage,
				 Tr2PrimaryRenderContext& renderContext );

	ALResult OpenShared( uintptr_t handle );

	bool SaveAsync( const wchar_t* filename );
	bool Save( const wchar_t* filename ); // synchronous


	Tr2TexturePipeline* GetPipeline() const;

	void RequestResolution( const uint32_t requestedLod );
	void UpdateLodRequest( Tr2TextureLodUpdateRequest & request, Tr2TextureLodManager & manager );
	void ProcessLodRequest( const Tr2TextureLodUpdateRequest& request, Tr2TextureLodManager& manager );
	uint32_t GetOriginalResolution() const;
	float GetOriginalResolutionAsFloat() const;


	//////////////////////////////////////////////////////////////////////////
	// IBlueResource
	//
	// Initialize is implemented by the BlueAsyncRes base class, but we
	// need to intercept it to capture mip-skip settings
	void Initialize( const wchar_t* name, const wchar_t* ext );
	void OnShutdown();

	/////////////////////////////////////////////////////////////////////////////////////
	// ITriDeviceResource
private:
	bool OnPrepareResources();

public:
	void ReleaseResources( TriStorage s );

#if TRINITYDEV
	virtual void GetDescription( std::string & desc );
#endif

	//////////////////////////////////////////////////////////////////////////
	// ICacheable
	bool IsMemoryUsageKnown();
	size_t GetMemoryUsage();

	// Handy name for debugging and memory overviews, otherwise unused.
	std::string m_name;

	virtual void SetName( const char* name )
	{
		m_name = name;
	}
	virtual const char* GetName() const
	{
		return m_name.c_str();
	}

	bool HadLodRequests() const;
	size_t GetOriginalMemoryUsage() const;

private:
	void OnWrappedRenderTargetChanged();
	void RasterizeProceduralTexture( const wchar_t* data, void ( *Rastrization )( const std::string_view&, ImageIO::HostBitmap& ) );

	Tr2TextureAL* m_texture;
	Tr2TextureAL m_ownTexture;
	Tr2RenderTargetPtr m_wrappedRenderTarget;
	OnTextureChangeEvent m_onTextureChange;

	unsigned ComputeMipSkipCount();

	unsigned int m_memoryUse;
	size_t m_originalMemoryUse; // Memory usage for non-LODed version (for stats)

	float m_cutoutX;
	float m_cutoutY;
	float m_cutoutWidth;
	float m_cutoutHeight;

	ImageIO::Metadata m_metadata;

	std::unique_ptr<ImageIO::HostBitmap> m_loadedBitmap;

	unsigned m_mipLevelMaxCount;
	bool m_isTextureLoadDisabled : 1;
	bool m_isTextureResizable : 1; // do we listen to the global mipskip setting?

	std::atomic<bool> m_hadLodRequests;
	bool m_lodEnabled;

	uint32_t m_originalResolution;
	std::atomic<uint32_t> m_requestedMip;
	uint32_t m_maxMip;
	uint32_t m_cpuMip;
	uint32_t m_gpuMip;
	uint32_t m_requestedLoadMip;

	Color m_averageColor;

private:
	bool SetTexture( Tr2TextureAL & texture );
	bool HasALObject( int type, size_t object );

	void TrimLods( uint32_t startLod, Tr2TextureLodManager& manager );

protected:
	// Provide the functions that do the actual work of loading and preparing.
	// The async management itself is done in BlueAsyncRes.
	virtual LoadingResult DoLoad();
	virtual bool DoPrepare();
	virtual void CleanupLoadData();

	// A .dds path opens its .ktx2 sibling first, when one exists and the backend samples ASTC
	// (spec O3, D7). The .red files name every texture as .dds and are EVE's own committed
	// content, so the path cannot be rewritten at its source; DDS also cannot carry ASTC, which
	// is why there is a second container to prefer at all.
	//
	// The shape is Blue's, not invented here: BlueFileUtil's SubstituteBlackForRedInFilename is
	// driven by BeResMan->GetSubstituteBlackForRed() and IBluePaths documents FileExists honouring
	// it. Same layer, same kind of preference, one extension instead of another.
	virtual bool DoOpenStream();

public:
	// Set once, from the render context's capability, when the device comes up -- not queried per
	// load. DoOpenStream runs on a loader thread and GetCaps() is main-thread-only, so a live
	// query here would be a thread violation dressed as a capability check.
	static void SetSubstituteKtx2ForDds( bool substitute );
	static bool GetSubstituteKtx2ForDds();

private:
	// The path actually opened. Empty unless the substitution above found a .ktx2, in which case
	// it is what LoadParameters must carry: imageio picks its handler by extension, so loading a
	// .ktx2 stream under a .dds name would hand ASTC blocks to the DDS parser. m_path stays the
	// requested name, which is what the resource is keyed and logged by.
	std::wstring m_actualPath;

private:
	// These member variables and functions exist to support async texture saving;
	// from Tr2AsyncSave
	virtual bool DoPrepareAsyncSave();
	virtual bool DoExecuteAsyncSave();
	virtual void DoCleanupAsyncSave();

	void DestroyOwnTexture();

	friend Tr2HostBitmap;

	Tr2TexturePipelinePtr m_pipeline;
	std::unordered_map<std::wstring, Tr2ImageResPtr> m_pipelineInputs;

	void ResourcePrepFinished();

	Tr2HostBitmapPtr m_asyncSaveBitmap;
	std::shared_ptr<Tr2ImageHandler> m_asyncSaveImage;

	Tr2LoadPrepareFence m_pipelineFence;

	uint32_t GetSrvIndexInHeap() const;

public:
	EXPOSE_TO_BLUE();

	/////////////////////////////////////////
	// Python thunkers
	typedef TriTextureRes _Class;
};

TYPEDEF_BLUECLASS_WR_SHUTDOWN( TriTextureRes );

#endif
