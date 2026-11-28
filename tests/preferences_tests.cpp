#include "core/NetPath.h"
#include "core/FileSave.h"
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
        netpath::setSubstitutions({{"D:\\show\\", "/mnt/show/"}});
        check(netpath::substitute("d:/show/shot\\plate.exr") == "/mnt/show/shot/plate.exr", "Windows to Linux normalizes mixed separators");
        check(netpath::substitute("D:\\show\\shot\\plate.exr") == "/mnt/show/shot/plate.exr", "Windows source separators recognized on every host");
        check(netpath::substitute("D:/showcase/plate.exr") == "D:/showcase/plate.exr", "trailing slash cannot match partial folder");
        for (const auto& from : {"/show", "/show/"})
            for (const auto& to : {"D:/show", "D:/show/", "D:\\show\\"}) {
                netpath::setSubstitutions({{from, to}});
                check(netpath::substitute("/show/shot/plate.exr") == "D:/show/shot/plate.exr", "mapping joins folder and filename with one separator");
            }
        netpath::setSubstitutions({{"/", "D:/"}});
        check(netpath::substitute("/shot/plate.exr") == "D:/shot/plate.exr", "mapping a filesystem root keeps the separator");
        netpath::setSubstitutions({{"/SHOW", "/mnt/show"}});
        check(netpath::substitute("/show/a.exr") == "/show/a.exr", "Linux mapping prefixes remain case sensitive");
        check(netpath::getSubstitutions().size() == 1, "worker mapping snapshot");
        netpath::setSubstitutions({});
        check(netpath::substitute("/show/plate.exr") == "/show/plate.exr", "clearing mappings works");
        // Save As to the current autosave (including a lexical alias) must preserve
        // the newly written comp, while other obsolete recovery files are removed.
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() / "welder-save-as-regression";
        fs::create_directories(dir);
        const auto destination = (dir / "current-autosave.weld").string();
        const auto other = (dir / "old-recovery.weld").string();
        const auto source = (dir / "shot.weld").string();
        const auto foreign = (dir / "other-shot.weld").string();
        const json recovery = {{"autosave_of", source}, {"nodes", json::array()}};
        std::string error;
        for (const auto& candidate : {destination, (dir / "." / "current-autosave.weld").string()}) {
            check(savefile::writeAtomic(destination, "old recovery", &error), "prepare old autosave");
            check(savefile::writeAtomic(other, recovery.dump(), &error), "prepare other autosave");
            const json saved = {{"nodes", json::array()}, {"artist_saved", true}};
            check(savefile::saveComp(destination, saved.dump(), {candidate, source}, {other, source}, &error), "Save As succeeds");
            check(fs::exists(destination), "Save As destination is not deleted by cleanup");
            std::ifstream file(destination);
            check(json::parse(file) == saved, "saved comp contains the new contents");
            check(!fs::exists(other), "unrelated obsolete autosave is cleaned up");
        }
        const auto unavailable = (dir / "absent" / "cannot-save.weld").string();
        check(savefile::writeAtomic(other, recovery.dump(), &error), "prepare recovery for failed Save As");
        check(!savefile::saveComp(unavailable, "new comp", {other, source}, {}, &error), "failed Save As reports failure");
        check(fs::exists(other), "failed Save As preserves recovery");
        const auto manual = json{{"nodes", json::array()}}.dump();
        check(savefile::writeAtomic(other, manual, &error), "prepare an artist comp at the recovery filename");
        const auto fallback = (dir / "fallback.weld").string();
        check(savefile::recoveryPath(other, fallback, source) == fallback, "use a fallback for an occupied template");
        check(!savefile::writeRecovery({other, source}, recovery.dump(), &error), "autosave cannot overwrite an artist comp");
        check(savefile::writeRecovery({fallback, source}, recovery.dump(), &error), "create fallback recovery");
        check(!savefile::writeRecovery({fallback, foreign}, recovery.dump(), &error), "another comp cannot overwrite this recovery");
        savefile::removeRecovery({other, source});
        check(fs::exists(other), "discard preserves an artist comp");
        check(savefile::saveComp(destination, manual, {other, source}, {other, destination}, &error), "save beside artist comp");
        check(fs::exists(other), "save cleanup preserves an artist comp");
        fs::remove(other);
        check(savefile::recoveryPath(other, fallback, source) == fallback, "recover fallback even when preferred name is free again");
        savefile::removeRecovery({fallback, source});
        fs::remove(destination);
        check(!ui::prefs::supported("monitor_ndi"), "unavailable NDI cannot appear functional");
        check(ui::prefs::supported("max_panels"), "supported panels controls enabled");
        std::printf("PASS: %d preferences validation, persistence and path-mapping checks\n", checks);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
