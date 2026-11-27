#include "imgui_internal.h"
#include "ui/FontRendering.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <vector>
using namespace strata::ui;
static int checks = 0;
void check(bool condition, const char *reason) {
    ++checks;
    if (!condition)
        throw std::runtime_error(reason);
}
int main() {
    try {
        check(uiFontPixels(11, 1) == 15, "11 points must not be squeezed into 11 pixels");
        check(uiFontPixels(11, 1.5f) == 22, "150 percent DPI raster size");
        check(uiFontPixels(11, 2) == 29, "200 percent DPI raster size");
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        configureFontRendering(*io.Fonts);
        std::vector<std::filesystem::path> paths = {std::filesystem::path(__FILE__).parent_path().parent_path() /
                                                    "third_party/fonts/Roboto-Regular.ttf"};
#ifdef _WIN32
        const char *windows = std::getenv("WINDIR");
        if (windows) {
            auto dir = std::filesystem::path(windows) / "Fonts";
            for (const char *name : {"verdana.ttf", "verdanab.ttf", "arial.ttf", "cour.ttf"})
                if (std::filesystem::exists(dir / name))
                    paths.push_back(dir / name);
        }
        check(std::getenv("WELDER_TEST_STB_FONTS") ||
                  std::string(io.Fonts->FontLoaderName).find("DirectWrite") != std::string::npos,
              "Windows must use the native hinted rasterizer");
#endif
        for (const auto &path : paths) {
            auto cfg = crispFontConfig();
            auto *font = io.Fonts->AddFontFromFileTTF(path.string().c_str(), 14, &cfg);
            check(font != nullptr, "font load");
            for (float size : {11.f, 13.f, 15.f, 18.f})
                for (float density : {1.f, 1.25f, 1.5f, 2.f}) {
                    auto *baked = font->GetFontBaked(size, density);
                    check(baked != nullptr, "dynamic font size / DPI");
                    check(baked->Ascent > 0 && baked->Descent < 0, "font metrics");
                    for (ImWchar code : {ImWchar('A'), ImWchar('I'), ImWchar('j'), ImWchar('g'), ImWchar('0'), ImWchar('1'),
                                         ImWchar(' '), ImWchar(0xe9), ImWchar(0xb0)}) {
                        const auto *glyph = baked->FindGlyph(code);
                        check(glyph != nullptr, "glyph lookup");
                        check(std::isfinite(glyph->AdvanceX) && glyph->AdvanceX > 0, "valid advance");
                        if (code != ' ') {
                            check(glyph->Visible, "glyph bitmap");
                            check(glyph->X1 > glyph->X0 && glyph->Y1 > glyph->Y0, "unclipped bitmap dimensions");
                            check(glyph->PackId != ImFontAtlasRectId_Invalid, "glyph packed into atlas");
                        }
                    }
                }
        }
        std::printf("PASS: %d font rasterization checks (%zu faces, 4 text sizes, 4 framebuffer densities); %s\n", checks,
                    paths.size(), io.Fonts->FontLoaderName);
        ImGui::DestroyContext();
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        return 1;
    }
}
