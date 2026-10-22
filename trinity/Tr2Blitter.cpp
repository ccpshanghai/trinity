// Copyright © 2023 CCP ehf.

#include "StdAfx.h"
#include "Tr2Blitter.h"
#include <set>
#include <map>
#include "Shader/Tr2Effect.h"
#include "Shader/Tr2Shader.h"
#include "Tr2TextureReference.h"

#include "TriDevice.h" //TODO gTriDev/ScreenVertexDecl.

static const char* BLIT_EFFECT_PATH = "res:/Graphics/Effect/Managed/space/system/Blit.fx";
static const char* BLIT_FILTERED_EFFECT_PATH = "res:/Graphics/Effect/Managed/space/system/BlitFiltered.fx";
static const char* BLITCUBE_EFFECT_PATH = "res:/Graphics/Effect/Managed/space/system/BlitCube.fx";

using namespace Tr2RenderContextEnum;

Tr2Blitter::Tr2Blitter() :
	m_screenVertexDecl( -1 ), m_mipLevelVar( "mipLevel", 0.f )
{
	GlobalStore().RegisterVariable( "BlitSource", static_cast<ITr2TextureProvider*>( nullptr ) );

	// create all shaders needed in the blitter
	m_blitEffect.CreateInstance();
	m_blitEffect->SetEffectPathName( BLIT_EFFECT_PATH );
	m_blitFilteredEffect.CreateInstance();
	m_blitFilteredEffect->SetEffectPathName( BLIT_FILTERED_EFFECT_PATH );

	Tr2Blitter::PrepareResources();
}

Tr2Blitter::~Tr2Blitter()
{
	GlobalStore().UnregisterVariable( "BlitSource" );
	GlobalStore().UnregisterVariable( "mipLevel" );
}

bool Tr2Blitter::Draw( Tr2RenderContext& renderContext, Tr2Material* effect, const Vector2& tlTexCoord, const Vector2& brTexCoord, const Vector2& tlVertexCoord, const Vector2& brVertexCoord )
{
	return DrawHelper( renderContext, effect->GetShaderStateInterface(), effect, nullptr, false, tlTexCoord, brTexCoord, tlVertexCoord, brVertexCoord );
}

bool Tr2Blitter::Draw( Tr2RenderContext& renderContext, const Tr2TextureAL& texture, const Vector2& tlTexCoord, const Vector2& brTexCoord, Filtering filter )
{
	switch( filter )
	{
	case FILTER_POINT:
		return DrawHelper( renderContext, m_blitEffect->GetShaderStateInterface(), m_blitEffect, &texture, false, tlTexCoord, brTexCoord );
	case FILTER_LINEAR:
		return DrawHelper( renderContext, m_blitFilteredEffect->GetShaderStateInterface(), m_blitFilteredEffect, &texture, false, tlTexCoord, brTexCoord );
	}
	return false;
}

bool Tr2Blitter::Draw( Tr2RenderContext& renderContext, const Tr2TextureAL& texture, const Vector2& tlTexCoord, const Vector2& brTexCoord, const Vector2& tlVertexCoord, const Vector2& brVertexCoord )
{
	return DrawHelper( renderContext, m_blitEffect->GetShaderStateInterface(), m_blitEffect, &texture, false, tlTexCoord, brTexCoord, tlVertexCoord, brVertexCoord );
}

bool Tr2Blitter::Draw( Tr2RenderContext& renderContext, Tr2Material* effect, const Tr2TextureAL& texture )
{
	return DrawHelper( renderContext, effect->GetShaderStateInterface(), effect, &texture, false );
}

bool Tr2Blitter::Draw( Tr2RenderContext& renderContext, Tr2Material* effect, const Tr2TextureAL& texture, const Vector2& tlTexCoord, const Vector2& brTexCoord )
{
	return DrawHelper( renderContext, effect->GetShaderStateInterface(), effect, &texture, false, tlTexCoord, brTexCoord );
}

bool Tr2Blitter::Draw( Tr2RenderContext& renderContext, Tr2Material* effect, const Tr2TextureAL& texture, const Vector2& tlTexCoord, const Vector2& brTexCoord, const Vector2& tlVertexCoord, const Vector2& brVertexCoord )
{
	return DrawHelper( renderContext, effect->GetShaderStateInterface(), effect, &texture, false, tlTexCoord, brTexCoord, tlVertexCoord, brVertexCoord );
}
// --------------------------------------------------------------------------------------
// Description:
//   Wrapper around DrawHelper.
// Arguments:
//   material - the Tr2ShaderMaterial to use when drawing.
// Return Value:
//   true if success, false if there is an error.
// See Also: Tr2Renderer::DrawFullScreenWithShader()
// --------------------------------------------------------------------------------------
bool Tr2Blitter::Draw( Tr2RenderContext& renderContext, Tr2Material* material )
{
	auto shader = material->GetShaderStateInterface();

	return DrawHelper( renderContext, shader, material, NULL, false );
}

bool Tr2Blitter::Draw( Tr2RenderContext& renderContext, Tr2Material* effect, const Vector2& tlTexCoord, const Vector2& brTexCoord )
{
	auto shader = effect->GetShaderStateInterface();

	return DrawHelper( renderContext, shader, effect, NULL, false, tlTexCoord, brTexCoord );
}

bool Tr2Blitter::DrawInCameraSpace( Tr2RenderContext& renderContext, Tr2Shader* shader, Tr2Material* material )
{
	return DrawHelper( renderContext, shader, material, NULL, true );
}

bool Tr2Blitter::DrawHelper( Tr2RenderContext& renderContext, Tr2Shader* shader, Tr2Material* material, const Tr2TextureAL* halTexture, bool isCameraSpace, const Vector2& tlTexCoord, const Vector2& brTexCoord, const Vector2& tlVertexCoord, const Vector2& brVertexCoord )
{
	CCP_ASSERT( material );
	if( !material )
	{
		// Final build should not crash even if we get here with a null effect
		return false;
	}

	// Each early return below used to be silent, and a silent false here is a frame with a
	// hole in it and no line in any log: the blitter is how a scene's post-processed result
	// reaches the back buffer, so a blit that declines is a black screen. Once per reason,
	// because a reason that holds for one frame holds for every frame after it.
	if( m_screenVertexDecl == -1 || !m_vertexBuffer.IsValid() )
	{
		static bool logged = false;
		if( !logged )
		{
			logged = true;
			CCP_LOGERR( "Tr2Blitter: draw skipped, no screen quad (vertexDecl %d, vertex buffer %s)",
						m_screenVertexDecl, m_vertexBuffer.IsValid() ? "valid" : "INVALID" );
		}
		return false;
	}

	// Skips are logged on the transitions, per material: once when a material starts being
	// skipped and once when it draws again, with a count of the frames in between. A single
	// log-once said "something" and hid which blit it was; a per-material log-once said which
	// but not whether it ever recovered -- and "skipped for one frame while its effect was
	// still loading" and "skipped for the whole run" are the benign and the fatal reading of
	// the same line.
	static std::map<const Tr2Material*, uint32_t> skipping;
	if( !shader )
	{
		auto inserted = skipping.emplace( material, 0u );
		if( inserted.second )
		{
			const Tr2Effect* effect = dynamic_cast<const Tr2Effect*>( material );
			CCP_LOGERR( "Tr2Blitter: draw skipped, the material has no shader state: %s (%s)",
						effect && effect->GetEffectPathName() ? effect->GetEffectPathName() : "(not a Tr2Effect)",
						effect ? effect->DescribeShaderResolution() : "-" );
		}
		++inserted.first->second;
		return false;
	}
	{
		auto found = skipping.find( material );
		if( found != skipping.end() )
		{
			const Tr2Effect* effect = dynamic_cast<const Tr2Effect*>( material );
			CCP_LOG( "Tr2Blitter: draws resumed after %u skipped: %s", found->second,
					 effect && effect->GetEffectPathName() ? effect->GetEffectPathName() : "(not a Tr2Effect)" );
			skipping.erase( found );
		}
	}

	Tr2ScreenVertex* quad = NULL;
	{
		auto hr = m_vertexBuffer.MapForWriting( quad, renderContext );
		if( FAILED( hr ) )
		{
			static bool logged = false;
			if( !logged )
			{
				logged = true;
				CCP_LOGERR( "Tr2Blitter: draw skipped, MapForWriting on the screen quad failed with HRESULT=0x%X", hr.GetResult() );
			}
			return false;
		}
	}

	if( isCameraSpace )
	{
		CCP_ASSERT( tlVertexCoord[0] == 0.0f && tlVertexCoord[1] == 0.0f );
		CCP_ASSERT( brVertexCoord[0] == 1.0f && brVertexCoord[1] == 1.0f );

		SetupScreenQuadInCameraSpace( quad );
	}
	else
	{
		SetupScreenQuad( quad, tlTexCoord, brTexCoord, tlVertexCoord, brVertexCoord );
	}

	m_vertexBuffer.UnmapForWriting( renderContext );

	renderContext.m_esm.ApplyVertexDeclaration( m_screenVertexDecl );
	renderContext.m_esm.ApplyStreamSource( 0, m_vertexBuffer, 0, sizeof( Tr2ScreenVertex ) );

	static CTr2TransientTextureReference textureReference;
	Tr2TextureAL texCopy;
	if( halTexture )
	{
		texCopy = *halTexture;
		textureReference.SetTexture( &texCopy );
		GlobalStore().GetVariable( "BlitSource" )->SetValue( &textureReference );
	}

	unsigned int passCount = shader->GetPassCount( 0 );
	if( passCount == 0 )
	{
		static bool logged = false;
		if( !logged )
		{
			logged = true;
			CCP_LOGERR( "Tr2Blitter: draw skipped, the shader's technique 0 has no passes" );
		}
	}

	for( unsigned int passIx = 0; passIx < passCount; ++passIx )
	{
		shader->ApplyAllStateForPass( 0, passIx, renderContext );
		material->ApplyMaterialDataForPass( 0, passIx, renderContext );
		{
			renderContext.SetTopology( TOP_TRIANGLE_STRIP );
			renderContext.DrawPrimitive( 0, 2 );
		}
	}

	textureReference.SetTexture( nullptr );
	GlobalStore().GetVariable( "BlitSource" )->Clear();

	return true;
}

bool Tr2Blitter::OnPrepareResources()
{
	if( m_screenVertexDecl == -1 )
	{
		Tr2VertexDefinition vd;
		vd.Add( vd.FLOAT32_4, vd.POSITION );
		vd.Add( vd.FLOAT32_2, vd.TEXCOORD );

		m_screenVertexDecl = Tr2EffectStateManager::GetVertexDeclarationHandle( vd );
	}

	if( !m_vertexBuffer.IsValid() )
	{
		USE_MAIN_THREAD_RENDER_CONTEXT();
		m_vertexBuffer.Create( sizeof( Tr2ScreenVertex ), 4, Tr2GpuUsage::VERTEX_BUFFER, Tr2CpuUsage::WRITE_OFTEN, nullptr, renderContext );
	}

	return true;
}

void Tr2Blitter::ReleaseResources( TriStorage s )
{
	m_screenVertexDecl = -1;
}
