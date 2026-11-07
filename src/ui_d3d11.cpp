// Direct3D 11 drawing for the multiplayer menu (ui.cpp; see ui_d3d11.h).
//
// public_beta-26494568's Direct3D 11 renderer, i_render11 (basic_render: constructor +0x968d0, vtable +0xeca00),
// gives the rest of the engine a Direct3D 9 style device made on top of its own device layer, which sits inside
// the object at +0x110 (made by +0x94a30, filled by +0x81450, called from the set-up +0x97160):
//   +0x148  ID3D11Device         D3D11CreateDevice (d3d11.dll loaded at run time), feature level 11_0, BGRA support
//   +0x150  ID3D11DeviceContext  the immediate context, from the same call
//   +0x158  the swap chain       +0x818b0: IDXGIFactory2::CreateSwapChainForHwnd (flip discard), else
//                                IDXGIFactory::CreateSwapChain; B8G8R8A8_UNORM, one sample
// The frame (+0x98450, from i_render's tick): the engine's pending Present, its default render states, then the
// "render" message to the pipeline - r_main, where this module's object sits just below the cursor - then the
// back buffer copied aside (+0x98620) and Present (+0x82ef0; with late presenting at the start of the next
// frame instead). The back buffer is what reaches the screen, so the menu is drawn straight onto it, whatever
// the engine has bound at that moment.
// The device layer binds its state lazily and remembers what it has bound (render targets and the viewport are
// set again only when they differ, +0x84720): everything changed here is put back exactly as it was.
// No reference to the back buffer is kept from one frame to the next: the engine resizes the swap chain when
// the window's size changes (ResizeBuffers, +0x82d60), and that fails while anything else holds one.
// Shaders are compiled at run time with d3dcompiler_47.dll (in Windows 10 and 11; the engine uses it too, for
// its own blits), d3d11.dll is only used through the engine's objects: the module imports nothing new.
#include "ui_d3d11.h"
#include "build.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

// i_render11's fields (beta26; the layer's +0x48 / +0x38). Only the swap chain is needed: it leads to the device
// (GetDevice) and that to the context; the device field is read for the log only.
static size_t r11_swap_chain(void) { return BV(NP, NP, 0x158); }
static size_t r11_device(void)     { return BV(NP, NP, 0x148); }

// ---------------------------------------------------------------- this module's objects (of one device)
static ID3D11Device *g_dev;                 // one reference held while the objects below exist
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11InputLayout *g_layout;
static ID3D11BlendState *g_blend;
static ID3D11RasterizerState *g_raster;
static ID3D11DepthStencilState *g_depth;
static ID3D11SamplerState *g_sampler;
static ID3D11ShaderResourceView *g_white;   // 1 x 1 white: the plain rectangles
static ID3D11Buffer *g_vb;
static UINT g_vb_cap;                       // vertices it holds
static bool g_broken;                       // cannot work in this game: no more tries
static int g_missing;                       // frames so far without a swap chain
static void *g_sc_seen;                     // the swap chain once checked (the engine's, while its field holds it)
static size_t g_sc_off;                     // ... and its field

// ---------------------------------------------------------------- the frame
static ID3D11DeviceContext *g_ctx;
static ID3D11Texture2D *g_bb;
static ID3D11RenderTargetView *g_rtv;
static D3D11_TEXTURE2D_DESC g_bbd;

struct Vtx11 { float x, y, u, v; UINT rgba; };                     // clip space, texture, R8G8B8A8 colour
struct Batch { ID3D11ShaderResourceView *srv; UINT first, count; };   // srv: one reference while queued
static std::vector<Vtx11> g_verts;
static std::vector<Batch> g_batches;

template <class T> static void drop(T *&p)
{
    if (p) { p->Release(); p = 0; }
}

static void drop_queue(void)
{
    for (Batch &b : g_batches) b.srv->Release();
    g_batches.clear();
    g_verts.clear();
}

static void release_objects(void)
{
    drop(g_vs); drop(g_ps); drop(g_layout); drop(g_blend); drop(g_raster); drop(g_depth); drop(g_sampler);
    drop(g_white); drop(g_vb);
    g_vb_cap = 0;
    drop(g_dev);
}

// ---------------------------------------------------------------- finding the engine's swap chain
// A pointer that can be asked QueryInterface: readable, with a vtable whose first three entries are code.
static bool com_object(const void *p)
{
    if (!readable(p, 8)) return false;
    void *const *vt = *(void *const *const *)p;
    if (!readable(vt, 3 * sizeof(void *))) return false;
    for (int i = 0; i < 3; i++) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vt[i], &mi, sizeof mi) || mi.State != MEM_COMMIT ||
            !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
    }
    return true;
}

static IDXGISwapChain *swap_chain_at(void *p, bool checked)
{
    IDXGISwapChain *sc = 0;
    if (!checked && !com_object(p)) return 0;
    if (FAILED(((IUnknown *)p)->QueryInterface(__uuidof(IDXGISwapChain), (void **)&sc))) sc = 0;
    return sc;
}

// The swap chain (one reference), 0 while there is none. Its field first (i_render11 +0x158); another layout
// is searched once for a pointer into dxgi.dll that is a swap chain.
static IDXGISwapChain *find_swap_chain(Object *render)
{
    if (g_sc_off && readable((char *)render + g_sc_off, 8)) {
        void *p = *(void **)((char *)render + g_sc_off);
        if (!p) return 0;
        if (p == g_sc_seen) return swap_chain_at(p, true);
        if (IDXGISwapChain *sc = swap_chain_at(p, false)) { g_sc_seen = p; return sc; }
    }
    size_t off = r11_swap_chain();
    if (off && readable((char *)render + off, 8)) {
        void *p = *(void **)((char *)render + off);
        if (!p) return 0;                   // not made yet
        if (IDXGISwapChain *sc = swap_chain_at(p, false)) { g_sc_off = off; g_sc_seen = p; return sc; }
    }
    static bool searched;
    if (searched) return 0;
    searched = true;
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    if (!dxgi) return 0;
    char *lo = (char *)dxgi, *hi = lo + ((IMAGE_NT_HEADERS *)(lo + ((IMAGE_DOS_HEADER *)lo)->e_lfanew))->OptionalHeader.SizeOfImage;
    for (off = sizeof(Object); off < 0x4400 && readable((char *)render + off, 8); off += 8) {
        void *p = *(void **)((char *)render + off);
        if (!readable(p, 8)) continue;
        char *vt = *(char **)p;
        if (vt < lo || vt >= hi) continue;
        if (IDXGISwapChain *sc = swap_chain_at(p, false)) {
            mp_log("ui: Direct3D 11 - the swap chain is at i_render+0x%zx in this build", off);
            g_sc_off = off;
            g_sc_seen = p;
            return sc;
        }
    }
    return 0;
}

// ---------------------------------------------------------------- the device's objects
static const char k_hlsl[] =
    "struct VI { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };\n"
    "struct PI { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0; };\n"
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "PI vs(VI i) { PI o; o.pos = float4(i.pos, 0, 1); o.uv = i.uv; o.color = i.color; return o; }\n"
    // as the Direct3D 9 stage 0: colour from the vertex, alpha the texture's times the vertex's
    "float4 ps(PI i) : SV_Target { return float4(i.color.rgb, i.color.a * tex.Sample(smp, i.uv).a); }\n";

static ID3DBlob *compile(pD3DCompile fn, const char *entry, const char *target)
{
    ID3DBlob *code = 0, *errors = 0;
    HRESULT hr = fn(k_hlsl, sizeof k_hlsl - 1, "mp_ui", 0, 0, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        mp_log("ui: Direct3D 11 - the %s shader does not compile (%08lx): %.300s", entry, (unsigned long)hr,
               errors ? (const char *)errors->GetBufferPointer() : "?");
        drop(code);
    }
    drop(errors);
    return code;
}

static ID3D11ShaderResourceView *make_texture(int w, int h, const DWORD *argb)
{
    if (!g_dev || w <= 0 || h <= 0 || !argb) return 0;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;          // the bytes of a D3DFMT_A8R8G8B8 pixel: b, g, r, a
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { argb, (UINT)w * 4, 0 };
    ID3D11Texture2D *tex = 0;
    ID3D11ShaderResourceView *srv = 0;
    if (FAILED(g_dev->CreateTexture2D(&d, &init, &tex)) || !tex) return 0;
    if (FAILED(g_dev->CreateShaderResourceView(tex, 0, &srv))) srv = 0;
    tex->Release();
    return srv;
}

static bool create_objects(void)
{
    HMODULE lib = LoadLibraryExA("d3dcompiler_47.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) lib = LoadLibraryA("d3dcompiler_47.dll");
    pD3DCompile fn = lib ? (pD3DCompile)GetProcAddress(lib, "D3DCompile") : 0;
    if (!fn) {
        mp_log("ui: Direct3D 11 - d3dcompiler_47.dll (D3DCompile) is not found: the multiplayer menu cannot be drawn");
        if (lib) FreeLibrary(lib);
        return false;
    }
    ID3DBlob *vs = compile(fn, "vs", "vs_4_0"), *ps = compile(fn, "ps", "ps_4_0");
    FreeLibrary(lib);
    bool ok = vs && ps;
    if (ok) {
        static const D3D11_INPUT_ELEMENT_DESC el[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ok = SUCCEEDED(g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), 0, &g_vs)) &&
             SUCCEEDED(g_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), 0, &g_ps)) &&
             SUCCEEDED(g_dev->CreateInputLayout(el, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &g_layout));
    }
    drop(vs);
    drop(ps);
    if (ok) {
        // the Direct3D 9 path's states: source alpha blending (alpha alike), no culling, no depth, linear clamped sampling
        D3D11_BLEND_DESC bd = {};
        for (D3D11_RENDER_TARGET_BLEND_DESC &rt : bd.RenderTarget) {
            rt.BlendEnable = TRUE;
            rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
            rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        }
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dd.StencilReadMask = dd.StencilWriteMask = 0xff;
        dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        dd.BackFace = dd.FrontFace;
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        static const DWORD white = 0xffffffff;
        ok = SUCCEEDED(g_dev->CreateBlendState(&bd, &g_blend)) && SUCCEEDED(g_dev->CreateRasterizerState(&rd, &g_raster)) &&
             SUCCEEDED(g_dev->CreateDepthStencilState(&dd, &g_depth)) && SUCCEEDED(g_dev->CreateSamplerState(&sd, &g_sampler)) &&
             (g_white = make_texture(1, 1, &white)) != 0;
    }
    if (!ok) mp_log("ui: Direct3D 11 - the menu's shaders and states could not be made: the multiplayer menu cannot be drawn");
    return ok;
}

static bool vb_room(UINT n)
{
    if (g_vb && g_vb_cap >= n) return true;
    drop(g_vb);
    g_vb_cap = 0;
    UINT cap = 6 * 256;
    while (cap < n) cap *= 2;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = cap * (UINT)sizeof(Vtx11);
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&d, 0, &g_vb)) || !g_vb) { g_vb = 0; return false; }
    g_vb_cap = cap;
    return true;
}

// ---------------------------------------------------------------- frame
void ui11_end(void)
{
    drop_queue();
    drop(g_rtv);
    drop(g_bb);
    drop(g_ctx);
}

void ui11_idle(void)
{
    if (!g_ctx) release_objects();
}

const void *ui11_owner(void) { return g_dev; }

int ui11_begin(Object *render, float *w, float *h)
{
    static unsigned fails;
    ui11_end();                             // (a frame left open)
    if (g_broken) return -1;
    IDXGISwapChain *sc = find_swap_chain(render);
    if (!sc) {
        if (++g_missing < 600) return 0;    // (made while the program loads)
        mp_log("ui: Direct3D 11 - no swap chain in i_render (%s): the multiplayer menu cannot be drawn", build_label());
        g_broken = true;
        return -1;
    }
    g_missing = 0;
    ID3D11Device *dev = 0;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&dev)) || !dev) {
        sc->Release();
        mp_log("ui: Direct3D 11 - the swap chain has no Direct3D 11 device: the multiplayer menu cannot be drawn");
        g_broken = true;
        return -1;
    }
    if (dev != g_dev) {
        static void *logged;
        release_objects();
        g_dev = dev;                        // (the reference GetDevice gave)
        if (dev != logged) {
            logged = dev;
            size_t df = r11_device();
            void *engine_dev = df && readable((char *)render + df, 8) ? *(void **)((char *)render + df) : 0;
            mp_log("ui: Direct3D 11 device %p (the engine's i_render+0x%zx: %p), swap chain %p (i_render+0x%zx)", dev, df, engine_dev,
                   g_sc_seen, g_sc_off);
        }
        if (!create_objects()) {
            release_objects();
            sc->Release();
            g_broken = true;
            return -1;
        }
    } else {
        dev->Release();
    }
    ID3D11Texture2D *bb = 0;
    HRESULT hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    sc->Release();
    if (FAILED(hr) || !bb) {
        if (fails++ % 600 == 0) mp_log("ui: Direct3D 11 - no back buffer (%08lx)", (unsigned long)hr);
        return 0;
    }
    bb->GetDesc(&g_bbd);
    // a view as the texture's format (the engine's swap chain: B8G8R8A8_UNORM, drawn without sRGB, as with Direct3D 9)
    D3D11_RENDER_TARGET_VIEW_DESC vd = {};
    vd.Format = g_bbd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM :
                g_bbd.Format == DXGI_FORMAT_B8G8R8X8_TYPELESS ? DXGI_FORMAT_B8G8R8X8_UNORM :
                g_bbd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : g_bbd.Format;
    vd.ViewDimension = g_bbd.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = g_dev->CreateRenderTargetView(bb, vd.Format != g_bbd.Format ? &vd : 0, &g_rtv);
    if (FAILED(hr) || !g_rtv || !g_bbd.Width || !g_bbd.Height) {
        if (fails++ % 600 == 0) mp_log("ui: Direct3D 11 - no view of the back buffer (%08lx, format %u)", (unsigned long)hr, (unsigned)g_bbd.Format);
        drop(g_rtv);
        bb->Release();
        return 0;
    }
    g_bb = bb;
    g_dev->GetImmediateContext(&g_ctx);
    if (!g_ctx) { ui11_end(); return 0; }
    *w = (float)g_bbd.Width;
    *h = (float)g_bbd.Height;
    return 1;
}

static void push(ID3D11ShaderResourceView *srv, float x, float y, float w, float h, DWORD color)
{
    if (!g_ctx || !srv) return;
    float sx = 2.0f / g_bbd.Width, sy = 2.0f / g_bbd.Height;
    float l = x * sx - 1, r = (x + w) * sx - 1, t = 1 - y * sy, b = 1 - (y + h) * sy;
    UINT c = (color & 0xff00ff00) | ((color >> 16) & 0xff) | ((color & 0xff) << 16);     // 0xAARRGGBB -> bytes r, g, b, a
    // (pixel centres at +0.5 here: the quad needs none of Direct3D 9's half-pixel shift)
    const Vtx11 q[6] = { { l, t, 0, 0, c }, { r, t, 1, 0, c }, { l, b, 0, 1, c }, { l, b, 0, 1, c }, { r, t, 1, 0, c }, { r, b, 1, 1, c } };
    UINT first = (UINT)g_verts.size();
    g_verts.insert(g_verts.end(), q, q + 6);
    if (!g_batches.empty() && g_batches.back().srv == srv) {
        g_batches.back().count += 6;
    } else {
        srv->AddRef();                      // (a text image may be dropped from ui.cpp's cache before the flush)
        g_batches.push_back({ srv, first, 6 });
    }
}

void ui11_rect(float x, float y, float w, float h, DWORD color) { push(g_white, x, y, w, h, color); }

void ui11_image(IUnknown *tex, float x, float y, float w, float h, DWORD color)
{
    push(static_cast<ID3D11ShaderResourceView *>(tex), x, y, w, h, color);
}

IUnknown *ui11_texture(int w, int h, const DWORD *argb) { return make_texture(w, h, argb); }

// What the drawing changes in the context, as it was (every Get gives a reference).
struct Saved {
    ID3D11RenderTargetView *rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *dsv;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nvp;
    ID3D11RasterizerState *rs;
    ID3D11BlendState *bs;
    FLOAT factor[4];
    UINT mask;
    ID3D11DepthStencilState *dss;
    UINT ref;
    ID3D11InputLayout *layout;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer *vb;
    UINT stride, offset;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11GeometryShader *gs;
    ID3D11HullShader *hs;
    ID3D11DomainShader *ds;
    ID3D11ClassInstance *vsi[256], *psi[256], *gsi[256], *hsi[256], *dsi[256];
    UINT nvsi, npsi, ngsi, nhsi, ndsi;
    // the shader inputs the engine's layer binds (pixel 0..15, vertex 0..3; +0x84720): binding the back buffer as
    // the target takes it out of any of them, and they come back after the targets
    ID3D11ShaderResourceView *ps_srv[16], *vs_srv[4];
    ID3D11SamplerState *smp;
};

static void save_state(ID3D11DeviceContext *c, Saved *s)
{
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s->rtv, &s->dsv);
    s->nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetViewports(&s->nvp, s->vp);
    c->RSGetState(&s->rs);
    c->OMGetBlendState(&s->bs, s->factor, &s->mask);
    c->OMGetDepthStencilState(&s->dss, &s->ref);
    c->IAGetInputLayout(&s->layout);
    c->IAGetPrimitiveTopology(&s->topo);
    c->IAGetVertexBuffers(0, 1, &s->vb, &s->stride, &s->offset);
    s->nvsi = s->npsi = s->ngsi = s->nhsi = s->ndsi = 256;
    c->VSGetShader(&s->vs, s->vsi, &s->nvsi);
    c->PSGetShader(&s->ps, s->psi, &s->npsi);
    c->GSGetShader(&s->gs, s->gsi, &s->ngsi);
    c->HSGetShader(&s->hs, s->hsi, &s->nhsi);
    c->DSGetShader(&s->ds, s->dsi, &s->ndsi);
    c->PSGetShaderResources(0, 16, s->ps_srv);
    c->VSGetShaderResources(0, 4, s->vs_srv);
    c->PSGetSamplers(0, 1, &s->smp);
}

static void restore_state(ID3D11DeviceContext *c, Saved *s)
{
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s->rtv, s->dsv);      // first: frees the back buffer
    c->OMSetBlendState(s->bs, s->factor, s->mask);
    c->OMSetDepthStencilState(s->dss, s->ref);
    c->RSSetState(s->rs);
    c->RSSetViewports(s->nvp, s->vp);
    c->IASetInputLayout(s->layout);
    c->IASetPrimitiveTopology(s->topo);
    c->IASetVertexBuffers(0, 1, &s->vb, &s->stride, &s->offset);
    c->VSSetShader(s->vs, s->vsi, s->nvsi);
    c->PSSetShader(s->ps, s->psi, s->npsi);
    c->GSSetShader(s->gs, s->gsi, s->ngsi);
    c->HSSetShader(s->hs, s->hsi, s->nhsi);
    c->DSSetShader(s->ds, s->dsi, s->ndsi);
    c->PSSetShaderResources(0, 16, s->ps_srv);
    c->VSSetShaderResources(0, 4, s->vs_srv);
    c->PSSetSamplers(0, 1, &s->smp);
    for (auto *&p : s->rtv) drop(p);
    drop(s->dsv); drop(s->rs); drop(s->bs); drop(s->dss); drop(s->layout); drop(s->vb);
    drop(s->vs); drop(s->ps); drop(s->gs); drop(s->hs); drop(s->ds);
    for (UINT i = 0; i < s->nvsi && i < 256; i++) drop(s->vsi[i]);
    for (UINT i = 0; i < s->npsi && i < 256; i++) drop(s->psi[i]);
    for (UINT i = 0; i < s->ngsi && i < 256; i++) drop(s->gsi[i]);
    for (UINT i = 0; i < s->nhsi && i < 256; i++) drop(s->hsi[i]);
    for (UINT i = 0; i < s->ndsi && i < 256; i++) drop(s->dsi[i]);
    for (auto *&p : s->ps_srv) drop(p);
    for (auto *&p : s->vs_srv) drop(p);
    drop(s->smp);
}

void ui11_flush(void)
{
    if (!g_ctx || !g_rtv || g_verts.empty() || !vb_room((UINT)g_verts.size())) { drop_queue(); return; }
    ID3D11DeviceContext *c = g_ctx;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(c->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { drop_queue(); return; }
    memcpy(m.pData, g_verts.data(), g_verts.size() * sizeof(Vtx11));
    c->Unmap(g_vb, 0);

    static Saved s;                         // (large: the class instance lists; drawing is on the render thread only)
    memset(&s, 0, sizeof s);
    save_state(c, &s);
    UINT stride = sizeof(Vtx11), offset = 0;
    D3D11_VIEWPORT vp = { 0, 0, (FLOAT)g_bbd.Width, (FLOAT)g_bbd.Height, 0, 1 };
    c->OMSetRenderTargets(1, &g_rtv, 0);
    c->OMSetBlendState(g_blend, 0, 0xffffffff);
    c->OMSetDepthStencilState(g_depth, 0);
    c->RSSetState(g_raster);
    c->RSSetViewports(1, &vp);
    c->IASetInputLayout(g_layout);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    c->VSSetShader(g_vs, 0, 0);
    c->PSSetShader(g_ps, 0, 0);
    c->GSSetShader(0, 0, 0);
    c->HSSetShader(0, 0, 0);
    c->DSSetShader(0, 0, 0);
    c->PSSetSamplers(0, 1, &g_sampler);
    for (const Batch &b : g_batches) {
        c->PSSetShaderResources(0, 1, &b.srv);
        c->Draw(b.count, b.first);
    }
    restore_state(c, &s);
    drop_queue();
}

bool ui11_save_frame(const char *path, unsigned *w, unsigned *h, unsigned *format)
{
    *w = g_bbd.Width;
    *h = g_bbd.Height;
    *format = (unsigned)g_bbd.Format;
    if (!g_ctx || !g_bb || g_bbd.SampleDesc.Count != 1) return false;
    bool rgba;                              // bytes r, g, b (else b, g, r as the BMP has them)
    switch (g_bbd.Format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        rgba = false;
        break;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        rgba = true;
        break;
    default:
        return false;
    }
    D3D11_TEXTURE2D_DESC d = g_bbd;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D *copy = 0;
    if (FAILED(g_dev->CreateTexture2D(&d, 0, &copy)) || !copy) return false;
    g_ctx->CopyResource(copy, g_bb);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = SUCCEEDED(g_ctx->Map(copy, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        // the Direct3D 9 path's file: every second pixel of every second row, 24 bits, top-down
        int bw = (int)d.Width / 2, bh = (int)d.Height / 2, stride = (bw * 3 + 3) & ~3;
        FILE *f = 0;
        if (!fopen_s(&f, path, "wb") && f) {
            BITMAPFILEHEADER fh = { 0x4d42, (DWORD)(54 + stride * bh), 0, 0, 54 };
            BITMAPINFOHEADER ih = { 40, bw, -bh, 1, 24, 0, (DWORD)(stride * bh) };
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&ih, sizeof ih, 1, f);
            std::string row(stride, 0);
            for (int y = 0; y < bh; y++) {
                const unsigned char *src = (const unsigned char *)m.pData + (size_t)(y * 2) * m.RowPitch;
                for (int x = 0; x < bw; x++) {
                    const unsigned char *p = src + x * 8;
                    row[x * 3] = (char)p[rgba ? 2 : 0];
                    row[x * 3 + 1] = (char)p[1];
                    row[x * 3 + 2] = (char)p[rgba ? 0 : 2];
                }
                fwrite(row.data(), 1, stride, f);
            }
            fclose(f);
        }
        g_ctx->Unmap(copy, 0);
    }
    copy->Release();
    return ok;
}
