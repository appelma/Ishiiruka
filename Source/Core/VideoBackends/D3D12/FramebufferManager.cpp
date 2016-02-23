// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/HW/Memmap.h"
#include "VideoBackends/D3D12/D3DBase.h"
#include "VideoBackends/D3D12/D3DCommandListManager.h"
#include "VideoBackends/D3D12/D3DUtil.h"
#include "VideoBackends/D3D12/FramebufferManager.h"
#include "VideoBackends/D3D12/Render.h"
#include "VideoBackends/D3D12/StaticShaderCache.h"
#include "VideoBackends/D3D12/XFBEncoder.h"
#include "VideoCommon/VideoConfig.h"

namespace DX12
{

static XFBEncoder s_xfbEncoder;

FramebufferManager::Efb FramebufferManager::m_efb;
unsigned int FramebufferManager::m_target_width;
unsigned int FramebufferManager::m_target_height;

D3DTexture2D*& FramebufferManager::GetEFBColorTexture() { return m_efb.color_tex; }
D3DTexture2D*& FramebufferManager::GetEFBDepthTexture() { return m_efb.depth_tex; }
D3DTexture2D*& FramebufferManager::GetEFBColorTempTexture() { return m_efb.color_temp_tex; }

void FramebufferManager::SwapReinterpretTexture()
{
	D3DTexture2D* swaptex = GetEFBColorTempTexture();
	m_efb.color_temp_tex = GetEFBColorTexture();
	m_efb.color_tex = swaptex;
}

D3DTexture2D*& FramebufferManager::GetResolvedEFBColorTexture()
{
	if (g_ActiveConfig.iMultisamples > 1)
	{
		m_efb.resolved_color_tex->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_RESOLVE_DEST);
		m_efb.color_tex->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);

		for (int i = 0; i < m_efb.slices; i++)
		{
			D3D::current_command_list->ResolveSubresource(m_efb.resolved_color_tex->GetTex(), D3D11CalcSubresource(0, i, 1), m_efb.color_tex->GetTex(), D3D11CalcSubresource(0, i, 1), DXGI_FORMAT_R8G8B8A8_UNORM);
		}

		return m_efb.resolved_color_tex;
	}
	else
	{
		return m_efb.color_tex;
	}
}

D3DTexture2D*& FramebufferManager::GetResolvedEFBDepthTexture()
{
	if (g_ActiveConfig.iMultisamples > 1)
	{
		ResolveDepthTexture();

		return m_efb.resolved_depth_tex;
	}
	else
	{
		return m_efb.depth_tex;
	}
}

void FramebufferManager::InitializeEFBCache(const D3D12_CLEAR_VALUE& color_clear_value, const D3D12_CLEAR_VALUE& depth_clear_value)
{
	ID3D12Resource* buff;
	D3D12_RESOURCE_DESC tex_desc;

	// Render buffer for AccessEFB (color data)
	tex_desc = CD3DX12_RESOURCE_DESC::Tex2D(color_clear_value.Format, EFB_WIDTH, EFB_HEIGHT, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
	HRESULT hr = D3D::device12->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COMMON, &color_clear_value, IID_PPV_ARGS(&buff));
	CHECK(hr == S_OK, "create EFB color cache texture (hr=%#x)", hr);
	m_efb.color_cache_tex = new D3DTexture2D(buff, (D3D11_BIND_FLAG)(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET), color_clear_value.Format, DXGI_FORMAT_UNKNOWN, color_clear_value.Format, false, D3D12_RESOURCE_STATE_COMMON);
	SAFE_RELEASE(buff);
	D3D::SetDebugObjectName12(m_efb.color_cache_tex->GetTex(), "EFB color cache texture");

	// AccessEFB - Sysmem buffer used to retrieve the pixel data from color_tex
	tex_desc = CD3DX12_RESOURCE_DESC::Buffer(EFB_CACHE_PITCH * EFB_HEIGHT);
	hr = D3D::device12->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK), D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_efb.color_cache_buf));
	CHECK(hr == S_OK, "create EFB color cache buffer (hr=%#x)", hr);

	// Render buffer for AccessEFB (depth data)
	tex_desc = CD3DX12_RESOURCE_DESC::Tex2D(depth_clear_value.Format, EFB_WIDTH, EFB_HEIGHT, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);	
	hr = D3D::device12->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COMMON, &depth_clear_value, IID_PPV_ARGS(&buff));
	CHECK(hr == S_OK, "create EFB depth cache texture (hr=%#x)", hr);
	m_efb.depth_cache_tex = new D3DTexture2D(buff, D3D11_BIND_RENDER_TARGET, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, D3D12_RESOURCE_STATE_COMMON);
	SAFE_RELEASE(buff);
	D3D::SetDebugObjectName12(m_efb.depth_cache_tex->GetTex(), "EFB depth cache texture (used in Renderer::AccessEFB)");

	// AccessEFB - Sysmem buffer used to retrieve the pixel data from depth_read_texture
	tex_desc = CD3DX12_RESOURCE_DESC::Buffer(EFB_CACHE_PITCH * EFB_HEIGHT);
	hr = D3D::device12->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK), D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_efb.depth_cache_buf));
	CHECK(hr == S_OK, "create EFB depth cache buffer (hr=%#x)", hr);
	D3D::SetDebugObjectName12(m_efb.depth_cache_buf, "EFB depth cache buffer");

}

FramebufferManager::FramebufferManager()
{
	m_target_width = std::max(Renderer::GetTargetWidth(), 1);
	m_target_height = std::max(Renderer::GetTargetHeight(), 1);

	DXGI_SAMPLE_DESC sample_desc;
	sample_desc.Count = g_ActiveConfig.iMultisamples;
	sample_desc.Quality = 0;

	ID3D12Resource* buff;
	D3D12_RESOURCE_DESC text_desc;
	D3D12_CLEAR_VALUE clear_valueRTV = { DXGI_FORMAT_R8G8B8A8_UNORM,{ 0.0f, 0.0f, 0.0f, 1.0f } };
	D3D12_CLEAR_VALUE clear_valueDSV = CD3DX12_CLEAR_VALUE(DXGI_FORMAT_D32_FLOAT, 0.0f, 0);

	HRESULT hr;

	m_EFBLayers = m_efb.slices = (g_ActiveConfig.iStereoMode > 0) ? 2 : 1;

	// EFB color texture - primary render target
	text_desc = CD3DX12_RESOURCE_DESC::Tex2D(clear_valueRTV.Format, m_target_width, m_target_height, m_efb.slices, 1, sample_desc.Count, sample_desc.Quality, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
	hr = D3D::device12->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &text_desc, D3D12_RESOURCE_STATE_COMMON, &clear_valueRTV, IID_PPV_ARGS(&buff));
	CHECK(hr == S_OK, "create EFB color texture (hr=%#x)", hr);
	m_efb.color_tex = new D3DTexture2D(buff, (D3D11_BIND_FLAG)(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET), clear_valueRTV.Format, DXGI_FORMAT_UNKNOWN, clear_valueRTV.Format, (sample_desc.Count > 1), D3D12_RESOURCE_STATE_COMMON);
	SAFE_RELEASE(buff);

	// Temporary EFB color texture - used in ReinterpretPixelData
	text_desc = CD3DX12_RESOURCE_DESC::Tex2D(clear_valueRTV.Format, m_target_width, m_target_height, m_efb.slices, 1, sample_desc.Count, sample_desc.Quality, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
	hr = D3D::device12->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &text_desc, D3D12_RESOURCE_STATE_COMMON, &clear_valueRTV, IID_PPV_ARGS(&buff));
	CHECK(hr == S_OK, "create EFB color temp texture (hr=%#x)", hr);
	m_efb.color_temp_tex = new D3DTexture2D(buff, (D3D11_BIND_FLAG)(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET), clear_valueRTV.Format, DXGI_FORMAT_UNKNOWN, clear_valueRTV.Format, (sample_desc.Count > 1), D3D12_RESOURCE_STATE_COMMON);
	SAFE_RELEASE(buff);
	D3D::SetDebugObjectName12(m_efb.color_temp_tex->GetTex(), "EFB color temp texture");

	// EFB depth buffer - primary depth buffer
	text_desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, m_target_width, m_target_height, m_efb.slices, 1, sample_desc.Count, sample_desc.Quality, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
	hr = D3D::device12->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &text_desc, D3D12_RESOURCE_STATE_COMMON, &clear_valueDSV, IID_PPV_ARGS(&buff));
	CHECK(hr == S_OK, "create EFB depth texture (hr=%#x)", hr);
	m_efb.depth_tex = new D3DTexture2D(buff, (D3D11_BIND_FLAG)(D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE), DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_UNKNOWN, (sample_desc.Count > 1), D3D12_RESOURCE_STATE_COMMON);
	SAFE_RELEASE(buff);
	D3D::SetDebugObjectName12(m_efb.depth_tex->GetTex(), "EFB depth texture");
	// For the rest of the depth data use plain float textures
	clear_valueDSV.Format = DXGI_FORMAT_R32_FLOAT;
	if (g_ActiveConfig.iMultisamples > 1)
	{
		// Framebuffer resolve textures (color+depth)
		text_desc = CD3DX12_RESOURCE_DESC::Tex2D(clear_valueRTV.Format, m_target_width, m_target_height, m_efb.slices, 1);
		hr = D3D::device12->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &text_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buff));
		CHECK(hr == S_OK, "create EFB color resolve texture (size: %dx%d)", m_target_width, m_target_height);
		m_efb.resolved_color_tex = new D3DTexture2D(buff, D3D11_BIND_SHADER_RESOURCE, clear_valueRTV.Format, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, D3D12_RESOURCE_STATE_COMMON);
		SAFE_RELEASE(buff);
		D3D::SetDebugObjectName12(m_efb.resolved_color_tex->GetTex(), "EFB color resolve texture shader resource view");

		text_desc = CD3DX12_RESOURCE_DESC::Tex2D(clear_valueDSV.Format, m_target_width, m_target_height, m_efb.slices, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
		hr = D3D::device12->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &text_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buff));
		CHECK(hr == S_OK, "create EFB depth resolve texture (size: %dx%d; hr=%#x)", m_target_width, m_target_height, hr);
		m_efb.resolved_depth_tex = new D3DTexture2D(buff, (D3D11_BIND_FLAG)(D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE), clear_valueDSV.Format, clear_valueDSV.Format, DXGI_FORMAT_UNKNOWN, false, D3D12_RESOURCE_STATE_COMMON);
		SAFE_RELEASE(buff);
		D3D::SetDebugObjectName12(m_efb.resolved_depth_tex->GetTex(), "EFB depth resolve texture shader resource view");
	}
	else
	{
		m_efb.resolved_color_tex = nullptr;
		m_efb.resolved_depth_tex = nullptr;
	}
	InitializeEFBCache(clear_valueRTV, clear_valueDSV);
	s_xfbEncoder.Init();
}

FramebufferManager::~FramebufferManager()
{
	s_xfbEncoder.Shutdown();
	FramebufferManager::InvalidateEFBCache();
	SAFE_RELEASE(m_efb.color_tex);
	SAFE_RELEASE(m_efb.color_temp_tex);
	SAFE_RELEASE(m_efb.color_cache_tex);
	D3D::command_list_mgr->DestroyResourceAfterCurrentCommandListExecuted(m_efb.color_cache_buf);

	SAFE_RELEASE(m_efb.resolved_color_tex);
	SAFE_RELEASE(m_efb.depth_tex);
	SAFE_RELEASE(m_efb.depth_cache_tex);
	D3D::command_list_mgr->DestroyResourceAfterCurrentCommandListExecuted(m_efb.depth_cache_buf);

	SAFE_RELEASE(m_efb.resolved_depth_tex);
}

void FramebufferManager::CopyToRealXFB(u32 xfbAddr, u32 fbStride, u32 fbHeight, const EFBRectangle& sourceRc, float gamma)
{
	u8* dst = Memory::GetPointer(xfbAddr);
	s_xfbEncoder.Encode(dst, fbStride / 2, fbHeight, sourceRc, gamma);
}

std::unique_ptr<XFBSourceBase> FramebufferManager::CreateXFBSource(unsigned int target_width, unsigned int target_height, unsigned int layers)
{
	return std::make_unique<XFBSource>(D3DTexture2D::Create(target_width, target_height,
		(D3D11_BIND_FLAG)(D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE),
		D3D11_USAGE_DEFAULT, DXGI_FORMAT_R8G8B8A8_UNORM, 1, layers), layers);
}

void FramebufferManager::GetTargetSize(unsigned int* width, unsigned int* height)
{
	*width = m_target_width;
	*height = m_target_height;
}

void FramebufferManager::ResolveDepthTexture()
{
	// ResolveSubresource does not work with depth textures.
	// Instead, we use a shader that selects the minimum depth from all samples.

	const D3D12_VIEWPORT vp = { 0.f, 0.f, static_cast<float>(m_target_width), static_cast<float>(m_target_height), D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
	D3D::current_command_list->RSSetViewports(1, &vp);

	m_efb.resolved_depth_tex->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_RENDER_TARGET);
	D3D::current_command_list->OMSetRenderTargets(0, &m_efb.resolved_depth_tex->GetRTV(), FALSE, nullptr);

	m_efb.depth_tex->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	D3D::SetPointCopySampler();

	// Render a quad covering the entire target, writing SV_Depth.
	const D3D12_RECT source_rect = CD3DX12_RECT(0, 0, m_target_width, m_target_height);
	D3D::DrawShadedTexQuad(
		m_efb.depth_tex,
		&source_rect,
		m_target_width,
		m_target_height,
		StaticShaderCache::GetDepthCopyPixelShader(true),
		StaticShaderCache::GetSimpleVertexShader(),
		StaticShaderCache::GetSimpleVertexShaderInputLayout(),
		StaticShaderCache::GetCopyGeometryShader(),
		1.0,
		0,
		DXGI_FORMAT_R32_FLOAT
		);

	m_efb.color_tex->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_efb.depth_tex->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	D3D::current_command_list->OMSetRenderTargets(1, &m_efb.color_tex->GetRTV(), FALSE, &m_efb.depth_tex->GetDSV());

	// Restores proper viewport/scissor settings.
	g_renderer->RestoreAPIState();
}

void XFBSource::DecodeToTexture(u32 xfbAddr, u32 fbWidth, u32 fbHeight)
{
	// DX12's XFB decoder does not use this function.
	// YUYV data is decoded in Render::Swap.
}

void XFBSource::CopyEFB(float gamma)
{
	// Copy EFB data to XFB and restore render target again
	const D3D12_VIEWPORT vp12 = { 0.f, 0.f,  static_cast<float>(texWidth), static_cast<float>(texHeight), D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
	D3D::current_command_list->RSSetViewports(1, &vp12);

	const D3D12_RECT rect = CD3DX12_RECT(0, 0, texWidth, texHeight);

	m_tex->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_RENDER_TARGET);
	D3D::current_command_list->OMSetRenderTargets(1, &m_tex->GetRTV(), FALSE, nullptr);

	D3D::SetPointCopySampler();

	D3D::DrawShadedTexQuad(
		FramebufferManager::GetEFBColorTexture(),
		&rect,
		Renderer::GetTargetWidth(),
		Renderer::GetTargetHeight(),
		StaticShaderCache::GetColorCopyPixelShader(true),
		StaticShaderCache::GetSimpleVertexShader(),
		StaticShaderCache::GetSimpleVertexShaderInputLayout(),
		StaticShaderCache::GetCopyGeometryShader(),
		gamma,
		0,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		false,
		m_tex->GetMultisampled()
		);

	FramebufferManager::GetEFBColorTexture()->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_RENDER_TARGET);
	FramebufferManager::GetEFBDepthTexture()->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	D3D::current_command_list->OMSetRenderTargets(1, &FramebufferManager::GetEFBColorTexture()->GetRTV(), FALSE, &FramebufferManager::GetEFBDepthTexture()->GetDSV());

	// Restores proper viewport/scissor settings.
	g_renderer->RestoreAPIState();
}

u32 FramebufferManager::GetEFBCachedColor(u32 x, u32 y)
{
	if (!m_efb.color_cache_data)
		PopulateEFBColorCache();

	const u32* row = reinterpret_cast<const u32*>(m_efb.color_cache_data + y * EFB_CACHE_PITCH);
	return row[x];
}

float FramebufferManager::GetEFBCachedDepth(u32 x, u32 y)
{
	if (!m_efb.depth_cache_data)
		PopulateEFBDepthCache();

	u32 row_offset = y * EFB_CACHE_PITCH;
	const float* row = reinterpret_cast<const float*>(m_efb.depth_cache_data + y * EFB_CACHE_PITCH);
	return row[x];
}

void FramebufferManager::SetEFBCachedColor(u32 x, u32 y, u32 value)
{
	if (!m_efb.color_cache_data)
		return;

	u32* row = reinterpret_cast<u32*>(m_efb.color_cache_data + y * EFB_CACHE_PITCH);
	row[x] = value;
}

void FramebufferManager::SetEFBCachedDepth(u32 x, u32 y, float value)
{
	if (!m_efb.depth_cache_data)
		return;

	float* row = reinterpret_cast<float*>(m_efb.depth_cache_data + y * EFB_CACHE_PITCH);
	row[x] = value;
}

void FramebufferManager::PopulateEFBColorCache()
{
	_dbg_assert_(!m_efb.color_readback_buffer_data, "cache is invalid");
	D3D::command_list_mgr->CPUAccessNotify();
	// for non-1xIR or multisampled cases, we need to copy to an intermediate texture first
	DX12::D3DTexture2D* src_texture;
	if (g_ActiveConfig.iEFBScale != SCALE_1X || g_ActiveConfig.iMultisamples > 1)
	{
		D3D12_RECT src_rect = { 0, 0, static_cast<LONG>(m_target_width), static_cast<LONG>(m_target_height) };
		D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(EFB_WIDTH),static_cast<float>(EFB_HEIGHT), D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
		D3D::current_command_list->RSSetViewports(1, &vp);
		m_efb.color_cache_tex->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_RENDER_TARGET);
		D3D::current_command_list->OMSetRenderTargets(1, &m_efb.color_cache_tex->GetRTV(), FALSE, nullptr);
		D3D::SetPointCopySampler();

		D3D::DrawShadedTexQuad(
			m_efb.color_tex,
			&src_rect,
			m_target_width,
			m_target_height,
			StaticShaderCache::GetColorCopyPixelShader(true),
			StaticShaderCache::GetSimpleVertexShader(),
			StaticShaderCache::GetSimpleVertexShaderInputLayout(),
			D3D12_SHADER_BYTECODE()
			);
		src_texture = m_efb.color_cache_tex;
	}
	else
	{
		// can copy directly from efb texture
		src_texture = m_efb.color_tex;
	}

	// copy to system memory
	D3D12_BOX src_box = CD3DX12_BOX(0, 0, 0, EFB_WIDTH, EFB_HEIGHT, 1);

	D3D12_TEXTURE_COPY_LOCATION dst_location = {};
	dst_location.pResource = m_efb.color_cache_buf;
	dst_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst_location.PlacedFootprint.Offset = 0;
	dst_location.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	dst_location.PlacedFootprint.Footprint.Width = EFB_WIDTH;
	dst_location.PlacedFootprint.Footprint.Height = EFB_HEIGHT;
	dst_location.PlacedFootprint.Footprint.Depth = 1;
	dst_location.PlacedFootprint.Footprint.RowPitch = EFB_CACHE_PITCH;

	D3D12_TEXTURE_COPY_LOCATION src_location = {};
	src_location.pResource = src_texture->GetTex();
	src_location.SubresourceIndex = 0;
	src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

	src_texture->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_COPY_SOURCE);
	D3D::current_command_list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, &src_box);
	// Need to wait for the CPU to complete the copy (and all prior operations) before we can read it on the CPU.
	D3D::command_list_mgr->ExecuteQueuedWork(true);

	// Restores proper viewport/scissor settings.
	m_efb.color_tex->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_efb.depth_tex->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	D3D::current_command_list->OMSetRenderTargets(1, &m_efb.color_tex->GetRTV(), FALSE, &m_efb.depth_tex->GetDSV());
	g_renderer->RestoreAPIState();

	HRESULT hr = m_efb.color_cache_buf->Map(0, nullptr, reinterpret_cast<void**>(&m_efb.color_cache_data));	
	CHECK(SUCCEEDED(hr), "failed to map efb peek color cache texture (hr=%08X)", hr);
}

void FramebufferManager::PopulateEFBDepthCache()
{
	_dbg_assert_(!m_efb.depth_staging_buf_map.pData, "cache is invalid");
	D3D::command_list_mgr->CPUAccessNotify();
	// for non-1xIR or multisampled cases, we need to copy to an intermediate texture first
	DX12::D3DTexture2D* src_texture;
	if (g_ActiveConfig.iEFBScale != SCALE_1X || g_ActiveConfig.iMultisamples > 1)
	{
		const D3D12_VIEWPORT vp12 = { 0.f, 0.f, static_cast<float>(EFB_WIDTH), static_cast<float>(EFB_HEIGHT), D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
		D3D::current_command_list->RSSetViewports(1, &vp12);

		m_efb.depth_cache_tex->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_RENDER_TARGET);
		D3D::current_command_list->OMSetRenderTargets(0, &m_efb.depth_cache_tex->GetRTV(), FALSE, nullptr);

		m_efb.depth_tex->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		D3D::SetPointCopySampler();

		// Render a quad covering the entire target, writing SV_Depth.
		const D3D12_RECT source_rect = CD3DX12_RECT(0, 0, m_target_width, m_target_height);
		D3D::DrawShadedTexQuad(
			m_efb.depth_tex,
			&source_rect,
			m_target_width,
			m_target_height,
			StaticShaderCache::GetDepthCopyPixelShader(true),
			StaticShaderCache::GetSimpleVertexShader(),
			StaticShaderCache::GetSimpleVertexShaderInputLayout(),
			StaticShaderCache::GetCopyGeometryShader(),
			1.0,
			0,
			DXGI_FORMAT_R32_FLOAT
			);		
		src_texture = m_efb.depth_cache_tex;
	}
	else
	{
		// can copy directly from efb texture
		src_texture = m_efb.depth_tex;
	}

	// copy to system memory
	D3D12_BOX src_box = CD3DX12_BOX(0, 0, 0, EFB_WIDTH, EFB_HEIGHT, 1);

	D3D12_TEXTURE_COPY_LOCATION dst_location = {};
	dst_location.pResource = m_efb.depth_cache_buf;
	dst_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst_location.PlacedFootprint.Offset = 0;
	dst_location.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
	dst_location.PlacedFootprint.Footprint.Width = EFB_WIDTH;
	dst_location.PlacedFootprint.Footprint.Height = EFB_HEIGHT;
	dst_location.PlacedFootprint.Footprint.Depth = 1;
	dst_location.PlacedFootprint.Footprint.RowPitch = EFB_CACHE_PITCH;

	D3D12_TEXTURE_COPY_LOCATION src_location = {};
	src_location.pResource = src_texture->GetTex();
	src_location.SubresourceIndex = 0;
	src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

	src_texture->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_COPY_SOURCE);
	D3D::current_command_list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, &src_box);
	
	// Need to wait for the CPU to complete the copy (and all prior operations) before we can read it on the CPU.
	D3D::command_list_mgr->ExecuteQueuedWork(true);
	
	// Restores proper viewport/scissor settings.
	m_efb.color_tex->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_efb.depth_tex->TransitionToResourceState(D3D::current_command_list, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	D3D::current_command_list->OMSetRenderTargets(1, &m_efb.color_tex->GetRTV(), FALSE, &m_efb.depth_tex->GetDSV());
	g_renderer->RestoreAPIState();

	HRESULT hr = m_efb.depth_cache_buf->Map(0, nullptr, reinterpret_cast<void**>(&m_efb.depth_cache_data));	
	CHECK(SUCCEEDED(hr), "failed to map efb peek color cache texture (hr=%08X)", hr);
}

void FramebufferManager::InvalidateEFBCache()
{
	if (m_efb.color_cache_data)
	{
		m_efb.color_cache_buf->Unmap(0, nullptr);
		m_efb.color_cache_data = nullptr;
	}

	if (m_efb.depth_cache_data)
	{
		m_efb.depth_cache_buf->Unmap(0, nullptr);
		m_efb.depth_cache_data = nullptr;
	}
}

}  // namespace DX12