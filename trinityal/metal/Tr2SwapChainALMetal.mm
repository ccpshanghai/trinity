// Copyright © 2023 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_METAL

#include "Tr2SwapChainALMetal.h"
#include "Tr2TextureALMetal.h"
#include "Tr2RenderContextMetal.h"
#include "ALLog.h"

namespace TrinityALImpl
{

Tr2SwapChainAL::Tr2SwapChainAL() : m_windowHandle( Tr2WindowHandle() )
{
	m_backBuffer.m_texture = std::make_shared<TrinityALImpl::Tr2TextureAL>();
}

ALResult Tr2SwapChainAL::Create( Tr2WindowHandle windowHandle, Tr2RenderContextAL& renderContext )
{
	if( !renderContext.IsValid() )
	{
		return E_INVALIDARG;
	}
	CAMetalLayer* layer = (CAMetalLayer*)windowHandle;
	if( !layer || ![layer isKindOfClass:CAMetalLayer.class] )
	{
		return E_INVALIDARG;
	}
	Destroy();

	uint32_t width = 0;
	uint32_t height = 0;
	Tr2RenderContextEnum::PixelFormat pixelFormat = Tr2RenderContextEnum::PIXEL_FORMAT_UNKNOWN;

	Tr2PresentParametersAL* presentParameters = renderContext.GetPresentParamaters();
	if( windowHandle == presentParameters->outputWindow )
	{
		width = presentParameters->mode.width;
		height = presentParameters->mode.height;
		pixelFormat = presentParameters->mode.format;
	}
	else
	{
		pixelFormat = Tr2RenderContextEnum::PIXEL_FORMAT_B8G8R8X8_UNORM;
	}

	// A zero dimension means "the whole window", and on Metal nobody used to resolve it.
	//
	// DXGI does: a swap-chain description of 0 x 0 is documented as the client rect, so callers
	// are written that way -- app/boot.py's create_device passes CreateWindowedDevice( hwnd, 0, 0 )
	// on purpose, with a comment explaining that the size then lives in one place instead of two.
	// TriDevice::CreateSimpleDevice copies those zeros straight into pp.mode, Metal's
	// SetPresentParameters repairs mode.format and leaves the size alone, and the branch above
	// then asked for a 0 x 0 back buffer. The texture creation failed, its ALResult was discarded
	// by both callers, and the only visible symptom was GetBackBufferFormat() returning UNKNOWN --
	// which reached Python as ui_init refusing an MTLPixelFormatInvalid colour format, three
	// layers away from the cause.
	//
	// Resolved from the layer, which is the only thing here that knows: drawableSize in pixels,
	// via bounds x contentsScale, exactly as the else branch above always did for a foreign
	// window handle.
	if( width == 0 || height == 0 )
	{
		const auto scale = layer.contentsScale;
		layer.drawableSize = CGSizeMake( layer.bounds.size.width * scale, layer.bounds.size.height * scale );
		width = layer.drawableSize.width;
		height = layer.drawableSize.height;

		if( width == 0 || height == 0 )
		{
			// A zero-sized layer is not something to paper over with a 1x1: it means the view
			// has no geometry yet, and every render target derived from this would be wrong.
			CCP_LOGERR( "Swap chain: CAMetalLayer has no size (bounds %g x %g, scale %g)",
				layer.bounds.size.width, layer.bounds.size.height, scale );
			return E_INVALIDARG;
		}
	}

	if( pixelFormat == Tr2RenderContextEnum::PIXEL_FORMAT_UNKNOWN )
	{
		// Same defaulting SetPresentParameters does, repeated here because Create is also
		// reachable from SwapChainResizing's tests without going through it.
		pixelFormat = Tr2RenderContextEnum::PIXEL_FORMAT_B8G8R8A8_UNORM;
	}

	Tr2MsaaDesc msaaDesc = Tr2MsaaDesc( presentParameters->msaaType, presentParameters->msaaQuality );
	Tr2BitmapDimensions textureInfo = Tr2BitmapDimensions( width, height, 1, pixelFormat );

	Tr2GpuUsage::Type gpuUsage = Tr2GpuUsage::RENDER_TARGET | Tr2GpuUsage::SHADER_RESOURCE;
	// Have to make this READ to get the screeshot facility to work
	Tr2CpuUsage::Type cpuUsage = Tr2CpuUsage::READ;

	CR_RETURN_HR( m_backBuffer.m_texture->Create( textureInfo, msaaDesc, gpuUsage, cpuUsage, nil, renderContext ) );

	m_windowHandle = windowHandle;

	return S_OK;
}

void Tr2SwapChainAL::Destroy()
{
	m_backBuffer.m_texture->Destroy();
}

bool Tr2SwapChainAL::IsValid() const
{
	return m_backBuffer.IsValid();
}

ALResult Tr2SwapChainAL::Present( Tr2RenderContextAL& renderContext )
{
	MetalContext* metalContext = renderContext.GetMetalContext();
	CAMetalLayer* layer = (CAMetalLayer*)m_windowHandle;
	id<MTLTexture> backBufferTexture = m_backBuffer.m_texture->GetMetalTexture();
	metalContext->BlitToDrawableAndPresent( backBufferTexture, layer );

	GetNextBackbuffer();

	return S_OK;
}

uint32_t Tr2SwapChainAL::GetWidth() const
{
	return m_backBuffer.GetWidth();
}

uint32_t Tr2SwapChainAL::GetHeight() const
{
	return m_backBuffer.GetHeight();
}

void Tr2SwapChainAL::Describe( Tr2DeviceResourceDescriptionAL& description ) const
{
	description["type"] = "Tr2SwapChainAL";
	description["name"] = m_name;
}

ALResult Tr2SwapChainAL::SetName( const char* name )
{
	m_name = name;
	return S_OK;
}

void Tr2SwapChainAL::GetNextBackbuffer()
{
}
}

#endif // TRINITY_PLATFORM == TRINITY_METAL
