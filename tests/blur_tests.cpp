// Pixel regressions for the controls exposed by Blur's reference panel.
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include "core/Comp.h"
#include "core/Engine.h"
#include "core/Schema.h"
#include "core/NkExport.h"
#include "core/ExprValue.h"
#include "ui/NumericWheel.h"

using namespace strata;

namespace {
int checks = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
void near(double actual, double expected, const char* message, double tolerance = 1e-5) {
    check(std::abs(actual - expected) <= tolerance, message);
}
class Fixture : public Node {
public:
    Box dod(Engine&, double) override { return {8, 8, 56, 40}; }
    ChannelSet channels(Engine&, double) override { return {"R", "G", "B", "A", "diffuse.R"}; }
    void render(RenderArgs&, Image& out) override {
        for (int y = out.bounds.y1; y < out.bounds.y2; ++y)
            for (int x = out.bounds.x1; x < out.bounds.x2; ++x) {
                auto* p = out.px(x, y); const float impulse = x == 32 && y == 24 ? 1.f : 0.f;
                p[0] = impulse; p[1] = impulse * .5f; p[2] = .25f; p[3] = 1.f; p[4] = 2 * impulse;
            }
    }
};
std::unique_ptr<Comp> make(json params, double mask = 1) {
    return Comp::fromJson({{"strata", 1}, {"format", {{"width", 64}, {"height", 48}}}, {"frame_range", {1, 10}},
        {"nodes", {
            {{"name", "Source"}, {"type", "BlurFixture"}},
            {{"name", "Mask"}, {"type", "Constant"}, {"params", {{"color", {1 - mask, 0, 0, mask}}, {"format", {64, 48}}}}},
            {{"name", "Blur1"}, {"type", "Blur"}, {"inputs", {"Source", "Mask"}}, {"params", params}}
        }}}, std::filesystem::temp_directory_path());
}
ImagePtr render(Comp& c, double frame = 1, int tile = 256) {
    EngineOptions options; options.tileSize = tile;
    Engine engine(options);
    return engine.renderRegion(c.find("Blur1"), frame, {0, 0, 64, 48});
}
double px(const ImagePtr& image, const char* channel, int x = 32, int y = 24) { return image->at(x, y, image->ch(channel)); }
}

int main() {
    try {
        registerBuiltinNodes();
        NodeRegistry::instance().add("BlurFixture", [] { return std::make_unique<Fixture>(); });
        for (const char* filter : {"box", "triangle", "quadratic", "gaussian"}) {
            auto c = make({{"size", 6}, {"filter", filter}, {"crop", false}});
            auto image = render(*c);
            check(px(image, "R") > 0 && px(image, "R") < 1, "Each filter must soften an impulse");
            near(px(image, "B"), .25, "Constant interior must stay constant");
            double energy = 0; for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) energy += px(image, "R", x, y);
            near(energy, 1, "Normalized kernels must preserve impulse energy");
        }
        auto base = make({{"size", 6}}); const double blurred = px(render(*base), "R");
        auto zero = make({{"size", 0}}); near(px(render(*zero), "R"), 1, "Size zero must pass through");
        auto none = make({{"size", 6}, {"channels", "none"}}); near(px(render(*none), "R"), 1, "Channels none must pass through");
        auto alpha = make({{"size", 6}, {"channels", "alpha"}}); near(px(render(*alpha), "R"), 1, "Alpha-only must leave RGB unchanged");
        auto layer = make({{"size", 6}, {"channels", "diffuse"}}); auto layerImage = render(*layer);
        near(px(layerImage, "R"), 1, "Custom-layer blur must leave RGB unchanged");
        near(px(layerImage, "diffuse.R"), 2 * blurred, "Custom-layer selection must affect the selected layer");
        auto red = make({{"size", 6}, {"channels", {"R"}}}); auto redImage = render(*red);
        near(px(redImage, "R"), blurred, "Individual red checkbox must enable red blur");
        near(px(redImage, "G"), .5, "Unchecked green must remain untouched");
        auto xy = make({{"size", {6, 0}}}); auto xyImage = render(*xy);
        check(px(xyImage, "R", 33, 24) > 0, "Separate X size must blur horizontally");
        near(px(xyImage, "R", 32, 25), 0, "Zero Y size must not blur vertically");
        for (double mix : {0., .25, 1.}) {
            auto c = make({{"size", 6}, {"mix", mix}});
            near(px(render(*c), "R"), 1 + mix * (blurred - 1), "Mix must dissolve original and effect");
        }
        for (double mask : {0., .25, 1.}) {
            auto c = make({{"size", 6}, {"mask_channel", "A"}}, mask);
            near(px(render(*c), "R"), 1 + mask * (blurred - 1), "Mask must weight the effect");
            auto inverse = make({{"size", 6}, {"mask_channel", "A"}, {"invert_mask", true}}, mask);
            near(px(render(*inverse), "R"), 1 + (1 - mask) * (blurred - 1), "Invert must reverse the mask");
            auto fringe = make({{"size", 6}, {"mask_channel", "A"}, {"fringe", true}}, mask);
            near(px(render(*fringe), "R"), 1 + 4 * mask * (1 - mask) * (blurred - 1), "Fringe must isolate the soft edge");
        }
        auto unmasked = make({{"size", 6}, {"mask_channel", "none"}}, 0);
        near(px(render(*unmasked), "R"), blurred, "Mask none must ignore a connected black mask");
        auto defaultMask = make({{"size", 6}}, 0);
        near(px(render(*defaultMask), "R"), blurred, "Mask must default to disabled");
        auto maskRed = make({{"size", 6}, {"mask_channel", "R"}}, .25);
        near(px(render(*maskRed), "R"), 1 + .75 * (blurred - 1), "Mask channel selection must use the requested component");
        for (double size : {0., 6., 40.}) {
            auto inject = make({{"size", size}, {"mask_channel", "A"}, {"inject", true}, {"mix", 0}, {"invert_mask", true}, {"fringe", true}}, .25);
            auto image = render(*inject);
            check(image->ch("mask.a") >= 0, "Inject must declare mask.a, including at size zero");
            near(px(image, "mask.a"), .25, "Injected mask must remain raw, independent of mix/invert/fringe");
            near(px(image, "R"), 1, "Mix zero must preserve original even with injection");
        }
        Engine engine;
        check(engine.dod(base->find("Blur1"), 1) == Box(8, 8, 56, 40), "Crop must preserve incoming bounds");
        auto uncropped = make({{"size", 6}, {"crop", false}});
        check(engine.dod(uncropped->find("Blur1"), 1).width() > 48, "Crop off must allow expanded bounds");
        auto animated = make({{"size", {{"keys", {{1, 0}, {10, 6}}}, {"interp", "linear"}}}});
        near(px(render(*animated, 1), "R"), 1, "First animation key must evaluate");
        near(px(render(*animated, 10), "R"), blurred, "Last animation key must evaluate");
        auto expression = make({{"size", {{"expr", "frame-1"}}}});
        near(px(render(*expression, 7), "R"), blurred, "Size expression must evaluate at the current frame");
        auto large = make({{"size", 40}, {"quality", 15}, {"inject", true}, {"mask_channel", "A"}}, .25);
        auto smallTiles = render(*large, 1, 16), bigTiles = render(*large, 1, 256);
        for (size_t i = 0; i < smallTiles->data.size(); ++i) near(smallTiles->data[i], bigTiles->data[i], "Reduced-quality blur must stay seamless across tiles", 1e-4);
        auto saved = Comp::fromJson(large->toJson(), std::filesystem::temp_directory_path());
        near(px(render(*saved), "mask.a"), .25, "Injected mask settings must survive save/load");
        const auto exported = nk::exportComp(*large).text;
        check(exported.find("quality 15") != std::string::npos, "NK export must preserve quality");
        check(exported.find("inject true") != std::string::npos, "NK export must preserve injection");
        auto disabled = make({{"size", 6}, {"disable", true}});
        near(px(render(*disabled), "R"), 1, "Node tab Disable must bypass the blur");
        auto lifetime = make({{"size", 6}, {"useLifetime", true}, {"lifetimeStart", 3}, {"lifetimeEnd", 5}});
        near(px(render(*lifetime, 1), "R"), 1, "Outside lifetime the blur must bypass");
        near(px(render(*lifetime, 4), "R"), blurred, "Inside lifetime the blur must run");
        auto connections = make({{"size", 6}, {"mask_channel", "none"}});
        auto* roto = connections->addNode("Roto", "Roto1");
        auto* blurNode = connections->find("Blur1");
        check(connections->setInput(blurNode, 1, roto), "Roto must connect to Blur's mask input");
        check(blurNode->params.str("mask_channel") == "A", "A new Roto mask connection must enable rgba.alpha");
        blurNode->params.data["mask_channel"] = "none";
        check(connections->setInput(blurNode, 1, roto), "An unchanged connection must remain valid");
        check(blurNode->params.str("mask_channel") == "none", "Explicitly disabling a connected mask must persist");
        auto disabledMask = Comp::fromJson(connections->toJson(), std::filesystem::temp_directory_path());
        check(disabledMask->find("Blur1")->params.str("mask_channel") == "none", "Loading must preserve an intentionally disabled Roto mask");
        check(connections->setInput(blurNode, 1, nullptr), "Mask disconnect must succeed");
        check(connections->setInput(blurNode, 1, roto), "Mask reconnect must succeed");
        check(blurNode->params.str("mask_channel") == "A", "Reconnecting a mask must enable it again");
        auto staticCurve = make({{"size", expr::withExpression(6, "curve*2", 1, 0)}});
        near(staticCurve->find("Blur1")->params.num("size", 1), 12, "curve must retain the knob's original static value");
        staticCurve->find("Blur1")->params.data["size"]["value"] = 7;
        near(staticCurve->find("Blur1")->params.num("size", 1), 14, "Changing the underlying value must update curve expressions");
        const json edited = expr::withExpression(staticCurve->find("Blur1")->params.data["size"], "curve*3", 1, 14);
        near(edited["value"].get<double>(), 7, "Reapplying an expression must not compound its evaluated value");
        near(expr::withExpression(edited, "", 1, 21).get<double>(), 7, "Removing an expression must restore the underlying value");
        json animation = {{"keys", {{1, 2}, {11, 12}}}, {"interp", "linear"}};
        auto curveComp = make({{"size", expr::withExpression(animation, "curve(frame-1)*2", 5, 0)}});
        near(curveComp->find("Blur1")->params.num("size", 6), 12, "curve(frame) must sample preserved animation keys");
        auto curveSaved = Comp::fromJson(staticCurve->toJson(), std::filesystem::temp_directory_path());
        near(curveSaved->find("Blur1")->params.num("size", 1), 14, "Static curve bases must survive save/load");
        auto maskedBlur = make({{"size", 12}});
        const auto expectedCurvePixels = render(*maskedBlur);
        auto pixelsComp = make({{"size", expr::withExpression(6, "curve*2", 1, 0)}});
        near(px(render(*pixelsComp), "R"), px(expectedCurvePixels, "R"), "Applied curve expressions must affect rendered pixels");
        ui::kit::NumericWheel wheel;
        double number = 0;
        for (int i = 0; i < 5; ++i) number = wheel.advance(number, .2, 1, true, i*.02);
        near(number, 1, "Small positive wheel deltas must accumulate into a whole step");
        for (int i = 0; i < 5; ++i) number = wheel.advance(number, -.2, 1, true, .2+i*.02);
        near(number, 0, "Small negative wheel deltas must accumulate without drift");
        near(wheel.advance(10000000000., 1, 1, true, 1), 10000000001., "Wheel edits must retain large-number digits");
        ui::kit::NumericWheel fine;
        double fraction = 0;
        for (int i = 0; i < 10; ++i) fraction = fine.advance(fraction, .1, .1, false, i*.02);
        near(fraction, .1, "Fractional controls must respond smoothly to small deltas");
        const auto* definition = ui::nodeDef("Blur");
        check(definition != nullptr, "Blur definition must exist");
        for (const auto& knob : definition->knobs) if (knob.name == "mask_channel") check(knob.def == "none", "UI mask default must match rendering");
        std::printf("PASS: %d Blur checks (filters, channels, masks, injection, crop, XY, quality, keys, expressions, save/load)\n", checks);
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL after %d checks: %s\n", checks, error.what()); return 1; }
}
