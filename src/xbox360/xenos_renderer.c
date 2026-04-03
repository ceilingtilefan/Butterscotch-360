#include "xenos_renderer.h"
#include "matrix_math.h"
#include "text_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_image.h"
#include "stb_ds.h"
#include "utils.h"

// ===[ Batch Flush ]===

static void flushBatch(XenosRenderer* xr) {
    if (xr->quadCount == 0) return;

    int32_t vertexCount = xr->quadCount * XE_VERTICES_PER_QUAD;
    int32_t indexCount = xr->quadCount * XE_INDICES_PER_QUAD;

    // Lock vertex buffer and upload CPU-side data
    float* vbData = Xe_VB_Lock(xr->xe, xr->vb, 0, vertexCount * XE_FLOATS_PER_VERTEX * sizeof(float), XE_LOCK_WRITE);
    memcpy(vbData, xr->vertexData, vertexCount * XE_FLOATS_PER_VERTEX * sizeof(float));
    Xe_VB_Unlock(xr->xe, xr->vb);

    // Bind texture
    if (xr->currentTextureIndex >= 0 && (uint32_t) xr->currentTextureIndex < xr->textureCount) {
        Xe_SetTexture(xr->xe, 0, xr->textures[xr->currentTextureIndex]);
    } else {
        Xe_SetTexture(xr->xe, 0, xr->whiteTexture);
    }

    Xe_SetStreamSource(xr->xe, 0, xr->vb, 0, XE_FLOATS_PER_VERTEX * sizeof(float));
    Xe_SetIndices(xr->xe, xr->ib);
    Xe_DrawIndexedPrimitive(xr->xe, XE_PRIMTYPE_TRIANGLELIST, 0, 0, vertexCount, 0, indexCount / 3);

    xr->quadCount = 0;
}

// ===[ Vtable Implementations ]===

static void xeInit(Renderer* renderer, DataWin* dataWin) {
    XenosRenderer* xr = (XenosRenderer*) renderer;
    renderer->dataWin = dataWin;

    // Allocate CPU-side vertex buffer
    xr->vertexData = safeMalloc(XE_MAX_QUADS * XE_VERTICES_PER_QUAD * XE_FLOATS_PER_VERTEX * sizeof(float));

    // Create GPU vertex buffer
    xr->vb = Xe_CreateVertexBuffer(xr->xe, XE_MAX_QUADS * XE_VERTICES_PER_QUAD * XE_FLOATS_PER_VERTEX * sizeof(float));

    // Create GPU index buffer with quad pattern (0,1,2,2,3,0 repeated)
    xr->ib = Xe_CreateIndexBuffer(xr->xe, XE_MAX_QUADS * XE_INDICES_PER_QUAD * sizeof(uint16_t), XE_FMT_INDEX16);
    uint16_t* indices = Xe_IB_Lock(xr->xe, xr->ib, 0, XE_MAX_QUADS * XE_INDICES_PER_QUAD * sizeof(uint16_t), XE_LOCK_WRITE);
    for (int32_t i = 0; XE_MAX_QUADS > i; i++) {
        uint16_t base = (uint16_t) (i * 4);
        indices[i * 6 + 0] = base + 0;
        indices[i * 6 + 1] = base + 1;
        indices[i * 6 + 2] = base + 2;
        indices[i * 6 + 3] = base + 2;
        indices[i * 6 + 4] = base + 3;
        indices[i * 6 + 5] = base + 0;
    }
    Xe_IB_Unlock(xr->xe, xr->ib);

    // Load textures from TXTR pages (decode PNG to RGBA, upload to Xenos)
    xr->textureCount = dataWin->txtr.count;
    xr->textures = safeMalloc(xr->textureCount * sizeof(struct XenosSurface*));
    xr->textureWidths = safeMalloc(xr->textureCount * sizeof(int32_t));
    xr->textureHeights = safeMalloc(xr->textureCount * sizeof(int32_t));

    for (uint32_t i = 0; xr->textureCount > i; i++) {
        Texture* txtr = &dataWin->txtr.textures[i];
        uint8_t* pngData = txtr->blobData;
        uint32_t pngSize = txtr->blobSize;

        int w, h, channels;
        uint8_t* pixels = stbi_load_from_memory(pngData, (int) pngSize, &w, &h, &channels, 4);
        if (pixels == nullptr) {
            fprintf(stderr, "Xenos: Failed to decode TXTR page %u\n", i);
            xr->textures[i] = nullptr;
            xr->textureWidths[i] = 0;
            xr->textureHeights[i] = 0;
            continue;
        }

        xr->textureWidths[i] = w;
        xr->textureHeights[i] = h;

        // Create Xenos texture surface
        struct XenosSurface* surface = Xe_CreateTexture(xr->xe, w, h, 1, XE_FMT_8888 | XE_FMT_ARGB, 0);
        uint8_t* texData = Xe_Surface_LockRect(xr->xe, surface, 0, 0, 0, 0, XE_LOCK_WRITE);

        // Convert RGBA to ARGB (Xenos native format) and handle pitch
        uint32_t pitch = surface->wpitch;
        for (int y = 0; h > y; y++) {
            uint8_t* srcRow = pixels + y * w * 4;
            uint8_t* dstRow = texData + y * pitch;
            for (int x = 0; w > x; x++) {
                uint8_t r = srcRow[x * 4 + 0];
                uint8_t g = srcRow[x * 4 + 1];
                uint8_t b = srcRow[x * 4 + 2];
                uint8_t a = srcRow[x * 4 + 3];
                // ARGB layout: A, R, G, B
                dstRow[x * 4 + 0] = a;
                dstRow[x * 4 + 1] = r;
                dstRow[x * 4 + 2] = g;
                dstRow[x * 4 + 3] = b;
            }
        }

        Xe_Surface_Unlock(xr->xe, surface);
        xr->textures[i] = surface;

        stbi_image_free(pixels);
        fprintf(stderr, "Xenos: Loaded TXTR page %u (%dx%d)\n", i, w, h);
    }

    // Create 1x1 white pixel texture for primitive drawing
    xr->whiteTexture = Xe_CreateTexture(xr->xe, 1, 1, 1, XE_FMT_8888 | XE_FMT_ARGB, 0);
    uint8_t* whiteData = Xe_Surface_LockRect(xr->xe, xr->whiteTexture, 0, 0, 0, 0, XE_LOCK_WRITE);
    whiteData[0] = 255; whiteData[1] = 255; whiteData[2] = 255; whiteData[3] = 255;
    Xe_Surface_Unlock(xr->xe, xr->whiteTexture);

    xr->quadCount = 0;
    xr->currentTextureIndex = -1;

    // Save original counts for dynamic sprite support
    xr->originalTexturePageCount = xr->textureCount;
    xr->originalTpagCount = dataWin->tpag.count;
    xr->originalSpriteCount = dataWin->sprt.count;

    fprintf(stderr, "Xenos: Renderer initialized (%u texture pages)\n", xr->textureCount);
}

static void xeDestroy(Renderer* renderer) {
    XenosRenderer* xr = (XenosRenderer*) renderer;

    for (uint32_t i = 0; xr->textureCount > i; i++) {
        if (xr->textures[i] != nullptr) {
            Xe_DestroyTexture(xr->xe, xr->textures[i]);
        }
    }
    if (xr->whiteTexture != nullptr) {
        Xe_DestroyTexture(xr->xe, xr->whiteTexture);
    }

    Xe_DestroyVertexBuffer(xr->xe, xr->vb);
    Xe_DestroyIndexBuffer(xr->xe, xr->ib);

    free(xr->textures);
    free(xr->textureWidths);
    free(xr->textureHeights);
    free(xr->vertexData);
    free(xr);
}

static void xeBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    XenosRenderer* xr = (XenosRenderer*) renderer;

    xr->quadCount = 0;
    xr->currentTextureIndex = -1;
    xr->gameW = gameW;
    xr->gameH = gameH;
    xr->screenW = windowW;
    xr->screenH = windowH;

    Xe_InvalidateState(xr->xe);
    Xe_SetClearColor(xr->xe, 0);
}

static void xeBeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    XenosRenderer* xr = (XenosRenderer*) renderer;

    xr->quadCount = 0;
    xr->currentTextureIndex = -1;

    // Compute scale and offset to map game view coords to screen coords
    // The port rectangle maps onto screen space
    xr->scaleX = (float) portW / (float) viewW;
    xr->scaleY = (float) portH / (float) viewH;
    xr->offsetX = (float) portX - (float) viewX * xr->scaleX;
    xr->offsetY = (float) portY - (float) viewY * xr->scaleY;

    // TODO: Handle viewAngle rotation
    (void) viewAngle;
}

static void xeEndView(Renderer* renderer) {
    XenosRenderer* xr = (XenosRenderer*) renderer;
    flushBatch(xr);
}

static void xeEndFrame(Renderer* renderer) {
    XenosRenderer* xr = (XenosRenderer*) renderer;
    (void) xr;
    // Resolve and present handled in main.c
    Xe_Resolve(xr->xe);
    Xe_Sync(xr->xe);
}

static void xeFlush(Renderer* renderer) {
    flushBatch((XenosRenderer*) renderer);
}

// Transform a game-space point to normalized device coordinates [-1, 1]
static inline void gameToNDC(XenosRenderer* xr, float gx, float gy, float* nx, float* ny) {
    float sx = gx * xr->scaleX + xr->offsetX;
    float sy = gy * xr->scaleY + xr->offsetY;
    // Map screen coords [0, screenW/H] to NDC [-1, 1]
    *nx = (sx / (float) xr->screenW) * 2.0f - 1.0f;
    *ny = 1.0f - (sy / (float) xr->screenH) * 2.0f; // Y-flip for top-down
}

// Helper: write a vertex into the batch buffer
static inline void writeVertex(float* v, float nx, float ny, float u, float vt, float r, float g, float b, float a) {
    v[0] = nx; v[1] = ny; v[2] = u; v[3] = vt;
    v[4] = r;  v[5] = g;  v[6] = b; v[7] = a;
}

static void xeDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    XenosRenderer* xr = (XenosRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || xr->textureCount <= (uint32_t) pageId) return;
    if (xr->textures[pageId] == nullptr) return;

    int32_t texW = xr->textureWidths[pageId];
    int32_t texH = xr->textureHeights[pageId];
    if (texW == 0 || texH == 0) return;

    // Flush if texture changed or batch full
    if (xr->quadCount > 0 && xr->currentTextureIndex != pageId) flushBatch(xr);
    if (xr->quadCount >= XE_MAX_QUADS) flushBatch(xr);
    xr->currentTextureIndex = pageId;

    // Compute normalized UVs
    float u0 = (float) tpag->sourceX / (float) texW;
    float v0 = (float) tpag->sourceY / (float) texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / (float) texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / (float) texH;

    // Compute local quad corners
    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float localX1 = localX0 + (float) tpag->sourceWidth;
    float localY1 = localY0 + (float) tpag->sourceHeight;

    // Build 2D transform
    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    // Transform 4 corners
    float gx0, gy0, gx1, gy1, gx2, gy2, gx3, gy3;
    Matrix4f_transformPoint(&transform, localX0, localY0, &gx0, &gy0);
    Matrix4f_transformPoint(&transform, localX1, localY0, &gx1, &gy1);
    Matrix4f_transformPoint(&transform, localX1, localY1, &gx2, &gy2);
    Matrix4f_transformPoint(&transform, localX0, localY1, &gx3, &gy3);

    // Convert to NDC
    float nx0, ny0, nx1, ny1, nx2, ny2, nx3, ny3;
    gameToNDC(xr, gx0, gy0, &nx0, &ny0);
    gameToNDC(xr, gx1, gy1, &nx1, &ny1);
    gameToNDC(xr, gx2, gy2, &nx2, &ny2);
    gameToNDC(xr, gx3, gy3, &nx3, &ny3);

    // Convert BGR color to RGB floats
    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    float* verts = xr->vertexData + xr->quadCount * XE_VERTICES_PER_QUAD * XE_FLOATS_PER_VERTEX;
    writeVertex(verts + 0,  nx0, ny0, u0, v0, r, g, b, alpha);
    writeVertex(verts + 8,  nx1, ny1, u1, v0, r, g, b, alpha);
    writeVertex(verts + 16, nx2, ny2, u1, v1, r, g, b, alpha);
    writeVertex(verts + 24, nx3, ny3, u0, v1, r, g, b, alpha);

    xr->quadCount++;
}

static void xeDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    XenosRenderer* xr = (XenosRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || xr->textureCount <= (uint32_t) pageId) return;
    if (xr->textures[pageId] == nullptr) return;

    int32_t texW = xr->textureWidths[pageId];
    int32_t texH = xr->textureHeights[pageId];
    if (texW == 0 || texH == 0) return;

    if (xr->quadCount > 0 && xr->currentTextureIndex != pageId) flushBatch(xr);
    if (xr->quadCount >= XE_MAX_QUADS) flushBatch(xr);
    xr->currentTextureIndex = pageId;

    float u0 = (float) (tpag->sourceX + srcOffX) / (float) texW;
    float v0 = (float) (tpag->sourceY + srcOffY) / (float) texH;
    float u1 = (float) (tpag->sourceX + srcOffX + srcW) / (float) texW;
    float v1 = (float) (tpag->sourceY + srcOffY + srcH) / (float) texH;

    float gx0 = x;
    float gy0 = y;
    float gx1 = x + (float) srcW * xscale;
    float gy1 = y + (float) srcH * yscale;

    float nx0, ny0, nx1, ny1, nx2, ny2, nx3, ny3;
    gameToNDC(xr, gx0, gy0, &nx0, &ny0);
    gameToNDC(xr, gx1, gy0, &nx1, &ny1);
    gameToNDC(xr, gx1, gy1, &nx2, &ny2);
    gameToNDC(xr, gx0, gy1, &nx3, &ny3);

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    float* verts = xr->vertexData + xr->quadCount * XE_VERTICES_PER_QUAD * XE_FLOATS_PER_VERTEX;
    writeVertex(verts + 0,  nx0, ny0, u0, v0, r, g, b, alpha);
    writeVertex(verts + 8,  nx1, ny1, u1, v0, r, g, b, alpha);
    writeVertex(verts + 16, nx2, ny2, u1, v1, r, g, b, alpha);
    writeVertex(verts + 24, nx3, ny3, u0, v1, r, g, b, alpha);

    xr->quadCount++;
}

// Helper: emit a colored quad using the white texture
static void emitColoredQuad(XenosRenderer* xr, float gx0, float gy0, float gx1, float gy1, float r, float g, float b, float a) {
    if (xr->quadCount > 0 && xr->currentTextureIndex != -2) {
        flushBatch(xr);
    }
    if (xr->quadCount >= XE_MAX_QUADS) flushBatch(xr);
    // Use -2 as sentinel for "white texture bound"
    xr->currentTextureIndex = -2;

    float nx0, ny0, nx1, ny1, nx2, ny2, nx3, ny3;
    gameToNDC(xr, gx0, gy0, &nx0, &ny0);
    gameToNDC(xr, gx1, gy0, &nx1, &ny1);
    gameToNDC(xr, gx1, gy1, &nx2, &ny2);
    gameToNDC(xr, gx0, gy1, &nx3, &ny3);

    float* verts = xr->vertexData + xr->quadCount * XE_VERTICES_PER_QUAD * XE_FLOATS_PER_VERTEX;
    writeVertex(verts + 0,  nx0, ny0, 0.5f, 0.5f, r, g, b, a);
    writeVertex(verts + 8,  nx1, ny1, 0.5f, 0.5f, r, g, b, a);
    writeVertex(verts + 16, nx2, ny2, 0.5f, 0.5f, r, g, b, a);
    writeVertex(verts + 24, nx3, ny3, 0.5f, 0.5f, r, g, b, a);

    xr->quadCount++;
}

static void xeDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    XenosRenderer* xr = (XenosRenderer*) renderer;

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    if (outline) {
        emitColoredQuad(xr, x1, y1, x2 + 1, y1 + 1, r, g, b, alpha);
        emitColoredQuad(xr, x1, y2, x2 + 1, y2 + 1, r, g, b, alpha);
        emitColoredQuad(xr, x1, y1 + 1, x1 + 1, y2, r, g, b, alpha);
        emitColoredQuad(xr, x2, y1 + 1, x2 + 1, y2, r, g, b, alpha);
    } else {
        emitColoredQuad(xr, x1, y1, x2 + 1, y2 + 1, r, g, b, alpha);
    }
}

static void xeDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    XenosRenderer* xr = (XenosRenderer*) renderer;

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    if (xr->quadCount > 0 && xr->currentTextureIndex != -2) flushBatch(xr);
    if (xr->quadCount >= XE_MAX_QUADS) flushBatch(xr);
    xr->currentTextureIndex = -2;

    float nx0, ny0, nx1, ny1, nx2, ny2, nx3, ny3;
    gameToNDC(xr, x1 + px, y1 + py, &nx0, &ny0);
    gameToNDC(xr, x1 - px, y1 - py, &nx1, &ny1);
    gameToNDC(xr, x2 - px, y2 - py, &nx2, &ny2);
    gameToNDC(xr, x2 + px, y2 + py, &nx3, &ny3);

    float* verts = xr->vertexData + xr->quadCount * XE_VERTICES_PER_QUAD * XE_FLOATS_PER_VERTEX;
    writeVertex(verts + 0,  nx0, ny0, 0.5f, 0.5f, r, g, b, alpha);
    writeVertex(verts + 8,  nx1, ny1, 0.5f, 0.5f, r, g, b, alpha);
    writeVertex(verts + 16, nx2, ny2, 0.5f, 0.5f, r, g, b, alpha);
    writeVertex(verts + 24, nx3, ny3, 0.5f, 0.5f, r, g, b, alpha);

    xr->quadCount++;
}

static void xeDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    XenosRenderer* xr = (XenosRenderer*) renderer;

    float r1 = (float) BGR_R(color1) / 255.0f;
    float g1 = (float) BGR_G(color1) / 255.0f;
    float b1 = (float) BGR_B(color1) / 255.0f;
    float r2 = (float) BGR_R(color2) / 255.0f;
    float g2 = (float) BGR_G(color2) / 255.0f;
    float b2 = (float) BGR_B(color2) / 255.0f;

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    if (xr->quadCount > 0 && xr->currentTextureIndex != -2) flushBatch(xr);
    if (xr->quadCount >= XE_MAX_QUADS) flushBatch(xr);
    xr->currentTextureIndex = -2;

    float nx0, ny0, nx1, ny1, nx2, ny2, nx3, ny3;
    gameToNDC(xr, x1 + px, y1 + py, &nx0, &ny0);
    gameToNDC(xr, x1 - px, y1 - py, &nx1, &ny1);
    gameToNDC(xr, x2 - px, y2 - py, &nx2, &ny2);
    gameToNDC(xr, x2 + px, y2 + py, &nx3, &ny3);

    float* verts = xr->vertexData + xr->quadCount * XE_VERTICES_PER_QUAD * XE_FLOATS_PER_VERTEX;
    writeVertex(verts + 0,  nx0, ny0, 0.5f, 0.5f, r1, g1, b1, alpha);
    writeVertex(verts + 8,  nx1, ny1, 0.5f, 0.5f, r1, g1, b1, alpha);
    writeVertex(verts + 16, nx2, ny2, 0.5f, 0.5f, r2, g2, b2, alpha);
    writeVertex(verts + 24, nx3, ny3, 0.5f, 0.5f, r2, g2, b2, alpha);

    xr->quadCount++;
}

// ===[ Text Drawing ]===

static void xeDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg) {
    XenosRenderer* xr = (XenosRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || dw->font.count <= (uint32_t) fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];

    int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    if (0 > fontTpagIndex) return;

    TexturePageItem* fontTpag = &dw->tpag.items[fontTpagIndex];
    int16_t pageId = fontTpag->texturePageId;
    if (0 > pageId || xr->textureCount <= (uint32_t) pageId) return;
    if (xr->textures[pageId] == nullptr) return;

    int32_t texW = xr->textureWidths[pageId];
    int32_t texH = xr->textureHeights[pageId];
    if (texW == 0 || texH == 0) return;

    uint32_t color = renderer->drawColor;
    float alpha = renderer->drawAlpha;
    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    char* processed = TextUtils_preprocessGmlText(text);
    int32_t textLen = (int32_t) strlen(processed);
    int32_t lineCount = TextUtils_countLines(processed, textLen);

    float totalHeight = (float) lineCount * (float) font->emSize;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    float cursorY = valignOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, processed + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        int32_t pos = 0;

        while (lineLen > pos) {
            uint16_t ch = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == nullptr) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            if (xr->quadCount > 0 && xr->currentTextureIndex != pageId) flushBatch(xr);
            if (xr->quadCount >= XE_MAX_QUADS) flushBatch(xr);
            xr->currentTextureIndex = pageId;

            float u0 = (float) (fontTpag->sourceX + glyph->sourceX) / (float) texW;
            float v0 = (float) (fontTpag->sourceY + glyph->sourceY) / (float) texH;
            float u1 = (float) (fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / (float) texW;
            float v1 = (float) (fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / (float) texH;

            float localX0 = cursorX + glyph->offset;
            float localY0 = cursorY;
            float localX1 = localX0 + (float) glyph->sourceWidth;
            float localY1 = localY0 + (float) glyph->sourceHeight;

            float gx0, gy0, gx1, gy1, gx2, gy2, gx3, gy3;
            Matrix4f_transformPoint(&transform, localX0, localY0, &gx0, &gy0);
            Matrix4f_transformPoint(&transform, localX1, localY0, &gx1, &gy1);
            Matrix4f_transformPoint(&transform, localX1, localY1, &gx2, &gy2);
            Matrix4f_transformPoint(&transform, localX0, localY1, &gx3, &gy3);

            float nx0, ny0, nx1, ny1, nx2, ny2, nx3, ny3;
            gameToNDC(xr, gx0, gy0, &nx0, &ny0);
            gameToNDC(xr, gx1, gy1, &nx1, &ny1);
            gameToNDC(xr, gx2, gy2, &nx2, &ny2);
            gameToNDC(xr, gx3, gy3, &nx3, &ny3);

            float* verts = xr->vertexData + xr->quadCount * XE_VERTICES_PER_QUAD * XE_FLOATS_PER_VERTEX;
            writeVertex(verts + 0,  nx0, ny0, u0, v0, r, g, b, alpha);
            writeVertex(verts + 8,  nx1, ny1, u1, v0, r, g, b, alpha);
            writeVertex(verts + 16, nx2, ny2, u1, v1, r, g, b, alpha);
            writeVertex(verts + 24, nx3, ny3, u0, v1, r, g, b, alpha);

            xr->quadCount++;

            cursorX += glyph->shift;
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += (float) font->emSize;
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }

    free(processed);
}

// ===[ Dynamic Sprite Creation/Deletion ]===
// Not yet supported on Xbox 360 (would require render-to-texture readback)

static int32_t xeCreateSpriteFromSurface(Renderer* renderer, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    (void) renderer; (void) x; (void) y; (void) w; (void) h;
    (void) removeback; (void) smooth; (void) xorig; (void) yorig;
    fprintf(stderr, "Xenos: createSpriteFromSurface not yet implemented\n");
    return -1;
}

static void xeDeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    (void) renderer; (void) spriteIndex;
    fprintf(stderr, "Xenos: deleteSprite not yet implemented\n");
}

// ===[ Vtable ]===

static RendererVtable xenosVtable = {
    .init = xeInit,
    .destroy = xeDestroy,
    .beginFrame = xeBeginFrame,
    .endFrame = xeEndFrame,
    .beginView = xeBeginView,
    .endView = xeEndView,
    .drawSprite = xeDrawSprite,
    .drawSpritePart = xeDrawSpritePart,
    .drawRectangle = xeDrawRectangle,
    .drawLine = xeDrawLine,
    .drawLineColor = xeDrawLineColor,
    .drawText = xeDrawText,
    .flush = xeFlush,
    .createSpriteFromSurface = xeCreateSpriteFromSurface,
    .deleteSprite = xeDeleteSprite,
    .drawTile = nullptr,
};

// ===[ Public API ]===

Renderer* XenosRenderer_create(struct XenosDevice* xe) {
    XenosRenderer* xr = safeCalloc(1, sizeof(XenosRenderer));
    xr->base.vtable = &xenosVtable;
    xr->base.drawColor = 0xFFFFFF;
    xr->base.drawAlpha = 1.0f;
    xr->base.drawFont = -1;
    xr->base.drawHalign = 0;
    xr->base.drawValign = 0;
    xr->xe = xe;
    xr->currentTextureIndex = -1;
    return (Renderer*) xr;
}
