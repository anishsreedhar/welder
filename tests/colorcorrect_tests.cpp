#include <cmath>
#include <cstdio>
#include "core/Comp.h"
#include "core/Engine.h"
#include "core/NkExport.h"
#include "nodes/ColorCorrect.h"
#include "nodes/NodeUtils.h"

using namespace strata;
int main() {
    registerBuiltinNodes();
    int failed = 0, checks = 0;
    auto check = [&](bool ok, const char* message) { ++checks; if (!ok) { ++failed; std::printf("FAIL: %s\n", message); } };
    auto near = [&](double a, double b, const char* message) { check(std::fabs(a - b) < 2e-5, message); };
    auto render = [&](json params, json color = {0.2, 0.4, 0.8, 0.5}, bool mask = false, double frame = 1) {
        json nodes = {{{"name", "Src"}, {"type", "Constant"}, {"params", {{"color", color}}}},
                      {{"name", "Mask"}, {"type", "Constant"}, {"params", {{"color", {0.1, 0.25, 0.75, 0.25}}}}},
                      {{"name", "CC"}, {"type", "ColorCorrect"}, {"inputs", mask ? json{"Src", "Mask"} : json{"Src"}}, {"params", params}}};
        auto comp = Comp::fromJson({{"strata", 1}, {"format", {{"width", 9}, {"height", 7}}}, {"nodes", nodes}}, ".");
        EngineOptions options; options.tileSize = 4;
        Engine engine(options);
        return engine.renderRegion(comp->find("CC"), frame, comp->format.box());
    };
    auto at = [](const ImagePtr& image, const char* ch) { return image->at(3, 3, image->ch(ch)); };
    auto identity = render(json::object());
    near(at(identity, "R"), .2, "neutral red"); near(at(identity, "G"), .4, "neutral green");
    near(at(identity, "B"), .8, "neutral blue"); near(at(identity, "A"), .5, "neutral alpha");
    auto red = render({{"channels", {"R"}}, {"saturation", 0}});
    const double lum = .2125 * .2 + .7154 * .4 + .0721 * .8;
    near(at(red, "R"), lum, "selected red desaturates using all RGB inputs");
    near(at(red, "G"), .4, "unselected green remains exact"); near(at(red, "B"), .8, "unselected blue remains exact");
    auto none = render({{"channels", "none"}, {"gain", 8}});
    near(at(none, "R"), .2, "channels none bypass");
    auto alpha = render({{"channels", "alpha"}, {"gain", {1, 1, 1, 2}}});
    near(at(alpha, "A"), 1, "alpha component correction"); near(at(alpha, "R"), .2, "alpha only preserves color");
    auto custom = render({{"lookup", {{"shadow", {{0, 1}, {1, 1}}}, {"highlight", {{0, 0}, {1, 0}}}}}, {"shadows.gain", 2}});
    near(at(custom, "R"), .4, "custom shadow range drives correction");
    auto highlight = render({{"highlights.offset", .25}}, {1, 1, 1, .5});
    near(at(highlight, "R"), 1.25, "default highlight weight");
    auto mid = render({{"midtones.gain", 2}}, {.25, .25, .25, .5});
    near(at(mid, "R"), .5, "derived midtone weight");
    auto shadow = render({{"shadows.offset", .1}}, {0, 0, 0, .5});
    near(at(shadow, "R"), .1, "default shadow weight");
    auto mixed = render({{"gain", 2}, {"mix", .5}});
    near(at(mixed, "R"), .3, "mix blends original and correction");
    auto masked = render({{"gain", 2}}, {.2, .4, .8, .5}, true);
    near(at(masked, "R"), .25, "mask alpha limits correction");
    auto disabledMask = render({{"gain", 2}, {"mask_channel", "none"}}, {.2, .4, .8, .5}, true);
    near(at(disabledMask, "R"), .4, "mask none bypasses connected mask");
    auto maskGreen = render({{"gain", 2}, {"mask_channel", "G"}, {"invert_mask", true}}, {.2, .4, .8, .5}, true);
    near(at(maskGreen, "R"), .35, "mask channel and inversion");
    auto fringe = render({{"gain", 2}, {"fringe", true}}, {.2, .4, .8, .5}, true);
    near(at(fringe, "R"), .35, "mask fringe");
    auto injected = render({{"gain", 2}, {"inject", true}, {"channels", "all"}, {"mask_channel", "B"}}, {.2, .4, .8, .5}, true);
    near(at(injected, "mask.a"), .75, "inject preserves raw selected mask channel");
    auto preserved = render({{"gain", {2, 1, 1, 1}}, {"mix_luminance", 1}});
    near(at(preserved, "R"), .3507007062, "Nuke reference mix luminance red");
    near(at(preserved, "G"), .3507007062, "Nuke reference mix luminance green");
    near(at(preserved, "B"), .7254254818, "Nuke reference mix luminance blue");
    auto halfLum = render({{"gain", {2, 1, 1, 1}}, {"mix_luminance", .5}});
    near(at(halfLum, "R"), .3748776019, "Nuke reference half luminance mix");
    auto hdrLum = render({{"gain", 2}, {"mix_luminance", 1}});
    near(at(hdrLum, "G"), .4415448904, "Nuke reference luminance mix channel order");
    near(at(hdrLum, "B"), .9866482019, "Nuke reference HDR luminance mix");
    auto disabledLum = render({{"gain", 2}, {"mix_luminance", 1}, {"enable_mix_luminance", false}});
    near(at(disabledLum, "R"), .4, "disabled mix luminance bypasses stored value");
    auto unpremult = render({{"channels", {"R"}}, {"saturation", 0}, {"unpremult", "A"}});
    near(at(unpremult, "R"), lum, "partial selection unpremult reads straight RGB");
    near(at(unpremult, "G"), .4, "unpremult keeps unselected green");
    auto transparent = render({{"gain", 4}, {"offset", 1}, {"unpremult", "A"}}, {.1, .2, .3, 0});
    near(at(transparent, "R"), 0, "zero alpha remains premultiplied black");
    auto animated = render({{"gain", {{"keys", {{1, 1}, {2, 3}}}, {"interp", "linear"}}}}, {.2, .4, .8, .5}, false, 1.5);
    near(at(animated, "R"), .4, "animated controls");
    cc::RangeCurve s(cc::defaultLookup(), "shadow"), h(cc::defaultLookup(), "highlight");
    near(s.eval(.045), .5, "smooth default shadow midpoint"); near(h.eval(.75), .5, "smooth default highlight midpoint");
    near(s.eval(-2), 1, "shadow flat outside range"); near(h.eval(8), 1, "highlight flat outside range");
    // Numerical references sampled in a separate Nuke 15.0v5 process.
    auto contrast = render({{"contrast", 2}});
    near(at(contrast, "R"), .22222222388, "Nuke reference contrast pivot");
    near(at(contrast, "B"), 3.55555558205, "Nuke reference contrast HDR output");
    auto order = render({{"gamma", 2}, {"gain", 2}, {"offset", .1}});
    near(at(order, "G"), 1.3649110794, "Nuke reference gamma, gain, offset order");
    auto negativeGamma = render({{"gamma", -1}}, {.25, .5, 2, .5});
    near(at(negativeGamma, "R"), 4, "negative gamma uses reciprocal power");
    near(at(negativeGamma, "G"), 2, "negative gamma affects the image");
    auto zeroGamma = render({{"gamma", 0}}, {.25, .5, 2, .5});
    near(at(zeroGamma, "R"), .25, "zero gamma avoids division by zero");
    auto testMid = render({{"test", true}}, {.25, .25, .25, 1});
    near(at(testMid, "R"), .25, "Nuke reference test midtone shade");
    auto testShadowEdge = render({{"test", true}}, {.045, .045, .045, 1});
    near(at(testShadowEdge, "R"), .25, "Nuke reference shadow overlap diagnostic");
    near(at(testShadowEdge, "G"), .125, "Nuke reference shadow weight diagnostic");
    auto testHighlightEdge = render({{"test", true}}, {.75, .75, .75, 1});
    near(at(testHighlightEdge, "R"), .25, "Nuke reference highlight overlap diagnostic");
    near(at(testHighlightEdge, "G"), .625, "Nuke reference highlight weight diagnostic");
    json exportedNodes = {{{"name", "ColorCorrect1"}, {"type", "ColorCorrect"},
        {"params", {{"lookup", cc::defaultLookup()}, {"test", true}, {"mix_luminance", .7}, {"inject", true}}},
        {"ui", {{"cc_controls", {{"gain", {{"wheel", true}, {"split", true}}}}}}}}};
    const auto exported = nk::exportNodes(exportedNodes);
    check(exported.text.find("gain_panelDropped true") != std::string::npos, "export preserves wheel visibility");
    check(exported.text.find("lookup {") != std::string::npos, "export includes sampled lookup curves");
    check(exported.text.find("mix_luminance 0.7") != std::string::npos, "export includes mix luminance");
    std::printf("ColorCorrect: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
