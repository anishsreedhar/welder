// The timeline, laid out the classic way: a frame ruler (a tick per frame, labels,
// the current frame marked above it, in/out, keys, the cache line) with the
// range fields at its ends, and under it the fps / time format / range menus,
// the transport and the range length. It has a fixed height: it never resizes
// vertically, and when it gets narrow the less-used controls drop out rather
// than anything being cut off.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <set>

#include "Panels.h"
#include "imgui_internal.h"
#include "nodes/NodeUtils.h"

namespace strata::ui {

namespace {

#define kAccent (themeAccent())  // the theme's accent
constexpr ImU32 kKey = IM_COL32(90, 170, 255, 255);
constexpr ImU32 kCache = IM_COL32(80, 200, 110, 230);

float rulerHeight() { return std::round(ImGui::GetFontSize() * 2.8f); }
float smallFont() { return std::round(ImGui::GetFontSize() * 0.85f); }

std::string timecode(int frame, double fps) {
    const int n = std::max(1, (int)std::lround(fps));
    const bool neg = frame < 0;
    int f = std::abs(frame);
    char b[32];
    std::snprintf(b, sizeof b, "%s%02d:%02d:%02d:%02d", neg ? "-" : "", f / (n * 3600), (f / (n * 60)) % 60, (f / n) % 60, f % n);
    return b;
}

// "hh:mm:ss:ff" (or fewer fields) or a frame number.
bool parseTime(const char* s, double fps, int& out) {
    int64_t a[4] = {0, 0, 0, 0};
    int k = 0;
    const char* p = s;
    bool neg = false;
    while (*p == ' ') ++p;
    if (*p == '-') { neg = true; ++p; }
    if (!*p) return false;
    int64_t cur = 0;
    bool any = false;
    for (; *p; ++p) {
        if (*p >= '0' && *p <= '9') {
            cur = cur * 10 + (*p - '0');
            if (cur > std::numeric_limits<int>::max()) return false;
            any = true;
        } else if (*p == ':' || *p == ';' || *p == '.') {
            if (k >= 3) return false;
            a[k++] = cur;
            cur = 0;
        } else if (*p != ' ')
            return false;
    }
    if (!any) return false;
    a[k++] = cur;
    const int n = std::max(1, (int)std::lround(fps));
    int64_t v = 0;
    for (int i = 0; i < k; ++i) {  // the last field is frames, then seconds, minutes, hours
        static const int mult[4] = {1, 1, 60, 3600};
        int idx = k - 1 - i;
        v += a[idx] * (i == 0 ? 1 : (int64_t)mult[i] * n);
    }
    if (v > std::numeric_limits<int>::max()) return false;
    out = (int)(neg ? -v : v);
    return true;
}

bool frameEntry(const char* id, int value, bool tc, double fps, int& result) {
    char buf[48];
    const std::string initial = tc ? timecode(value, fps) : std::to_string(value);
    std::snprintf(buf, sizeof buf, "%s", initial.c_str());
    if (!ImGui::InputText(id, buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
        return false;
    const char* p = buf;
    while (*p == ' ') ++p;
    const bool relative = (*p == '+' || *p == '-') && p[1] == '=';
    if (relative) {
        int delta = 0;
        if (!parseTime(p + 2, fps, delta)) return false;
        const int64_t target = (int64_t)value + (*p == '-' ? -(int64_t)delta : delta);
        result = (int)std::clamp<int64_t>(target, -std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
        return true;
    }
    if (*p == '+') ++p;
    return parseTime(p, fps, result);
}

enum class Icon { First, Last, StepBack, StepFwd, PlayBack, PlayFwd, Stop, KeyBack, KeyFwd, IncBack, IncFwd, Repeat, Bounce, Once, Cache, Lock, Unlock };

void tri(ImDrawList* dl, ImVec2 c, float s, bool right, ImU32 col) {
    const float h = s * 0.5f;
    if (right) dl->AddTriangleFilled(ImVec2(c.x - h * 0.8f, c.y - h), ImVec2(c.x - h * 0.8f, c.y + h), ImVec2(c.x + h * 0.9f, c.y), col);
    else dl->AddTriangleFilled(ImVec2(c.x + h * 0.8f, c.y - h), ImVec2(c.x + h * 0.8f, c.y + h), ImVec2(c.x - h * 0.9f, c.y), col);
}

void drawIcon(ImDrawList* dl, const ImRect& r, Icon ic, ImU32 col) {
    const ImVec2 c = r.GetCenter();
    const float s = std::min(r.GetWidth(), r.GetHeight()) * 0.5f;  // icon size
    const float t = std::max(1.5f, s * 0.13f);                     // bar thickness
    auto bar = [&](float x) { dl->AddRectFilled(ImVec2(x - t * 0.5f, c.y - s * 0.5f), ImVec2(x + t * 0.5f, c.y + s * 0.5f), col); };
    auto diamond = [&](ImVec2 p, float d) {
        dl->AddQuadFilled(ImVec2(p.x, p.y - d), ImVec2(p.x + d, p.y), ImVec2(p.x, p.y + d), ImVec2(p.x - d, p.y), col);
    };
    switch (ic) {
        case Icon::PlayFwd: tri(dl, c, s, true, col); break;
        case Icon::PlayBack: tri(dl, c, s, false, col); break;
        case Icon::Stop: dl->AddRectFilled(ImVec2(c.x - s * 0.38f, c.y - s * 0.38f), ImVec2(c.x + s * 0.38f, c.y + s * 0.38f), col); break;
        case Icon::StepFwd:
            tri(dl, ImVec2(c.x - s * 0.12f, c.y), s * 0.8f, true, col);
            bar(c.x + s * 0.42f);
            break;
        case Icon::StepBack:
            tri(dl, ImVec2(c.x + s * 0.12f, c.y), s * 0.8f, false, col);
            bar(c.x - s * 0.42f);
            break;
        case Icon::First:
            bar(c.x - s * 0.55f);
            tri(dl, ImVec2(c.x - s * 0.12f, c.y), s * 0.8f, false, col);
            tri(dl, ImVec2(c.x + s * 0.38f, c.y), s * 0.8f, false, col);
            break;
        case Icon::Last:
            tri(dl, ImVec2(c.x - s * 0.38f, c.y), s * 0.8f, true, col);
            tri(dl, ImVec2(c.x + s * 0.12f, c.y), s * 0.8f, true, col);
            bar(c.x + s * 0.55f);
            break;
        case Icon::KeyFwd:
            tri(dl, ImVec2(c.x - s * 0.2f, c.y), s * 0.8f, true, col);
            diamond(ImVec2(c.x + s * 0.45f, c.y + s * 0.3f), s * 0.2f);
            break;
        case Icon::KeyBack:
            tri(dl, ImVec2(c.x + s * 0.2f, c.y), s * 0.8f, false, col);
            diamond(ImVec2(c.x - s * 0.45f, c.y + s * 0.3f), s * 0.2f);
            break;
        case Icon::IncFwd:
            tri(dl, ImVec2(c.x - s * 0.22f, c.y), s * 0.7f, true, col);
            tri(dl, ImVec2(c.x + s * 0.22f, c.y), s * 0.7f, true, col);
            break;
        case Icon::IncBack:
            tri(dl, ImVec2(c.x - s * 0.22f, c.y), s * 0.7f, false, col);
            tri(dl, ImVec2(c.x + s * 0.22f, c.y), s * 0.7f, false, col);
            break;
        case Icon::Repeat: {  // a circular arrow
            const float rad = s * 0.42f;
            dl->PathArcTo(c, rad, IM_PI * 0.15f, IM_PI * 1.75f, 16);
            dl->PathStroke(col, 0, t);
            ImVec2 e(c.x + rad * std::cos(IM_PI * 0.15f), c.y + rad * std::sin(IM_PI * 0.15f));
            dl->AddTriangleFilled(ImVec2(e.x - s * 0.22f, e.y), ImVec2(e.x + s * 0.22f, e.y), ImVec2(e.x, e.y + s * 0.26f), col);
            break;
        }
        case Icon::Bounce:  // arrows both ways
            dl->AddLine(ImVec2(c.x - s * 0.4f, c.y - s * 0.2f), ImVec2(c.x + s * 0.3f, c.y - s * 0.2f), col, t);
            tri(dl, ImVec2(c.x + s * 0.35f, c.y - s * 0.2f), s * 0.4f, true, col);
            dl->AddLine(ImVec2(c.x + s * 0.4f, c.y + s * 0.25f), ImVec2(c.x - s * 0.3f, c.y + s * 0.25f), col, t);
            tri(dl, ImVec2(c.x - s * 0.35f, c.y + s * 0.25f), s * 0.4f, false, col);
            break;
        case Icon::Once:  // an arrow to a stop
            dl->AddLine(ImVec2(c.x - s * 0.5f, c.y), ImVec2(c.x + s * 0.15f, c.y), col, t);
            tri(dl, ImVec2(c.x + s * 0.18f, c.y), s * 0.5f, true, col);
            bar(c.x + s * 0.52f);
            break;
        case Icon::Cache:  // a stack of frames
            for (int i = 2; i >= 0; --i) {
                ImVec2 o(c.x - s * 0.3f + i * s * 0.18f, c.y - s * 0.3f + i * s * 0.12f);
                dl->AddRectFilled(o, ImVec2(o.x + s * 0.55f, o.y + s * 0.42f), IM_COL32(50, 50, 50, 255));
                dl->AddRect(o, ImVec2(o.x + s * 0.55f, o.y + s * 0.42f), col, 0, 0, std::max(1.0f, t * 0.7f));
            }
            break;
        case Icon::Lock:
        case Icon::Unlock: {
            const float bw = s * 0.62f, bh = s * 0.48f;
            ImVec2 a(c.x - bw * 0.5f, c.y - bh * 0.1f), b(c.x + bw * 0.5f, c.y + bh * 0.9f);
            dl->AddRectFilled(a, b, col, 1.5f);
            const float rad = bw * 0.3f;
            ImVec2 sc(ic == Icon::Lock ? c.x : c.x + bw * 0.42f, a.y);
            dl->PathArcTo(sc, rad, IM_PI, 2 * IM_PI, 10);
            dl->PathStroke(col, 0, t);
            dl->AddLine(ImVec2(sc.x - rad, sc.y), ImVec2(sc.x - rad, a.y + 1), col, t);
            if (ic == Icon::Lock) dl->AddLine(ImVec2(sc.x + rad, sc.y), ImVec2(sc.x + rad, a.y + 1), col, t);
            break;
        }
    }
}

bool iconButton(const char* id, Icon ic, float w, bool on = false, const char* tip = nullptr) {
    const float h = ImGui::GetFrameHeight();
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    bool r = ImGui::Button(id, ImVec2(w, h));
    if (on) ImGui::PopStyleColor();
    ImRect rr(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    drawIcon(ImGui::GetWindowDrawList(), rr, ic, on ? kAccent : ImGui::GetColorU32(ImGuiCol_Text));
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return r;
}

bool textButton(const char* label, float w, bool on = false, const char* tip = nullptr) {
    if (on) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kAccent));
    bool r = ImGui::Button(label, ImVec2(w, ImGui::GetFrameHeight()));
    if (on) ImGui::PopStyleColor();
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return r;
}

// A frame step that labels land on: 1, 2, 5, 10, 20, 50, 100...
int niceStep(double minFrames) {
    for (int m = 1;; m *= 10) {
        for (int k : {1, 2, 5})
            if (k * m >= minFrames) return k * m;
        if (m > 100000000) return m;
    }
}

std::vector<double> openKeys(App& app) {
    // Walking the params every frame is slow for a RotoPaint with thousands of strokes: cached
    // until an edit lands, the comp is swapped or the open panels change.
    static std::vector<double> cached;
    static std::tuple<const void*, uint64_t, std::vector<std::string>> cachedFor;
    std::tuple<const void*, uint64_t, std::vector<std::string>> key{app.comp.get(), app.compRevision(), {app.panels.begin(), app.panels.end()}};
    if (key == cachedFor) return cached;
    std::set<double> keys;
    std::function<void(const json&)> collect = [&](const json& v) {
        if (params::isAnimated(v)) {
            for (auto& k : v["keys"]) keys.insert(k[0].get<double>());
        } else if (v.is_object() && v.contains("anim") && v["anim"].is_array()) {  // roto shape keys
            for (auto& k : v["anim"])
                if (k.is_array() && !k.empty() && k[0].is_number()) keys.insert(k[0].get<double>());
            for (auto& e : v) collect(e);
        } else if (v.is_array() || v.is_object()) {
            for (auto& e : v) collect(e);
        }
    };
    for (auto& pn : app.panels)
        if (Node* n = app.node(pn)) collect(n->params.data);
    cached.assign(keys.begin(), keys.end());
    cachedFor = std::move(key);
    return cached;
}

}  // namespace

float timelineHeight() {
    const ImGuiStyle& st = ImGui::GetStyle();
    return std::round(6 * 2 + rulerHeight() + st.ItemSpacing.y + ImGui::GetFrameHeight());
}

// Called before the dockspace each frame: the timeline's node keeps its height when the
// panels around it are resized (only the viewer and node graph grow), but its own edge can be
// dragged: the height it's dragged to is kept from then on (never less than the timeline needs).
namespace {
float gTimelineH = 0;  // the dragged height (0: the default)
float gLastNodeH = 0, gLastParentH = 0;
}  // namespace

void resetTimelineHeight() {
    gTimelineH = 0;
    gLastNodeH = gLastParentH = 0;
}

void keepDockResizable() {
    ImGuiContext& g = *GImGui;
    const ImGuiDockNodeFlags no = ImGuiDockNodeFlags_NoResize | ImGuiDockNodeFlags_NoResizeX | ImGuiDockNodeFlags_NoResizeY;
    for (int i = 0; i < g.DockContext.Nodes.Data.Size; ++i)
        if (auto* n = (ImGuiDockNode*)g.DockContext.Nodes.Data[i].val_p)
            if (n->LocalFlags & no) {
                n->LocalFlags &= ~no;
                n->UpdateMergedFlags();
            }
}

void lockTimelineHeight() {
    ImGuiWindow* w = ImGui::FindWindowByName("Timeline");
    if (!w || !w->DockNode) return;
    ImGuiDockNode* n = w->DockNode;
    ImGuiDockNode* parent = n->ParentNode;
    if (!parent || parent->SplitAxis != ImGuiAxis_Y || parent->ChildNodes[0] == nullptr || parent->ChildNodes[1] == nullptr) return;
    // nodes not laid out yet (a workspace just loaded, #103): their sizes are still 0, and locking now
    // would squeeze the viewer's area down to the timeline's minimum
    if (parent->Size.y <= 0 || n->Size.y <= 0) return;
    const float minH = timelineHeight();
    ImGuiDockNode* other = parent->ChildNodes[0] == n ? parent->ChildNodes[1] : parent->ChildNodes[0];
    if (gLastNodeH <= 0 && gTimelineH <= 0 && n->Size.y > minH + 1) gTimelineH = n->Size.y;  // as saved in the layout
    // its own splitter was dragged: the parent kept its size and the timeline's changed
    if (gLastNodeH > 0 && std::fabs(parent->Size.y - gLastParentH) < 0.5f && std::fabs(n->Size.y - gLastNodeH) > 0.5f &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left))
        gTimelineH = n->Size.y;
    const float maxH = std::max(minH, parent->Size.y - 60 - ImGui::GetStyle().DockingSeparatorSize);
    const float H = std::clamp(gTimelineH > 0 ? gTimelineH : minH, minH, maxH);
    n->Size.y = H;
    n->SizeRef.y = H;
    n->WantLockSizeOnce = true;
    other->WantLockSizeOnce = false;
    gLastNodeH = H;
    gLastParentH = parent->Size.y;
    // the area holding the timeline never gets too short to show all of it
    const float minArea = minH + 60 + ImGui::GetStyle().DockingSeparatorSize;
    for (ImGuiDockNode* a = parent; a && a->ParentNode; a = a->ParentNode) {
        ImGuiDockNode* p = a->ParentNode;
        if (p->SplitAxis != ImGuiAxis_Y) continue;
        ImGuiDockNode* sib = p->ChildNodes[0] == a ? p->ChildNodes[1] : p->ChildNodes[0];
        if (a->Size.y < minArea && sib && !sib->WantLockSizeOnce) {
            a->Size.y = minArea;
            a->WantLockSizeOnce = true;
        }
        break;
    }
}

void drawTimeline(App& app, UiState& ui) {
    // floating: the same fixed height; under a maximized viewer: that plus its title bar
    const float H = timelineHeight() + (ui.maximized == "Viewer" ? ImGui::GetFrameHeight() : 0.0f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(160, H), ImVec2(FLT_MAX, FLT_MAX));  // at least tall enough; taller when dragged
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    ImGui::Begin(ui.win("Timeline").c_str(), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ui.noteHover("Timeline");
    Comp& c = *app.comp;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float sf = smallFont();
    const bool tc = ui.timeFormat == 1;
    auto label = [&](int fr) { return tc ? timecode(fr, c.fps) : std::to_string(fr); };

    auto [visA, visB] = app.visibleRange();
    auto marked = app.markedRange();
    const auto [markIn, markOut] = marked.value_or(App::FrameRange{visA, visB});
    const double navFirst = std::min(visA, markIn), navLast = std::max(visB, markOut);
    auto [rulerA, rulerB] = ui.viewer.timelineWindow.value_or(App::FrameRange{navFirst, navLast});
    const int first = (int)std::floor(rulerA), last = (int)std::ceil(rulerB), f = (int)std::lround(app.frame);
    auto [inD, outD] = app.playRange();
    const int inF = (int)inD, outF = (int)outD;
    auto setFrame = [&](int64_t nf) {
        nf = std::clamp(nf, (int64_t)std::floor(navFirst), (int64_t)std::ceil(navLast));
        if (nf != (int)app.frame) {
            app.frame = nf;
            app.requestView();
        }
    };
    const std::vector<double> keys = openKeys(app);
    const auto goToKey = [&](bool forward) {
        if (forward) {
            auto it = std::upper_bound(keys.begin(), keys.end(), (double)f);
            if (it != keys.end()) setFrame(std::llround(*it));
        } else {
            auto it = std::lower_bound(keys.begin(), keys.end(), (double)f);
            if (it != keys.begin()) setFrame(std::llround(*std::prev(it)));
        }
    };

    const int mode = c.project.timelineRange == "input" ? 1 : c.project.timelineRange == "custom" ? 2 : 0;
    const bool inputMode = mode == 1, customMode = mode == 2;
    const bool inputFound = inputMode && nodeFrameRange(app.node(app.viewed)).has_value();
    const char* rangeLabels[] = {"Global", "Input", "Custom"};
    const auto chooseSource = [&](int source) {
        const char* names[] = {"global", "input", "custom"};
        std::string name = names[source];
        ui.viewer.timelineWindow.reset();
        app.edit([&app, name, visA, visB](Comp& cc) {
            if (name == "custom" && cc.project.timelineRange != "custom") {
                cc.project.customFirst = visA;
                cc.project.customLast = visB;
            }
            cc.project.timelineRange = name;
            auto [firstFrame, lastFrame] = app.visibleRange();
            if (app.frame < firstFrame || app.frame > lastFrame) app.frame = firstFrame;
        });
    };

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float W = ImGui::GetContentRegionAvail().x;
    const float RH = rulerHeight();

    // ---------------------------------------------------------- row 1: [first] ruler [last]
    // First / last fields: edit the project range (Global, unless locked) or the
    // custom range; Input shows the input's range read-only.
    const float fieldW = std::max(44.0f, ImGui::CalcTextSize(tc ? "00:00:00:00" : "000000").x + 10);
    auto rangeField = [&](const char* id, int value, bool isFirst, float x) {
        ImGui::SetCursorScreenPos(ImVec2(x, origin.y + RH - ImGui::GetFrameHeight()));
        ImGui::SetNextItemWidth(fieldW);
        ImGui::BeginDisabled(inputMode || (!customMode && c.project.lockRange));
        int v = value;
        if (frameEntry(id, value, tc, c.fps, v)) {
            ui.viewer.timelineWindow.reset();
            if (customMode) {
                app.edit([v, isFirst](Comp& cc) {
                    if (isFirst) cc.project.customFirst = std::min<double>(v, cc.project.customLast);
                    else cc.project.customLast = std::max<double>(v, cc.project.customFirst);
                });
            } else {
                app.checkpoint();
                app.edit([v, isFirst](Comp& cc) {
                    if (isFirst) cc.firstFrame = std::min<double>(v, cc.lastFrame);
                    else cc.lastFrame = std::max<double>(v, cc.firstFrame);
                });
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (inputMode)
                ImGui::SetTooltip(inputFound ? "%s frame of %s's input" : "%s has no sequence upstream: showing the project range",
                                  inputFound ? (isFirst ? "first" : "last") : app.viewed.c_str(), app.viewed.c_str());
            else if (customMode)
                ImGui::SetTooltip("custom %s frame", isFirst ? "first" : "last");
            else
                ImGui::SetTooltip(c.project.lockRange ? "frame range is locked (the lock button, or Project Settings)" : "project %s frame",
                                  isFirst ? "first" : "last");
        }
    };
    rangeField("##first", (int)std::floor(visA), true, origin.x);
    rangeField("##last", (int)std::ceil(visB), false, origin.x + W - fieldW);

    // the ruler
    const float gap = st.ItemSpacing.x;
    const ImVec2 r0(origin.x + fieldW + gap, origin.y), r1(origin.x + W - fieldW - gap, origin.y + RH);
    const float rw = std::max(20.0f, r1.x - r0.x);
    const float margin = std::min(rw * 0.35f, std::max(6.0f, ImGui::CalcTextSize(label(last).c_str()).x * 0.5f * sf / ImGui::GetFontSize() + 2));
    const double span = std::max(1.0, (double)last - first);
    const float ppf = (float)((rw - 2 * margin) / span);  // pixels per frame
    auto xOf = [&](double fr) { return r0.x + margin + (float)(fr - first) * ppf; };
    auto frameAt = [&](float x) { return (int)std::lround(first + (x - r0.x - margin) / std::max(1e-6f, ppf)); };

    const float labelTop = r1.y - sf - 1;          // frame numbers under the ticks
    const float tickBase = labelTop - 1;           // ticks hang above them
    const float curTop = r0.y;                     // the current frame's number, above the marker
    const float markTop = r0.y + sf + 1;           // the marker
    const float tickTop = tickBase - std::round(RH * 0.32f);

    dl->AddRectFilled(r0, ImVec2(r0.x + rw, r1.y), IM_COL32(40, 40, 40, 255), 2);
    // outside in/out: darker; in/out: orange brackets
    if (marked) {
        const double a = ui.timelineDrag == 1 ? ui.timelineDragFrame : markIn;
        const double b = ui.timelineDrag == 2 ? ui.timelineDragFrame : markOut;
        if (a > first) dl->AddRectFilled(ImVec2(xOf(first), markTop), ImVec2(xOf(std::min<double>(a, last)), tickBase), IM_COL32(0, 0, 0, 70));
        if (b < last) dl->AddRectFilled(ImVec2(xOf(std::max<double>(b, first)), markTop), ImVec2(xOf(last), tickBase), IM_COL32(0, 0, 0, 70));
        for (bool isIn : {true, false}) {
            const double e = isIn ? a : b;
            if (e < first || e > last) continue;
            const float x = xOf(e), d = isIn ? 5.0f : -5.0f;
            dl->AddLine(ImVec2(x, markTop + 2), ImVec2(x, tickBase), kAccent, 2);
            dl->AddLine(ImVec2(x, tickBase), ImVec2(x + d, tickBase), kAccent, 2);
            dl->AddLine(ImVec2(x, markTop + 2), ImVec2(x + d, markTop + 2), kAccent, 2);
        }
    }
    // the cache line: frames in the RAM playback cache (green), on the disk cache (amber, under it)
    for (int cf : app.diskCachedFrames(first, last))
        dl->AddRectFilled(ImVec2(xOf(cf) - ppf * 0.5f, tickTop - 2), ImVec2(xOf(cf) + ppf * 0.5f + 0.5f, tickTop), IM_COL32(220, 150, 40, 220));
    for (int cf : app.cachedFrames())
        if (cf >= first && cf <= last)
            dl->AddRectFilled(ImVec2(xOf(cf) - ppf * 0.5f, tickTop - 4), ImVec2(xOf(cf) + ppf * 0.5f + 0.5f, tickTop - 2), kCache);
    // ticks: one per frame when they fit, else every 2 / 5 / 10...; labels
    // where they don't crowd
    const int minor = niceStep(4.0 / std::max(1e-6f, ppf));
    const float labelW = ImGui::CalcTextSize(label(last).c_str()).x * sf / ImGui::GetFontSize();
    const int major = std::max(minor, niceStep((labelW * 2 + 30) / std::max(1e-6f, ppf)));  // roomy, as is standard
    const int mid = major % 5 == 0 && major / 5 >= minor && (major / 5) % minor == 0 ? major / 5 : (major % 2 == 0 && major / 2 >= minor ? major / 2 : major);
    const ImU32 tickCol = IM_COL32(150, 150, 150, 255), labelCol = IM_COL32(170, 170, 170, 255);
    auto floorTo = [](int v, int s) { return (int64_t)std::floor((double)v / s) * s; };
    for (int64_t fr = floorTo(first, minor); fr <= last; fr += minor) {
        if (fr < first) continue;
        const float x = std::round(xOf(fr)) + 0.5f;
        const bool isMajor = fr % major == 0, isMid = fr % mid == 0;
        const float th = isMajor ? tickBase - tickTop : isMid ? (tickBase - tickTop) * 0.6f : (tickBase - tickTop) * 0.35f;
        dl->AddLine(ImVec2(x, tickBase - th), ImVec2(x, tickBase), tickCol, 1);
    }
    // labels: the range's ends, and the major ticks clear of them
    std::vector<std::pair<float, float>> taken;  // label x-extents
    auto putLabel = [&](int fr, bool force) {
        std::string s = label(fr);
        float tw = font->CalcTextSizeA(sf, FLT_MAX, 0, s.c_str()).x;
        float x = std::clamp(xOf(fr) - tw * 0.5f, r0.x + 1, std::max(r0.x + 1, r0.x + rw - tw - 1));
        for (auto& [a, b] : taken)
            if (!force && x < b + 8 && x + tw > a - 8) return;
        taken.push_back({x, x + tw});
        dl->AddText(font, sf, ImVec2(x, labelTop), labelCol, s.c_str());
    };
    putLabel(first, true);
    putLabel(last, true);
    for (int64_t fr = floorTo(first, major); fr <= last; fr += major)
        if (fr > first && fr < last) putLabel(fr, false);
    // keys of the open panels' nodes
    for (double k : keys)
        if (k >= first && k <= last) {
            const float x = std::round(xOf(k));
            dl->AddRectFilled(ImVec2(x - 1, tickTop), ImVec2(x + 1, tickBase), kKey);
        }

    // scrubbing
    ImGui::SetCursorScreenPos(r0);
    ImGui::InvisibleButton("##ruler", ImVec2(rw, RH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hov = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const auto& io = ImGui::GetIO();
    const float mouseX = io.MousePos.x;
    const auto setMarker = [&](bool in, int frame) {
        const double a = in ? std::clamp<double>(frame, navFirst, markOut) : markIn;
        const double b = in ? markOut : std::clamp<double>(frame, markIn, navLast);
        app.setMarkedRange(App::FrameRange{a, b});
    };
    const auto setWindow = [&](double a, double width) {
        if (width >= navLast - navFirst) {
            ui.viewer.timelineWindow.reset();
            return;
        }
        a = std::clamp(a, navFirst, navLast - width);
        ui.viewer.timelineWindow = App::FrameRange{a, a + width};
    };
    const auto zoomRuler = [&](double delta) {
        const double anchor = std::clamp<double>((mouseX - xOf(first)) / (rw - 2 * margin), 0, 1);
        const double oldWidth = rulerB - rulerA;
        const double width = std::max(2.0, oldWidth * std::pow(0.8, delta));
        setWindow(rulerA + anchor * (oldWidth - width), width);
    };
    if (hov && !active && io.MouseWheel != 0) zoomRuler(io.MouseWheel);
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        zoomRuler(io.MouseDelta.x * 0.035);
    if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Middle) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Middle] < 4)
        ui.viewer.timelineWindow.reset();
    if (active && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ui.timelineDrag = io.KeyAlt ? 3 : 0;
        if (!ui.timelineDrag && marked && io.MousePos.y >= markTop) {
            const float toIn = std::fabs(mouseX - xOf(markIn)), toOut = std::fabs(mouseX - xOf(markOut));
            if (std::min(toIn, toOut) <= 6) {
                ui.timelineDrag = toIn <= toOut ? 1 : 2;
                ui.timelineDragFrame = (int)(ui.timelineDrag == 1 ? markIn : markOut);
            }
        }
    }
    if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const int mouseFrame = frameAt(mouseX);
        if (ui.timelineDrag == 1) ui.timelineDragFrame = std::clamp(mouseFrame, first, std::min(last, (int)markOut));
        else if (ui.timelineDrag == 2) ui.timelineDragFrame = std::clamp(mouseFrame, std::max(first, (int)markIn), last);
        else if (ui.timelineDrag == 3) setWindow(rulerA - io.MouseDelta.x / std::max(1e-6f, ppf), rulerB - rulerA);
        else setFrame(std::clamp(mouseFrame, first, last));
    } else if (ui.timelineDrag) {
        if (ui.timelineDrag <= 2 && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            setMarker(ui.timelineDrag == 1, ui.timelineDragFrame);
        ui.timelineDrag = 0;
    }
    if (hov && !active) {  // where a click would go
        int hf = std::clamp(frameAt(ImGui::GetIO().MousePos.x), first, last);
        const float x = std::round(xOf(hf)) + 0.5f;
        dl->AddLine(ImVec2(x, markTop), ImVec2(x, tickBase), IM_COL32(255, 255, 255, 70), 1);
        if (hf != f) ImGui::SetTooltip("%s", label(hf).c_str());
    }
    // the current frame: a marker above the ruler with its number, and a line through the ticks
    if (f >= first && f <= last) {
        const float x = std::round(xOf(f)) + 0.5f;
        const float ms = std::round(sf * 0.45f);
        dl->AddTriangleFilled(ImVec2(x - ms, markTop), ImVec2(x + ms, markTop), ImVec2(x, markTop + ms * 1.3f), kAccent);
        dl->AddLine(ImVec2(x, markTop + ms), ImVec2(x, tickBase), kAccent, 1.5f);
        std::string s = label(f);
        float tw = font->CalcTextSizeA(sf, FLT_MAX, 0, s.c_str()).x;
        float tx = std::clamp(x - tw * 0.5f, r0.x + 1, std::max(r0.x + 1, r0.x + rw - tw - 1));
        dl->AddText(font, sf, ImVec2(tx, curTop), kAccent, s.c_str());
    }

    // ---------------------------------------------------------- row 2
    const float y2 = origin.y + RH + st.ItemSpacing.y;
    const float bw = std::round(ImGui::GetFrameHeight() * 1.25f);  // transport buttons
    const float sp = 2;                                               // between transport buttons
    const float frameW = std::max(48.0f, ImGui::CalcTextSize(tc ? "00:00:00:00" : "000000").x + 12);
    const float fpsW = ImGui::CalcTextSize("000.00").x + ImGui::GetFrameHeight() + 6;
    const float tfW = ImGui::CalcTextSize("TF").x + ImGui::GetFrameHeight() + 8;
    const float modeW = ImGui::CalcTextSize("Custom").x + ImGui::GetFrameHeight() + 10;
    const float incW = ImGui::CalcTextSize("000").x + 8;
    const float countW = ImGui::CalcTextSize("000000").x + 10;
    const float flipW = ImGui::CalcTextSize("Flipbook").x + st.FramePadding.x * 2;
    const float captureW = ImGui::CalcTextSize("Capture").x + st.FramePadding.x * 2;

    // What fits: the less-used parts drop out as the panel narrows.
    struct Show { bool tf = true, inc = true, loop = true, keys = true, io = true, ends = true, actions = true, duration = true, steps = true; } show;
    auto widths = [&](float& left, float& mid, float& right) {
        left = fpsW + (show.tf ? gap + tfW : 0) + gap + modeW;
        int nb = 2 /*play*/ + (show.steps ? 2 : 0) + (show.keys ? 2 : 0) + (show.ends ? 2 : 0) + (show.io ? 2 : 0) + (show.loop ? 1 : 0) +
                 (show.inc ? 2 : 0);
        mid = nb * (bw + sp) + frameW + sp + (show.inc ? incW + sp : 0) + (show.inc ? 8 : 0) + (show.loop ? 6 : 0);
        right = bw + (show.actions ? flipW + captureW + sp * 2 : 0) + (show.duration ? countW + sp : 0);
    };
    float lw, mw, rwid;
    bool* drops[] = {&show.actions, &show.inc, &show.loop, &show.tf, &show.keys, &show.duration, &show.io, &show.ends, &show.steps};
    for (bool* d : drops) {
        widths(lw, mw, rwid);
        if (lw + mw + rwid + gap * 4 <= W) break;
        *d = false;
    }
    widths(lw, mw, rwid);
    const bool leftFits = lw + mw + rwid + gap * 4 <= W || lw + mw + gap * 2 <= W;
    float midX = origin.x + (W - mw) * 0.5f;  // centred, as is standard
    if (leftFits) midX = std::clamp(midX, origin.x + lw + gap * 2, std::max(origin.x + lw + gap * 2, origin.x + W - rwid - gap * 2 - mw));
    else midX = origin.x;  // very narrow: transport and overflow

    if (leftFits) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y2));
        // fps: the comp's rate; while playing, the rate actually reached (red when behind), as is standard
        char fpsPrev[32];
        const bool live = app.playing && ui.measuredFps > 0;
        std::snprintf(fpsPrev, sizeof fpsPrev, "%g", live ? std::round(ui.measuredFps * 10) / 10 : c.fps);
        const bool slow = live && ui.measuredFps < c.fps * 0.95;
        if (slow) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.45f, 0.4f, 1));
        ImGui::SetNextItemWidth(fpsW);
        if (ImGui::BeginCombo("##fps", fpsPrev)) {
            if (slow) ImGui::PopStyleColor();
            for (double r : {12.0, 15.0, 23.976, 24.0, 25.0, 29.97, 30.0, 48.0, 50.0, 59.94, 60.0}) {
                char b[16];
                std::snprintf(b, sizeof b, "%g", r);
                if (ImGui::Selectable(b, std::fabs(c.fps - r) < 1e-3)) {
                    app.checkpoint();
                    app.edit([r](Comp& cc) { cc.fps = r; });
                }
            }
            ImGui::EndCombo();
            if (slow) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.45f, 0.4f, 1));
        }
        if (slow) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(live ? "playing at %.1f fps (target %g)" : "frames per second (%g)", live ? ui.measuredFps : c.fps, c.fps);
        if (show.tf) {
            ImGui::SameLine(0, gap);
            ImGui::SetNextItemWidth(tfW);
            if (ImGui::BeginCombo("##tf", "TF")) {
                if (ImGui::Selectable("Frame", ui.timeFormat == 0)) ui.timeFormat = 0;
                if (ImGui::Selectable("Timecode", ui.timeFormat == 1)) ui.timeFormat = 1;
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("time format: frame numbers or timecode");
        }
        // Range menu (the classic Global / Input / Custom).
        ImGui::SameLine(0, gap);
        ImGui::SetNextItemWidth(modeW);
        int m = mode;
        if (ImGui::Combo("##rangemode", &m, rangeLabels, 3)) chooseSource(m);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Global: the project frame range\nInput: the frame range of the viewed node's input (its Read nodes)\n"
                              "Custom: a range of your own; doesn't change the project");
    }

    // transport, the classic order
    ImGui::SetCursorScreenPos(ImVec2(midX, y2));
    auto next = [&](float s = 2) { ImGui::SameLine(0, s); };
    const auto setPlayMode = [&](int mode) {
        ui.playMode = mode;
        app.playWholeRange = mode == 3;
    };
    static const char* kPlayback[] = {"Loop", "Bounce", "Play once (in/out)", "Play once (full range)"};
    if (show.loop) {
        static const Icon kLoop[] = {Icon::Repeat, Icon::Bounce, Icon::Once, Icon::Once};
        if (iconButton("##loop", kLoop[ui.playMode], bw, false, kPlayback[ui.playMode])) setPlayMode((ui.playMode + 1) % 4);
        next(6);
    }
    auto setInOut = [&](bool in) { toggleInOut(app, in); };  // (as the I / O keys)
    auto clearInOut = [&] { app.setMarkedRange(std::nullopt); };
    if (show.io) {
        if (textButton("I##in", bw, marked.has_value(), "Set in point (I)\nRight-click to clear in and out")) setInOut(true);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) clearInOut();
        next();
    }
    if (show.ends) {
        if (iconButton("##tofirst", Icon::First, bw, false, "in point / first frame (Home)")) setFrame(inF);
        next();
    }
    if (show.keys) {
        if (iconButton("##pkey", Icon::KeyBack, bw, false, "previous keyframe (Alt+Left)")) goToKey(false);
        next();
    }
    if (show.steps) {
        if (iconButton("##pf", Icon::StepBack, bw, false, "previous frame (Left)")) setFrame((int64_t)f - 1);
        next();
    }
    const bool back = app.playing && app.playDir < 0, fwd = app.playing && app.playDir > 0;
    if (iconButton("##playb", back ? Icon::Stop : Icon::PlayBack, bw, back, back ? "stop (K)" : "play backward (J)")) {
        app.playDir = -1;
        app.playing = !back;
    }
    next();
    // the current frame
    ImGui::SetNextItemWidth(frameW);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kAccent));
    int nf = f;
    if (frameEntry("##frame", f, tc, c.fps, nf)) setFrame(nf);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Current frame (Alt+G). Use += or -= to jump by an offset.");
    next();
    if (iconButton("##playf", fwd ? Icon::Stop : Icon::PlayFwd, bw, fwd, fwd ? "stop (K)" : "play forward (L)")) {
        app.playDir = 1;
        app.playing = !fwd;
    }
    if (show.steps) {
        next();
        if (iconButton("##nf", Icon::StepFwd, bw, false, "next frame (Right)")) setFrame((int64_t)f + 1);
    }
    if (show.keys) {
        next();
        if (iconButton("##nkey", Icon::KeyFwd, bw, false, "next keyframe (Alt+Right)")) goToKey(true);
    }
    if (show.ends) {
        next();
        if (iconButton("##tolast", Icon::Last, bw, false, "out point / last frame (End)")) setFrame(outF);
    }
    if (show.io) {
        next();
        if (textButton("O##out", bw, marked.has_value(), "Set out point (O)\nRight-click to clear in and out")) setInOut(false);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) clearInOut();
    }
    if (show.inc) {
        next(8);
        if (iconButton("##incb", Icon::IncBack, bw, false, "back by the increment (Shift+Left)")) setFrame((int64_t)f - ui.frameIncrement);
        next();
        ImGui::SetNextItemWidth(incW);
        int inc = ui.frameIncrement;
        if (ImGui::InputInt("##inc", &inc, 0, 0)) ui.frameIncrement = std::clamp(inc, 1, 100000);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("frame increment for the << >> buttons");
        next();
        if (iconButton("##incf", Icon::IncFwd, bw, false, "forward by the increment (Shift+Right)")) setFrame((int64_t)f + ui.frameIncrement);
    }

    // The overflow keeps secondary actions available at every panel width.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + W - rwid, y2));
    if (show.actions && leftFits) {
        if (textButton("Flipbook", flipW, false, "Render a preview of the current Viewer input over the playback range"))
            openFlipbook(app, ui);
        next();
        if (textButton("Capture", captureW, false, "Save the visible Viewer image area as a PNG"))
            openViewerCapture(app, ui);
        next();
    }
    if (show.duration && leftFits) {
        ImGui::SetNextItemWidth(countW);
        char cnt[32];
        std::snprintf(cnt, sizeof cnt, "%lld", (long long)visB - (long long)visA + 1);
        ImGui::BeginDisabled();
        ImGui::InputText("##count", cnt, sizeof cnt, ImGuiInputTextFlags_ReadOnly);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Sequence duration: %s frames\nPlayback: %lld frames", cnt, (long long)outF - inF + 1);
        next();
    }
    if (textButton("...##timeline_options", bw, false, "Timeline options")) ImGui::OpenPopup("timeline_options");
    if (ImGui::BeginPopup("timeline_options")) {
        if (ImGui::MenuItem("Flipbook")) openFlipbook(app, ui);
        if (ImGui::MenuItem("Capture Viewer")) openViewerCapture(app, ui);
        if (ImGui::MenuItem("Cache playback range", nullptr, app.precaching())) app.setPrecache(!app.precaching());
        ImGui::Separator();
        if (ImGui::BeginMenu("Playback mode")) {
            for (int m = 0; m < IM_ARRAYSIZE(kPlayback); ++m)
                if (ImGui::MenuItem(kPlayback[m], nullptr, ui.playMode == m)) setPlayMode(m);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Frame range source")) {
            for (int m = 0; m < IM_ARRAYSIZE(rangeLabels); ++m)
                if (ImGui::MenuItem(rangeLabels[m], nullptr, m == mode)) chooseSource(m);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Playback rate")) {
            double fps = c.fps;
            ImGui::SetNextItemWidth(90);
            if (ImGui::InputDouble("fps", &fps, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue) &&
                std::isfinite(fps) && fps >= 1 && fps <= 1000) {
                app.checkpoint();
                app.edit([fps](Comp& cc) { cc.fps = fps; });
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Display timecode", nullptr, tc)) ui.timeFormat = tc ? 0 : 1;
        if (ImGui::MenuItem("Reset timeline zoom", "Middle click", false, ui.viewer.timelineWindow.has_value()))
            ui.viewer.timelineWindow.reset();
        if (ImGui::MenuItem("Synchronize Viewer playheads", nullptr, ui.linkViewerPlayback)) {
            ui.linkViewerPlayback = !ui.linkViewerPlayback;
            for (auto& [name, viewer] : ui.otherViewers) viewer.timelineFrame = app.frame;
        }
        const bool pinned = app.pinnedRange().has_value();
        if (ImGui::MenuItem("Lock this Viewer's playback range", nullptr, pinned)) {
            if (pinned) app.pinRange(std::nullopt);
            else app.pinRange(App::FrameRange{markIn, markOut});
        }
        const bool locked = c.project.lockRange;
        if (ImGui::MenuItem("Lock project frame edits", nullptr, locked))
            app.edit([locked](Comp& cc) { cc.project.lockRange = !locked; });
        if (ImGui::BeginMenu("In / Out")) {
            ImGui::SetNextItemWidth(100);
            int in = (int)markIn, out = (int)markOut;
            if (frameEntry("In", in, tc, c.fps, in)) setMarker(true, in);
            ImGui::SetNextItemWidth(100);
            if (frameEntry("Out", out, tc, c.fps, out)) setMarker(false, out);
            if (ImGui::MenuItem("Set in at playhead", "I")) setInOut(true);
            if (ImGui::MenuItem("Set out at playhead", "O")) setInOut(false);
            if (ImGui::MenuItem("Clear in / out")) clearInOut();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Navigation")) {
            if (ImGui::MenuItem("First playback frame", "Home")) setFrame(inF);
            if (ImGui::MenuItem("Last playback frame", "End")) setFrame(outF);
            if (ImGui::MenuItem("Previous frame", "Left")) setFrame((int64_t)f - 1);
            if (ImGui::MenuItem("Next frame", "Right")) setFrame((int64_t)f + 1);
            if (ImGui::MenuItem("Previous keyframe", "Alt+Left")) goToKey(false);
            if (ImGui::MenuItem("Next keyframe", "Alt+Right")) goToKey(true);
            if (ImGui::MenuItem("Back by increment", "Shift+Left")) setFrame((int64_t)f - ui.frameIncrement);
            if (ImGui::MenuItem("Forward by increment", "Shift+Right")) setFrame((int64_t)f + ui.frameIncrement);
            ImGui::SetNextItemWidth(75);
            int inc = ui.frameIncrement;
            if (ImGui::InputInt("Frame increment", &inc, 0, 0)) ui.frameIncrement = std::clamp(inc, 1, 100000);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::TextDisabled("%lld frames", (long long)visB - (long long)visA + 1);
        ImGui::EndPopup();
    }
    ImGui::End();
}

}  // namespace strata::ui
