// The Curve Editor and the Dope Sheet (bottom panels, next to the Node Graph).
//
// Both show the animation of the nodes whose properties are open (or that are
// selected): every animated param, and every animated component of a vector
// param. Curve Editor: keys and tangents in value / time; Dope Sheet: keys in
// time only, per node and per param.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "Panels.h"
#include "nodes/NodeUtils.h"
#include "nodes/Track.h"
#include "imgui_internal.h"

namespace strata::ui {

namespace {

// ------------------------------------------------------------------ curve access
// A Tracker's tracks show as curves too: param "track:<i>:x" / "track:<i>:y" (track_x / track_y),
// built from the track's keys; edits to them are written back to the track (keys moved,
// deleted, or added as keyframes) when the edit is applied.
bool trackCurveRef(const CurveRef& r, int& index, int& axis) {
    if (r.param.rfind("track:", 0) != 0) return false;
    const size_t c2 = r.param.find(':', 6);
    if (c2 == std::string::npos) return false;
    index = std::atoi(r.param.c_str() + 6);
    axis = r.param.substr(c2 + 1) == "y" ? 1 : 0;
    return true;
}
std::map<std::string, json> gTrackCurves;
std::set<std::pair<std::string, std::string>> gTrackDirty;  // (node, param)
const json* trackCurve(const Comp& c, const CurveRef& r, bool dirty) {
    int i, axis;
    if (!trackCurveRef(r, i, axis)) return nullptr;
    Node* n = c.find(r.node);
    if (!n || !n->params.has("tracks") || !n->params.data["tracks"].is_array() || i < 0 || i >= (int)n->params.data["tracks"].size())
        return nullptr;
    json cv = {{"keys", json::array()}, {"interp", "linear"}};
    for (auto& k : track::keysOf(n->params.data["tracks"][i])) cv["keys"].push_back({k.frame, axis ? k.y : k.x});
    json& slot = gTrackCurves[r.node + "|" + r.param];
    if (!dirty || !gTrackDirty.count({r.node, r.param})) slot = cv;  // (an edit in progress keeps its own copy)
    if (dirty) gTrackDirty.insert({r.node, r.param});
    return &slot;
}
void writeBackTrackCurves(Comp& c) {
    for (auto& [node, param] : gTrackDirty) {
        int i, axis;
        CurveRef r{node, param, -1};
        if (!trackCurveRef(r, i, axis)) continue;
        Node* n = c.find(node);
        if (!n || !n->params.has("tracks") || i < 0 || i >= (int)n->params.data["tracks"].size()) continue;
        json& t = n->params.data["tracks"][i];
        const json& cv = gTrackCurves[node + "|" + param];
        auto old = track::keysOf(t);
        std::vector<track::Key> out;
        for (auto& e : cv["keys"]) {
            const double f = e[0].get<double>(), v = e[1].get<double>();
            track::Key k;
            bool found = false;
            for (auto& o : old)
                if (std::fabs(o.frame - f) < 1e-6) {
                    k = o;
                    found = true;
                }
            if (!found) {  // a key added on the curve: a keyframe, the other coordinate where the track is
                auto s = track::sampleAt(t, f);
                if (s) k = *s;
                k.frame = f;
                k.user = true;
                k.score = 1;
            }
            (axis ? k.y : k.x) = v;
            out.push_back(k);
        }
        std::sort(out.begin(), out.end(), [](const track::Key& a, const track::Key& b) { return a.frame < b.frame; });
        t["keys"] = track::keysJson(out);
    }
    gTrackDirty.clear();
}

const json* curveJson(const Comp& c, const CurveRef& r) {
    if (r.param.rfind("track:", 0) == 0) return trackCurve(c, r, false);
    Node* n = c.find(r.node);
    if (!n || !n->params.data.contains(r.param)) return nullptr;
    const json& v = n->params.data[r.param];
    const json* cv = r.comp < 0 ? &v : (v.is_array() && r.comp < (int)v.size() ? &v[r.comp] : nullptr);
    return cv && params::isAnimated(*cv) ? cv : nullptr;
}

json* curveJsonMut(Comp& c, const CurveRef& r) {
    if (r.param.rfind("track:", 0) == 0) return const_cast<json*>(trackCurve(c, r, true));
    Node* n = c.find(r.node);
    if (!n || !n->params.data.contains(r.param)) return nullptr;
    json& v = n->params.data[r.param];
    json* cv = r.comp < 0 ? &v : (v.is_array() && r.comp < (int)v.size() ? &v[r.comp] : nullptr);
    return cv && params::isAnimated(*cv) ? cv : nullptr;
}

std::vector<std::string> shownNodes(App& app) {
    std::vector<std::string> v;
    std::set<std::string> seen;
    for (auto& p : app.panels)
        if (app.node(p) && seen.insert(p).second) v.push_back(p);
    for (auto& s : app.selection)
        if (app.node(s) && seen.insert(s).second) v.push_back(s);
    return v;
}

// params that are internal data, not knobs
bool internalParam(const std::string& k) {
    return k == "ml" || k == "tracks" || k == "solve" || k == "shapes" || k == "scene" || k == "keys" || k == "ui";
}

std::vector<CurveRef> curvesOf(App& app, const std::string& node) {
    std::vector<CurveRef> v;
    Node* n = app.node(node);
    if (!n) return v;
    if (n->type == "Tracker" && n->params.has("tracks") && n->params.data["tracks"].is_array())
        for (int i = 0; i < (int)n->params.data["tracks"].size(); ++i)
            if (!track::keysOf(n->params.data["tracks"][i]).empty()) {
                v.push_back({node, "track:" + std::to_string(i) + ":x", -1});
                v.push_back({node, "track:" + std::to_string(i) + ":y", -1});
            }
    for (auto& [k, val] : n->params.data.items()) {
        if (internalParam(k)) continue;
        if (params::isAnimated(val)) v.push_back({node, k, -1});
        else if (val.is_array())
            for (int i = 0; i < (int)val.size() && i < 16; ++i)
                if (params::isAnimated(val[i])) v.push_back({node, k, i});
    }
    return v;
}

std::string curveName(const CurveRef& r) {
    int ti, ta;
    if (trackCurveRef(r, ti, ta)) return std::string(ta ? "track_y" : "track_x") + " " + std::to_string(ti + 1);
    static const char* xyzw[] = {"x", "y", "z", "w"};
    std::string s = r.param;
    if (r.comp >= 0) s += std::string(".") + (r.comp < 4 ? xyzw[r.comp] : std::to_string(r.comp).c_str());
    return s;
}

ImU32 curveColor(const CurveRef& r, int idx) {
    int ti, ta;
    if (trackCurveRef(r, ti, ta)) return ta ? IM_COL32(110, 220, 100, 255) : IM_COL32(235, 90, 80, 255);
    if (r.comp == 0) return IM_COL32(235, 90, 80, 255);
    if (r.comp == 1) return IM_COL32(110, 220, 100, 255);
    if (r.comp == 2) return IM_COL32(100, 150, 255, 255);
    static const ImU32 pal[] = {IM_COL32(230, 200, 90, 255), IM_COL32(200, 130, 230, 255), IM_COL32(90, 210, 210, 255),
                                IM_COL32(240, 150, 90, 255)};
    return pal[idx % 4];
}

int keyIndex(const json& cv, double frame) {
    const json& keys = cv["keys"];
    for (size_t i = 0; i < keys.size(); ++i)
        if (std::fabs(keys[i][0].get<double>() - frame) < 1e-6) return (int)i;
    return -1;
}

void sortKeys(json& cv) {
    auto& keys = cv["keys"];
    std::vector<json> v(keys.begin(), keys.end());
    std::stable_sort(v.begin(), v.end(), [](const json& a, const json& b) { return a[0].get<double>() < b[0].get<double>(); });
    // two keys on one frame: keep the later (moved) one
    std::vector<json> u;
    for (auto& k : v) {
        if (!u.empty() && std::fabs(u.back()[0].get<double>() - k[0].get<double>()) < 1e-6) u.back() = k;
        else u.push_back(k);
    }
    keys = u;
}

json& keyOptsMut(json& key) {
    if (key.size() < 3 || !key[2].is_object()) {
        while (key.size() < 2) key.push_back(0);
        if (key.size() < 3) key.push_back(json::object());
        else key[2] = json::object();
    }
    return key[2];
}

// nice tick spacing for a range spanning `pixels`
double niceStep(double range, double pixels, double minPx) {
    double raw = range * minPx / std::max(1.0, pixels);
    double p = std::pow(10.0, std::floor(std::log10(std::max(raw, 1e-12))));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (p * m >= raw) return p * m;
    return p * 10;
}

void edit(App& app, std::function<void(Comp&)> fn) {
    app.edit([fn](Comp& c) {
        fn(c);
        writeBackTrackCurves(c);
        for (auto& n : c.nodes) n->paramsChanged();
    });
}

bool isDopeTimeNode(const Node& node) {
    return node.type == "AppendClip" || node.type == "FrameRange" || node.type == "Retime" ||
           node.type == "TimeOffset" || node.type == "TimeWarp" || node.type == "TimeClip";
}

std::map<CurveRef, json> snapshotKeys(const Comp& comp, const std::set<KeySel>& selection) {
    std::map<CurveRef, json> curves;
    for (const auto& key : selection)
        if (!curves.count(key.curve))
            if (const auto* curve = curveJson(comp, key.curve)) curves.emplace(key.curve, *curve);
    return curves;
}

// Clamp the whole selection as one block so keys keep their spacing and can't pass unselected keys.
double limitKeyOffset(const std::map<CurveRef, json>& curves, const std::set<KeySel>& selection, double offset) {
    for (const auto& [ref, curve] : curves) {
        std::optional<double> previous;
        bool previousSelected = false;
        for (const auto& key : curve["keys"]) {
            const double frame = key[0].get<double>();
            const bool selected = selection.count({ref, frame}) != 0;
            if (previous && selected != previousSelected) {
                const double limit = std::max(0.0, std::floor(frame - *previous - 1e-6));
                offset = selected ? std::max(offset, -limit) : std::min(offset, limit);
            }
            previous = frame;
            previousSelected = selected;
        }
    }
    return offset;
}

void applyDopeKeyTimes(App& app, DopeSheetState& state, const std::map<CurveRef, json>& curves,
                       const std::set<KeySel>& original, const std::function<double(double)>& time, bool checkpoint) {
    std::set<KeySel> next;
    std::map<CurveRef, json> edited;
    for (const auto& [ref, curve] : curves) {
        auto copy = curve;
        std::optional<double> previous;
        for (auto& key : copy["keys"]) {
            const double frame = key[0].get<double>();
            double target = frame;
            if (original.count({ref, frame})) {
                target = time(frame);
                key[0] = target;
                next.insert({ref, target});
            }
            if (previous && target - *previous <= 1e-6) return;
            previous = target;
        }
        edited.emplace(ref, std::move(copy));
    }
    if (next.size() == state.sel.size() &&
        std::all_of(next.begin(), next.end(), [&](const KeySel& key) { return state.sel.count(key); })) return;
    if (checkpoint) app.checkpoint();
    edit(app, [edited = std::move(edited)](Comp& comp) {
        for (const auto& [ref, curve] : edited)
            if (auto* current = curveJsonMut(comp, ref)) *current = curve;
    });
    state.sel = std::move(next);
}

}  // namespace

// =================================================================== Curve Editor
void drawCurveEditor(App& app, UiState& ui) {
    CurveEditorState& st = ui.curves;
    if (ImGuiWindow* g = ImGui::FindWindowByName("Node Graph"))
        if (g->DockId) ImGui::SetNextWindowDockID(g->DockId, ImGuiCond_FirstUseEver);
    if (!ui.beginPanel("Curve Editor", ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }
    ui.noteHover("Curve Editor");
    if (!app.comp) {
        ImGui::End();
        return;
    }
    Comp& comp = *app.comp;

    // ---- the curves
    std::vector<std::pair<std::string, std::vector<CurveRef>>> groups;
    std::vector<CurveRef> all;
    for (auto& n : shownNodes(app)) {
        auto cs = curvesOf(app, n);
        if (cs.empty()) continue;
        groups.push_back({n, cs});
        all.insert(all.end(), cs.begin(), cs.end());
    }
    // drop selections of keys that no longer exist
    for (auto it = st.sel.begin(); it != st.sel.end();) {
        const json* cv = curveJson(comp, it->curve);
        if (!cv || keyIndex(*cv, it->frame) < 0) it = st.sel.erase(it);
        else ++it;
    }

    // ---- left: the list
    const float listW = std::clamp(ImGui::GetContentRegionAvail().x * 0.24f, 120.0f, 310.0f);
    const float footerH = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##curves", ImVec2(listW, -footerH), ImGuiChildFlags_Borders);
    if (groups.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Open a node's properties (or select it) to see its animation here. Set keys with the key "
                            "buttons next to the knobs.");
        ImGui::PopTextWrapPos();
    }
    int ci = 0;
    std::map<CurveRef, ImU32> colors;
    for (auto& [node, cs] : groups) {
        const bool expanded = ImGui::TreeNodeEx(node.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
        for (auto& c : cs) {
            ImU32 col = curveColor(c, ci++);
            colors[c] = col;
            if (!expanded) continue;
            bool vis = !st.hidden.count(c);
            ImGui::PushID((c.node + "/" + curveName(c)).c_str());
            ImGui::PushStyleColor(ImGuiCol_CheckMark, col);
            if (ImGui::Checkbox(curveName(c).c_str(), &vis)) {
                if (vis) st.hidden.erase(c);
                else st.hidden.insert(c);
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (expanded) ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- right: the graph
    ImGui::BeginChild("##graph", ImVec2(0, -footerH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImGui::GetContentRegionAvail();
    sz.x = std::max(sz.x, 50.0f);
    sz.y = std::max(sz.y, 50.0f);
    ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
    ImGui::InvisibleButton("##canvas", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                                               ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rulerH = 18;
    const float graphBottom = p1.y - rulerH;

    // fit
    auto visibleCurves = [&] {
        std::vector<CurveRef> v;
        for (auto& c : all)
            if (!st.hidden.count(c)) v.push_back(c);
        return v;
    };
    {
        static std::string lastSig;
        std::string sig;
        for (auto& c : all) sig += c.node + "/" + curveName(c) + ";";
        if (sig != lastSig) {
            lastSig = sig;
            st.fitPending = true;
        }
    }
    if ((st.fitPending || (hovered && ImGui::IsKeyPressed(ImGuiKey_F) && !ImGui::GetIO().WantTextInput))) {
        const bool pressedF = !st.fitPending;
        st.fitPending = false;
        double fa = 1e300, fb = -1e300, va = 1e300, vb = -1e300;
        bool onlySel = !st.sel.empty() && pressedF;
        for (auto& c : visibleCurves()) {
            const json* cv = curveJson(comp, c);
            if (!cv) continue;
            for (auto& k : (*cv)["keys"]) {
                double f = k[0].get<double>(), v = k[1].get<double>();
                if (onlySel && !st.sel.count({c, f})) continue;
                fa = std::min(fa, f);
                fb = std::max(fb, f);
                va = std::min(va, v);
                vb = std::max(vb, v);
            }
            // the curve between keys can overshoot
            if (!onlySel && !(*cv)["keys"].empty())
                for (int i = 0; i <= 100; ++i) {
                    double f = fa + (fb - fa) * i / 100.0, v = params::evalCurve(*cv, f);
                    va = std::min(va, v);
                    vb = std::max(vb, v);
                }
        }
        if (fa > fb) {
            fa = comp.firstFrame;
            fb = comp.lastFrame;
            va = 0;
            vb = 1;
        }
        if (fb - fa < 1) {
            fa -= 5;
            fb += 5;
        }
        if (vb - va < 1e-6) {
            va -= 0.5;
            vb += 0.5;
        }
        double fm = (fb - fa) * 0.06, vm = (vb - va) * 0.12;
        st.f0 = fa - fm;
        st.f1 = fb + fm;
        st.v0 = va - vm;
        st.v1 = vb + vm;
    }
    if (ui.dope.linkRange) {
        st.f0 = ui.dope.f0;
        st.f1 = ui.dope.f1;
    }
    auto X = [&](double f) { return (float)(p0.x + (f - st.f0) / (st.f1 - st.f0) * sz.x); };
    auto Y = [&](double v) { return (float)(graphBottom - (v - st.v0) / (st.v1 - st.v0) * (sz.y - rulerH)); };
    auto F = [&](float x) { return st.f0 + (x - p0.x) / sz.x * (st.f1 - st.f0); };
    auto V = [&](float y) { return st.v0 + (graphBottom - y) / (sz.y - rulerH) * (st.v1 - st.v0); };

    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(p0, p1, IM_COL32(21, 24, 27, 255));
    dl->AddRectFilled(ImVec2(p0.x, graphBottom), p1, IM_COL32(34, 37, 41, 255));
    // grid
    double fs = niceStep(st.f1 - st.f0, sz.x, 70), vs = niceStep(st.v1 - st.v0, sz.y, 40);
    const double minor = fs / 5;
    for (double f = std::ceil(st.f0 / minor) * minor; f <= st.f1; f += minor)
        dl->AddLine(ImVec2(X(f), p0.y), ImVec2(X(f), graphBottom), IM_COL32(31, 37, 43, 255));
    for (double f = std::ceil(st.f0 / fs) * fs; f <= st.f1; f += fs) {
        dl->AddLine(ImVec2(X(f), p0.y), ImVec2(X(f), graphBottom), IM_COL32(57, 69, 81, 255));
        char b[32];
        std::snprintf(b, sizeof(b), "%g", f);
        dl->AddText(ImVec2(X(f) + 3, graphBottom + 2), IM_COL32(165, 177, 189, 255), b);
    }
    for (double v = std::ceil(st.v0 / vs) * vs; v <= st.v1; v += vs) {
        dl->AddLine(ImVec2(p0.x, Y(v)), ImVec2(p1.x, Y(v)), std::fabs(v) < vs * 1e-6 ? IM_COL32(103, 132, 157, 255) : IM_COL32(57, 69, 81, 255));
        char b[32];
        std::snprintf(b, sizeof(b), "%g", std::fabs(v) < vs * 1e-6 ? 0.0 : v);
        dl->AddText(ImVec2(p0.x + 3, Y(v) - 14), IM_COL32(120, 120, 120, 255), b);
    }
    // ruler (click / drag to change frame)
    const float playhead = X(app.frame);
    const ImU32 playheadColor = IM_COL32(245, 159, 49, 255);
    dl->AddLine(ImVec2(playhead, p0.y), ImVec2(playhead, p1.y), playheadColor);
    dl->AddTriangleFilled(ImVec2(playhead, graphBottom), ImVec2(playhead - 4, graphBottom + 7),
                          ImVec2(playhead + 4, graphBottom + 7), playheadColor);

    // curves and keys
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    struct Hit {
        KeySel key;
        int handle = -1;  // -1 key, 0 in, 1 out
        float d = 1e9;
    } hit;
    auto handlePos = [&](const json& cv, size_t i, int side, ImVec2 kp) {
        double slope = params::keySlope(cv, i, side);
        // screen direction of (1 frame, slope)
        double dx = (double)sz.x / (st.f1 - st.f0), dy = -(double)(sz.y - rulerH) / (st.v1 - st.v0) * slope;
        double len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-12) return kp;
        double s = (side ? 45.0 : -45.0) / len;
        return ImVec2((float)(kp.x + dx * s), (float)(kp.y + dy * s));
    };
    dl->PushClipRect(p0, ImVec2(p1.x, graphBottom), true);
    for (auto& c : visibleCurves()) {
        const json* cvp = curveJson(comp, c);
        if (!cvp) continue;
        const json& cv = *cvp;
        ImU32 col = colors.count(c) ? colors[c] : IM_COL32_WHITE;
        // the curve
        std::vector<ImVec2> pts;
        for (float x = p0.x; x <= p1.x; x += 2) pts.push_back(ImVec2(x, Y(params::evalCurve(cv, F(x)))));
        dl->AddPolyline(pts.data(), (int)pts.size(), (col & 0x00FFFFFF) | 0xC0000000, 0, 1.5f);
        const json& keys = cv["keys"];
        for (size_t i = 0; i < keys.size(); ++i) {
            double f = keys[i][0].get<double>(), v = keys[i][1].get<double>();
            ImVec2 kp(X(f), Y(v));
            KeySel ks{c, f};
            bool sel = st.sel.count(ks) > 0;
            if (sel) {
                std::string in = i > 0 ? params::keyInterp(cv, i - 1) : "", out = params::keyInterp(cv, i);
                for (int side : {0, 1}) {
                    std::string ip = side ? out : in;
                    if (ip.empty() || ip == "linear" || ip == "constant") continue;
                    if ((side == 0 && i == 0) || (side == 1 && i + 1 == keys.size())) continue;
                    ImVec2 hp = handlePos(cv, i, side, kp);
                    dl->AddLine(kp, hp, IM_COL32(200, 200, 200, 180));
                    dl->AddCircleFilled(hp, 3.5f, IM_COL32(230, 230, 230, 255));
                    float d = std::hypot(mouse.x - hp.x, mouse.y - hp.y);
                    if (d < 7 && d < hit.d) hit = {ks, side, d};
                }
            }
            dl->AddRectFilled(ImVec2(kp.x - 3.5f, kp.y - 3.5f), ImVec2(kp.x + 3.5f, kp.y + 3.5f),
                              sel ? IM_COL32(255, 230, 120, 255) : col);
            if (sel) dl->AddRect(ImVec2(kp.x - 4.5f, kp.y - 4.5f), ImVec2(kp.x + 4.5f, kp.y + 4.5f), IM_COL32(0, 0, 0, 255));
            float d = std::hypot(mouse.x - kp.x, mouse.y - kp.y);
            if (d < 7 && d < hit.d - 0.5f) hit = {ks, -1, d};
        }
    }

    // ---- interaction
    ImGuiIO& io = ImGui::GetIO();
    const bool inRuler = mouse.y >= graphBottom;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        if (inRuler) {
            st.drag = 5;  // scrub
        } else if (io.KeyCtrl && io.KeyAlt) {
        } else if (hit.d < 1e8 && hit.handle >= 0) {
            app.checkpoint();
            st.drag = 2;
            st.dragHandleKey = hit.key;
            st.dragHandleSide = hit.handle;
        } else if (hit.d < 1e8) {
            if (io.KeyShift) {
                if (st.sel.count(hit.key)) st.sel.erase(hit.key);
                else st.sel.insert(hit.key);
            } else if (!st.sel.count(hit.key)) {
                st.sel = {hit.key};
            }
            if (st.sel.count(hit.key)) {
                app.checkpoint();
                st.drag = 1;
                st.dragStart = mouse;
                st.dragOrig.clear();
                for (auto& k : st.sel)
                    if (const json* cv = curveJson(comp, k.curve)) {
                        int i = keyIndex(*cv, k.frame);
                        if (i >= 0) st.dragOrig[k] = {k.frame, (*cv)["keys"][i][1].get<double>()};
                    }
            }
        } else {
            st.drag = 3;
            st.dragStart = mouse;
            if (!io.KeyShift) st.sel.clear();
        }
    }
    // Ctrl+Alt+click on a curve: add a key there
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyCtrl && io.KeyAlt && !inRuler) {
        double f = F(mouse.x);
        if (st.snap) f = std::round(f);
        CurveRef best;
        float bd = 12;
        for (auto& c : visibleCurves())
            if (const json* cv = curveJson(comp, c)) {
                float d = std::fabs(Y(params::evalCurve(*cv, f)) - mouse.y);
                if (d < bd) {
                    bd = d;
                    best = c;
                }
            }
        if (!best.node.empty()) {
            app.checkpoint();
            edit(app, [best, f](Comp& c) {
                if (json* cv = curveJsonMut(c, best)) {
                    double v = params::evalCurve(*cv, f);
                    (*cv)["keys"].push_back({f, v});
                    sortKeys(*cv);
                }
            });
            st.sel = {{best, f}};
        }
    }
    // pan: middle drag or Alt+left drag; zoom: wheel (Ctrl: time only, Shift: values only)
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyAlt && !io.KeyCtrl))) {
        st.drag = 4;
        st.dragStart = mouse;
    }
    if (hovered && io.MouseWheel != 0) {
        double z = std::pow(0.85, io.MouseWheel);
        double fm = F(mouse.x), vm = V(mouse.y);
        if (!io.KeyShift) {
            st.f0 = fm + (st.f0 - fm) * z;
            st.f1 = fm + (st.f1 - fm) * z;
        }
        if (!io.KeyCtrl) {
            st.v0 = vm + (st.v0 - vm) * z;
            st.v1 = vm + (st.v1 - vm) * z;
        }
    }
    if (st.drag == 5) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            app.frame = std::round(std::clamp(F(mouse.x), -1e6, 1e6));
            app.requestView();
        } else st.drag = 0;
    }
    if (st.drag == 4) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImVec2 d = io.MouseDelta;
            double df = -d.x / sz.x * (st.f1 - st.f0), dv = d.y / (sz.y - rulerH) * (st.v1 - st.v0);
            st.f0 += df;
            st.f1 += df;
            st.v0 += dv;
            st.v1 += dv;
        } else st.drag = 0;
    }
    if (st.drag == 1) {
        // every moved key goes from its position at the drag start (a snapshot of its curve)
        static std::map<CurveRef, json> snap;
        if (snap.empty())
            for (auto& [k, fv] : st.dragOrig)
                if (!snap.count(k.curve))
                    if (const json* cv = curveJson(comp, k.curve)) snap[k.curve] = *cv;
        double df = (mouse.x - st.dragStart.x) / sz.x * (st.f1 - st.f0);
        double dv = -(mouse.y - st.dragStart.y) / (sz.y - rulerH) * (st.v1 - st.v0);
        if (io.KeyCtrl) {  // constrain to the main direction
            if (std::fabs(mouse.x - st.dragStart.x) > std::fabs(mouse.y - st.dragStart.y)) dv = 0;
            else df = 0;
        }
        if (st.snap) df = std::round(df);
        if (mouse.x != st.dragStart.x || mouse.y != st.dragStart.y) {
            auto orig = st.dragOrig;
            auto snapCopy = snap;
            edit(app, [snapCopy, orig, df, dv](Comp& c) {
                for (auto& [cr, base] : snapCopy) {
                    json* cv = curveJsonMut(c, cr);
                    if (!cv) continue;
                    json nv = base;
                    for (auto& k : nv["keys"]) {
                        double f = k[0].get<double>();
                        if (orig.count({cr, f})) {
                            k[0] = f + df;
                            k[1] = k[1].get<double>() + dv;
                        }
                    }
                    sortKeys(nv);
                    *cv = nv;
                }
            });
            std::set<KeySel> newSel;
            for (auto& [k, fv] : orig) newSel.insert({k.curve, fv.first + df});
            st.sel = newSel;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            snap.clear();
            st.drag = 0;
            st.dragOrig.clear();
        }
    }
    if (st.drag == 2) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const json* cv = curveJson(comp, st.dragHandleKey.curve);
            int i = cv ? keyIndex(*cv, st.dragHandleKey.frame) : -1;
            if (i >= 0) {
                double kf = (*cv)["keys"][i][0].get<double>(), kv = (*cv)["keys"][i][1].get<double>();
                double mf = F(mouse.x), mv = V(mouse.y);
                double dfr = mf - kf;
                if (st.dragHandleSide == 0) dfr = -dfr;
                double slope = std::fabs(mf - kf) < 1e-9 ? 0 : (mv - kv) / (mf - kf);
                CurveRef cr = st.dragHandleKey.curve;
                double frame = kf;
                int side = st.dragHandleSide;
                bool breakT = io.KeyAlt || io.KeyCtrl;
                edit(app, [cr, frame, slope, side, breakT](Comp& c) {
                    json* cv = curveJsonMut(c, cr);
                    if (!cv) return;
                    int k = keyIndex(*cv, frame);
                    if (k < 0) return;
                    json& key = (*cv)["keys"][k];
                    json& o = keyOptsMut(key);
                    bool broken = o.contains("l") && o.contains("r") && std::fabs(o["l"].get<double>() - o["r"].get<double>()) > 1e-12;
                    if (breakT || broken) {
                        if (!o.contains("r")) o["r"] = params::keySlope(*cv, k, 1);
                        if (!o.contains("l")) o["l"] = params::keySlope(*cv, k, 0);
                        o[side ? "r" : "l"] = slope;
                    } else {
                        o["r"] = slope;
                        o.erase("l");
                    }
                    o["i"] = "user";
                    // the segment coming in uses the in slope too
                    if (k > 0) {
                        json& prev = (*cv)["keys"][k - 1];
                        std::string pi = params::keyInterp(*cv, k - 1);
                        if (pi == "linear" || pi == "constant") {
                        } else keyOptsMut(prev)["i"] = pi == "user" ? "user" : "smooth";
                    }
                });
            }
        } else st.drag = 0;
    }
    if (st.drag == 3) {
        ImVec2 a(std::min(st.dragStart.x, mouse.x), std::min(st.dragStart.y, mouse.y));
        ImVec2 b(std::max(st.dragStart.x, mouse.x), std::max(st.dragStart.y, mouse.y));
        dl->AddRectFilled(a, b, IM_COL32(255, 200, 80, 30));
        dl->AddRect(a, b, IM_COL32(255, 200, 80, 160));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            for (auto& c : visibleCurves())
                if (const json* cv = curveJson(comp, c))
                    for (auto& k : (*cv)["keys"]) {
                        ImVec2 kp(X(k[0].get<double>()), Y(k[1].get<double>()));
                        if (kp.x >= a.x && kp.x <= b.x && kp.y >= a.y && kp.y <= b.y) st.sel.insert({c, k[0].get<double>()});
                    }
            st.drag = 0;
        }
    }
    // keys: Delete; context menu
    auto setInterp = [&](const std::string& mode) {
        auto sel = st.sel;
        app.checkpoint();
        edit(app, [sel, mode](Comp& c) {
            for (auto& k : sel) {
                json* cv = curveJsonMut(c, k.curve);
                if (!cv) continue;
                int i = keyIndex(*cv, k.frame);
                if (i < 0) continue;
                json& o = keyOptsMut((*cv)["keys"][i]);
                if (mode == "horizontal") {
                    o["i"] = "user";
                    o["r"] = 0.0;
                    o.erase("l");
                } else if (mode == "break") {
                    o["r"] = params::keySlope(*cv, i, 1);
                    o["l"] = params::keySlope(*cv, i, 0);
                    o["i"] = "user";
                } else if (mode == "unify") {
                    double s = 0.5 * (params::keySlope(*cv, i, 1) + params::keySlope(*cv, i, 0));
                    o["r"] = s;
                    o.erase("l");
                } else {
                    o["i"] = mode;
                    o.erase("l");
                    o.erase("r");
                }
                if (o.empty()) (*cv)["keys"][i].erase(2);
            }
        });
    };
    auto deleteSel = [&] {
        auto sel = st.sel;
        app.checkpoint();
        edit(app, [sel](Comp& c) {
            for (auto& k : sel) {
                Node* n = c.find(k.curve.node);
                json* cv = curveJsonMut(c, k.curve);
                if (!n || !cv) continue;
                int i = keyIndex(*cv, k.frame);
                if (i < 0) continue;
                double v = (*cv)["keys"][i][1].get<double>();
                (*cv)["keys"].erase(i);
                if ((*cv)["keys"].empty()) *cv = v;  // last key gone: a constant again
            }
        });
        st.sel.clear();
    };
    if (hovered && !st.sel.empty() && !io.WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)))
        deleteSel();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hit.d < 1e8 && !st.sel.count(hit.key)) st.sel = {hit.key};
        ImGui::OpenPopup("##curvemenu");
    }
    dl->PopClipRect();
    dl->PopClipRect();
    if (ImGui::BeginPopup("##curvemenu")) {
        ImGui::BeginDisabled(st.sel.empty());
        ImGui::TextDisabled("interpolation");
        for (const char* m : {"smooth", "linear", "constant", "horizontal"})
            if (ImGui::MenuItem(m)) setInterp(m);
        ImGui::Separator();
        if (ImGui::MenuItem("break tangents")) setInterp("break");
        if (ImGui::MenuItem("unify tangents")) setInterp("unify");
        ImGui::Separator();
        if (ImGui::MenuItem("delete keys", "Del")) deleteSel();
        ImGui::EndDisabled();
        if (ImGui::MenuItem("frame all", "F")) st.fitPending = true;
        ImGui::EndPopup();
    }
    if (hovered && hit.d < 1e8 && st.drag == 0) {
        const json* cv = curveJson(comp, hit.key.curve);
        int i = cv ? keyIndex(*cv, hit.key.frame) : -1;
        if (i >= 0)
            ImGui::SetTooltip("%s %s\nframe %g  value %.5g  (%s)", hit.key.curve.node.c_str(), curveName(hit.key.curve).c_str(),
                              hit.key.frame, (*cv)["keys"][i][1].get<double>(), params::keyInterp(*cv, i).c_str());
    }
    if (ui.dope.linkRange) {
        ui.dope.f0 = st.f0;
        ui.dope.f1 = st.f1;
    }
    ImGui::EndChild();
    ImGui::Checkbox("Snap frames", &st.snap);
    ImGui::SameLine();
    if (ImGui::Button("Frame all")) st.fitPending = true;
    ImGui::SetItemTooltip("Frame all visible curves (F)");
    ImGui::SameLine();
    ImGui::Checkbox("Link range", &ui.dope.linkRange);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu key%s selected", st.sel.size(), st.sel.size() == 1 ? "" : "s");
    ImGui::End();
}

// =================================================================== Dope Sheet
void drawDopeSheet(App& app, UiState& ui) {
    DopeSheetState& st = ui.dope;
    if (ImGuiWindow* g = ImGui::FindWindowByName("Node Graph"))
        if (g->DockId) ImGui::SetNextWindowDockID(g->DockId, ImGuiCond_FirstUseEver);
    if (!ui.beginPanel("Dope Sheet", ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }
    ui.noteHover("Dope Sheet");
    if (!app.comp) {
        ImGui::End();
        return;
    }
    Comp& comp = *app.comp;
    if (st.fitPending) {
        st.fitPending = false;
        st.f0 = comp.firstFrame - 2;
        st.f1 = comp.lastFrame + 2;
    }
    struct Row {
        std::string node, param, label;
        int depth = 0;
        std::vector<CurveRef> curves;
        std::set<double> frames;
        std::optional<std::pair<double, double>> span;
        bool timeNode = false;
        bool expandable = false;
    };
    std::vector<Row> rows;
    auto names = shownNodes(app);
    if (st.showAllClips) {
        std::set<std::string> listed(names.begin(), names.end());
        for (const auto& node : comp.nodes)
            if ((node->type == "Read" || node->type == "TimeClip") && listed.insert(node->name).second)
                names.push_back(node->name);
    }
    const std::set<std::string> openPanels(app.panels.begin(), app.panels.end());
    auto makeRow = [&](const std::string& node, const std::string& param, const std::string& label,
                       int depth, const std::vector<CurveRef>& curves) {
        Row row;
        row.node = node;
        row.param = param;
        row.label = label;
        row.depth = depth;
        row.curves = curves;
        for (const auto& ref : curves)
            if (const auto* curve = curveJson(comp, ref))
                for (const auto& key : (*curve)["keys"]) row.frames.insert(key[0].get<double>());
        return row;
    };
    std::map<std::string, bool> showSpans;
    std::map<std::string, std::vector<CurveRef>> nodeCurves;
    for (const auto& name : names) {
        Node* node = app.node(name);
        const bool showSpan = ((node->type == "Read" || isDopeTimeNode(*node)) && openPanels.count(name)) ||
                              (st.showAllClips && (node->type == "Read" || node->type == "TimeClip"));
        auto curves = curvesOf(app, name);
        if (curves.empty() && !showSpan) continue;
        showSpans.emplace(name, showSpan);
        nodeCurves.emplace(name, std::move(curves));
    }
    std::map<std::string, std::string> parents;
    std::map<std::string, std::vector<std::string>> children;
    for (const auto& name : names) {
        Node* node = app.node(name);
        if (!showSpans.count(name) || !isDopeTimeNode(*node)) continue;
        for (const Node* input : node->inputs) {
            if (!input || !showSpans.count(input->name) || parents.count(input->name)) continue;
            std::string ancestor = name;
            while (ancestor != input->name && parents.count(ancestor)) ancestor = parents.at(ancestor);
            if (ancestor == input->name) continue;
            parents.emplace(input->name, name);
            children[name].push_back(input->name);
        }
    }
    std::function<void(const std::string&, int)> appendNode = [&](const std::string& name, int depth) {
        Node* node = app.node(name);
        const auto& curves = nodeCurves.at(name);
        std::string label = name;
        if (node->type == "Read") {
            const auto file = node->params.str("file", "");
            if (!file.empty()) label += "  " + file.substr(file.find_last_of("/\\") + 1);
        }
        Row nodeRow = makeRow(name, "", label, depth, curves);
        nodeRow.timeNode = isDopeTimeNode(*node);
        nodeRow.expandable = !curves.empty() || children.count(name);
        if (showSpans.at(name)) {
            nodeRow.span = nodeFrameRange(node);
            if (!nodeRow.span) nodeRow.span = {comp.firstFrame, comp.lastFrame};
        }
        rows.push_back(std::move(nodeRow));
        if (st.collapsed.count({name, ""})) return;
        std::map<std::string, std::vector<CurveRef>> controls;
        for (const auto& curve : curves) controls[curve.param].push_back(curve);
        for (const auto& [param, channels] : controls) {
            const bool vectorControl = channels.front().comp >= 0;
            Row control = makeRow(name, param, vectorControl ? param : curveName(channels.front()), depth + 1, channels);
            control.expandable = vectorControl;
            rows.push_back(std::move(control));
            if (vectorControl && !st.collapsed.count({name, param}))
                for (const auto& channel : channels)
                    rows.push_back(makeRow(name, param, curveName(channel), depth + 2, {channel}));
        }
        if (auto nested = children.find(name); nested != children.end())
            for (const auto& child : nested->second) appendNode(child, depth + 1);
    };
    for (const auto& name : names)
        if (showSpans.count(name) && !parents.count(name)) appendNode(name, 0);
    for (auto it = st.sel.begin(); it != st.sel.end();) {
        const json* cv = curveJson(comp, it->curve);
        if (!app.editsPending() && (!cv || keyIndex(*cv, it->frame) < 0)) it = st.sel.erase(it);
        else ++it;
    }
    const float rowH = ImGui::GetTextLineHeight() + 6, rulerH = 18;
    const float footerH = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##dopeViewport", ImVec2(0, -footerH), 0, ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImGui::GetContentRegionAvail();
    sz.x = std::max(1.0f, sz.x);
    const float nameW = std::min(190.0f, sz.x * 0.40f);
    sz.y = std::max(sz.y, rulerH + rowH * std::max<size_t>(1, rows.size()) + 4);
    ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
    const float gx0 = p0.x + nameW, gx1 = p1.x;
    ImGui::InvisibleButton("##dope", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                             ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto X = [&](double f) { return (float)(gx0 + (f - st.f0) / (st.f1 - st.f0) * (gx1 - gx0)); };
    auto F = [&](float x) { return st.f0 + (x - gx0) / (gx1 - gx0) * (st.f1 - st.f0); };
    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(p0, p1, IM_COL32(32, 32, 32, 255));
    dl->AddRectFilled(ImVec2(gx0, p0.y), ImVec2(gx1, p0.y + rulerH), IM_COL32(40, 40, 40, 255));
    double fs = niceStep(st.f1 - st.f0, gx1 - gx0, 50);
    for (double f = std::ceil(st.f0 / fs) * fs; f <= st.f1; f += fs) {
        dl->AddLine(ImVec2(X(f), p0.y + rulerH), ImVec2(X(f), p1.y), IM_COL32(46, 46, 46, 255));
        char b[32];
        std::snprintf(b, sizeof(b), "%g", f);
        dl->AddText(ImVec2(X(f) + 3, p0.y + 2), IM_COL32(140, 140, 140, 255), b);
    }
    dl->AddRectFilled(ImVec2(X(comp.firstFrame), p0.y + rulerH), ImVec2(X(comp.lastFrame), p1.y), IM_COL32(255, 255, 255, 8));
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    struct Hit {
        int row = -1;
        double frame = 0;
        float d = 1e9;
    } hit;
    int hoverRow = -1;
    for (size_t r = 0; r < rows.size(); ++r) {
        float y = p0.y + rulerH + 2 + r * rowH;
        const Row& row = rows[r];
        const bool nodeRow = row.param.empty();
        if (nodeRow) dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p1.x, y + rowH), IM_COL32(44, 44, 48, 255));
        if (mouse.y >= y && mouse.y < y + rowH) hoverRow = (int)r;
        const float indent = p0.x + 6 + row.depth * 14.0f;
        dl->PushClipRect(p0, ImVec2(gx0 - 2, p1.y), true);
        if (row.expandable)
            ImGui::RenderArrow(dl, ImVec2(indent, y + 4), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                               st.collapsed.count({row.node, row.param}) ? ImGuiDir_Right : ImGuiDir_Down, 0.65f);
        dl->AddText(ImVec2(indent + 14, y + 3), ImGui::GetColorU32(nodeRow ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                    row.label.c_str());
        dl->PopClipRect();
        dl->PushClipRect(ImVec2(gx0, p0.y), p1, true);
        if (row.span) {
            const auto [first, last] = *row.span;
            const float left = X(std::min(first, last));
            const float right = std::max(left + 3, X(std::max(first, last)));
            const auto fill = row.timeNode ? IM_COL32(55, 91, 72, 255) : IM_COL32(87, 91, 101, 255);
            dl->AddRectFilled(ImVec2(left, y + 3), ImVec2(right, y + rowH - 3), fill, 3.0f);
        }
        for (double f : row.frames) {
            const float x = X(f), cy = y + rowH * 0.5f;
            const float halfHeight = nodeRow ? 6.0f : 5.0f;
            bool sel = true;
            for (auto& c : row.curves)
                if (const json* cv = curveJson(comp, c))
                    if (keyIndex(*cv, f) >= 0 && !st.sel.count({c, f})) sel = false;
            const ImU32 color = sel ? IM_COL32(255, 174, 66, 255) : IM_COL32(174, 177, 184, 255);
            const float tick = std::floor(x);
            dl->AddRectFilled(ImVec2(tick - 1, cy - halfHeight), ImVec2(tick + 2, cy + halfHeight), color);
            float d = std::fabs(mouse.x - x) + std::fabs(mouse.y - cy) * 0.5f;
            if (mouse.x >= gx0 && x >= gx0 && x <= gx1 && d < 8 && d < hit.d) hit = {(int)r, f, d};
        }
        dl->PopClipRect();
    }
    if (rows.empty())
        dl->AddText(ImVec2(p0.x + 8, p0.y + rulerH + 6), IM_COL32(140, 140, 140, 255),
                    "Open a node's properties (or select it) to see its keys here.");
    dl->PushClipRect(ImVec2(gx0, p0.y), p1, true);
    for (double frame : {double(comp.firstFrame), double(comp.lastFrame)})
        dl->AddLine(ImVec2(X(frame), p0.y), ImVec2(X(frame), p1.y), IM_COL32(145, 149, 157, 210));
    dl->AddLine(ImVec2(X(app.frame), p0.y), ImVec2(X(app.frame), p1.y), IM_COL32(245, 159, 49, 255));
    dl->PopClipRect();
    dl->AddLine(ImVec2(gx0, p0.y), ImVec2(gx0, p1.y), IM_COL32(60, 60, 60, 255));

    // keys of a row at a frame
    auto rowKeys = [&](int r, double f) {
        std::vector<KeySel> v;
        for (auto& c : rows[r].curves)
            if (const json* cv = curveJson(comp, c))
                if (keyIndex(*cv, f) >= 0) v.push_back({c, f});
        return v;
    };
    // selection bounds (for the scale handles)
    double sa = 1e300, sb = -1e300;
    for (auto& k : st.sel) {
        sa = std::min(sa, k.frame);
        sb = std::max(sb, k.frame);
    }
    bool scalable = st.sel.size() > 1 && sb > sa;
    if (scalable) {
        float xa = X(sa) - 8, xb = X(sb) + 8, y = p0.y + rulerH;
        dl->AddRectFilled(ImVec2(xa - 3, y), ImVec2(xa, p1.y), IM_COL32(255, 200, 80, 120));
        dl->AddRectFilled(ImVec2(xb, y), ImVec2(xb + 3, p1.y), IM_COL32(255, 200, 80, 120));
    }
    ImGuiIO& io = ImGui::GetIO();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (mouse.x >= gx0 && mouse.y < p0.y + rulerH) {
            st.drag = 4;  // scrub
        } else if (mouse.x < gx0) {
            if (hoverRow >= 0) {
                const Row& row = rows[hoverRow];
                const bool arrow = mouse.x < p0.x + 20 + row.depth * 14.0f;
                if (arrow && row.expandable) {
                    if (!st.collapsed.erase({row.node, row.param})) st.collapsed.insert({row.node, row.param});
                } else if (row.param.empty()) {
                    ui.closedPanels.erase("Properties");
                    app.openPanel(row.node);
                }
            }
        } else if (scalable && (std::fabs(mouse.x - (X(sa) - 9.5f)) < 4 || std::fabs(mouse.x - (X(sb) + 9.5f)) < 4)) {
            app.checkpoint();
            st.drag = 3;
            st.scaleEnd = std::fabs(mouse.x - (X(sb) + 9.5f)) < 4 ? 1 : 0;
            st.dragStart = mouse;
            st.dragOrig = st.sel;
            st.dragCurves = snapshotKeys(comp, st.sel);
        } else if (hit.row >= 0) {
            auto ks = rowKeys(hit.row, hit.frame);
            bool allSel = !ks.empty() && std::all_of(ks.begin(), ks.end(), [&](const KeySel& k) { return st.sel.count(k) > 0; });
            if (io.KeyShift) {
                for (auto& k : ks) {
                    if (allSel) st.sel.erase(k);
                    else st.sel.insert(k);
                }
            } else if (!allSel) {
                st.sel.clear();
                st.sel.insert(ks.begin(), ks.end());
            }
            app.checkpoint();
            st.drag = 1;
            st.dragStart = mouse;
            st.dragOrig = st.sel;
            st.dragCurves = snapshotKeys(comp, st.sel);
        } else {
            st.drag = 2;
            st.dragStart = mouse;
            if (!io.KeyShift) st.sel.clear();
        }
    }
    if (hovered && io.MouseWheel != 0 && (mouse.x < gx0 || io.KeyShift)) {
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseWheel * rowH * 3);
    } else if (hovered && io.MouseWheel != 0) {
        double z = std::pow(0.85, io.MouseWheel), fm = F(mouse.x);
        z = std::clamp(z, 1.0 / (st.f1 - st.f0), 1e9 / (st.f1 - st.f0));
        st.f0 = fm + (st.f0 - fm) * z;
        st.f1 = fm + (st.f1 - fm) * z;
    }
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        double df = -io.MouseDelta.x / (gx1 - gx0) * (st.f1 - st.f0);
        st.f0 += df;
        st.f1 += df;
    }
    auto applyRetime = [&](const std::function<double(double)>& map) {
        applyDopeKeyTimes(app, st, st.dragCurves, st.dragOrig, map, false);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            st.dragCurves.clear();
            st.dragOrig.clear();
            st.drag = 0;
        }
    };
    if (st.drag == 1) {
        double df = std::round((mouse.x - st.dragStart.x) / (gx1 - gx0) * (st.f1 - st.f0));
        df = limitKeyOffset(st.dragCurves, st.dragOrig, df);
        applyRetime([df](double f) { return f + df; });
    } else if (st.drag == 3) {
        double a0 = 1e300, b0 = -1e300;
        for (const auto& key : st.dragOrig) {
            a0 = std::min(a0, key.frame);
            b0 = std::max(b0, key.frame);
        }
        double target = F(mouse.x);
        int end = st.scaleEnd;
        double s = end ? (target - a0) / std::max(1e-9, b0 - a0) : (b0 - target) / std::max(1e-9, b0 - a0);
        s = std::max(0.05, s);
        applyRetime([a0, b0, s, end](double f) { return std::round(end ? a0 + (f - a0) * s : b0 - (b0 - f) * s); });
    } else if (st.drag == 2) {
        ImVec2 a(std::min(st.dragStart.x, mouse.x), std::min(st.dragStart.y, mouse.y));
        ImVec2 b(std::max(st.dragStart.x, mouse.x), std::max(st.dragStart.y, mouse.y));
        dl->AddRectFilled(a, b, IM_COL32(255, 200, 80, 30));
        dl->AddRect(a, b, IM_COL32(255, 200, 80, 160));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            for (size_t r = 0; r < rows.size(); ++r) {
                float y = p0.y + rulerH + 2 + r * rowH + rowH * 0.5f;
                if (y < a.y || y > b.y) continue;
                for (double f : rows[r].frames)
                    if (X(f) >= a.x && X(f) <= b.x)
                        for (auto& k : rowKeys((int)r, f)) st.sel.insert(k);
            }
            st.drag = 0;
        }
    } else if (st.drag == 4) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            app.frame = std::round(F(mouse.x));
            app.requestView();
        } else st.drag = 0;
    }
    if (hovered && !st.sel.empty() && !io.WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        auto sel = st.sel;
        app.checkpoint();
        edit(app, [sel](Comp& c) {
            for (auto& k : sel) {
                json* cv = curveJsonMut(c, k.curve);
                if (!cv) continue;
                int i = keyIndex(*cv, k.frame);
                if (i < 0) continue;
                double v = (*cv)["keys"][i][1].get<double>();
                (*cv)["keys"].erase(i);
                if ((*cv)["keys"].empty()) *cv = v;
            }
        });
        st.sel.clear();
    }
    if (hovered && ImGui::IsKeyPressed(ImGuiKey_F) && !io.WantTextInput) {
        double a = 1e300, b = -1e300;
        for (auto& r : rows)
            for (double f : r.frames) {
                a = std::min(a, f);
                b = std::max(b, f);
            }
        if (a <= b) {
            double m = std::max(2.0, (b - a) * 0.06);
            st.f0 = a - m;
            st.f1 = b + m;
        }
    }
    if (hovered && hit.row >= 0 && st.drag == 0)
        ImGui::SetTooltip("%s  frame %g", rows[hit.row].param.empty() ? rows[hit.row].node.c_str() : rows[hit.row].param.c_str(), hit.frame);
    dl->PopClipRect();
    ImGui::EndChild();
    ImGui::Checkbox("Link range", &st.linkRange);
    ImGui::SetItemTooltip("Share the visible frame range with the Curve Editor");
    ImGui::SameLine();
    ImGui::Checkbox("All Read / TimeClip", &st.showAllClips);
    ImGui::SameLine();
    ImGui::BeginDisabled(st.sel.empty() || st.drag != 0);
    const bool move = ImGui::Button("Move");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(58);
    const bool enter = ImGui::InputInt("##moveFrames", &st.moveFrames, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetItemTooltip("Frame offset for selected keys. Use a negative number to move earlier.");
    if ((move || enter) && !st.sel.empty() && st.drag == 0) {
        const auto curves = snapshotKeys(comp, st.sel);
        const double offset = limitKeyOffset(curves, st.sel, st.moveFrames);
        applyDopeKeyTimes(app, st, curves, st.sel, [offset](double frame) { return frame + offset; }, true);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Range");
    ImGui::SameLine();
    double first = st.f0, last = st.f1;
    ImGui::SetNextItemWidth(65);
    const bool firstEdited = ImGui::InputDouble("##rangeFirst", &first, 0, 0, "%.3g", ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::TextUnformatted("to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(65);
    const bool lastEdited = ImGui::InputDouble("##rangeLast", &last, 0, 0, "%.3g", ImGuiInputTextFlags_EnterReturnsTrue);
    if ((firstEdited || lastEdited) && std::isfinite(first) && std::isfinite(last) &&
        std::abs(first) <= 1e9 && std::abs(last) <= 1e9 && last - first >= 1 && last - first <= 1e9) {
        st.f0 = first;
        st.f1 = last;
    }
    ImGui::End();
}

}  // namespace strata::ui
