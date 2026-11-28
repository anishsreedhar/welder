#include "ui/SystemFonts.h"
#include <cstdio>
#include <fstream>
#include <set>

int main() {
    const auto families = strata::ui::systemFontFamilies();
#ifdef _WIN32
    if (families.empty()) { std::fprintf(stderr, "No installed font families discovered\n"); return 1; }
#endif
    std::set<std::string> names;
    std::set<std::pair<std::filesystem::path,int>> faces;
    for (const auto& family : families) {
        if (family.name.empty() || !names.insert(family.name).second) return 2;
        for (const auto& face : family.faces) {
            if (!faces.insert({face.path,face.index}).second) continue;
            std::ifstream file(face.path, std::ios::binary | std::ios::ate);
            if (!file || file.tellg() <= 0) { std::fprintf(stderr, "Unreadable font in %s\n", family.name.c_str()); return 3; }
            std::vector<unsigned char> bytes((size_t)file.tellg());
            file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), (std::streamsize)bytes.size());
            if (!file || !strata::ui::systemFontDataSupported(bytes, face.index)) {
                std::fprintf(stderr, "Unsupported installed font: %s (face %d)\n", family.name.c_str(), face.index); return 4;
            }
        }
    }
    if (strata::ui::systemFontDataSupported({}, 0)) return 5;
    std::printf("PASS: %zu installed font choices, %zu unique font faces; all discovered faces can render\n", families.size(), faces.size());
    return 0;
}
