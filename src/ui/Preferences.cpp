// Reference-style Preferences. Edits are transactional: only OK changes the application.
#include "McpService.h"
#include "PanelKit.h"
#include "Panels.h"
#include "PreferenceSettings.h"
#include "ScriptRunner.h"
#include "core/Brand.h"
#include "core/CacheDir.h"
#include "core/Mcp.h"
#include "core/Platform.h"
#include "gpu/Gpu.h"
#include "imgui_internal.h"
#include "ml/ML.h"
#include "nodes/BlenderNode.h"
#include "ofx/OfxHost.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
namespace strata::ui {
namespace {
struct Category {
    const char *name;
    int depth;
    bool group;
};
constexpr Category preferenceCategories[] = {{"General", 0, false},
                                             {"Project Defaults", 0, true},
                                             {"Channel Management", 1, false},
                                             {"Color Management", 1, false},
                                             {"Views", 1, false},
                                             {"Performance", 0, true},
                                             {"Caching", 1, false},
                                             {"Expressions", 1, false},
                                             {"Hardware", 1, false},
                                             {"Localization", 1, false},
                                             {"Threads/Processes", 1, false},
                                             {"Behaviors", 0, true},
                                             {"Documentation", 1, false},
                                             {"File Handling", 1, false},
                                             {"Nodes", 1, false},
                                             {"OFX Plug-ins", 1, false},
                                             {"Positions", 1, false},
                                             {"Scripting", 1, false},
                                             {"Startup", 1, false},
                                             {"UI Scaling", 1, false},
                                             {"Panels", 0, true},
                                             {"Appearance", 1, false},
                                             {"Control Panels", 1, false},
                                             {"File Browser", 1, false},
                                             {"Node Colors", 1, false},
                                             {"Node Graph", 1, false},
                                             {"Scopes", 1, false},
                                             {"Script Editor", 1, false},
                                             {"Viewer", 1, false},
                                             {"Viewer (Flipbook)", 1, false},
                                             {"Viewer (Monitor Out)", 1, false},
                                             {"Viewer Handles", 1, false}};
struct State {
    json draft;
    std::string page = "General";
    bool expanded[32]{};
    bool initialized = false;
    bool browsing = false;
    std::string error;
    int pathRow = -1;
};
State &state() {
    static State s;
    return s;
}
struct Form {
    json &d;
    App &app;
    UiState &ui;
    float label = 164;
    void row(const char *text) {
        kit::setLabelWidth(label);
        kit::rowLabel(text);
    }
    void indent() {
        if (label <= 0) {
            ImGui::SetCursorPosX(0);
            return;
        }
        kit::setLabelWidth(label);
        kit::skipLabel();
    }
    void section(const char *title) {
        ImGui::Spacing();
        const float y = ImGui::GetCursorScreenPos().y;
        const float x = ImGui::GetWindowPos().x + label - 4;
        const float w = ImGui::CalcTextSize(title).x;
        ImFont *bold = preferenceFont("Verdana", 1);
        ImGui::GetWindowDrawList()->AddText(bold, 13.f, ImVec2(std::max(ImGui::GetCursorScreenPos().x, x - w), y),
                                            IM_COL32(205, 205, 205, 255), title);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x + 5, y + 6),
                                            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x, y + 6),
                                            IM_COL32(30, 30, 30, 255));
        ImGui::Dummy(ImVec2(1, 13));
    }
    void note(const char *text) {
        indent();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
    }
    void unsupported(const char *why) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", why);
    }
    void text(const char *key, const char *title, float width = -1, bool browse = false, bool folder = false) {
        ImGui::PushID(key);
        ImGui::BeginDisabled(!prefs::supported(key));
        row(title);
        char buf[4096];
        std::snprintf(buf, sizeof buf, "%s", d[key].get<std::string>().c_str());
        ImGui::SetNextItemWidth(width < 0 ? std::max(60.f, ImGui::GetContentRegionAvail().x - (browse ? 23 : 0)) : width);
        if (ImGui::InputText("##value", buf, sizeof buf))
            d[key] = buf;
        if (browse) {
            ImGui::SameLine(0, 4);
            if (ImGui::Button("\\", ImVec2(18, 19))) {
                state().browsing = true;
                ImGui::CloseCurrentPopup();
                const std::string k = key;
                auto apply = [k](const std::string &path) { state().draft[k] = path; };
                if (folder)
                    openFolderDialog(ui, title, apply, d[key].get<std::string>());
                else
                    openFileDialog(ui, title, false, {}, apply);
            }
        }
        ImGui::EndDisabled();
        if (!prefs::supported(key))
            unsupported("This feature is not available in this Welder build");
        ImGui::PopID();
    }
    void number(const char *key, const char *title, double lo, double hi, bool slider = false, const char *unit = nullptr) {
        ImGui::PushID(key);
        ImGui::BeginDisabled(!prefs::supported(key));
        row(title);
        double n = d[key].get<double>();
        const bool integer = d[key].is_number_integer() || d[key].is_number_unsigned();
        bool changed = kit::valueField("##value", &n, 73, nullptr, integer, integer ? 1 : .01, lo, hi, integer ? 1 : .1);
        if (slider) {
            ImGui::SameLine(0, 3);
            float w = ImGui::GetContentRegionAvail().x - (unit ? ImGui::CalcTextSize(unit).x + 5 : 0);
            changed = kit::tickSlider("##ruler", &n, lo, hi, std::max(40.f, w), false, integer) || changed;
        }
        if (unit) {
            ImGui::SameLine(0, 4);
            ImGui::TextUnformatted(unit);
        }
        if (changed)
            d[key] = integer ? json((int)std::clamp(n, lo, hi)) : json(std::clamp(n, lo, hi));
        ImGui::EndDisabled();
        if (!prefs::supported(key))
            unsupported("This setting is not available in this Welder build");
        ImGui::PopID();
    }
    bool check(const char *key, const char *title, bool same = false, bool available = true,
               const char *why = "Not available in this Welder build") {
        ImGui::PushID(key);
        if (same) {
            ImGui::SameLine(0, 10);
            if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(title).x + 24) {
                ImGui::NewLine();
                indent();
            }
        } else
            indent();
        const bool integer = d[key].is_number_integer();
        bool value = integer ? d[key].get<int>() != 0 : d[key].get<bool>();
        available = available && prefs::supported(key);
        ImGui::BeginDisabled(!available);
        bool hit = kit::checkbox(title, &value);
        ImGui::EndDisabled();
        if (!available)
            unsupported(why);
        if (hit)
            d[key] = integer ? json(value ? 1 : 0) : json(value);
        ImGui::PopID();
        return hit;
    }
    void combo(const char *key, const char *title, std::initializer_list<const char *> items, float width = 195,
               bool available = true) {
        ImGui::PushID(key);
        row(title);
        available = available && prefs::supported(key);
        ImGui::BeginDisabled(!available);
        ImGui::SetNextItemWidth(std::min(width, std::max(50.f, ImGui::GetContentRegionAvail().x)));
        const std::string value = d[key].get<std::string>();
        if (kit::beginComboFlat("##value", value.c_str())) {
            for (const char *item : items) {
                const std::string k = key;
                const bool transform = k == "working_space" || k == "viewer_transform" || k == "integer_transform";
                const bool usable = !(k == "color_mode" && std::string(item) == "OCIO" && !color::haveOCIO()) &&
                                    (!transform || !color::canonical(d.value("ocio_config", std::string()), item).empty());
                if (ImGui::Selectable(item, value == item, usable ? 0 : ImGuiSelectableFlags_Disabled)) {
                    d[key] = item;
                    if (std::string(key) == "scale_mode" && std::string(item) == "manual" && d["ui_scale"].get<double>() <= 0)
                        d["ui_scale"] = 1.0;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (!available)
            unsupported("This setting requires a service not present in this Welder build");
        ImGui::PopID();
    }
    void color(const char *key, const char *title, bool same = false, float width = 107) {
        if (same) {
            ImGui::SameLine(0, 5);
            if (ImGui::GetContentRegionAvail().x < width) {
                ImGui::NewLine();
                indent();
            }
        } else
            indent();
        ImGui::PushID(key);
        ImGui::BeginDisabled(!prefs::supported(key));
        auto c = d[key];
        float v[4] = {(float)c[0].get<double>(), (float)c[1].get<double>(), (float)c[2].get<double>(), (float)c[3].get<double>()};
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::Button("##color", ImVec2(width, 22));
        auto *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(pos.x + 5, pos.y + 6), ImVec2(pos.x + 16, pos.y + 17),
                          ImGui::ColorConvertFloat4ToU32(ImVec4(v[0], v[1], v[2], v[3])));
        dl->AddRect(ImVec2(pos.x + 5, pos.y + 6), ImVec2(pos.x + 16, pos.y + 17), IM_COL32(5, 5, 5, 255));
        dl->AddText(ImVec2(pos.x + 26, pos.y + 5), IM_COL32(205, 205, 205, 255), title);
        if (ImGui::IsItemClicked())
            ImGui::OpenPopup("##pick");
        if (ImGui::BeginPopup("##pick")) {
            if (ImGui::ColorPicker4("##picker", v, ImGuiColorEditFlags_AlphaBar))
                d[key] = {v[0], v[1], v[2], v[3]};
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        if (!prefs::supported(key))
            unsupported("This color is not configurable in this Welder build");
        ImGui::PopID();
    }
    void pair(const char *first, const char *second, const char *title) {
        row(title);
        double a = d[first], b = d[second];
        ImGui::PushID(first);
        ImGui::BeginDisabled(!prefs::supported(first));
        if (kit::valueField("##first", &a, 73, nullptr, false, 1, 1, 1000, 1))
            d[first] = a;
        ImGui::EndDisabled();
        ImGui::SameLine(0, 5);
        ImGui::BeginDisabled(!prefs::supported(second));
        if (kit::valueField("##second", &b, 73, nullptr, false, 1, 1, 1000, 1))
            d[second] = b;
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    void font(const char *key, const char *title, const char *sizeKey = nullptr) {
        ImGui::PushID(key);
        row(title);
        ImGui::SetNextItemWidth(std::min(228.f, std::max(70.f, ImGui::GetContentRegionAvail().x)));
        std::string value = d[key].get<std::string>();
        if (kit::beginComboFlat("##font", value.c_str())) {
            for (auto &family : nodeLabelFamilies())
                if (ImGui::Selectable(family.c_str(), family == value))
                    d[key] = family;
            ImGui::EndCombo();
        }
        if (std::string(key) == "ui_font" || std::string(key) == "graph_font") {
            const bool graph = std::string(key) == "graph_font";
            const char *bold = graph ? "graph_bold" : "ui_font_bold";
            const char *italic = graph ? "graph_italic" : "ui_font_italic";
            ImGui::SameLine(0, 5);
            if (ImGui::GetContentRegionAvail().x < 61) {
                ImGui::NewLine();
                indent();
            }
            ImGui::PushStyleColor(ImGuiCol_Button, d[bold].get<bool>() ? IM_COL32(95, 95, 95, 255) : IM_COL32(67, 67, 67, 255));
            if (ImGui::Button("B", ImVec2(28, 23)))
                d[bold] = !d[bold].get<bool>();
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 5);
            ImGui::PushStyleColor(ImGuiCol_Button, d[italic].get<bool>() ? IM_COL32(95, 95, 95, 255) : IM_COL32(67, 67, 67, 255));
            if (ImGui::Button("I", ImVec2(28, 23)))
                d[italic] = !d[italic].get<bool>();
            ImGui::PopStyleColor();
        }
        if (sizeKey) {
            ImGui::SameLine(0, 5);
            if (ImGui::GetContentRegionAvail().x < 73) {
                ImGui::NewLine();
                indent();
            }
            double n = d[sizeKey];
            if (kit::valueField("##size", &n, 73, nullptr, false, .1, 8, 40, 1))
                d[sizeKey] = std::clamp(n, 8., 40.);
        }
        ImGui::PopID();
    }
};
void unavailableBlock(Form &f, const char *message, std::function<void()> draw) {
    ImGui::BeginDisabled();
    draw();
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", message);
}
void pathTable(Form &f) {
    auto &s = state();
    auto &rows = f.d["path_substitutions"];
    f.row("path substitutions");
    float h = std::max(90.f, ImGui::GetContentRegionAvail().y - 79);
    if (ImGui::BeginTable("##paths", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(-1, h))) {
        for (auto name : {"OSX", "Windows", "Linux"})
            ImGui::TableSetupColumn(name);
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (const char *name : {"OSX", "Windows", "Linux"}) {
            ImGui::TableNextColumn();
            const float x = ImGui::GetCursorPosX();
            ImGui::SetCursorPosX(x + std::max(0.f, (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(name).x) * .5f));
            ImGui::TextUnformatted(name);
        }
        for (int i = 0; i < (int)rows.size(); ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            int col = 0;
            for (const char *k : {"osx", "windows", "linux"}) {
                ImGui::TableSetColumnIndex(col++);
                char b[1024];
                std::snprintf(b, sizeof b, "%s", rows[i].value(k, std::string()).c_str());
                ImGui::PushID(k);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##path", b, sizeof b))
                    rows[i][k] = b;
                if (ImGui::IsItemActive())
                    s.pathRow = i;
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    f.indent();
    if (ImGui::Button("+", ImVec2(29, 25))) {
        rows.push_back({{"osx", ""}, {"windows", ""}, {"linux", ""}});
        s.pathRow = (int)rows.size() - 1;
    }
    ImGui::SameLine(0, 6);
    ImGui::BeginDisabled(s.pathRow < 0 || s.pathRow >= (int)rows.size());
    if (ImGui::Button("-", ImVec2(29, 25))) {
        rows.erase(rows.begin() + s.pathRow);
        s.pathRow = -1;
    }
    ImGui::EndDisabled();
}
void viewsTable(Form &f) {
    ImGui::TextUnformatted("Views");
    ImGui::BeginDisabled();
    ImGui::Button("+", ImVec2(29, 25));
    ImGui::SameLine();
    ImGui::Button("-", ImVec2(29, 25));
    ImGui::EndDisabled();
    if (ImGui::BeginTable("##views", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(-1, std::max(80.f, ImGui::GetContentRegionAvail().y - 110)))) {
        ImGui::TableSetupColumn("View", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Color");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("main");
        ImGui::TableNextColumn();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImGui::GetCursorScreenPos(),
            ImVec2(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y + 11),
            IM_COL32(255, 255, 255, 255));
        ImGui::EndTable();
    }
    f.label = 32;
    f.combo("hero_view", "hero", {"main"}, 55);
    ImGui::BeginDisabled();
    ImGui::Button("Set up views for stereo");
    ImGui::EndDisabled();
    f.unsupported("Multiview / stereo rendering is not implemented in this Welder build");
    f.check("view_colors", "Use colors in UI?", false, false,
            "Multiview / stereo rendering is not implemented in this Welder build");
}

void page(Form &f, const std::string &p) {
    auto &d = f.d;
    if (p == "General") {
        f.label = 161;
        f.section("Autosave");
        f.number("autosave_idle", "idle comp autosave after", 0, 3600, false, "seconds");
        f.number("autosave_force", "force comp autosave after", 0, 3600, false, "seconds");
        f.text("autosave_filename", "autosave comp filename");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("${settings}, ${cache}, ${project}, ${hash}. ${hash} keeps identically named projects separate.");
        f.section("Path Substitutions");
        pathTable(f);
        f.section("Crash Reporter");
        f.check("suppress_crash_report", "Don't show the crash reporter automatically");
    } else if (p == "Channel Management") {
        f.section("Channel Management");
        f.number("channel_warning", "Channel Warning Threshold", 1, 65535);
    } else if (p == "Color Management") {
        f.label = 177;
        f.section("Default Color Management");
        f.combo("color_mode", "", {"Welder", "OCIO"}, 75);
        f.section("Default OCIO Config");
        f.text("ocio_config", "", std::max(120.f, ImGui::GetContentRegionAvail().x - f.label - 176));
        ImGui::SameLine(0, 5);
        ImGui::BeginDisabled(!color::haveOCIO());
        if (ImGui::Button("Choose...")) {
            state().browsing = true;
            ImGui::CloseCurrentPopup();
            openFileDialog(f.ui, "OCIO configuration", false, {".ocio"},
                           [](const std::string &path) { state().draft["ocio_config"] = path; });
        }
        ImGui::SameLine(0, 5);
        if (ImGui::Button("Reload Config")) {
            const std::string error = color::configError(d["ocio_config"]);
            f.app.message(error.empty() ? "OCIO configuration validated" : error);
        }
        ImGui::EndDisabled();
        f.indent();
        ImGui::TextDisabled("%s", color::haveOCIO() ? "OpenColorIO configuration (new projects)"
                                                    : "Built-in color transforms; OpenColorIO is unavailable in this build");
        f.section("Default Color Transforms");
        f.note("When a new project is created, it will default to these settings.\nChanging these will not affect the state of "
               "existing projects.");
        ImGui::Spacing();
        for (auto pair : {std::pair{"working_space", "Working Space"},
                          {"viewer_transform", "Viewer"},
                          {"thumbnail_transform", "Thumbnails"},
                          {"monitor_transform", "Monitor Out"},
                          {"integer_transform", "8-bit Files"},
                          {"integer16_transform", "16-bit Files"},
                          {"log_transform", "Log Files"},
                          {"float_transform", "Floating Point Files"}})
            f.combo(pair.first, pair.second, {"linear", "sRGB", "rec709", "Cineon", "Gamma2.2", "ACEScg"}, 197,
                    color::haveOCIO() || std::string(pair.first) == "viewer_transform" ||
                        std::string(pair.first) == "working_space" || std::string(pair.first) == "integer_transform");
        f.section("OCIO Roles");
        f.check("ocio_roles", "Allow OCIO Roles in Colorspace Knobs*", false, false);
        f.check("ocio_roles_first", "Prioritize OCIO Roles*", false, false);
    } else if (p == "Views")
        viewsTable(f);
    else if (p == "Caching") {
        f.label = 274;
        f.section("Comp Disk Caching");
        f.text("cache_dir", "temp directory", -1, true, true);
        f.number("disk_cache_gb", "comp disk cache size", 0, 100, true, "GB");
        ImGui::BeginDisabled();
        f.number("paint_cache_gb", "rotopaint cache size", 0, 100, true, "GB");
        ImGui::EndDisabled();
        f.unsupported("RotoPaint uses the shared comp cache in Welder");
        f.section("Memory Caching");
        f.number("playback_cache_gb", "playback cache size", 0, 256, false, "GB (0: automatic)");
        unavailableBlock(f, "Welder shares its render cache between nodes; separate percentage budgets are unavailable.", [&] {
            f.number("comp_cache_percent", "comp cache size (%)", 10, 90, true);
            f.number("playback_cache_percent", "comp playback cache size, (% of comp cache)", 0, 50, true);
            f.number("paint_cache_percent", "comp paint cache size (% of comp cache)", 0, 100, true);
        });
        f.section("Application in Background");
        f.check("pause_background_cache", "pause caching when the application goes to the background");
        f.check("clear_background_cache", "clear cache when the application goes to the background");
        f.section("Undo Caching");
        f.number("undo_size", "undo history size", 1, 10000);
        ImGui::BeginDisabled();
        f.number("undo_min", "minimum undo events", 1, 100);
        ImGui::EndDisabled();
        f.indent();
        if (ImGui::Button("Clear disk cache")) {
            if (auto *cache = f.app.diskCache())
                cache->clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear memory cache"))
            f.app.clearCaches();
    } else if (p == "Expressions") {
        f.section("Expressions Re-Evaluation");
        f.combo("expression_mode", "Mode", {"Lazy", "Eager"}, 70);
    } else if (p == "Hardware") {
        f.label = 151;
        f.section("GPU");
        ImGui::BeginDisabled();
        f.number("gpu_texture_mb", "GPU texture cache size*", 16, 16384, false, "Mb");
        ImGui::EndDisabled();
        f.row("default compute device*");
        ImGui::SetNextItemWidth(290);
        if (kit::beginComboFlat("##device", gpu::status().c_str())) {
            ImGui::Selectable(gpu::status().c_str(), true);
            ImGui::EndCombo();
        }
        f.check("gpu_enabled", "use GPU for rendering", false, gpu::built(), "GPU rendering was not compiled into this build");
        f.check("gpu_cache_disabled", "disable GPU cache", false, false, "Welder manages Vulkan memory automatically");
        ImGui::BeginDisabled();
        f.number("gpu_cache_percent", "GPU cache size %*", 10, 50, true);
        ImGui::EndDisabled();
        f.note("The compute device and GPU memory budget are managed automatically.");
    } else if (p == "Localization") {
        f.label = 177;
        unavailableBlock(f, "Media localization is not available in this Welder build.", [&] {
            f.section("System");
            f.combo("localization_mode", "mode", {"on", "off", "manual"}, 70);
            f.check("localization_read_source", "read source files when localized files are out of date");
            f.check("localization_hide_progress", "hide out of date progress bar");
            f.check("localization_pause", "pause localization on script/project open");
            f.section("Inputs");
            f.combo("localization_policy", "localization policy", {"from auto-localize path", "always", "never"}, 160);
            f.section("Paths");
            f.text("localization_source", "auto-localize from", -1, true, true);
            f.text("localization_dir", "localize to", -1, true, true);
            f.section("Storage");
            f.number("localization_limit_gb", "limit to (GB)", 0, 100, true);
            f.section("Network");
            f.number("localization_check_minutes", "check for updated files every", 1, 1440, false, "mins");
        });
    } else if (p == "Threads/Processes") {
        f.label = 205;
        f.section("Rendering");
        f.check("frame_server", "render using frame server (Welder)");
        ImGui::BeginDisabled();
        f.number("frame_server_timeout", "frame server render timeout", 1, 10, true, "min");
        ImGui::EndDisabled();
        f.check("focus_background_renders", "focus background renders", false, false);
        f.number("frame_server_processes", "frame server processes to run*", 1, 1024);
        f.combo("render_limits", "export renders*",
                {"limit renderer (more responsive ui)", "no renderer limits (fastest transcoding)", "customize render limits"},
                300);
    } else if (p == "Documentation") {
        f.label = 135;
        f.combo("documentation_source", "documentation source", {"local", "online"}, 90);
        f.text("documentation_host", "documentation host", 360);
        ImGui::BeginDisabled();
        f.number("documentation_port", "documentation port", 1, 65535);
        ImGui::EndDisabled();
        f.indent();
        if (ImGui::Button("Open documentation")) {
            const std::string path = d["documentation_source"] == "local"
                                         ? (std::filesystem::path(f.app.exeDir).parent_path() / "Welder-Manual.html").string()
                                         : d["documentation_host"].get<std::string>();
            ml::openFolder(path);
        }
    } else if (p == "File Handling") {
        f.label = 120;
        f.combo("frame_number_style", "frame number style", {"Hashes (#)", "Printf (%04d)"}, 145);
        f.check("fixed_frame_width", "assume fixed width frame numbers in file sequences");
        f.check("alembic_all_in_one", "always load abc files as all-in-one", false, false,
                "Alembic loading is not compiled into this build");
        f.check("unc_paths", "Use network paths for mapped drives");
        f.check("ask_match_project", "Offer to match the project to the first Read");
    } else if (p == "Nodes") {
        f.label = 120;
        f.check("merge_connect_a", "New Merge nodes connect A input");
        f.check("autokey_roto", "Autokey roto shapes", false, false, "Welder's Roto animation mode is controlled per node");
        f.section("Viewer");
        f.check("viewer_delete_on_close", "When Viewer is closed delete its node");
        f.check("viewer_double_click_settings", "Double click the Viewer node to open the Viewer settings");
        f.section("Tab Search Menu");
        f.check("search_weighting", "Weighting", false, false);
        f.check("search_favorites", "Favorites", true, false);
        f.indent();
        ImGui::BeginDisabled();
        ImGui::Button("Clear Weighting");
        ImGui::SameLine();
        ImGui::Button("Clear Favorites");
        ImGui::EndDisabled();
    } else if (p == "OFX Plug-ins") {
        f.label = 0;
        f.check("ofx_trial", "allow trial mode in OFX plug-ins", false, false,
                "Trial licensing is controlled by each plug-in vendor");
        auto blocked = ofxBlockedPlugins();
        for (auto &name : blocked)
            ImGui::TextUnformatted(name.c_str());
        if (!blocked.empty() && ImGui::Button("Retry blocked plugins")) {
            ofxUnblockPlugins();
            f.app.message("Blocked plugins will be tried when Welder restarts");
        }
    } else if (p == "Positions") {
        f.label = 0;
        f.check("menus_previous_item", "show menus with previous item under the cursor");
    } else if (p == "Scripting") {
        f.label = 0;
        f.check("script_default_tcl", "script command dialog defaults to TCL", false, false,
                "Welder's script editor runs Python; Tcl execution is not available");
        f.label = 120;
        f.text("python_interpreter", "Python interpreter", -1, true);
        f.check("python_run_init", "Run init.py at startup");
        f.section("MCP server");
        f.check("mcp_enabled", "Let AI clients drive Welder");
        f.number("mcp_port", "port", 1, 65535);
        f.check("mcp_remote", "Allow other computers");
        if (ImGui::TreeNode("Connection details")) {
            if (ImGui::Button("Copy HTTP address and token"))
                ImGui::SetClipboardText(
                    ("http://127.0.0.1:" + std::to_string(f.app.mcpPort) + "/mcp\nAuthorization: Bearer " + mcp::loadToken())
                        .c_str());
            ImGui::SameLine();
            if (ImGui::Button("New token")) {
                mcp::renewToken();
                McpService::instance().restart(f.app);
            }
            ImGui::TreePop();
        }
    } else if (p == "Startup") {
        f.label = 112;
        f.row("startup workspace");
        int slot = d["startup_workspace"];
        ImGui::SetNextItemWidth(170);
        std::string current = workspaceName(slot);
        if (current.empty())
            current = slot == 1 ? "Compositing" : "Workspace " + std::to_string(slot);
        if (kit::beginComboFlat("##workspace", current.c_str())) {
            for (int i = 1; i <= kWorkspaceSlots; ++i) {
                std::string name = workspaceName(i);
                if (name.empty())
                    name = i == 1 ? "Compositing" : "Workspace " + std::to_string(i);
                if (ImGui::Selectable(name.c_str(), slot == i))
                    d["startup_workspace"] = i;
            }
            ImGui::EndCombo();
        }
        f.check("splash_enabled", "show splash screen at startup");
        f.section("Analytics");
        f.check("usage_statistics", "submit usage statistics", false, false,
                "Welder does not collect or submit usage statistics");
        f.check("check_updates", "Check for a newer Welder at startup");
    } else if (p == "UI Scaling") {
        f.label = 99;
        f.combo("scale_mode", "UI Scaling Mode", {"auto", "manual"}, 92);
        ImGui::BeginDisabled(d["scale_mode"] != "manual");
        f.number("ui_scale", "Scale All", .5, 3);
        ImGui::EndDisabled();
        f.row("Per Display");
        ImGui::BeginDisabled();
        if (ImGui::BeginTable("##displays", 2, ImGuiTableFlags_Borders, ImVec2(303, 120))) {
            ImGui::TableSetupColumn("Display");
            ImGui::TableSetupColumn("Scaling");
            ImGui::TableHeadersRow();
            ImGui::EndTable();
        }
        f.indent();
        ImGui::Button("+");
        ImGui::SameLine();
        ImGui::Button("-");
        ImGui::EndDisabled();
        f.unsupported("Use auto scaling to follow the active display's DPI");
        ImGui::Spacing();
        ImGui::TextUnformatted("Changing the UI scaling settings requires a restart to take effect.");
    }

    else if (p == "Appearance") {
        f.label = 177;
        f.section("Font");
        f.font("ui_font", "", "ui_font_size");
        f.section("UI Colors");
        f.color("ui_background", "Background");
        f.color("ui_base", "Base", true);
        f.color("ui_highlight", "Highlight", true);
        f.color("ui_highlight_text", "Highlighted Text", true, 128);
        f.color("ui_label", "Label");
        f.color("ui_button", "Button", true);
        f.color("animated_color", "Animated");
        f.color("keyframe_color", "Keyframe", true);
        f.color("disk_color", "Disk cached frames", true, 150);
        f.color("ram_color", "RAM cached frames", true, 155);
        f.color("playhead_color", "Playhead");
        f.color("inout_color", "In/Out Markers", true, 123);
        f.section("Curve Editor / DopeSheet");
        f.number("curves_visible", "no. of curves visible", 1, 10000);
        f.color("curve_background", "background");
        f.color("curve_unselected", "unselected key", true, 125);
        f.color("curve_part_selected", "part-selected key", true, 135);
        f.color("curve_selected", "selected key", true, 117);
        f.color("curve_timeline", "timeline");
        f.color("curve_text", "control text", true);
        f.color("curve_shadow", "control text shadow", true, 145);
        f.color("curve_time_label", "time label", true);
        f.color("curve_current", "current frame");
        f.color("curve_project", "project frame range", true, 155);
    } else if (p == "Control Panels") {
        f.label = 166;
        f.number("max_panels", "max nodes in properties bin", 1, 100);
        f.check("reopen_new_panel", "reopen acts like new panel");
        f.check("panels_match_selection", "expand/collapse panels in Properties bin to match selection");
        f.combo("input_button_action", "input button action", {"select input node only", "open input properties"}, 183);
        f.number("channel_menu_limit", "max items in channel menu", 10, 10000);
        f.section("Color Panel");
        f.combo("color_picker_mode", "color picker button opens", {"in-panel color picker", "popup color picker"}, 183);
    } else if (p == "File Browser") {
        f.label = 0;
        f.check("browser_recent_dir", "start file browser from most recently used directory");
    } else if (p == "Node Colors") {
        f.label = 0;
        f.check("node_autocolor", "autocolor");
        f.check("node_shade", "Shade Nodes", true);
        auto &rows = d["node_colors"];
        for (int i = 0; i < (int)rows.size(); ++i) {
            ImGui::PushID(i);
            char b[1024];
            std::snprintf(b, sizeof b, "%s", rows[i].value("match", std::string()).c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 48);
            if (ImGui::InputText("##match", b, sizeof b))
                rows[i]["match"] = b;
            ImGui::SameLine(0, 5);
            auto c = rows[i].value("color", json::array({.5, .5, .5, 1.}));
            float rgb[3] = {(float)c[0], (float)c[1], (float)c[2]};
            if (ImGui::ColorEdit3("##color", rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                rows[i]["color"] = {rgb[0], rgb[1], rgb[2], 1.};
            ImGui::PopID();
        }
        f.color("node_other", "All Others");
        f.color("node_text", "Text", true);
        f.color("node_selected", "Selected", true);
        f.color("node_input_selected", "Selected Input", true, 124);
        f.color("node_gl", "GL Color", true);
    } else if (p == "Node Graph") {
        f.label = 240;
        f.check("graph_autolabel", "autolabel");
        f.check("graph_highlight_running", "highlight running operators", true);
        f.combo("stamp_mode", "postage stamp mode", {"Current frame", "Off"}, 112);
        f.number("name_background", "node name background", 0, 1, true);
        f.font("graph_font", "label font");
        f.pair("tile_width", "tile_height", "tile size (WxH)");
        f.check("snap_node", "snap to node", true);
        f.pair("grid_width", "grid_height", "grid size (WxH)");
        f.check("snap_grid", "snap to grid", true);
        f.check("show_grid", "show grid", true);
        f.number("snap_threshold", "snap threshold", 0, 50);
        f.section("Colors");
        f.color("graph_color", "Node Graph");
        f.color("overlay_color", "Overlay", true);
        f.color("elbow_color", "Elbow", true);
        f.section("Bounding Box Warning");
        f.color("bbox_highlight", "Highlight");
        f.color("bbox_line", "Line", true);
        f.check("bbox_warning", "enable", true, false);
        ImGui::BeginDisabled();
        f.number("bbox_threshold", "threshold %", 0, 100, true);
        ImGui::EndDisabled();
        f.section("Arrow");
        f.color("arrow_left", "<");
        f.color("arrow_right", ">", true);
        f.color("arrow_up", "^", true);
        f.color("arrow_down", "v", true);
        f.row("deep arrows");
        f.color("deep_arrow", "", true, 40);
        f.row("expression arrows");
        f.color("expression_arrow", "", true, 40);
        f.check("expression_arrows", "enable", true);
        f.row("link knob arrows");
        f.color("link_arrow", "", true, 40);
        f.check("link_arrows", "enable", true);
        f.row("clone arrows");
        f.color("clone_arrow", "", true, 40);
        f.check("clone_arrows", "enable", true);
        f.number("top_arrow_length", "unconnected top input arrow length", 0, 40, true);
        f.number("side_arrow_length", "unconnected left/right input arrow length", 0, 40, true);
        f.number("output_arrow_length", "unconnected output arrow length", 0, 40, true);
        f.number("arrow_head_length", "arrow head length", 1, 40, true);
        f.number("arrow_head_width", "arrow head width", 1, 40, true);
        f.number("arrow_width", "arrow width", .5, 10, true);
        f.check("pick_arrow_heads", "allow picking of connected arrow heads");
        f.check("pick_elbows", "allow picking of arrow elbows to create Dots");
        f.check("insert_near_middle", "drag-to-insert only works near middle of arrows");
        f.number("dot_size", "size of dots", .25, 4, true);
        f.section("Disconnect Nodes");
        f.check("shake_disconnect", "shake to disconnect nodes");
        f.number("shake_sensitivity", "shake sensitivity", 1, 5, true);
        if (ImGui::TreeNode("Welder controls")) {
            f.check("right_angle_pipes", "Right-angle pipes");
            f.check("alt_drag_cuts", "Alt+drag cuts pipes");
            ImGui::TreePop();
        }
    } else if (p == "Scopes") {
        f.label = 140;
        f.number("scope_black", "black point", 0, 1, true);
        f.number("scope_white", "white point", .001, 1, true);
        f.combo("scope_encoding", "luma/chroma encoding", {"REC 709", "REC 601"}, 90);
        f.check("scope_viewer_transform", "Include viewer color transforms");
        f.check("scope_full_frame", "Force full frame");
    } else if (p == "Script Editor") {
        f.label = 43;
        f.font("script_font", "font", "script_font_size");
        f.number("script_indent", "indent", 1, 16);
        f.check("script_history", "save and restore script editor history");
        f.check("script_echo", "echo python commands to output window");
        f.check("script_clear_success", "clear input window on successful script execution");
    } else if (p == "Viewer") {
        f.label = 136;
        f.section("Defaults");
        f.combo("viewer_renderer", "default renderer", {"GL"}, 55);
        f.check("viewer_own_window", "new Viewers go to own window");
        f.check("viewer_prevent_autozoom", "prevent auto zoom for new Viewers");
        f.check("viewer_lut_rgb_only", "apply LUT to color channels only", false, false,
                "Welder's display transform always preserves alpha");
        f.combo("viewer_bit_depth", "viewer buffer bit depth", {"float"}, 80);
        f.check("viewer_gpu", "use GPU for Viewer when possible", false, false, "The Viewer always uses OpenGL for display");
        f.check("input_gpu", "use GPU for inputs when possible");
        f.check("viewer_disable_dither", "disable GPU Viewer dithering", false, false);
        f.combo("dither_algorithm", "dithering algorithm", {"high frequency"}, 115, false);
        f.check("viewer_no_incomplete_stereo", "no incomplete stereo for new Viewers", false, false);
        f.check("viewer_stereo_warning", "show hardware stereo warning", false, false);
        f.section("Settings");
        f.check("viewer_flip_stereo", "flip stereo interlaced views", false, false);
        f.combo("viewer_texture_size", "texture size", {"512x512"}, 90, false);
        f.combo("viewer_texture_mode", "texture mode", {"Multiframe"}, 90, false);
    } else if (p == "Viewer (Flipbook)") {
        f.label = 193;
        f.combo("flipbook_default", "default flipbook", {"Flipbook Viewer"}, 140);
        f.combo("flipbook_playback", "playback mode", {"Play All Frames", "Real Time"}, 140);
        f.check("flipbook_guides", "guides");
        f.row("fullscreen display");
        ImGui::SetNextItemWidth(180);
        if (kit::beginComboFlat("##display", "Primary Monitor (1)")) {
            ImGui::Selectable("Primary Monitor (1)", true);
            ImGui::EndCombo();
        }
        f.check("flipbook_missing", "see through missing media", false, false);
        f.combo("flipbook_background", "background", {"black", "gray", "checkerboard"}, 120);
        f.number("flipbook_gray", "gray", 0, 255, true);
        f.number("flipbook_checker", "checkerboard", 0, 255, true);
        f.number("flipbook_increment", "frame increment", 1, 100);
        f.combo("flipbook_filter", "filtering mode", {"Auto", "Nearest", "Linear"}, 135);
        f.section("Audio");
        ImGui::BeginDisabled();
        f.number("audio_latency", "default latency adjustment (ms)", -1000, 1000);
        f.number("audio_volume", "default volume", 0, 100, true);
        ImGui::EndDisabled();
        f.unsupported("Audio playback is not available in the flipbook");
    } else if (p == "Viewer (Monitor Out)") {
        f.label = 86;
        unavailableBlock(f, "Monitor Out, HMD and NDI output are not available in this Welder build.", [&] {
            f.check("monitor_legal", "Use video legal range for Monitor Out");
            f.check("monitor_topmost", "Always keep the floating window on top");
            f.check("monitor_hmd", "Enable support for HMD devices*");
            f.check("monitor_global", "Enable global monitor out mode*");
            f.section("Background");
            f.combo("monitor_background", "", {"Black", "Gray", "Checkerboard"}, 120);
            f.number("monitor_gray", "Gray", 0, 255, true);
            f.number("monitor_checker", "Checkerboard", 0, 255, true);
            f.section("NDI");
            f.check("monitor_ndi", "Enable NDI*");
        });
    } else if (p == "Viewer Handles") {
        f.label = 150;
        f.section("Colors");
        f.color("handle_2d_bg", "2D bg");
        f.color("handle_2d_fg", "fg", true);
        f.color("handle_3d_bg", "3D bg");
        f.color("handle_3d_fg", "fg", true);
        f.color("handle_sel", "sel", true);
        f.color("handle_min_sel", "min sel", true);
        f.section("Splines");
        f.number("spline_width", "line width", .5, 5, true);
        f.check("spline_shadow", "draw shadow");
        f.color("handle_expression", "Expression color");
        f.color("handle_focus", "Focus color", true);
        f.color("roto_points", "Points");
        f.color("roto_curves", "Curves", true);
        f.color("roto_transform", "Transform", true);
        f.color("roto_locked", "Locked", true);
        f.color("warp_a", "A Source color");
        f.color("warp_b", "B Source color", true);
        f.color("warp_a_dest", "A Destination color");
        f.color("warp_b_dest", "B Destination color", true);
        f.color("warp_correspondence", "Correspondence color");
        f.color("warp_boundary", "Boundary color", true);
        f.color("warp_hard_boundary", "Hard boundary color", true, 150);
        f.section("Controls");
        f.check("handle_middle_pan", "middle button pans");
        f.check("handle_combo_zoom", "left+middle to zoom", true);
        f.check("transform_preview", "show transform preview", true);
        f.combo("hotkeys_3d", "3D hotkeys", {"QWER (Katana/Maya)"}, 185, false);
        f.combo("navigation_3d", "3D navigation", {"Welder"}, 90, false);
        f.number("handle_transform_size", "3D transform handle size", .5, 10, true);
        f.number("handle_3d_size", "3D handle size", 1, 20, true);
        f.number("handle_2d_size", "2D handle size", 1, 20, true);
        f.number("handle_icon_size", "icon size", 1, 100, true);
        f.number("handle_icon_scale", "icon scaling", .01, 1, true);
        f.number("object_speed", "object interaction speed", .001, 1, true);
        f.number("camera_speed", "camera interaction speed", .001, 1, true);
        if (ImGui::TreeNode("Welder controls")) {
            f.check("middle_drag_zooms", "Middle-drag zooms");
            f.check("roto_show_points", "Always show roto points");
            ImGui::TreePop();
        }
    }

    else {
        f.section(p.c_str());
        ImGui::TextUnformatted("Select a category from the tree on the left.");
    }
}

} // namespace
void drawPreferences(App &app, UiState &ui) {
    auto &s = state();
    if (const char *test = std::getenv("STRATA_TEST_PREFS"); test && *test && !s.initialized) {
        ui.showPrefs = true;
        s.page = test;
    }
    if (s.browsing && !ui.fileDialog.open && !ImGui::IsPopupOpen("###filebrowser", ImGuiPopupFlags_AnyPopupLevel)) {
        s.browsing = false;
        ImGui::OpenPopup("##preferences");
    }
    if (ui.showPrefs) {
        s.draft = app.preferenceSnapshot();
        s.error.clear();
        if (!s.initialized) {
            std::fill(std::begin(s.expanded), std::end(s.expanded), true);
            s.initialized = true;
        }
        ui.showPrefs = false;
        ImGui::OpenPopup("##preferences");
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(680.f, display.x - 16), std::min(480.f, display.y - 16)),
                                        ImVec2(display.x - 16, display.y - 16));
    ImGui::SetNextWindowSize(ImVec2(std::min(1090.f, display.x - 16), std::min(838.f, display.y - 16)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(display.x * .5f, display.y * .5f), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    ImGui::PushFont(preferenceFont("Verdana"), 13.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(11, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, IM_COL32(35, 35, 35, 255));
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, IM_COL32(35, 35, 35, 255));
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, IM_COL32(44, 44, 44, 255));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(50, 50, 50, 255));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(50, 50, 50, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(205, 205, 205, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(57, 57, 57, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(35, 35, 35, 255));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(67, 67, 67, 255));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(247, 151, 22, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(94, 77, 53, 255));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, IM_COL32(205, 205, 205, 255));
    if (ImGui::BeginPopupModal("##preferences", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking)) {
        const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        auto *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + 30), IM_COL32(247, 241, 243, 255));
        const std::string title = "Preferences - " + s.page;
        dl->AddText(ImVec2(pos.x + 7, pos.y + 8), IM_COL32(15, 15, 15, 255), title.c_str());
        ImGui::SetCursorPos(ImVec2(size.x - 22, 6));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(195, 64, 68, 255));
        if (ImGui::Button("x", ImVec2(19, 19)))
            ImGui::CloseCurrentPopup();
        ImGui::PopStyleColor();
        ImGui::SetCursorPos(ImVec2(11, 42));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(56, 56, 56, 255));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##preferenceCategories", ImVec2(190, size.y - 84), ImGuiChildFlags_Borders, ImGuiWindowFlags_None);
        int group = -1;
        bool visible = true;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        for (int i = 0; i < (int)std::size(preferenceCategories); ++i) {
            auto c = preferenceCategories[i];
            if (c.group) {
                group = i;
                visible = true;
            } else if (!c.depth) {
                group = -1;
                visible = true;
            } else if (group >= 0)
                visible = s.expanded[group];
            if (!visible)
                continue;
            const ImVec2 r = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            if (c.group)
                ImGui::GetWindowDrawList()->AddRectFilled(r, ImVec2(r.x + 190, r.y + 13), IM_COL32(38, 38, 38, 255));
            if (ImGui::Selectable("##category", s.page == c.name, 0, ImVec2(188, 13))) {
                if (c.group)
                    s.expanded[i] = !s.expanded[i];
                s.page = c.name;
            }
            auto *draw = ImGui::GetWindowDrawList();
            const ImU32 col = s.page == c.name ? IM_COL32(0, 0, 0, 255) : IM_COL32(205, 205, 205, 255);
            const float tx = r.x + (c.depth ? 43 : 23);
            draw->AddText(ImVec2(tx, r.y), col, c.name);
            const float bx = r.x + (c.depth ? 30 : 10);
            draw->AddLine(ImVec2(bx, r.y - 3), ImVec2(bx, r.y + 6), IM_COL32(24, 24, 24, 255));
            draw->AddLine(ImVec2(bx, r.y + 6), ImVec2(tx - 3, r.y + 6), IM_COL32(24, 24, 24, 255));
            if (c.group) {
                draw->AddRectFilled(ImVec2(bx - 4, r.y + 2), ImVec2(bx + 4, r.y + 10), IM_COL32(28, 28, 28, 255));
                draw->AddLine(ImVec2(bx - 2, r.y + 6), ImVec2(bx + 2, r.y + 6), col);
                if (!s.expanded[i])
                    draw->AddLine(ImVec2(bx, r.y + 4), ImVec2(bx, r.y + 8), col);
            }
            ImGui::SetCursorScreenPos(ImVec2(r.x, r.y + 13));
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::SetCursorPos(ImVec2(211, 50));
        ImGui::BeginChild("##preferencePage", ImVec2(size.x - 222, size.y - 92), ImGuiChildFlags_None, ImGuiWindowFlags_None);
        static std::string shownPage;
        if (shownPage != s.page) {
            ImGui::SetScrollY(0);
            shownPage = s.page;
        }
        const ImU32 oldMuted = theme().muted;
        theme().muted = IM_COL32(205, 205, 205, 255);
        const float old = kit::setLabelWidth(164);
        const bool ref = kit::setReferenceFields(true);
        Form f{s.draft, app, ui};
        page(f, s.page);
        kit::setReferenceFields(ref);
        kit::setLabelWidth(old);
        theme().muted = oldMuted;
        ImGui::EndChild();
        ImGui::SetCursorPos(ImVec2(11, size.y - 33));
        if (ImGui::Button("Restore Defaults", ImVec2(105, 22)))
            s.draft = prefs::defaults();
        if (!s.error.empty()) {
            ImGui::SameLine(0, 12);
            ImGui::PushTextWrapPos(size.x - 190);
            ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "%s", s.error.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::SetCursorPos(ImVec2(size.x - 178, size.y - 33));
        if (ImGui::Button("OK", ImVec2(79, 22))) {
            const bool changingPaths = s.draft["path_substitutions"] != app.preferenceSnapshot()["path_substitutions"];
            std::string error;
            if (changingPaths && app.batchStatus().running)
                error = "Finish or stop the current render before changing path substitutions.";
            if (error.empty() && s.draft.value("color_mode", std::string()) == "OCIO")
                error = color::configError(s.draft.value("ocio_config", std::string()));
            s.error = error;
            if (!error.empty())
                app.message(error);
            else {
                app.applyPreferences(s.draft);
                McpService::instance().apply(app);
                ui.pendingTheme = app.themeName;
                ui.renderFrameServer = s.draft.value("frame_server", false);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine(0, 7);
        if (ImGui::Button("Cancel", ImVec2(79, 22)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(12);
    ImGui::PopStyleVar(6);
    ImGui::PopFont();
}
} // namespace strata::ui
