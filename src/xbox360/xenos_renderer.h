#pragma once

#include "renderer.h"

#include <xenos/xe.h>
#include <stdbool.h>

// Maximum quads per batch before flushing
#define XE_MAX_QUADS 2048
#define XE_VERTICES_PER_QUAD 4
#define XE_INDICES_PER_QUAD 6
// x, y, u, v, r, g, b, a
#define XE_FLOATS_PER_VERTEX 8

// ===[ XenosRenderer Struct ]===
typedef struct {
    Renderer base; // Must be first field for struct embedding

    struct XenosDevice* xe;

    // Shader programs
    struct XenosShader* vertexShader;
    struct XenosShader* pixelShader;

    // Vertex/index buffers for sprite batching
    struct XenosVertexBuffer* vb;
    struct XenosIndexBuffer* ib;
    float* vertexData; // CPU-side staging buffer

    int32_t quadCount;
    int32_t currentTextureIndex; // which texture is currently bound (-1 = none)

    // Textures loaded from TXTR pages (RGBA, decoded from PNG)
    struct XenosSurface** textures; // one per TXTR page
    int32_t* textureWidths;
    int32_t* textureHeights;
    uint32_t textureCount;

    // 1x1 white texture for primitives (rectangles, lines)
    struct XenosSurface* whiteTexture;

    // View transform state
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;

    // Framebuffer dimensions
    int32_t gameW;
    int32_t gameH;
    int32_t screenW;
    int32_t screenH;

    // Original counts from data.win (dynamic slots start at these indices)
    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;
} XenosRenderer;

Renderer* XenosRenderer_create(struct XenosDevice* xe);
