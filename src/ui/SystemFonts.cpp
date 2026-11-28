#include "SystemFonts.h"

#include <algorithm>
#include <cctype>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../third_party/imgui/imstb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwrite.h>

namespace {
template<class T> struct FontCom {
    T* p = nullptr;
    ~FontCom() { if (p) p->Release(); }
    T* operator->() const { return p; }
};
std::string fontUtf8(const std::wstring& wide) {
    int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    if (length) WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), result.data(), length, nullptr, nullptr);
    return result;
}
bool fontLocation(IDWriteFontFamily* family, int style, strata::ui::SystemFontFace& result,
                  DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL,
                  DWRITE_FONT_STYLE slant = DWRITE_FONT_STYLE_NORMAL) {
    FontCom<IDWriteFont> font;
    if (FAILED(family->GetFirstMatchingFont(style & 1 ? DWRITE_FONT_WEIGHT_BOLD : weight,
        stretch, style & 2 ? DWRITE_FONT_STYLE_ITALIC : slant, &font.p))) return false;
    FontCom<IDWriteFontFace> face;
    if (FAILED(font->CreateFontFace(&face.p))) return false;
    UINT32 count = 0;
    if (FAILED(face->GetFiles(&count, nullptr)) || count != 1) return false;
    FontCom<IDWriteFontFile> file;
    if (FAILED(face->GetFiles(&count, &file.p))) return false;
    const void* key = nullptr; UINT32 keySize = 0;
    if (FAILED(file->GetReferenceKey(&key, &keySize))) return false;
    FontCom<IDWriteFontFileLoader> loader;
    if (FAILED(file->GetLoader(&loader.p))) return false;
    FontCom<IDWriteLocalFontFileLoader> local;
    if (FAILED(loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader), reinterpret_cast<void**>(&local.p)))) return false;
    UINT32 length = 0;
    if (FAILED(local->GetFilePathLengthFromKey(key, keySize, &length))) return false;
    std::wstring path(length + 1, L'\0');
    if (FAILED(local->GetFilePathFromKey(key, keySize, path.data(), length + 1))) return false;
    path.resize(length);
    result.path = path;
    result.index = (int)face->GetIndex();
    result.symbol = font->IsSymbolFont() != FALSE;
    return true;
}
}
#endif

namespace strata::ui {
bool systemFontDataSupported(const std::vector<unsigned char>& data, int faceIndex) {
    if (data.size() < 12 || faceIndex < 0) return false;
    const int offset = stbtt_GetFontOffsetForIndex(data.data(), faceIndex);
    if (offset < 0 || (size_t)offset + 12 > data.size()) return false;
    stbtt_fontinfo info{};
    return stbtt_InitFont(&info, data.data(), offset) != 0;
}
std::vector<SystemFontFamily> systemFontFamilies() {
    std::vector<SystemFontFamily> result;
#ifdef _WIN32
    FontCom<IDWriteFactory> factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&factory.p)))) return result;
    FontCom<IDWriteFontCollection> collection;
    if (FAILED(factory->GetSystemFontCollection(&collection.p))) return result;
    for (UINT32 i = 0; i < collection->GetFontFamilyCount(); ++i) {
        FontCom<IDWriteFontFamily> family;
        FontCom<IDWriteLocalizedStrings> names;
        if (FAILED(collection->GetFontFamily(i, &family.p)) || FAILED(family->GetFamilyNames(&names.p))) continue;
        UINT32 nameIndex = 0; BOOL exists = FALSE;
        names->FindLocaleName(L"en-us", &nameIndex, &exists);
        if (!exists) nameIndex = 0;
        UINT32 length = 0;
        if (FAILED(names->GetStringLength(nameIndex, &length))) continue;
        std::wstring name(length + 1, L'\0');
        if (FAILED(names->GetString(nameIndex, name.data(), length + 1))) continue;
        name.resize(length);
        SystemFontFamily entry; entry.name = fontUtf8(name);
        if (entry.name.empty() || !fontLocation(family.p, 0, entry.faces[0])) continue;
        for (int style = 1; style < 4; ++style)
            if (!fontLocation(family.p, style, entry.faces[style])) entry.faces[style] = entry.faces[0];
        result.push_back(entry);
        // Also expose installed weights and named faces (Light, Semibold,
        // Condensed, etc.), rather than limiting the picker to four styles.
        for (UINT32 f = 0; f < family->GetFontCount(); ++f) {
            FontCom<IDWriteFont> font;
            FontCom<IDWriteLocalizedStrings> faceNames;
            if (FAILED(family->GetFont(f, &font.p)) || FAILED(font->GetFaceNames(&faceNames.p))) continue;
            SystemFontFamily variant;
            if (!fontLocation(family.p, 0, variant.faces[0], font->GetWeight(), font->GetStretch(), font->GetStyle())) continue;
            if (variant.faces[0].path == entry.faces[0].path && variant.faces[0].index == entry.faces[0].index) continue;
            UINT32 faceNameIndex = 0, faceLength = 0; BOOL faceExists = FALSE;
            faceNames->FindLocaleName(L"en-us", &faceNameIndex, &faceExists);
            if (!faceExists) faceNameIndex = 0;
            if (FAILED(faceNames->GetStringLength(faceNameIndex, &faceLength))) continue;
            std::wstring faceName(faceLength + 1, L'\0');
            if (FAILED(faceNames->GetString(faceNameIndex, faceName.data(), faceLength + 1))) continue;
            faceName.resize(faceLength);
            variant.name = entry.name + " / " + fontUtf8(faceName);
            for (int style = 1; style < 4; ++style)
                if (!fontLocation(family.p, style, variant.faces[style], font->GetWeight(), font->GetStretch(), font->GetStyle())) variant.faces[style] = variant.faces[0];
            if (std::none_of(result.begin(), result.end(), [&](const auto& existing) { return existing.name == variant.name; })) result.push_back(std::move(variant));
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        auto fold = [](std::string text) { for (char& ch : text) ch = (char)std::tolower((unsigned char)ch); return text; };
        return fold(a.name) < fold(b.name);
    });
#endif
    return result;
}
std::vector<int> systemFontSymbolRemaps(const std::vector<unsigned char>& data, int faceIndex) {
    std::vector<int> result;
    if (!systemFontDataSupported(data, faceIndex)) return result;
    stbtt_fontinfo info{};
    stbtt_InitFont(&info, data.data(), stbtt_GetFontOffsetForIndex(data.data(), faceIndex));
    for (int code = 0x20; code <= 0xff; ++code)
        if (!stbtt_FindGlyphIndex(&info, code) && stbtt_FindGlyphIndex(&info, code + 0xf000)) result.push_back(code);
    return result;
}
}
