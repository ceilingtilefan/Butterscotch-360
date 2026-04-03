#pragma once

#include "renderer.h"

// Maximum quads per batch before flushing
#define D3D9_MAX_QUADS 2048
#define D3D9_VERTS_PER_QUAD 4
#define D3D9_INDICES_PER_QUAD 6

// Vertex: position(4f), texcoord(2f), color(4f)
#define D3D9_VERTEX_STRIDE 40

typedef struct {
    Renderer base; // Must be first field

    void* pd3dDevice; // IDirect3DDevice9* (opaque in C header)

    // Shader programs (compiled at init from HLSL source)
    void* pVertexShader;
    void* pPixelShader;
    void* pVertexDecl;

    // Sprite batch state
    int32_t quadCount;
    int32_t currentTextureIndex;
    uint8_t* vertexData; // CPU-side staging (D3D9_MAX_QUADS * D3D9_VERTS_PER_QUAD * D3D9_VERTEX_STRIDE)

    // Textures loaded from TXTR pages (decoded PNG -> D3D textures)
    void** textures;     // IDirect3DTexture9*[]
    int32_t* textureWidths;
    int32_t* textureHeights;
    uint32_t textureCount;

    // 1x1 white texture for primitives
    void* whiteTexture;

    // View transform state
    float portScaleX, portScaleY; // portW/viewW, portH/viewH
    float offsetX, offsetY;       // viewX, viewY
    float portOffsetX, portOffsetY; // portX, portY (game coords)

    // Frame dimensions
    int32_t gameW, gameH;
    int32_t screenW, screenH;

    // Letterbox: uniform-scaled render area within screen
    float renderScale;   // uniform scale factor
    float renderOffsetX; // pixel offset for centering
    float renderOffsetY;

    // Dynamic sprite tracking
    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;
} D3D9Renderer;

Renderer* D3D9Renderer_create(void* pd3dDevice);
