#include <xtl.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <xgraphics.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#include "utils.h"
#include "text_utils.h"
#include "d3d9_renderer.h"

extern "C" unsigned long __cdecl DbgPrint(const char* format, ...);

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ===[ Vertex Format ]===
// Uses FLOAT4 position (pre-transformed screen coords, z=0, w=1)
// and FLOAT4 color to avoid D3DCOLOR endianness issues on Xbox 360.
struct SpriteVertex {
    float x, y, z, w; // position (screen-space, z=0, w=1)
    float u, v;        // texcoord
    float r, g, b, a;  // color as floats
};

// ===[ HLSL Shader Source ]===
// Vertex shader: simple pass-through for pre-transformed screen-space vertices.
// Position is already in screen pixels with z=0, w=1.
// With D3DRS_VIEWPORTENABLE=FALSE, the GPU uses these directly.
static const char* g_vsSource =
    "struct VS_IN  { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
    "struct VS_OUT { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
    "VS_OUT main(VS_IN i) {\n"
    "  VS_OUT o;\n"
    "  o.Pos = i.Pos;\n"
    "  o.Tex = i.Tex;\n"
    "  o.Col = i.Col;\n"
    "  return o;\n"
    "}\n";

static const char* g_psSource =
    "sampler2D s0 : register(s0);\n"
    "struct PS_IN { float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
    "float4 main(PS_IN i) : COLOR0 {\n"
    "  return tex2D(s0, i.Tex) * i.Col;\n"
    "}\n";

// ===[ Helpers ]===

static inline void setVertex(SpriteVertex* sv, float px, float py, float tu, float tv,
                              float cr, float cg, float cb, float ca) {
    sv->x = px; sv->y = py; sv->z = 0.0f; sv->w = 1.0f;
    sv->u = tu; sv->v = tv;
    sv->r = cr; sv->g = cg; sv->b = cb; sv->a = ca;
}

static inline IDirect3DDevice9* Dev(D3D9Renderer* r) {
    return (IDirect3DDevice9*)r->pd3dDevice;
}

// Convert Butterscotch BGR color + alpha to float RGBA
static inline void bgrToFloatColor(uint32_t bgr, float alpha, float* outR, float* outG, float* outB, float* outA) {
    *outR = (float)(bgr & 0xFF) / 255.0f;
    *outG = (float)((bgr >> 8) & 0xFF) / 255.0f;
    *outB = (float)((bgr >> 16) & 0xFF) / 255.0f;
    *outA = alpha;
}

// ===[ Batch Flush ]===

static void flushBatch(D3D9Renderer* dr) {
    if (dr->quadCount == 0) return;

    IDirect3DDevice9* dev = Dev(dr);

    // Bind texture
    if (dr->currentTextureIndex >= 0 && (uint32_t)dr->currentTextureIndex < dr->textureCount) {
        dev->SetTexture(0, (IDirect3DBaseTexture9*)dr->textures[dr->currentTextureIndex]);
    } else {
        dev->SetTexture(0, (IDirect3DBaseTexture9*)dr->whiteTexture);
    }

    // Draw using DrawPrimitiveUP — simpler than managing a vertex buffer for 2D
    int32_t vertCount = dr->quadCount * D3D9_VERTS_PER_QUAD;
    // We use QUADLIST (Xbox 360 extension) — 4 verts per quad, no index buffer needed
    dev->DrawPrimitiveUP(D3DPT_QUADLIST, dr->quadCount,
                         dr->vertexData, sizeof(SpriteVertex));

    dr->quadCount = 0;
}

static void ensureTexture(D3D9Renderer* dr, int32_t textureIndex) {
    if (dr->currentTextureIndex != textureIndex) {
        flushBatch(dr);
        dr->currentTextureIndex = textureIndex;
    }
}

static SpriteVertex* allocQuad(D3D9Renderer* dr) {
    if (dr->quadCount >= D3D9_MAX_QUADS) {
        flushBatch(dr);
    }
    SpriteVertex* v = (SpriteVertex*)(dr->vertexData + dr->quadCount * D3D9_VERTS_PER_QUAD * sizeof(SpriteVertex));
    dr->quadCount++;
    return v;
}

// Transform a game-space point to screen-space pixels (with pillarbox centering)
static inline void transformPoint(D3D9Renderer* dr, float inX, float inY, float* outX, float* outY) {
    // Game-space: view → port mapping
    float gx = dr->portOffsetX + (inX - dr->offsetX) * dr->portScaleX;
    float gy = dr->portOffsetY + (inY - dr->offsetY) * dr->portScaleY;
    // Screen-space: uniform scale + center offset
    *outX = gx * dr->renderScale + dr->renderOffsetX;
    *outY = gy * dr->renderScale + dr->renderOffsetY;
}

// ===[ Vtable Implementations ]===

static void d3d9Init(Renderer* renderer, DataWin* dataWin) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    renderer->dataWin = dataWin;

    // Allocate CPU vertex staging buffer
    dr->vertexData = (uint8_t*)malloc(D3D9_MAX_QUADS * D3D9_VERTS_PER_QUAD * sizeof(SpriteVertex));

    // Compile shaders from source
    ID3DXBuffer* pCode = NULL;
    ID3DXBuffer* pErr = NULL;

    HRESULT hr = D3DXCompileShader(g_vsSource, (UINT)strlen(g_vsSource),
                                   NULL, NULL, "main", "vs_2_0", 0, &pCode, &pErr, NULL);
    if (FAILED(hr)) {
        OutputDebugStringA("VS compile failed: ");
        if (pErr) OutputDebugStringA((const char*)pErr->GetBufferPointer());
        if (pErr) pErr->Release();
        return;
    }
    dev->CreateVertexShader((const DWORD*)pCode->GetBufferPointer(),
                            (IDirect3DVertexShader9**)&dr->pVertexShader);
    pCode->Release();

    hr = D3DXCompileShader(g_psSource, (UINT)strlen(g_psSource),
                           NULL, NULL, "main", "ps_2_0", 0, &pCode, &pErr, NULL);
    if (FAILED(hr)) {
        OutputDebugStringA("PS compile failed: ");
        if (pErr) OutputDebugStringA((const char*)pErr->GetBufferPointer());
        if (pErr) pErr->Release();
        return;
    }
    dev->CreatePixelShader((const DWORD*)pCode->GetBufferPointer(),
                           (IDirect3DPixelShader9**)&dr->pPixelShader);
    pCode->Release();

    // Create vertex declaration
    static const D3DVERTEXELEMENT9 decl[] = {
        { 0,  0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        D3DDECL_END()
    };
    dev->CreateVertexDeclaration(decl, (IDirect3DVertexDeclaration9**)&dr->pVertexDecl);

    // Create 1x1 white texture for primitives
    IDirect3DTexture9* whiteTex = NULL;
    dev->CreateTexture(1, 1, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &whiteTex, NULL);
    if (whiteTex) {
        D3DLOCKED_RECT lr;
        whiteTex->LockRect(0, &lr, NULL, 0);
        *(DWORD*)lr.pBits = 0xFFFFFFFF;
        whiteTex->UnlockRect(0);
    }
    dr->whiteTexture = whiteTex;

    // Load TXTR pages: decode PNG to RGBA, upload as D3D textures
    dr->textureCount = dataWin->txtr.count;
    dr->textures = (void**)calloc(dr->textureCount, sizeof(void*));
    dr->textureWidths = (int32_t*)calloc(dr->textureCount, sizeof(int32_t));
    dr->textureHeights = (int32_t*)calloc(dr->textureCount, sizeof(int32_t));

    for (uint32_t i = 0; i < dr->textureCount; i++) {
        Texture* txtr = &dataWin->txtr.textures[i];
        int w, h, channels;
        uint8_t* pixels = stbi_load_from_memory(txtr->blobData, (int)txtr->blobSize, &w, &h, &channels, 4);
        if (!pixels) {
            OutputDebugStringA("Failed to decode TXTR page\n");
            continue;
        }

        dr->textureWidths[i] = w;
        dr->textureHeights[i] = h;

        // On Xbox 360, GPU textures use a tiled memory layout.
        // Create a LINEAR texture first, fill it, then create a tiled copy.
        IDirect3DTexture9* linearTex = NULL;
        IDirect3DTexture9* tex = NULL;

        // Create linear staging texture
        dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &linearTex, NULL);
        if (linearTex) {
            D3DLOCKED_RECT lr;
            linearTex->LockRect(0, &lr, NULL, 0);
            for (int y2 = 0; y2 < h; y2++) {
                uint8_t* src = pixels + y2 * w * 4;
                DWORD* dst = (DWORD*)((uint8_t*)lr.pBits + y2 * lr.Pitch);
                for (int x2 = 0; x2 < w; x2++) {
                    uint8_t r = src[x2 * 4 + 0];
                    uint8_t g = src[x2 * 4 + 1];
                    uint8_t b = src[x2 * 4 + 2];
                    uint8_t a = src[x2 * 4 + 3];
                    // Clear RGB on fully transparent pixels to prevent color bleeding
                    // from texture filtering sampling adjacent texels
                    if (a == 0) { r = 0; g = 0; b = 0; }
                    dst[x2] = D3DCOLOR_ARGB(a, r, g, b);
                }
            }
            linearTex->UnlockRect(0);
        }

        // Just use the linear texture directly — Xbox 360 can sample from linear textures
        tex = linearTex;
        dr->textures[i] = tex;
        stbi_image_free(pixels);
    }

    dr->originalTexturePageCount = dataWin->txtr.count;
    dr->originalTpagCount = dataWin->tpag.count;
    dr->originalSpriteCount = dataWin->sprt.count;

    dr->currentTextureIndex = -1;
    dr->quadCount = 0;
}

static void d3d9Destroy(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    for (uint32_t i = 0; i < dr->textureCount; i++) {
        if (dr->textures[i]) ((IDirect3DTexture9*)dr->textures[i])->Release();
    }
    free(dr->textures);
    free(dr->textureWidths);
    free(dr->textureHeights);
    free(dr->vertexData);
    if (dr->whiteTexture) ((IDirect3DTexture9*)dr->whiteTexture)->Release();
    if (dr->pVertexShader) ((IDirect3DVertexShader9*)dr->pVertexShader)->Release();
    if (dr->pPixelShader) ((IDirect3DPixelShader9*)dr->pPixelShader)->Release();
    if (dr->pVertexDecl) ((IDirect3DVertexDeclaration9*)dr->pVertexDecl)->Release();
    free(dr);
}

static void d3d9BeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);

    dr->gameW = gameW;
    dr->gameH = gameH;
    dr->screenW = windowW;
    dr->screenH = windowH;

    // Compute uniform scale to fit game in screen with letterboxing
    float scaleX = (float)windowW / (float)gameW;
    float scaleY = (float)windowH / (float)gameH;
    dr->renderScale = (scaleX < scaleY) ? scaleX : scaleY;
    dr->renderOffsetX = ((float)windowW - (float)gameW * dr->renderScale) * 0.5f;
    dr->renderOffsetY = ((float)windowH - (float)gameH * dr->renderScale) * 0.5f;

    // Debug: print once
    static bool printedOnce = false;
    if (!printedOnce) {
        DbgPrint("BS: renderScale=%d/1000 offsetX=%d offsetY=%d gameW=%d gameH=%d screenW=%d screenH=%d\n",
            (int)(dr->renderScale * 1000), (int)dr->renderOffsetX, (int)dr->renderOffsetY,
            gameW, gameH, windowW, windowH);
        printedOnce = true;
    }

    dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    dev->BeginScene();

    // Set shared render state
    dev->SetVertexShader((IDirect3DVertexShader9*)dr->pVertexShader);
    dev->SetPixelShader((IDirect3DPixelShader9*)dr->pPixelShader);
    dev->SetVertexDeclaration((IDirect3DVertexDeclaration9*)dr->pVertexDecl);


    // Alpha blending
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    // No depth testing for 2D
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    // Disable viewport transform — we use pre-transformed screen-space vertices
    dev->SetRenderState(D3DRS_VIEWPORTENABLE, FALSE);

    // Point filtering — pixel-perfect for 2D sprite games like Undertale
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}

static void d3d9EndFrame(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    Dev(dr)->EndScene();
    Dev(dr)->Present(NULL, NULL, NULL, NULL);
}

static void d3d9BeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
                           int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    (void)viewAngle; // TODO: support rotated views

    D3DVIEWPORT9 vp;
    vp.X = 0;
    vp.Y = 0;
    vp.Width = (DWORD)dr->screenW;
    vp.Height = (DWORD)dr->screenH;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);

    // Store view transform for point mapping
    dr->offsetX = (float)viewX;
    dr->offsetY = (float)viewY;
    dr->portScaleX = (float)portW / (float)viewW;
    dr->portScaleY = (float)portH / (float)viewH;
    dr->portOffsetX = (float)portX;
    dr->portOffsetY = (float)portY;

    // No projection matrix needed — vertices are pre-transformed screen coords.
    // D3DRS_VIEWPORTENABLE=FALSE means the GPU uses positions directly as pixels.
}

static void d3d9EndView(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
}

// ===[ Sprite Drawing ]===

static void d3d9DrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y,
                            float originX, float originY, float xscale, float yscale,
                            float angleDeg, uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t texPageId = tpag->texturePageId;
    if (0 > texPageId || (uint32_t)texPageId >= dr->textureCount) return;
    if (!dr->textures[texPageId]) return;

    ensureTexture(dr, texPageId);

    float texW = (float)dr->textureWidths[texPageId];
    float texH = (float)dr->textureHeights[texPageId];
    if (texW <= 0 || texH <= 0) return;

    // UV coordinates on the texture atlas
    float u0 = (float)tpag->sourceX / texW;
    float v0 = (float)tpag->sourceY / texH;
    float u1 = (float)(tpag->sourceX + tpag->sourceWidth) / texW;
    float v1 = (float)(tpag->sourceY + tpag->sourceHeight) / texH;

    // Quad corners in local space (before transform)
    float localX0 = (float)tpag->targetX - originX;
    float localY0 = (float)tpag->targetY - originY;
    float localX1 = localX0 + (float)tpag->sourceWidth;
    float localY1 = localY0 + (float)tpag->sourceHeight;

    // Scale
    localX0 *= xscale; localY0 *= yscale;
    localX1 *= xscale; localY1 *= yscale;

    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

    // Build 4 corners
    float cx[4], cy[4];
    if (angleDeg != 0.0f) {
        float rad = angleDeg * (3.14159265f / 180.0f);
        float cosA = cosf(rad);
        float sinA = sinf(rad);

        float lx[4] = { localX0, localX1, localX1, localX0 };
        float ly[4] = { localY0, localY0, localY1, localY1 };
        for (int i = 0; i < 4; i++) {
            cx[i] = lx[i] * cosA - ly[i] * sinA;
            cy[i] = lx[i] * sinA + ly[i] * cosA;
        }
    } else {
        cx[0] = localX0; cy[0] = localY0;
        cx[1] = localX1; cy[1] = localY0;
        cx[2] = localX1; cy[2] = localY1;
        cx[3] = localX0; cy[3] = localY1;
    }

    // Transform to screen space
    SpriteVertex* v = allocQuad(dr);
    float sx, sy;
    for (int i = 0; i < 4; i++) {
        transformPoint(dr, x + cx[i], y + cy[i], &sx, &sy);
        v[i].x = sx;
        v[i].y = sy;
        v[i].z = 0.0f;
        v[i].w = 1.0f;
        v[i].r = cr; v[i].g = cg; v[i].b = cb; v[i].a = ca;
    }
    v[0].u = u0; v[0].v = v0;
    v[1].u = u1; v[1].v = v0;
    v[2].u = u1; v[2].v = v1;
    v[3].u = u0; v[3].v = v1;
}

static void d3d9DrawSpritePart(Renderer* renderer, int32_t tpagIndex,
                                int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH,
                                float x, float y, float xscale, float yscale,
                                uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t texPageId = tpag->texturePageId;
    if (0 > texPageId || (uint32_t)texPageId >= dr->textureCount) return;
    if (!dr->textures[texPageId]) return;

    ensureTexture(dr, texPageId);

    float texW = (float)dr->textureWidths[texPageId];
    float texH = (float)dr->textureHeights[texPageId];
    if (texW <= 0 || texH <= 0) return;

    float u0 = (float)(tpag->sourceX + srcOffX) / texW;
    float v0 = (float)(tpag->sourceY + srcOffY) / texH;
    float u1 = (float)(tpag->sourceX + srcOffX + srcW) / texW;
    float v1 = (float)(tpag->sourceY + srcOffY + srcH) / texH;

    float drawW = (float)srcW * xscale;
    float drawH = (float)srcH * yscale;

    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

    SpriteVertex* v = allocQuad(dr);
    float x0, y0, x1, y1;
    transformPoint(dr, x, y, &x0, &y0);
    transformPoint(dr, x + drawW, y + drawH, &x1, &y1);

    setVertex(&v[0], x0, y0, u0, v0, cr, cg, cb, ca);
    setVertex(&v[1], x1, y0, u1, v0, cr, cg, cb, ca);
    setVertex(&v[2], x1, y1, u1, v1, cr, cg, cb, ca);
    setVertex(&v[3], x0, y1, u0, v1, cr, cg, cb, ca);
}

static void d3d9DrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2,
                               uint32_t color, float alpha, bool outline) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    if (outline) {
        // Draw 4 lines as thin rectangles
        float lw = 1.0f;
        d3d9DrawRectangle(renderer, x1, y1, x2, y1 + lw, color, alpha, false); // top
        d3d9DrawRectangle(renderer, x1, y2 - lw, x2, y2, color, alpha, false); // bottom
        d3d9DrawRectangle(renderer, x1, y1, x1 + lw, y2, color, alpha, false); // left
        d3d9DrawRectangle(renderer, x2 - lw, y1, x2, y2, color, alpha, false); // right
        return;
    }

    ensureTexture(dr, -1); // white texture

    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);
    SpriteVertex* v = allocQuad(dr);

    float sx0, sy0, sx1, sy1;
    transformPoint(dr, x1, y1, &sx0, &sy0);
    transformPoint(dr, x2, y2, &sx1, &sy1);

    setVertex(&v[0], sx0, sy0, 0, 0, cr, cg, cb, ca);
    setVertex(&v[1], sx1, sy0, 1, 0, cr, cg, cb, ca);
    setVertex(&v[2], sx1, sy1, 1, 1, cr, cg, cb, ca);
    setVertex(&v[3], sx0, sy1, 0, 1, cr, cg, cb, ca);
}

static void d3d9DrawLine(Renderer* renderer, float x1, float y1, float x2, float y2,
                          float width, uint32_t color, float alpha) {
    // Draw line as a thin rotated rectangle
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;

    float nx = -dy / len * width * 0.5f;
    float ny = dx / len * width * 0.5f;

    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    ensureTexture(dr, -1);
    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

    SpriteVertex* v = allocQuad(dr);
    float sx, sy;

    transformPoint(dr, x1 + nx, y1 + ny, &sx, &sy); setVertex(&v[0], sx, sy, 0, 0, cr, cg, cb, ca);
    transformPoint(dr, x2 + nx, y2 + ny, &sx, &sy); setVertex(&v[1], sx, sy, 1, 0, cr, cg, cb, ca);
    transformPoint(dr, x2 - nx, y2 - ny, &sx, &sy); setVertex(&v[2], sx, sy, 1, 1, cr, cg, cb, ca);
    transformPoint(dr, x1 - nx, y1 - ny, &sx, &sy); setVertex(&v[3], sx, sy, 0, 1, cr, cg, cb, ca);
}

static void d3d9DrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                               float width, uint32_t color1, uint32_t color2, float alpha) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;

    float nx = -dy / len * width * 0.5f;
    float ny = dx / len * width * 0.5f;

    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    ensureTexture(dr, -1);
    float c1r, c1g, c1b, c1a;
    bgrToFloatColor(color1, alpha, &c1r, &c1g, &c1b, &c1a);
    float c2r, c2g, c2b, c2a;
    bgrToFloatColor(color2, alpha, &c2r, &c2g, &c2b, &c2a);

    SpriteVertex* v = allocQuad(dr);
    float sx, sy;

    transformPoint(dr, x1 + nx, y1 + ny, &sx, &sy); setVertex(&v[0], sx, sy, 0, 0, c1r, c1g, c1b, c1a);
    transformPoint(dr, x2 + nx, y2 + ny, &sx, &sy); setVertex(&v[1], sx, sy, 1, 0, c2r, c2g, c2b, c2a);
    transformPoint(dr, x2 - nx, y2 - ny, &sx, &sy); setVertex(&v[2], sx, sy, 1, 1, c2r, c2g, c2b, c2a);
    transformPoint(dr, x1 - nx, y1 - ny, &sx, &sy); setVertex(&v[3], sx, sy, 0, 1, c1r, c1g, c1b, c1a);
}

static void d3d9DrawText(Renderer* renderer, const char* text, float x, float y,
                          float xscale, float yscale, float angleDeg) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || (uint32_t)fontIndex >= dw->font.count) return;

    Font* font = &dw->font.fonts[fontIndex];
    uint32_t color = renderer->drawColor;
    float alpha = renderer->drawAlpha;

    // Resolve font texture page
    int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    if (0 > fontTpagIndex) return;

    TexturePageItem* fontTpag = &dw->tpag.items[fontTpagIndex];
    int16_t pageId = fontTpag->texturePageId;
    if (0 > pageId || dr->textureCount <= (uint32_t)pageId) return;
    if (!dr->textures[pageId]) return;

    float texW = (float)dr->textureWidths[pageId];
    float texH = (float)dr->textureHeights[pageId];
    if (texW <= 0 || texH <= 0) return;

    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

    // Preprocess: convert # to \n (and \# to literal #)
    char* processed = TextUtils_preprocessGmlText(text);
    int32_t textLen = (int32_t)strlen(processed);

    // Count lines
    int32_t lineCount = TextUtils_countLines(processed, textLen);

    // Vertical alignment offset
    float totalHeight = (float)lineCount * (float)font->emSize;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float fontScaleX = xscale * font->scaleX;
    float fontScaleY = yscale * font->scaleY;

    // Build rotation transform (if needed)
    float cosA = 1.0f, sinA = 0.0f;
    bool hasRotation = (angleDeg != 0.0f);
    if (hasRotation) {
        float rad = -angleDeg * (3.14159265f / 180.0f);
        cosA = cosf(rad);
        sinA = sinf(rad);
    }

    ensureTexture(dr, (int32_t)pageId);

    // Iterate through lines
    float cursorY = valignOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        // Horizontal alignment offset for this line
        float lineWidth = TextUtils_measureLineWidth(font, processed + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;

        // Render each glyph
        int32_t pos = 0;
        while (lineLen > pos) {
            uint16_t ch = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (!glyph) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            // Compute UVs from glyph position in the font's atlas
            float u0 = (float)(fontTpag->sourceX + glyph->sourceX) / texW;
            float v0 = (float)(fontTpag->sourceY + glyph->sourceY) / texH;
            float u1 = (float)(fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / texW;
            float v1 = (float)(fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / texH;

            // Local quad position
            float localX0 = cursorX + glyph->offset;
            float localY0 = cursorY;
            float localX1 = localX0 + (float)glyph->sourceWidth;
            float localY1 = localY0 + (float)glyph->sourceHeight;

            // Scale
            float sx0 = localX0 * fontScaleX;
            float sy0 = localY0 * fontScaleY;
            float sx1 = localX1 * fontScaleX;
            float sy1 = localY1 * fontScaleY;

            // Build 4 corners (with optional rotation)
            float cx[4], cy[4];
            if (hasRotation) {
                float lx[4] = { sx0, sx1, sx1, sx0 };
                float ly[4] = { sy0, sy0, sy1, sy1 };
                for (int i = 0; i < 4; i++) {
                    cx[i] = lx[i] * cosA - ly[i] * sinA;
                    cy[i] = lx[i] * sinA + ly[i] * cosA;
                }
            } else {
                cx[0] = sx0; cy[0] = sy0;
                cx[1] = sx1; cy[1] = sy0;
                cx[2] = sx1; cy[2] = sy1;
                cx[3] = sx0; cy[3] = sy1;
            }

            SpriteVertex* v = allocQuad(dr);
            float screenX, screenY;
            for (int i = 0; i < 4; i++) {
                transformPoint(dr, x + cx[i], y + cy[i], &screenX, &screenY);
                v[i].x = screenX;
                v[i].y = screenY;
                v[i].z = 0.0f;
                v[i].w = 1.0f;
                v[i].r = cr; v[i].g = cg; v[i].b = cb; v[i].a = ca;
            }
            v[0].u = u0; v[0].v = v0;
            v[1].u = u1; v[1].v = v0;
            v[2].u = u1; v[2].v = v1;
            v[3].u = u0; v[3].v = v1;

            // Advance cursor (shift + kerning)
            cursorX += glyph->shift;
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += (float)font->emSize;

        // Advance past the newline
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }

    free(processed);
}

static void d3d9Flush(Renderer* renderer) {
    flushBatch((D3D9Renderer*)renderer);
}

static int32_t d3d9CreateSpriteFromSurface(Renderer* renderer, int32_t x, int32_t y,
                                            int32_t w, int32_t h, bool removeback,
                                            bool smooth, int32_t xorig, int32_t yorig) {
    (void)renderer; (void)x; (void)y; (void)w; (void)h;
    (void)removeback; (void)smooth; (void)xorig; (void)yorig;
    // TODO: implement surface capture
    return -1;
}

static void d3d9DeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    (void)renderer; (void)spriteIndex;
    // TODO: implement dynamic sprite deletion
}

// ===[ Vtable ]===

static RendererVtable d3d9RendererVtable = {
    d3d9Init,
    d3d9Destroy,
    d3d9BeginFrame,
    d3d9EndFrame,
    d3d9BeginView,
    d3d9EndView,
    d3d9DrawSprite,
    d3d9DrawSpritePart,
    d3d9DrawRectangle,
    d3d9DrawLine,
    d3d9DrawLineColor,
    d3d9DrawText,
    d3d9Flush,
    d3d9CreateSpriteFromSurface,
    d3d9DeleteSprite,
    NULL, // drawTile — use default path
};

// ===[ Public API ]===

Renderer* D3D9Renderer_create(void* pd3dDevice) {
    D3D9Renderer* dr = (D3D9Renderer*)calloc(1, sizeof(D3D9Renderer));
    dr->base.vtable = &d3d9RendererVtable;
    dr->base.drawColor = 0xFFFFFF;
    dr->base.drawAlpha = 1.0f;
    dr->base.drawFont = -1;
    dr->pd3dDevice = pd3dDevice;
    dr->currentTextureIndex = -1;
    return (Renderer*)dr;
}
