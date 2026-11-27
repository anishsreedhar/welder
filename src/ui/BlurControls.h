// Blur's compact channel and animation menus. Included inside Properties.cpp's anonymous namespace.
// Reuse the normal edit/checkpoint path so every operation is undoable and saved in the comp.

bool validBlurKnobValue(const json& value, int components) {
    auto scalar = [](const json& v) {
        if (v.is_number()) return std::isfinite(v.get<double>());
        if (!v.is_object()) return false;
        if (v.contains("expr") && !v["expr"].is_string()) return false;
        if (v.contains("interp") && (!v["interp"].is_string() ||
            (v["interp"] != "constant" && v["interp"] != "linear" && v["interp"] != "smooth" && v["interp"] != "user"))) return false;
        if (!v.contains("keys")) return v.contains("expr");
        if (!v["keys"].is_array() || v["keys"].empty()) return false;
        double last = -std::numeric_limits<double>::infinity();
        for (const auto& key : v["keys"]) {
            if (!key.is_array() || key.size() < 2 || !key[0].is_number() || !key[1].is_number()) return false;
            const double frame = key[0].get<double>(), number = key[1].get<double>();
            if (!std::isfinite(frame) || !std::isfinite(number) || frame <= last) return false;
            if (key.size() > 2 && !key[2].is_object()) return false;
            if (key.size() > 2) {
                const auto& options = key[2];
                if (options.contains("i") && (!options["i"].is_string() ||
                    (options["i"] != "constant" && options["i"] != "linear" && options["i"] != "smooth" && options["i"] != "user"))) return false;
                for (const char* slope : {"l", "r"}) if (options.contains(slope) &&
                    (!options[slope].is_number() || !std::isfinite(options[slope].get<double>()))) return false;
            }
            last = frame;
        }
        return true;
    };
    if (!value.is_array()) return scalar(value);
    if ((int)value.size() != components) return false;
    return std::all_of(value.begin(), value.end(), scalar);
}

void blurAnimationMenu(Ctx& c, const KnobDef& k, int n, const std::vector<double>& cur,
                       const std::vector<double>& defs, const json& value) {
    const double frame = c.frame;
    bool onKey = false;
    for (int i = 0; i < n; ++i) onKey = onKey || keyAt(value.is_array() && !value.empty() ? value[std::min(i, (int)value.size() - 1)] : value, frame);
    const bool anim = anyAnimated(value), hasExpression = expr::hasExpr(value);
    auto edit = [&](const std::function<void(json&)>& fn) { c.app.checkpoint(); change(c, k.name, fn); };
    auto reset = [&]() {
        c.app.checkpoint();
        const std::string name = c.node->name, key = k.name;
        c.app.edit([name, key](Comp& comp) { if (auto* node = comp.find(name)) { eraseParam(node, key); node->paramsChanged(); } });
    };
    if (ImGui::MenuItem("Set key")) edit([=](json& v) {
        json& a = expand(v, n, defs); for (int i = 0; i < n; ++i) setKey(n == 1 ? a : a[i], frame, cur[i]);
    });
    if (ImGui::MenuItem("Delete key", nullptr, false, onKey)) edit([=](json& v) {
        json& a = expand(v, n, defs); for (int i = 0; i < n; ++i) removeKey(n == 1 ? a : a[i], frame);
    });
    if (ImGui::MenuItem("No animation", nullptr, false, anim || hasExpression)) edit([=](json& v) {
        v = n == 1 ? json(cur[0]) : json(cur);
    });
    if (ImGui::MenuItem("Set to default")) reset();
    auto focus = [&](const char* panel) {
        c.app.selection = {c.node->name};
        c.ui.curves.fitPending = c.ui.dope.fitPending = true;
        c.ui.curves.hidden.clear();
        ImGui::SetWindowFocus(c.ui.win(panel).c_str());
    };
    if (ImGui::MenuItem("Curve editor...")) focus("Curve Editor");
    if (ImGui::MenuItem("Dope sheet...")) focus("Dope Sheet");
    if (ImGui::MenuItem("Edit expressions...", "=")) openExprEditor(c, k, n, cur, defs);
    if (ImGui::BeginMenu("Interpolation", anim)) {
        for (const char* mode : {"constant", "linear", "smooth"})
            if (ImGui::MenuItem(mode)) edit([=](json& v) {
                json& a = expand(v, n, defs);
                for (int i = 0; i < n; ++i) {
                    json& e = n == 1 ? a : a[i];
                    if (!animated(e)) continue;
                    e["interp"] = mode;
                    for (auto& key : e["keys"]) if (key.size() > 2) {
                        key[2].erase("i"); key[2].erase("l"); key[2].erase("r");
                    }
                }
            });
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Copy")) {
        json copy = value.is_null() ? (n == 1 ? json(cur[0]) : json(cur)) : value;
        ImGui::SetClipboardText(json{{"welder_knob", 1}, {"value", copy}}.dump().c_str());
    }
    json clipboard;
    if (const char* text = ImGui::GetClipboardText()) {
        auto parsed = json::parse(text, nullptr, false);
        if (!parsed.is_discarded()) {
            if (parsed.is_object() && parsed.contains("welder_knob") && parsed.contains("value")) parsed = parsed["value"].get<json>();
            if (validBlurKnobValue(parsed, n)) clipboard = std::move(parsed);
        }
    }
    if (ImGui::BeginMenu("Paste", !clipboard.is_null())) {
        if (ImGui::MenuItem("Animation / expression")) edit([clipboard](json& v) { v = clipboard; });
        if (ImGui::MenuItem("Value at current frame")) {
            std::vector<double> numbers(n);
            for (int i = 0; i < n; ++i) numbers[i] = compValue(clipboard, i, frame, defs[i], c.node);
            edit([=](json& v) { for (int i = 0; i < n; ++i) setComp(v, n, i, numbers[i], frame, defs, &cur); });
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("File")) {
        App* app = &c.app; const std::string node = c.node->name, key = k.name;
        if (ImGui::MenuItem("Import animation...")) openFileDialog(c.ui, "Import animation", false, {".wcurve", ".json"},
            [app, node, key, n](const std::string& path) {
                try {
                    std::ifstream file(std::filesystem::u8path(path)); json v; file >> v;
                    if (!validBlurKnobValue(v, n)) throw std::runtime_error("Invalid animation data or component count");
                    app->checkpoint(); app->edit([=](Comp& comp) { if (auto* nd = comp.find(node)) { nd->params.data[key] = v; nd->paramsChanged(); } });
                } catch (const std::exception& e) { app->message(std::string("Cannot import animation: ") + e.what()); }
            });
        if (ImGui::MenuItem("Export animation...")) {
            const json saved = value.is_null() ? (n == 1 ? json(cur[0]) : json(cur)) : value;
            openFileDialog(c.ui, "Export animation", true, {".wcurve"}, [app, saved](const std::string& path) {
                std::ofstream file(std::filesystem::u8path(path)); file << saved.dump(2) << '\n';
                if (!file) app->message("Cannot write animation file: " + path);
            });
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Reverse keys", nullptr, false, anim)) edit([=](json& v) {
            json& a = expand(v, n, defs);
            for (int i = 0; i < n; ++i) {
                json& e = n == 1 ? a : a[i]; if (!animated(e) || e["keys"].empty()) continue;
                const json original = e;
                auto& keys = e["keys"]; double sum = keys.front()[0].get<double>() + keys.back()[0].get<double>();
                for (size_t j = 0; j < keys.size(); ++j) {
                    auto& key = keys[j];
                    key[0] = sum - key[0].get<double>();
                    if (key.size() == 2) key.push_back(json::object());
                    key[2]["i"] = params::keyInterp(original, j > 0 ? j - 1 : 0);
                    // Mirror tangent direction and exchange incoming/outgoing slopes.
                    if (key.size() > 2) {
                        auto& opts = key[2];
                        if (opts.contains("l") || opts.contains("r")) {
                            double l = opts.value("l", opts.value("r", 0.0)), r = opts.value("r", l);
                            opts["l"] = -r; opts["r"] = -l;
                        }
                    }
                }
                std::reverse(keys.begin(), keys.end());
            }
        });
        if (ImGui::MenuItem("Move first key to current frame", nullptr, false, anim)) edit([=](json& v) {
            json& a = expand(v, n, defs); double first = std::numeric_limits<double>::infinity();
            for (int i = 0; i < n; ++i) { auto& e = n == 1 ? a : a[i]; if (animated(e) && !e["keys"].empty()) first = std::min(first, e["keys"][0][0].get<double>()); }
            if (!std::isfinite(first)) return;
            for (int i = 0; i < n; ++i) { auto& e = n == 1 ? a : a[i]; if (animated(e)) for (auto& key : e["keys"]) key[0] = key[0].get<double>() + frame - first; }
        });
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Predefined")) {
        if (ImGui::MenuItem("Linear ramp over project range")) {
            const double first = c.node->comp ? c.node->comp->firstFrame : 1, last = c.node->comp ? c.node->comp->lastFrame : 100;
            edit([=](json& v) { json curve = {{"keys", {{first, k.lo}, {std::max(first + 1, last), k.hi}}}, {"interp", "linear"}}; v = n == 1 ? curve : json(std::vector<json>(n, curve)); });
        }
        if (ImGui::MenuItem("Sine wave (24 frames)")) edit([=](json& v) {
            const std::string expression = std::to_string(k.lo) + "+(" + std::to_string(k.hi - k.lo) + ")*(sin(frame*0.2617993878)+1)/2";
            json e = {{"expr", expression}}; v = n == 1 ? e : json(std::vector<json>(n, e));
        });
        ImGui::EndMenu();
    }
}

std::string blurChannelLabel(const std::string& channel) {
    return channel == "R" ? "rgba.red" : channel == "G" ? "rgba.green" :
           channel == "B" ? "rgba.blue" : channel == "A" ? "rgba.alpha" : channel;
}

bool blurMenuOption(const std::string& text, bool selected) {
    const bool clicked = ImGui::Selectable(("      " + text).c_str());
    const ImVec2 p = ImGui::GetItemRectMin();
    const float size = 11, top = p.y + (ImGui::GetItemRectSize().y - size) * .5f;
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 a(p.x + 4, top), b(a.x + size, top + size);
    draw->AddRectFilled(a, b, IM_COL32(62, 62, 62, 255)); draw->AddRect(a, b, IM_COL32(24, 24, 24, 255));
    if (selected) {
        draw->AddLine(ImVec2(a.x + 2, a.y + 2), ImVec2(b.x - 2, b.y - 2), IM_COL32(225, 225, 225, 255), 2);
        draw->AddLine(ImVec2(a.x + 2, b.y - 2), ImVec2(b.x - 2, a.y + 2), IM_COL32(225, 225, 225, 255), 2);
    }
    return clicked;
}

void blurChannelKnob(Ctx& c, const KnobDef& k, float avail, bool mask) {
    ChannelSet available = rgbaChannels();
    // Standard channel names remain selectable before a particular upstream
    // image supplies them, just like the standard rgba choices.
    available = unionChannels(available, {"depth.Z", "forward.u", "forward.v", "backward.u", "backward.v",
        "deep.front", "deep.back", "mask.a", "rotopaint_mask.a", "mask_planartrack.a", "mask_splinewarp.a",
        "sample.coverage", "cutout.alpha", "cutout.depth"});
    if (Node* in = c.node->input(mask ? 1 : 0)) available = unionChannels(available, c.app.nodeInfo(in->name).channels);
    for (const auto& channel : c.node->ui.value("blur_custom_channels", ChannelSet{}))
        if (std::find(available.begin(), available.end(), channel) == available.end()) available.push_back(channel);
    const json* raw = findParam(c.node, k.name);
    std::string current = raw && raw->is_string() ? raw->get<std::string>() : mask ? "none" : "all";
    if (!mask && raw && raw->is_array()) {
        current = "rgba";
        if (!raw->empty() && (*raw)[0].is_string()) {
            const std::string first = (*raw)[0].get<std::string>();
            if (auto dot = first.rfind('.'); dot != std::string::npos) current = first.substr(0, dot);
        }
    }
    if (mask && raw) current = resolveChannelName(current);
    auto set = [&](json v) { c.app.checkpoint(); change(c, k.name, [v](json& j) { j = v; }); };
    auto componentName = [](const std::string& channel) { auto pos = channel.rfind('.'); return pos == std::string::npos ? channel : channel.substr(pos + 1); };
    if (mask) {
        bool enabled = current != "none";
        if (kit::checkbox("##maskEnabled", &enabled)) set(enabled ? "A" : "none");
        ImGui::SameLine(0, 3); avail -= ImGui::GetFrameHeight() + 3;
    }
    std::vector<std::string> layers;
    for (const auto& channel : available) {
        const auto dot = channel.rfind('.'); if (dot == std::string::npos) continue;
        const std::string layer = channel.substr(0, dot);
        if (std::find(layers.begin(), layers.end(), layer) == layers.end()) layers.push_back(layer);
    }
    const bool boxes = !mask && current != "none" && current != "all";
    const float menuWidth = std::max(55.f, (k.width > 0 ? k.width : avail) - 24 - (boxes ? 150 : 0));
    bool create = false;
    const std::string preview = mask ? blurChannelLabel(current) : current;
    ImGui::SetNextWindowSize(ImVec2(mask ? 185.f : 170.f, 0));
    if (kit::beginCombo("##channel", preview.c_str(), menuWidth)) {
        auto option = [&](const std::string& text, const std::string& value) { if (blurMenuOption(text, current == value)) set(value); };
        if (!mask) option("all", "all");
        option("none", "none");
        if (mask) { for (const char* ch : {"R", "G", "B", "A"}) option(blurChannelLabel(ch), ch); }
        else { option("rgb", "rgb"); option("rgba", "rgba"); option("alpha", "alpha"); }
        if (ImGui::BeginMenu(mask ? "other channels" : "other layers")) {
            if (mask) { for (const auto& ch : available) if (ch != "R" && ch != "G" && ch != "B" && ch != "A") option(ch, ch); }
            else for (const auto& layer : layers) option(layer, layer);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("new")) create = true;
        ImGui::EndCombo();
    }
    if (boxes) {
        auto selected = parseChannels(c.node->params, k.name, "all", available);
        ChannelSet choices = (current == "rgb" || current == "rgba" || current == "alpha" || (raw && raw->is_array())) ? rgbaChannels() : ChannelSet{};
        if (choices.empty()) for (const auto& ch : available) if (ch.rfind(current + ".", 0) == 0) choices.push_back(ch);
        // Explicit component lists retain their layer, including partially selected custom layers.
        if (raw && raw->is_array() && !selected.empty() && selected.front().find('.') != std::string::npos) {
            choices.clear(); const auto prefix = selected.front().substr(0, selected.front().rfind('.') + 1);
            for (const auto& ch : available) if (ch.rfind(prefix, 0) == 0) choices.push_back(ch);
        }
        for (const auto& channel : choices) {
            ImGui::SameLine(0, 3); bool on = std::find(selected.begin(), selected.end(), channel) != selected.end();
            const std::string name = componentName(channel);
            if (kit::checkbox((name + "##" + channel).c_str(), &on)) {
                auto next = selected; if (on) next.push_back(channel); else next.erase(std::remove(next.begin(), next.end(), channel), next.end());
                set(next.empty() ? json("none") : json(next));
            }
        }
    }
    ImGui::SameLine(0, 3);
    if (ImGui::Button("=", ImVec2(19, ImGui::GetFrameHeight()))) ImGui::OpenPopup("##channelOptions");
    if (ImGui::BeginPopup("##channelOptions")) {
        if (ImGui::MenuItem("Set to default")) set(mask ? "none" : "all");
        if (ImGui::MenuItem("new")) create = true;
        ImGui::EndPopup();
    }
    if (create) ImGui::OpenPopup("New channel / layer");
    if (ImGui::BeginPopup("New channel / layer")) {
        static char name[128] = "";
        ImGui::TextUnformatted(mask ? "Channel name (for example mask.a)" : "Layer name (for example diffuse)");
        ImGui::InputText("##newName", name, sizeof name);
        const std::string text(name);
        const bool valid = !text.empty() && text.front() != '.' && text.back() != '.' && text.find("..") == std::string::npos &&
            std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isalnum(ch) || ch == '_' || ch == '.'; });
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Create")) {
            auto channels = c.node->ui.value("blur_custom_channels", ChannelSet{});
            const ChannelSet added = mask ? ChannelSet{text} : ChannelSet{text + ".R", text + ".G", text + ".B", text + ".A"};
            for (auto& ch : added) if (std::find(channels.begin(), channels.end(), ch) == channels.end()) channels.push_back(ch);
            c.app.checkpoint(); const std::string node = c.node->name;
            c.app.edit([node, channels, key = k.name, text](Comp& comp) { if (auto* nd = comp.find(node)) { nd->ui["blur_custom_channels"] = channels; nd->params.data[key] = text; nd->paramsChanged(); } });
            name[0] = 0; ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled(); ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
