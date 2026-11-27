#include "core/NetPath.h"
#include "ui/PreferenceSettings.h"
#include <cstdio>
#include <stdexcept>
using namespace strata;
static int checks = 0;
static void check(bool ok, const char *name) {
    ++checks;
    if (!ok)
        throw std::runtime_error(name);
}
int main() {
    try {
        auto defaults = ui::prefs::defaults();
        check(defaults.is_object(), "defaults");
        check(ui::prefs::normalize(json()) == defaults, "null settings restore defaults");
        auto p = ui::prefs::normalize({{"max_panels", -5},
                                       {"undo_size", 0},
                                       {"ui_font_size", 999},
                                       {"autosave_idle", -2},
                                       {"ui_font", false},
                                       {"gpu_enabled", "true"},
                                       {"disk_cache_gb", -8}});
        check(p["max_panels"] == 1, "panel count cannot underflow");
        check(p["undo_size"] == 1, "undo limit cannot underflow");
        check(p["ui_font_size"] == 32, "font upper bound");
        check(p["autosave_idle"] == 0, "idle bounds");
        check(p["ui_font"] == "Verdana", "wrong font type rejected");
        check(p["gpu_enabled"] == true, "wrong bool type rejected");
        check(p["disk_cache_gb"] == 0, "negative cache budget rejected");
        p = ui::prefs::normalize({{"ui_background", json::array({2, -3, .5, 1})},
                                  {"node_selected", json::array({"bad"})},
                                  {"path_substitutions", json::array({7, {{"windows", false}, {"linux", "/shots"}}})},
                                  {"node_colors", json::array({{{"match", 5}, {"color", json::array({"red"})}}})}});
        check(p["ui_background"] == json::array({1., 0., .5, 1.}), "color components bounded");
        check(p["node_selected"] == defaults["node_selected"], "malformed color rejected");
        check(p["path_substitutions"].size() == 1, "malformed table row rejected");
        check(p["path_substitutions"][0]["windows"] == "", "malformed path cell rejected");
        check(p["node_colors"][0]["color"] == json::array({.5, .5, .5, 1.}), "malformed node color rejected");
        json draft = defaults;
        draft["autosave_force"] = 90;
        draft["max_panels"] = 20;
        draft["splash_enabled"] = false;
        auto round = ui::prefs::normalize(json::parse(draft.dump()));
        check(round == draft, "all settings survive JSON round trip");
        check(defaults["max_panels"] == 10, "draft changes do not modify original");
        netpath::setSubstitutions({{"/show", "D:/show"}, {"/show/shot", "E:/specific"}});
        check(netpath::substitute("/show/shot/plate.exr") == "E:/specific/plate.exr", "longest path mapping wins");
        check(netpath::substitute("/showcase/plate.exr") == "/showcase/plate.exr", "only path component prefixes match");
        check(netpath::substitute("/show") == "D:/show", "exact path mapping");
        netpath::setSubstitutions({});
        check(netpath::substitute("/show/plate.exr") == "/show/plate.exr", "clearing mappings works");
        check(!ui::prefs::supported("monitor_ndi"), "unavailable NDI cannot appear functional");
        check(ui::prefs::supported("max_panels"), "supported panels controls enabled");
        std::printf("PASS: %d preferences validation, persistence and path-mapping checks\n", checks);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
