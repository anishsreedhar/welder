#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace strata::ui {
struct SystemFontFace {
    std::filesystem::path path;
    int index = 0;  // face within a .ttc collection
    bool symbol = false;
};
struct SystemFontFamily {
    std::string name;
    // regular, bold, italic, bold italic (nearest installed face when a style is missing)
    std::array<SystemFontFace, 4> faces;
};
std::vector<SystemFontFamily> systemFontFamilies();
bool systemFontDataSupported(const std::vector<unsigned char>& data, int faceIndex);
std::vector<int> systemFontSymbolRemaps(const std::vector<unsigned char>& data, int faceIndex);
}
