// WebGPU device ownership lives in webgpu.mjs. The engine still uses the same
// CKFFPLib translator, vertex conversion and uniform ABI as native renderers.
#include "CKFFContextState.h"
#include "CKFFProgramLayout.h"
#include "CKFFResourceStore.h"
#include "CKFFShaderInterface.h"
#include "CKFixedFunctionPipeline.h"
#include "CKRasterizer.h"
#include "CKVertexLayoutCache.h"
#include <SDL3/SDL_video.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <emscripten.h>
#include <vector>

EM_JS(int, web_init, (int w, int h, int pixelWidth, int pixelHeight),
      { return Module['ballanceGpu'] ?.resize(w, h, pixelWidth, pixelHeight) ? 1 : 0; });
EM_JS(void, web_clear, (unsigned flags, unsigned color, float depth, unsigned stencil, const int *rect), {
    Module['ballanceGpu'].clearRect(flags, color, depth, stencil,
                                    Array.from(HEAP32.subarray(rect >>> 2, (rect >>> 2) + 4)));
});
EM_JS(int, web_samples, (unsigned samples), { return Module['ballanceGpu'].setSamples(samples) ? 1 : 0; });
EM_JS(void, web_present, (), { Module['ballanceGpu'].present(); });
EM_JS(void, web_shutdown, (), { Module['ballanceGpu'].shutdown(); });
EM_JS(int, web_texture, (unsigned id, int w, int h, int depth, int levels, int kind),
      { return Module['ballanceGpu'].texture(id, w, h, depth, levels, kind) ? 1 : 0; });
EM_JS(int, web_upload, (unsigned id, int mip, int face, int w, int h, int x, int y, const void *data), {
    return Module['ballanceGpu'].upload(id, mip, face, w, h, x, y, HEAPU8.subarray(data, data + w * h * 4))
               ? 1
               : 0;
});
EM_JS(void, web_delete, (unsigned id), { Module['ballanceGpu'].deleteTexture(id); });
EM_JS(void, web_draw_begin,
      (const unsigned *state, int pt, int layout, const void *vertices, int count, const void *indices,
       int indexCount, int index32),
      {
          Module['ballanceGpu'].beginDraw(
              Array.from(HEAPU32.subarray(state >>> 2, (state >>> 2) + 13)), pt, layout,
              HEAPU8.slice(vertices, vertices + count * 256), count,
              indexCount ? HEAPU8.slice(indices, indices + indexCount * (index32 ? 4 : 2)) : null, indexCount,
              index32);
      });
EM_JS(void, web_uniform, (int group, int binding, const void *data, int size),
      { Module['ballanceGpu'].uniform(group, binding, HEAPU8.subarray(data, data + size)); });
EM_JS(void, web_binding, (int slot, unsigned texture, const unsigned *sampler, int dimension), {
    Module['ballanceGpu'].binding(
        slot, texture, Array.from(HEAPU32.subarray(sampler >>> 2, (sampler >>> 2) + 12)), dimension);
});
EM_JS(int, web_draw_end, (), { return Module['ballanceGpu'].endDraw() ? 1 : 0; });

namespace {
constexpr uint64_t features = CKRST_DEVCAPS_VERTEX_SHADER | CKRST_DEVCAPS_PIXEL_SHADER |
                              CKRST_DEVCAPS_TRANSIENT_BUFFERS | CKRST_DEVCAPS_SCISSOR |
                              CKRST_DEVCAPS_BUFFER_UPDATE | CKRST_DEVCAPS_TEXTURE_UPDATE |
                              CKRST_DEVCAPS_BLEND_EQUATION | CKRST_DEVCAPS_TEXTURE_CUBE |
                              CKRST_DEVCAPS_TEXTURE_3D | CKRST_DEVCAPS_STENCIL_WRITE_MASK;

class Context final : public CKRasterizerContext {
    CKFixedFunctionPipeline ff;
    CKFFResourceStore resources;
    CKDWORD nextHandle = 1;
    CKFFProgramDesc interfaces[2][3];
    CKFFProgramLayout uniforms[2][3];
    bool scene = false;
    bool ResizeDrawable(WIN_HANDLE window, int width, int height) {
        int pixelWidth = width, pixelHeight = height;
        if (window)
            SDL_GetWindowSizeInPixels(static_cast<SDL_Window *>(window), &pixelWidth, &pixelHeight);
        if (!web_init(width, height, pixelWidth, pixelHeight))
            return false;
        ff.SetTargetExtents(width, height, pixelWidth, pixelHeight);
        return true;
    }

  public:
    explicit Context(CKRasterizerDriver *driver) : CKRasterizerContext(driver) {}
    ~Context() override {
        BeginShutdown();
    }
    CKBOOL Create(WIN_HANDLE window, int x, int y, int w, int h, int bpp, CKBOOL full, int hz, int z,
                  int stencil) override {
        if (m_Created)
            return FALSE;
        SetContextDesc(window, x, y, w, h, bpp, full, hz, z, stencil);
        if (!ff.Init(features, 16,
                     CKRST_SHADER_TARGET_MANUAL_VOLUME_ANISO | CKRST_SHADER_TARGET_MANUAL_BORDER |
                         CKRST_SHADER_TARGET_MANUAL_COMPARE_BIAS))
            return FALSE;
        for (int pt = 0; pt < 2; ++pt)
            for (int layout = 0; layout < 3; ++layout) {
                interfaces[pt][layout] =
                    CKFFBuildProgramInterface(1, 2, FALSE, pt, CKFFSamplerLayout(layout));
                uniforms[pt][layout].Init(interfaces[pt][layout]);
            }
        ff.InitDefaultStates();
        CKViewportData viewport;
        viewport.ViewWidth = w;
        viewport.ViewHeight = h;
        ff.SetViewport(viewport);
        if (!ResizeDrawable(window, w, h))
            return FALSE;
        m_Created = TRUE;
        return TRUE;
    }
    CKBOOL Resize(int x, int y, int w, int h, CKDWORD flags) override {
        if (!m_Created || scene || !ResolveResize(x, y, w, h, flags) || !ResizeDrawable(m_Window, w, h))
            return FALSE;
        m_PosX = x;
        m_PosY = y;
        m_Width = w;
        m_Height = h;
        auto vp = ff.GetViewport();
        vp.ViewX = vp.ViewY = 0;
        vp.ViewWidth = w;
        vp.ViewHeight = h;
        ff.SetViewport(vp);
        return TRUE;
    }
    CKBOOL SetOptions(const CKRasterizerOptions *options) override {
        if (!options || !web_samples(options->MSAASamples))
            return FALSE;
        SetContextOptions(*options);
        ff.SetRenderOptions(options->DisableTextureFiltering, options->DisableMipmaps,
                            options->ForceAnisotropicFiltering);
        return TRUE;
    }
    CKBOOL GetCaps(CKRasterizerCapsDesc *caps) const override {
        if (!caps)
            return FALSE;
        CKRasterizerNativeCapsDesc source;
        if (!m_Driver->GetNativeCaps(&source))
            return FALSE;
        caps->Features = source.Features;
        caps->MaxTextureSize = source.MaxTextureSize;
        caps->MaxTextureStages = source.MaxTextureStages;
        caps->MaxAnisotropy = source.MaxAnisotropy;
        caps->MaxUserClipPlanes = source.MaxUserClipPlanes;
        caps->MaxVertexBlendMatrices = source.MaxVertexBlendMatrices;
        caps->MaxMSAASamples = source.MaxMSAASamples;
        caps->MaxPointSize = source.MaxPointSize;
        caps->MaxLights = source.MaxLights;
        return TRUE;
    }
    CKERROR GetDeviceStatus() const override {
        return m_Created && !m_ShuttingDown ? CK_OK : CKERR_INVALIDOPERATION;
    }
    CKBOOL BeginShutdown() override {
        if (m_ShuttingDown)
            return TRUE;
        m_ShuttingDown = TRUE;
        if (m_Created) {
            web_shutdown();
            resources.Clear();
            ff.Shutdown();
        }
        m_Created = FALSE;
        return TRUE;
    }
    CKBOOL IsIdle() const override {
        return !scene;
    }
    CKBOOL Clear(CKDWORD flags, CKDWORD color, float z, CKDWORD stencil, int count, CKRECT *rects) override {
        if (!m_Created || count < 0 || (count && !rects))
            return FALSE;
        CKRECT viewport;
        if (!count) {
            const auto &vp = ff.GetViewport();
            viewport = {int(vp.ViewX), int(vp.ViewY), int(vp.ViewX + vp.ViewWidth),
                        int(vp.ViewY + vp.ViewHeight)};
            rects = &viewport;
            count = 1;
        }
        for (int i = 0; i < count; ++i)
            web_clear(flags, color, z, stencil, &rects[i].left);
        return TRUE;
    }
    CKBOOL BeginScene() override {
        if (scene || !m_Created)
            return FALSE;
        scene = true;
        return TRUE;
    }
    CKBOOL EndScene() override {
        if (!scene)
            return FALSE;
        scene = false;
        return TRUE;
    }
    CKBOOL BeginOverlayPhase() override {
        return m_Created;
    }
    CKBOOL BackToFront(CKBOOL) override {
        if (!m_Created)
            return FALSE;
        web_present();
        ff.SetFrameNumber(++m_FrameNumber);
        return TRUE;
    }
    CKBOOL SetRenderState(VXRENDERSTATETYPE s, CKDWORD v) override {
        return CKFFSetRenderState(ff, m_Stats, s, v);
    }
    CKBOOL GetRenderState(VXRENDERSTATETYPE s, CKDWORD *v) override {
        return CKFFGetRenderState(ff, m_Stats, s, v);
    }
    CKBOOL SetTextureStageState(int stage, CKRST_TEXTURESTAGESTATETYPE s, CKDWORD v) override {
        return CKFFSetTextureStageState(ff, m_Stats, stage, s, v);
    }
    CKBOOL GetTextureStageState(int stage, CKRST_TEXTURESTAGESTATETYPE s, CKDWORD *v) override {
        return CKFFGetTextureStageState(ff, m_Stats, stage, s, v);
    }
    CKBOOL ResetTextureStages(int first, int count) override {
        return CKFFResetTextureStages(ff, m_Stats, first, count);
    }
    CKBOOL SetTexture(CKDWORD texture, int stage) override {
        return CKFFSetTextureBinding(ff, m_Stats, texture, resources.FindTexture(texture), stage);
    }
    CKBOOL GetTexture(int stage, CKDWORD *value) override {
        return CKFFGetTextureBinding(ff, m_Stats, stage, value);
    }
    CKBOOL SetTransformMatrix(VXMATRIX_TYPE type, const VxMatrix &matrix) override {
        return CKFFSetTransform(ff, m_Stats, type, matrix);
    }
    CKBOOL GetTransformMatrix(VXMATRIX_TYPE type, VxMatrix &matrix) override {
        return CKFFGetTransform(ff, m_Stats, type, matrix);
    }
    CKBOOL SetLight(CKDWORD i, const CKLightData *light) override {
        return CKFFSetLight(ff, m_Stats, i, light);
    }
    CKBOOL EnableLight(CKDWORD i, CKBOOL enabled) override {
        return CKFFEnableLight(ff, m_Stats, i, enabled);
    }
    CKBOOL SetMaterial(const CKMaterialData *material) override {
        return CKFFSetMaterial(ff, material);
    }
    CKBOOL ApplyMaterial(const CKMaterialRenderState &state) override {
        return CKFFApplyMaterial(ff, state);
    }
    CKBOOL SetViewport(const CKViewportData *data) override {
        return CKFFSetViewport(ff, m_Stats, data);
    }
    CKBOOL SetUserClipPlane(CKDWORD i, const VxPlane &plane) override {
        return CKFFSetUserClipPlane(ff, m_Stats, i, plane);
    }
    CKBOOL GetUserClipPlane(CKDWORD i, VxPlane &plane) override {
        return CKFFGetUserClipPlane(ff, m_Stats, i, plane);
    }
    void InitDefaultRenderStatesValue() override {
        ff.InitDefaultStates();
    }
    CKBOOL DrawPrimitive(VXPRIMITIVETYPE type, CKWORD *indices, int count,
                         VxDrawPrimitiveData *data) override {
        if (!data || !m_Created ||
            !ff.PreparePrimitive(type, indices, indices ? count : data->VertexCount, data))
            return FALSE;
        return Submit();
    }
    CKBOOL DrawPrimitiveVB(VXPRIMITIVETYPE type, CKDWORD handle, CKDWORD first, CKDWORD count,
                           CKWORD *indices, int icount) override {
        auto *vb = resources.FindVertexBuffer(handle);
        if (!vb || vb->Locked || first >= vb->Desc.m_MaxVertexCount ||
            count > vb->Desc.m_MaxVertexCount - first)
            return FALSE;
        VxDrawPrimitiveData data;
        vb->SetupDrawData(data, first, count);
        for (int stage = 0; stage < vb->Layout.TexcoordCount; ++stage)
            ff.SetTexcoordComponentCount(stage, vb->Layout.TexcoordDims[stage]);
        const CKBOOL result = DrawPrimitive(type, indices, icount, &data);
        ff.ResetTexcoordComponentCounts();
        return result;
    }
    CKBOOL DrawPrimitiveVBIB(VXPRIMITIVETYPE type, CKDWORD vh, CKDWORD ih, CKDWORD first, CKDWORD count,
                             CKDWORD start, int icount) override {
        auto *ib = resources.FindIndexBuffer(ih);
        if (!ib || ib->Locked || icount < 0 || start > ib->Desc.m_MaxIndexCount ||
            unsigned(icount) > ib->Desc.m_MaxIndexCount - start)
            return FALSE;
        auto *vb = resources.FindVertexBuffer(vh);
        if (!vb || first >= vb->Desc.m_MaxVertexCount || count > vb->Desc.m_MaxVertexCount - first)
            return FALSE;
        // D3D7 indexed-buffer indices are absolute within the entire VB.
        // MinVertexIndex describes the touched range; it is not a base offset.
        return DrawPrimitiveVB(type, vh, 0, first + count, const_cast<CKWORD *>(ib->DrawIndices(start)),
                               icount);
    }
    CKBOOL CreateTexture(const CKTextureDesc *desc, CKDWORD *handle) override {
        if (handle)
            *handle = 0;
        if (!desc || !handle || desc->Format.Width <= 0 || desc->Format.Height <= 0)
            return FALSE;
        CKTextureDesc copy = *desc;
        copy.Flags |= CKRST_TEXTURE_VALID;
        copy.Depth = std::max(1u, copy.Depth);
        int levels = copy.MipMapCount;
        if (copy.MipMapCount == CKRST_MIPMAP_GENERATE) {
            levels = 1;
            for (int size = std::max(copy.Format.Width, copy.Format.Height); size > 1; size >>= 1)
                ++levels;
        }
        levels = std::max(1, levels);
        const int kind = copy.Flags & CKRST_TEXTURE_CUBEMAP     ? 1
                         : copy.Flags & CKRST_TEXTURE_VOLUMEMAP ? 2
                                                                : 0;
        const CKDWORD id = nextHandle++;
        if (copy.Flags & CKRST_TEXTURE_DEPTHSTENCIL ||
            !web_texture(id, copy.Format.Width, copy.Format.Height, copy.Depth, levels, kind))
            return FALSE;
        if (!resources.InsertTexture(id, copy)) {
            web_delete(id);
            return FALSE;
        }
        *handle = id;
        return TRUE;
    }
    CKBOOL LoadTexture(CKDWORD id, const VxImageDescEx &image, int level, CKRST_CUBEFACE face,
                       const CKRECT *region) override {
        const auto *desc = resources.FindTexture(id);
        if (!desc || !image.Image || level < 0 || level >= 32 || image.Width <= 0 || image.Height <= 0)
            return FALSE;
        VxImageDescEx rgba;
        VxPixelFormat2ImageDesc(_32_ABGR8888, rgba);
        rgba.Width = image.Width;
        rgba.Height = image.Height;
        rgba.BytesPerLine = rgba.Width * 4;
        std::vector<CKBYTE> pixels(size_t(rgba.Width) * rgba.Height * 4);
        rgba.Image = pixels.data();
        VxDoBlit(image, rgba);
        return web_upload(id, level, int(face), rgba.Width, rgba.Height, region ? region->left : 0,
                          region ? region->top : 0, pixels.data());
    }
    CKBOOL GetTextureDesc(CKDWORD h, CKTextureDesc *desc) const override {
        return resources.GetTextureDesc(h, desc);
    }
    CKBOOL CreateVertexBuffer(const CKVertexBufferDesc *desc, const void *data, CKDWORD *out) override {
        if (out)
            *out = 0;
        if (!desc || !out)
            return FALSE;
        CKFFVertexBufferData vb;
        XArray<CKBYTE> initial;
        if (vb.Initialize(*desc, data, initial) != CK_OK)
            return FALSE;
        CKDWORD h = nextHandle++;
        if (!resources.InsertVertexBuffer(h, vb))
            return FALSE;
        *out = h;
        return TRUE;
    }
    CKBOOL GetVertexBufferDesc(CKDWORD h, CKVertexBufferDesc *desc) const override {
        return resources.GetVertexBufferDesc(h, desc);
    }
    CKBOOL CreateIndexBuffer(const CKIndexBufferDesc *desc, const void *data, CKDWORD *out) override {
        if (out)
            *out = 0;
        if (!desc || !out)
            return FALSE;
        CKFFIndexBufferData ib;
        if (ib.Initialize(*desc, data) != CK_OK)
            return FALSE;
        CKDWORD h = nextHandle++;
        if (!resources.InsertIndexBuffer(h, ib))
            return FALSE;
        *out = h;
        return TRUE;
    }
    CKBOOL GetIndexBufferDesc(CKDWORD h, CKIndexBufferDesc *desc) const override {
        return resources.GetIndexBufferDesc(h, desc);
    }
    void *LockVertexBuffer(CKDWORD h, CKDWORD start, CKDWORD count, CKRST_LOCKFLAGS flags) override {
        auto *b = resources.FindVertexBuffer(h);
        CKERROR error;
        return b ? b->Lock(start, count, flags, error) : nullptr;
    }
    CKBOOL UnlockVertexBuffer(CKDWORD h) override {
        auto *b = resources.FindVertexBuffer(h);
        CKFFBufferUpload u;
        return b && b->PrepareUnlock(u) == CK_OK;
    }
    void *LockIndexBuffer(CKDWORD h, CKDWORD start, CKDWORD count, CKRST_LOCKFLAGS flags) override {
        auto *b = resources.FindIndexBuffer(h);
        CKERROR error;
        return b ? b->Lock(start, count, flags, error) : nullptr;
    }
    CKBOOL UnlockIndexBuffer(CKDWORD h) override {
        auto *b = resources.FindIndexBuffer(h);
        CKFFBufferUpload u;
        return b && b->PrepareUnlock(u) == CK_OK;
    }
    CKBOOL DeleteObject(CKRST_HANDLE h, CKRST_OBJECTTYPE type) override {
        if (!resources.IsAlive(h, type))
            return FALSE;
        if (type == CKRST_OBJ_TEXTURE) {
            for (int i = 0; i < 8; ++i)
                if (ff.GetTexture(i) == h)
                    ff.SetTexture(i, 0);
            web_delete(h);
        }
        resources.Remove(h, type);
        return TRUE;
    }
    CKBOOL FlushObjects(CKRST_OBJECTMASK mask) override {
        XClassArray<CKFFResourceHandle> handles;
        resources.CollectHandles(mask, handles);
        for (auto h : handles)
            DeleteObject(h.Handle, h.Type);
        return TRUE;
    }
    CKBOOL SetResourceName(CKRST_HANDLE h, CKRST_OBJECTTYPE type, CKSTRING) override {
        return resources.IsAlive(h, type);
    }
    CKBOOL SetTargetTexture(CKDWORD h, int, int, CKRST_CUBEFACE) override {
        return h == 0;
    }
    CKBOOL CopyToTexture(CKDWORD, const VxRect *, const VxRect *, CKRST_CUBEFACE) override {
        return FALSE;
    }
    int CopyToMemoryBuffer(const CKRECT *, VXBUFFER_TYPE, VxImageDescEx &) override {
        return 0;
    }
    int CopyFromMemoryBuffer(const CKRECT *, VXBUFFER_TYPE, const VxImageDescEx &) override {
        return 0;
    }
    CKBOOL RequestReadback(const CKRECT *, VXBUFFER_TYPE, CKReadbackCallback, void *) override {
        return FALSE;
    }

  private:
    CKBOOL Submit() {
        const auto &draw = ff.GetDraw();
        if (draw.SkipSubmit)
            return TRUE;
        if (!draw.Vertices || !draw.Constants || !draw.Textures || !draw.ProgramContext)
            return FALSE;
        const int pt = draw.ProgramContext->ShaderKey.VS.GetHasPositionT() ? 1 : 0;
        const int layout = int(draw.Textures->SamplerLayoutPlan.Layout);
        if (layout < 0 || layout >= 3)
            return FALSE;
        auto &iface = interfaces[pt][layout];
        auto &packed = uniforms[pt][layout];
        packed.Update(*draw.Constants);
        CKVertexElementDesc elements[20];
        CKVertexLayoutDesc vertexLayout;
        if (!CKFFVertexLayout::BuildLayout(draw.VertexFormat, elements, 20, vertexLayout))
            return FALSE;
        std::vector<CKDWORD> vertices(size_t(draw.VertexCount) * 64);
        for (unsigned v = 0; v < draw.VertexCount; ++v)
            for (const auto &input : iface.VertexInputs) {
                CKDWORD *dest = vertices.data() + size_t(v) * 64 + input.Location * 4;
                memcpy(dest, input.DefaultValue, 16);
                for (unsigned e = 0; e < vertexLayout.ElementCount; ++e) {
                    const auto &elem = elements[e];
                    if (elem.Attrib != input.Attribute)
                        continue;
                    const CKBYTE *src = draw.Vertices + size_t(v) * draw.VertexStride + elem.Offset;
                    if (elem.Type == CKRST_ATTRIBTYPE_FLOAT)
                        memcpy(dest, src, elem.Count * 4);
                    else if (elem.Type == CKRST_ATTRIBTYPE_UINT8)
                        for (unsigned j = 0; j < elem.Count; ++j) {
                            if (input.Integer)
                                dest[j] = src[j];
                            else
                                reinterpret_cast<float *>(dest)[j] =
                                    elem.Normalized ? src[j] / 255.f : float(src[j]);
                        }
                    else
                        return FALSE;
                }
            }
        const auto &p = draw.Pipeline;
        CKDWORD state[13] = {p.State.Lo,
                             p.State.Mid,
                             p.State.Hi,
                             p.StencilRef,
                             p.StencilReadMask,
                             p.StencilWriteMask,
                             CKDWORD(p.ScissorEnabled),
                             CKDWORD(p.Scissor.left),
                             CKDWORD(p.Scissor.top),
                             CKDWORD(p.Scissor.right),
                             CKDWORD(p.Scissor.bottom),
                             CKDWORD(m_Width),
                             CKDWORD(m_Height)};
        web_draw_begin(state, pt, layout, vertices.data(), draw.VertexCount, draw.Indices, draw.IndexCount,
                       draw.Index32);
        for (const auto &decl : iface.Samplers) {
            const auto &binding = draw.Textures->NativeBindings[decl.Slot];
            const auto &sampler = binding.Sampler;
            web_binding(decl.NativeSlot, binding.Texture, reinterpret_cast<const CKDWORD *>(&sampler),
                        int(decl.Dimension));
            const auto offset = packed.BufferOffset(CKRST_SHADER_PIXEL, decl.MetadataBufferSlot);
            if (offset == UINT32_MAX)
                continue;
            float rgba[4] = {float((sampler.BorderColor >> 16) & 255) / 255,
                             float((sampler.BorderColor >> 8) & 255) / 255,
                             float(sampler.BorderColor & 255) / 255, float(sampler.BorderColor >> 24) / 255};
            float info[4] = {
                float(unsigned(sampler.AddressU) | (unsigned(sampler.AddressV) << 4) |
                      (unsigned(sampler.AddressW) << 8)),
                float(sampler.MinFilter), float(sampler.MagFilter),
                float((unsigned(sampler.MipFilter) & 15) | ((sampler.MaxAnisotropy & 255) << 4) |
                      (decl.Dimension == CKFF_TEXTURE_CUBE ? (sampler.MinMipLevel & 31) << 12 : 0))};
            memcpy(packed.Data.Begin() + offset + decl.BorderColorOffset, rgba, 16);
            memcpy(packed.Data.Begin() + offset + decl.SamplerStateOffset, info, 16);
        }
        for (const auto &b : packed.Buffers)
            web_uniform(b.Stage == CKRST_SHADER_VERTEX ? 1 : 3, b.Slot, packed.Data.Begin() + b.Offset,
                        b.Size);
        return ff.FinishDraw(web_draw_end() ? CK_OK : CKERR_INVALIDOPERATION, 0);
    }
};
class Driver final : public CKRasterizerDriver {
  public:
    explicit Driver(CKRasterizer *owner) : CKRasterizerDriver(owner, 0, "WebGPU", TRUE) {
        for (auto size :
             {std::pair{640, 480}, std::pair{800, 600}, std::pair{1280, 720}, std::pair{1920, 1080}}) {
            VxDisplayMode mode;
            mode.Width = size.first;
            mode.Height = size.second;
            mode.Bpp = 32;
            mode.RefreshRate = 60;
            m_DisplayModes.PushBack(mode);
        }
        CKTextureDesc texture;
        texture.Flags = CKRST_TEXTURE_VALID | CKRST_TEXTURE_RGB | CKRST_TEXTURE_ALPHA;
        VxPixelFormat2ImageDesc(_32_ARGB8888, texture.Format);
        m_TextureFormats.PushBack(texture);
        m_NativeCaps.MaxTextureSize = 8192;
        m_NativeCaps.MaxTextureStages = 8;
        m_NativeCaps.MaxLights = 8;
        m_NativeCaps.MaxUserClipPlanes = 0;
        m_NativeCaps.MaxVertexBlendMatrices = 4;
        m_NativeCaps.MaxAnisotropy = 1;
        m_NativeCaps.MaxPointSize = 1;
        m_NativeCaps.MaxMSAASamples = 4;
        m_NativeCaps.Features = CKRST_CAPS_MSAA | CKRST_CAPS_TEXTURE_CUBE | CKRST_CAPS_TEXTURE_VOLUME |
                                CKRST_CAPS_SEPARATE_ALPHA_BLEND | CKRST_CAPS_STENCIL_WRITE_MASK;
        m_CapsFinal = TRUE;
    }
    CKRasterizerContext *CreateContext() override {
        auto *c = new Context(this);
        AddContext(c);
        return c;
    }
    ~Driver() override {
        DestroyContexts();
    }
};
class Rasterizer final : public CKRasterizer {
  public:
    CKBOOL Start(WIN_HANDLE window) override {
        if (!GetDriverCount()) {
            CKRasterizer::Start(window);
            AddDriver(new Driver(this));
        }
        return TRUE;
    }
};
CKRasterizer *start(WIN_HANDLE w) {
    auto *r = new Rasterizer;
    if (!r->Start(w)) {
        delete r;
        return nullptr;
    }
    return r;
}
void close(CKRasterizer *r) {
    delete r;
}
} // namespace
void CKWebGpuRasterizerGetInfo(CKRasterizerInfo *info) {
    info->DllName = "CKWebGpuRasterizer";
    info->Desc = "WebGPU";
    info->StartFct = start;
    info->CloseFct = close;
    info->InterfaceRevision = CKRST_INTERFACE_REVISION;
}
