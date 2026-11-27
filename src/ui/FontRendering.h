#pragma once
#include "imgui.h"

namespace strata::ui {
// Configure before adding fonts. Windows uses hinted grayscale DirectWrite;
// other platforms, older Windows and unsupported faces retain the stb loader.
void configureFontRendering(ImFontAtlas &atlas);
ImFontConfig crispFontConfig();
float uiFontPixels(float points, float displayScale);
} // namespace strata::ui
