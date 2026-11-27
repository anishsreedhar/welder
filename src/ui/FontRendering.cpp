#include "FontRendering.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dwrite_3.h>
#include <windows.h>

namespace {
template <class T> struct Com {
    T *p = nullptr;
    ~Com() {
        if (p)
            p->Release();
    }
    T *operator->() const { return p; }
};
struct NativeFactory {
    Com<IDWriteFactory5> factory;
    Com<IDWriteInMemoryFontFileLoader> files;
    bool ready = false;
    NativeFactory() {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                       reinterpret_cast<IUnknown **>(&factory.p))))
            return;
        if (FAILED(factory->CreateInMemoryFontFileLoader(&files.p)))
            return;
        ready = SUCCEEDED(factory->RegisterFontFileLoader(files.p));
    }
    ~NativeFactory() {
        if (ready)
            factory->UnregisterFontFileLoader(files.p);
    }
};
NativeFactory &native() {
    static NativeFactory f;
    return f;
}
struct Face {
    Com<IDWriteFontFace> face;
    DWRITE_FONT_METRICS metrics{};
    void *fallback = nullptr;
};
// Keep stb's state per face as a real fallback, including for individual glyphs.
struct FallbackScope {
    ImFontConfig *src;
    Face *state;
    FallbackScope(ImFontConfig *s) : src(s), state(static_cast<Face *>(s->FontLoaderData)) {
        src->FontLoaderData = state->fallback;
    }
    ~FallbackScope() {
        state->fallback = src->FontLoaderData;
        src->FontLoaderData = state;
    }
};
const ImFontLoader *fallback() { return ImFontAtlasGetFontLoaderForStbTruetype(); }
bool sourceInit(ImFontAtlas *atlas, ImFontConfig *src) {
    if (!fallback()->FontSrcInit(atlas, src))
        return false;
    auto *state = IM_NEW(Face);
    state->fallback = src->FontLoaderData;
    src->FontLoaderData = state;
    auto &f = native();
    if (!f.ready)
        return true;
    Com<IDWriteFontFile> file;
    // A null owner makes DirectWrite retain its own copy of the bytes.
    if (FAILED(f.files->CreateInMemoryFontFileReference(f.factory.p, src->FontData, src->FontDataSize, nullptr, &file.p)))
        return true;
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE fileType{};
    DWRITE_FONT_FACE_TYPE faceType{};
    UINT32 faces = 0;
    if (FAILED(file->Analyze(&supported, &fileType, &faceType, &faces)) || !supported || src->FontNo < 0 ||
        UINT32(src->FontNo) >= faces)
        return true;
    if (FAILED(f.factory->CreateFontFace(faceType, 1, &file.p, src->FontNo, DWRITE_FONT_SIMULATIONS_NONE, &state->face.p)))
        return true;
    state->face->GetMetrics(&state->metrics);
    return true;
}
void sourceDestroy(ImFontAtlas *atlas, ImFontConfig *src) {
    auto *state = static_cast<Face *>(src->FontLoaderData);
    if (!state)
        return; // ImGui may destroy a source before it was initialized.
    {
        FallbackScope scope(src);
        fallback()->FontSrcDestroy(atlas, src);
    }
    IM_DELETE(state);
    src->FontLoaderData = nullptr;
}
int glyphIndex(ImFontAtlas *atlas, ImFontConfig *src, ImWchar code) {
    auto *state = static_cast<Face *>(src->FontLoaderData);
    if (state->face.p) {
        UINT32 c = code;
        UINT16 index = 0;
        if (SUCCEEDED(state->face->GetGlyphIndices(&c, 1, &index)) && index)
            return index;
    }
    FallbackScope scope(src);
    return fallback()->FontSrcGetGlyphIndexFromCodepoint(atlas, src, code);
}
bool containsGlyph(ImFontAtlas *atlas, ImFontConfig *src, int index) {
    auto *state = static_cast<Face *>(src->FontLoaderData);
    if (state->face.p)
        return index > 0 && index < state->face->GetGlyphCount();
    FallbackScope scope(src);
    return fallback()->FontSrcContainsGlyph(atlas, src, index);
}
float layoutScale(const Face &face, const ImFontConfig *src, const ImFontBaked *baked) {
    // Preserve the existing pixel-height convention and hence panel geometry.
    return baked->Size * src->ExtraSizeScale / std::max(1u, unsigned(face.metrics.ascent) + face.metrics.descent);
}
bool bakedInit(ImFontAtlas *atlas, ImFontConfig *src, ImFontBaked *baked, void *data) {
    auto *state = static_cast<Face *>(src->FontLoaderData);
    if (!state->face.p) {
        FallbackScope scope(src);
        return fallback()->FontBakedInit(atlas, src, baked, data);
    }
    if (!src->MergeMode) {
        const float scale = layoutScale(*state, src, baked) / src->ExtraSizeScale;
        baked->Ascent = std::ceil(state->metrics.ascent * scale);
        baked->Descent = std::floor(-float(state->metrics.descent) * scale);
    }
    return true;
}
bool nativeGlyph(ImFontAtlas *atlas, ImFontConfig *src, ImFontBaked *baked, int index, ImFontGlyph *glyph, float *advanceOnly) {
    auto *state = static_cast<Face *>(src->FontLoaderData);
    if (!state->face.p)
        return false;
    const UINT16 gi = UINT16(index);
    DWRITE_GLYPH_METRICS metrics{};
    if (FAILED(state->face->GetDesignGlyphMetrics(&gi, 1, &metrics, FALSE)))
        return false;
    const float scale = layoutScale(*state, src, baked);
    const float density = src->RasterizerDensity * baked->RasterizerDensity;
    if (!(density > 0) || !std::isfinite(density))
        return false;
    const float advance = metrics.advanceWidth * scale;
    if (advanceOnly) {
        *advanceOnly = advance;
        return true;
    }
    DWRITE_GLYPH_RUN run{};
    run.fontFace = state->face.p;
    run.fontEmSize = state->metrics.designUnitsPerEm * scale * density;
    run.glyphCount = 1;
    run.glyphIndices = &gi;
    Com<IDWriteGlyphRunAnalysis> analysis;
    // Native hinting at the actual framebuffer density. Grayscale alpha avoids
    // ClearType color fringes over transparent panels and non-RGB displays.
    if (FAILED(native().factory->CreateGlyphRunAnalysis(&run, nullptr, DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC,
                                                        DWRITE_MEASURING_MODE_NATURAL, DWRITE_GRID_FIT_MODE_ENABLED,
                                                        DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0, 0, &analysis.p)))
        return false;
    RECT bounds{};
    if (FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &bounds)))
        return false;
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    glyph->AdvanceX = advance;
    if (width <= 0 || height <= 0)
        return true; // spaces and other invisible glyphs
    if (width > 8192 || height > 8192)
        return false;
    std::vector<unsigned char> pixels(size_t(width) * height);
    if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds, pixels.data(), UINT32(pixels.size()))))
        return false;
    const ImFontAtlasRectId packed = ImFontAtlasPackAddRect(atlas, width, height);
    if (packed == ImFontAtlasRectId_Invalid)
        return false;
    ImTextureRect *rect = ImFontAtlasPackGetRect(atlas, packed);
    const float reference = src->DstFont->Sources[0]->SizePixels;
    const float offsetScale = reference > 0 ? baked->Size / reference : 1.f;
    const float ox = std::round(src->GlyphOffset.x * offsetScale),
                oy = std::round(src->GlyphOffset.y * offsetScale) + std::round(baked->Ascent);
    glyph->X0 = bounds.left / density + ox;
    glyph->Y0 = bounds.top / density + oy;
    glyph->X1 = bounds.right / density + ox;
    glyph->Y1 = bounds.bottom / density + oy;
    glyph->Visible = true;
    glyph->PackId = packed;
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, glyph, rect, pixels.data(), ImTextureFormat_Alpha8, width);
    return true;
}
bool loadGlyph(ImFontAtlas *atlas, ImFontConfig *src, ImFontBaked *baked, void *data, int index, ImFontGlyph *glyph,
               float *advance) {
    if (nativeGlyph(atlas, src, baked, index, glyph, advance))
        return true;
    FallbackScope scope(src);
    return fallback()->FontBakedLoadGlyph(atlas, src, baked, data, index, glyph, advance);
}
const ImFontLoader *nativeLoader() {
    static const ImFontLoader loader = [] {
        ImFontLoader l;
        l.Name = "DirectWrite grayscale (stb fallback)";
        l.FontSrcInit = sourceInit;
        l.FontSrcDestroy = sourceDestroy;
        l.FontSrcGetGlyphIndexFromCodepoint = glyphIndex;
        l.FontSrcContainsGlyph = containsGlyph;
        l.FontBakedInit = bakedInit;
        l.FontBakedLoadGlyph = loadGlyph;
        return l;
    }();
    return &loader;
}
} // namespace
#endif

namespace strata::ui {
void configureFontRendering(ImFontAtlas &atlas) {
    const ImFontLoader *loader = ImFontAtlasGetFontLoaderForStbTruetype();
#ifdef _WIN32
    if (native().ready && !std::getenv("WELDER_TEST_STB_FONTS"))
        loader = nativeLoader();
#endif
    atlas.SetFontLoader(loader);
}
ImFontConfig crispFontConfig() {
    ImFontConfig config;
    config.OversampleH = 1;
    config.OversampleV = 1;
    config.PixelSnapH = true;
    return config;
}
float uiFontPixels(float points, float displayScale) {
    // Preferences use typographic points (96 desktop pixels / 72 points).
    return std::round(std::clamp(points, 8.f, 40.f) * (96.f / 72.f) * std::clamp(displayScale, .5f, 4.f));
}
} // namespace strata::ui
