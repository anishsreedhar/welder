// The ImageGen node's properties panel: the Generate / Cancel bar with live progress and a status
// banner, the contact sheet of a batch's takes (filling in while the batch runs), Set as Source /
// Branch to New Node, the mode menu with Auto resolved, the LoRA stack and the size presets.
//
// Everything goes through the node's state API (nodes/ImageGenNode.h): takes() / liveTakes() /
// livePreview() / live() to draw, selectTake() / promoteTake() / clearPromoted() / setSize() and
// branchTakeToNewNode() inside App::edit to change things.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "App.h"
#include "GL.h"
#include "PanelKit.h"
#include "Panels.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ml/ML.h"
#include "nodes/ImageGenNode.h"

namespace strata::ui {

namespace {

using imagegen::ImageGenNode;

// ------------------------------------------------------------------ colours
ImU32 col(unsigned int c) { return (ImU32)c; }
ImU32 withAlpha(ImU32 c, int a) { return (c & 0x00FFFFFFu) | ((ImU32)a << 24); }
const ImU32 kWarnBg = IM_COL32(92, 70, 28, 255), kWarnText = IM_COL32(255, 210, 120, 255);
const ImU32 kErrBg = IM_COL32(96, 36, 32, 255), kErrText = IM_COL32(255, 150, 135, 255);
const ImU32 kInfoBg = IM_COL32(40, 52, 64, 255), kInfoText = IM_COL32(170, 205, 235, 255);
const ImU32 kCancel = IM_COL32(150, 54, 46, 255), kCancelHot = IM_COL32(176, 66, 56, 255);

// ------------------------------------------------------------------ textures of takes
struct Tex {
    unsigned int id = 0;
    int w = 0, h = 0;
    const void* src = nullptr;  // the NpyImage it was made from
    int lastUsed = 0;
};
std::map<std::string, Tex> gTex;

unsigned char enc8(float v, bool linear) {
    if (!(v > 0)) return 0;
    if (linear) v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return (unsigned char)std::lround(std::min(1.0f, v) * 255.0f);
}

// An RGB(A) float image as an 8-bit texture, at most `maxSide` px (0 = full size). `linear`: the
// pixels are linear Rec.709 (takes) and get the sRGB curve; else they're display-referred (previews).
const Tex* texture(const std::string& id, const std::shared_ptr<const ml::NpyImage>& img, int maxSide, bool linear) {
    if (!img || img->w <= 0 || img->h <= 0 || img->c < 3) return nullptr;
    Tex& t = gTex[id];
    t.lastUsed = ImGui::GetFrameCount();
    if (t.id && t.src == img.get()) return &t;
    int step = 1;
    if (maxSide > 0)
        while (std::max(img->w, img->h) / step > maxSide) ++step;
    const int W = img->w / step, H = img->h / step;
    std::vector<unsigned char> px((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float s[3] = {0, 0, 0};
            for (int dy = 0; dy < step; ++dy)  // a box filter: thumbnails of noisy images stay calm
                for (int dx = 0; dx < step; ++dx)
                    for (int ch = 0; ch < 3; ++ch) s[ch] += img->at(x * step + dx, y * step + dy, ch);
            unsigned char* o = &px[((size_t)y * W + x) * 4];
            for (int ch = 0; ch < 3; ++ch) o[ch] = enc8(s[ch] / (step * step), linear);
            o[3] = 255;
        }
    if (!t.id) glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, maxSide > 0 ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    t.w = W;
    t.h = H;
    t.src = img.get();
    return &t;
}

// Textures not drawn for a while go (a contact sheet of 16 takes is ~6 MB of thumbnails).
void pruneTextures() {
    const int now = ImGui::GetFrameCount();
    for (auto it = gTex.begin(); it != gTex.end();)
        if (now - it->second.lastUsed > 600 || gTex.size() > 96) {
            if (it->second.id) glDeleteTextures(1, &it->second.id);
            it = gTex.erase(it);
        } else
            ++it;
}

// ------------------------------------------------------------------ per-node UI state
struct SheetState {
    bool inspect = false;       // 1:1 single-take view
    std::string inspectKey;     // its run
    int inspectIndex = 0;
    std::string pendingKey;     // a take clicked while its batch was still running: selected when it lands
    int pendingIndex = -1;
};
std::map<std::string, SheetState> gSheet;

// One edit of the node (undoable), with its params marked changed.
void editGen(App& app, const std::string& name, std::function<void(ImageGenNode&, Comp&)> fn, bool undoStep = true) {
    if (undoStep) app.checkpoint();
    app.edit([name, fn](Comp& c) {
        if (auto* g = dynamic_cast<ImageGenNode*>(c.find(name))) {
            fn(*g, c);
            g->paramsChanged();
        }
    });
}

void selectTake(App& app, const std::string& name, const std::string& key, int i) {
    editGen(app, name, [key, i](ImageGenNode& g, Comp&) { g.selectTake(i, key); });
}

void promote(App& app, const std::string& name, const std::string& key, int i) {
    editGen(app, name, [key, i](ImageGenNode& g, Comp&) { g.promoteTake(key, i); });
    app.message(name + ": take #" + std::to_string(i + 1) + " is the source");
}

void branch(App& app, const std::string& name, const std::string& key, int i) {
    app.checkpoint();
    app.edit([&app, name, key, i](Comp& c) {
        auto* g = dynamic_cast<ImageGenNode*>(c.find(name));
        if (!g) return;
        if (ImageGenNode* b = imagegen::branchTakeToNewNode(c, *g, key, i)) {
            app.selection = {b->name};
            app.openPanel(b->name);
            app.message(b->name + ": refines take #" + std::to_string(i + 1) + " of " + name);
        }
    });
}

// ------------------------------------------------------------------ small widgets
// A full-width banner with wrapped text.
void banner(const std::string& text, ImU32 bg, ImU32 fg) {
    if (text.empty()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x, pad = 6;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, w - 2 * pad);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + ts.y + 2 * pad), bg, 4);
    dl->AddRectFilled(p, ImVec2(p.x + 3, p.y + ts.y + 2 * pad), fg, 4, ImDrawFlags_RoundCornersLeft);
    ImGui::SetCursorScreenPos(ImVec2(p.x + pad + 2, p.y + pad));
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 2 * pad - 2);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ts.y + 2 * pad));
    ImGui::Dummy(ImVec2(w, 4));
}

// A pill button (aspect presets, mode chips): rounded, the accent when on.
bool pill(const char* label, bool on, const char* tip = nullptr) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 2));
    ImGui::PushStyleColor(ImGuiCol_Button, on ? themeAccent(200) : col(theme().segment));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? themeAccent(235) : col(theme().segmentOn));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, themeAccent(255));
    ImGui::PushStyleColor(ImGuiCol_Text, on ? IM_COL32(20, 20, 20, 255) : col(theme().text));
    bool r = ImGui::SmallButton(label);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return r;
}

// Lay pills out left to right, wrapping to the next row when one doesn't fit.
void sameLineIfFits(const char* nextLabel) {
    const float need = ImGui::CalcTextSize(nextLabel).x + 16 + ImGui::GetStyle().ItemSpacing.x;
    const float right = ImGui::GetItemRectMax().x + need;
    if (right < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
}

// A badge in a tile's corner.
void badge(ImDrawList* dl, ImVec2 at, const char* text, bool rightAlign = false) {
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 p0 = rightAlign ? ImVec2(at.x - ts.x - 8, at.y) : at;
    dl->AddRectFilled(p0, ImVec2(p0.x + ts.x + 8, p0.y + ts.y + 4), IM_COL32(0, 0, 0, 170), 3);
    dl->AddText(ImVec2(p0.x + 4, p0.y + 2), IM_COL32(235, 235, 235, 255), text);
}

std::string seedText(uint32_t s) { return "Seed " + std::to_string(s); }

// ------------------------------------------------------------------ the Generate bar
struct RunState {
    bool busy = false, stale = false, exporting = false;
    ml::JobStatus st;
};

void generateBar(App& app, ImageGenNode& g, const mlnodes::JobSpec& s, const RunState& rs) {
    const std::string name = g.name;
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = std::round(ImGui::GetFrameHeight() * 1.45f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    if (rs.busy) {
        ImGui::PushStyleColor(ImGuiCol_Button, kCancel);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kCancelHot);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kCancelHot);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 235, 230, 255));
        if (ImGui::Button("Cancel##imagegen", ImVec2(w, h))) app.cancelMLJob(name, "main");
        ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stops the batch within one transformer block; finished takes are kept on disk.");
    } else {
        const bool blocked = !s.error.empty() || app.mlExportStatus().running;
        ImGui::BeginDisabled(blocked);
        ImGui::PushStyleColor(ImGuiCol_Button, themeAccent(215));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, themeAccent(245));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, themeAccent(255));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(18, 18, 18, 255));
        const int n = std::clamp((int)g.params.num("batch_count", 0, 1), 1, imagegen::kMaxBatch);
        char label[64];
        std::snprintf(label, sizeof(label), n > 1 ? "Generate %d##imagegen" : "Generate##imagegen", n);
        if (ImGui::Button(label, ImVec2(w, h))) app.startMLJob(name, "main");
        ImGui::PopStyleColor(4);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", !s.error.empty() ? s.error.c_str()
                                                     : "One job: batch takes with seeds seed, seed+1, ...  An identical finished job is reused.");
    }
    ImGui::PopStyleVar();

    // progress: the whole batch fills the bar, the text says which image and step
    if (rs.busy || rs.exporting) {
        imagegen::Live l = g.live();
        float frac = (float)rs.st.progress;
        std::string text;
        if (rs.exporting) {
            MLExportStatus ex = app.mlExportStatus();
            frac = ex.total ? (float)ex.done / ex.total : 0.0f;
            text = ex.label.empty() ? "preparing inputs" : ex.label;
        } else if (rs.st.state == "queued") {
            text = "waiting for the ML worker";
        } else if (l.steps > 0) {
            char b[96];
            std::snprintf(b, sizeof(b), "Image %d / %d  \xC2\xB7  step %d / %d", l.batchIndex + 1, std::max(1, l.batchSize), l.step, l.steps);
            text = b;
        } else {
            text = rs.st.message.empty() ? "starting" : rs.st.message;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float bh = ImGui::GetTextLineHeight() + 6;
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + bh), col(theme().frame), 3);
        dl->AddRectFilled(p, ImVec2(p.x + w * std::clamp(frac, 0.0f, 1.0f), p.y + bh), themeAccent(150), 3);
        if (l.steps > 0 && l.batchSize > 1) {  // the current image's steps: a thin line under the batch
            const float seg = w / l.batchSize, x0 = p.x + seg * l.batchIndex;
            dl->AddRectFilled(ImVec2(x0, p.y + bh - 2), ImVec2(x0 + seg * l.step / std::max(1, l.steps), p.y + bh), themeAccent(255));
        }
        dl->AddText(ImVec2(p.x + 6, p.y + 3), col(theme().value), text.c_str());
        ImGui::Dummy(ImVec2(w, bh));
    }
}

// What the status banner says: errors first, then warnings (ignored negative prompt, ...).
void statusBanner(ImageGenNode& g, const mlnodes::JobSpec& s, const RunState& rs) {
    if (rs.stale) banner("Interrupted: the ML worker stopped. Start it again to resume.", kWarnBg, kWarnText);
    if (rs.st.state == "error") banner(rs.st.message.empty() ? "the job failed: see the log below" : rs.st.message, kErrBg, kErrText);
    if (!rs.busy && !s.error.empty() && g.params.str("checkpoint", "") != "") banner(s.error, kWarnBg, kWarnText);
    if (rs.st.state == "cancelled") banner("Cancelled. Finished takes of that batch are below until the next run.", kInfoBg, kInfoText);
    std::string res = g.result("main");
    if (!res.empty() && !rs.busy) {
        if (auto r = ml::jobResult(res); r && r->contains("warnings") && (*r)["warnings"].is_array())
            for (auto& w : (*r)["warnings"])
                if (w.is_string()) banner(w.get<std::string>(), kWarnBg, kWarnText);
    }
}

// ------------------------------------------------------------------ the contact sheet
struct Tile {
    enum Kind { Done, Running, Waiting } kind = Done;
    std::string key;  // its run
    int index = 0;
    uint32_t seed = 0;
    int w = 0, h = 0;
    bool selectable = true;  // its run is the node's result (else: picked when the batch lands)
};

int gridFor(int n) { return n <= 1 ? 1 : n <= 4 ? 2 : n <= 9 ? 3 : 4; }

// Fit an image of w x h into a cell, centred.
void fitRect(ImVec2 c0, ImVec2 c1, int w, int h, ImVec2& p0, ImVec2& p1) {
    const float cw = c1.x - c0.x, ch = c1.y - c0.y;
    const float s = (w > 0 && h > 0) ? std::min(cw / w, ch / h) : 1.0f;
    const float iw = w > 0 ? w * s : cw, ih = h > 0 ? h * s : ch;
    p0 = ImVec2(c0.x + (cw - iw) * 0.5f, c0.y + (ch - ih) * 0.5f);
    p1 = ImVec2(p0.x + iw, p0.y + ih);
}

void inspectView(UiState& ui, ImageGenNode& g, SheetState& ss) {
    auto img = g.takeImage(ss.inspectIndex, ss.inspectKey);
    uint32_t seed = 0;
    auto ts = g.takes(ss.inspectKey);
    if (ts.empty()) ts = g.liveTakes();  // a take of the batch still running
    for (auto& t : ts)
        if (t.index == ss.inspectIndex) seed = t.seed;
    char head[128];
    std::snprintf(head, sizeof(head), "Take #%d  \xC2\xB7  %s  \xC2\xB7  %d x %d  \xC2\xB7  1:1", ss.inspectIndex + 1, seedText(seed).c_str(),
                  img ? img->w : 0, img ? img->h : 0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(head);
    ImGui::SameLine();
    if (pill("Back to sheet", false, "Double-click or Space also goes back")) ss.inspect = false;
    const float h = std::max(160.0f, std::min(ImGui::GetContentRegionAvail().y - 8, ImGui::GetContentRegionAvail().x * 1.1f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col(theme().viewerBg));
    ImGui::BeginChild("##inspect", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    if (const Tex* t = texture("full:" + ss.inspectKey + "#" + std::to_string(ss.inspectIndex), img, 0, true)) {
        ImGui::Image((ImTextureID)(intptr_t)t->id, ImVec2((float)t->w, (float)t->h));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ss.inspect = false;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {  // drag to pan
            ImVec2 d = ImGui::GetIO().MouseDelta;
            ImGui::SetScrollX(ImGui::GetScrollX() - d.x);
            ImGui::SetScrollY(ImGui::GetScrollY() - d.y);
        }
    } else {
        ImGui::TextDisabled("the take's pixels aren't in the ML cache any more");
    }
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) ui.imageGen.sheetHoverFrame = ImGui::GetFrameCount();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void contactSheet(App& app, UiState& ui, ImageGenNode& g, const RunState& rs) {
    SheetState& ss = gSheet[g.name];
    const std::string res = g.result("main"), job = g.job("main");
    const bool live = rs.busy && !job.empty() && job != res;

    // a take clicked mid-batch is selected once its run is the result
    if (!ss.pendingKey.empty() && ss.pendingKey == res) {
        selectTake(app, g.name, res, ss.pendingIndex);
        ss.pendingKey.clear();
    }
    if (ui.imageGen.spaceToggle && ui.imageGen.spaceNode == g.name) {  // Space over the sheet
        ui.imageGen.spaceToggle = false;
        if (ss.inspect) ss.inspect = false;
        else if (auto sel = g.selectedTakeIndex(); sel && !live) {
            ss.inspect = true;
            ss.inspectKey = res;
            ss.inspectIndex = *sel;
        }
    }

    std::vector<Tile> tiles;
    if (live) {
        imagegen::Live l = g.live();
        const int n = std::max({l.batchSize, (int)g.params.num("batch_count", 0, 1), 1});
        auto done = g.liveTakes();
        for (int i = 0; i < n; ++i) {
            Tile t;
            t.key = job;
            t.index = i;
            t.selectable = false;
            auto it = std::find_if(done.begin(), done.end(), [&](const imagegen::Take& k) { return k.index == i; });
            if (it != done.end()) {
                t.kind = Tile::Done;
                t.seed = it->seed;
                t.w = it->width;
                t.h = it->height;
            } else {
                t.kind = i == (int)done.size() ? Tile::Running : Tile::Waiting;
                t.w = (int)g.params.num("width", 0, 1024);
                t.h = (int)g.params.num("height", 0, 1024);
            }
            tiles.push_back(t);
        }
    } else {
        for (auto& k : g.takes()) {
            Tile t;
            t.key = res;
            t.index = k.index;
            t.seed = k.seed;
            t.w = k.width;
            t.h = k.height;
            tiles.push_back(t);
        }
    }

    // ---- the header: run picker, and Set as Source / Branch for the selected take
    std::optional<int> sel = live ? std::nullopt : g.selectedTakeIndex();
    {
        auto hist = g.history();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", live ? "BATCH" : "TAKES");
        if (!hist.empty() && !live) {
            ImGui::SameLine();
            int at = (int)(std::find(hist.begin(), hist.end(), res) - hist.begin());
            char preview[48];
            std::snprintf(preview, sizeof(preview), at < (int)hist.size() ? (at == 0 ? "latest run" : "run -%d") : "run", at);
            ImGui::SetNextItemWidth(std::min(110.0f, ImGui::GetContentRegionAvail().x * 0.3f));
            if (kit::beginComboFlat("##runs", preview)) {
                for (int i = 0; i < (int)hist.size(); ++i) {
                    auto ts = g.takes(hist[i]);
                    if (ts.empty()) continue;  // cleared from the cache
                    char b[96];
                    std::snprintf(b, sizeof(b), "%s  (%d take%s, seed %u)##%d", i == 0 ? "latest run" : ("run -" + std::to_string(i)).c_str(),
                                  (int)ts.size(), ts.size() == 1 ? "" : "s", ts.front().seed, i);
                    if (ImGui::Selectable(b, hist[i] == res)) selectTake(app, g.name, hist[i], 0);
                    if (ImGui::IsItemHovered() && ts.front().meta.contains("prompt"))
                        ImGui::SetTooltip("%s", ts.front().meta.value("prompt", std::string()).c_str());
                }
                ImGui::EndCombo();
            }
        }
        if (sel) {
            ImGui::SameLine();
            if (pill("Set as Source", false, "Promote the selected take: Auto runs Img2Img on it (denoise 1.0 becomes 0.55)"))
                promote(app, g.name, res, *sel);
            ImGui::SameLine();
            if (pill("Branch to New Node", false, "A new ImageGen below this one that refines the selected take (denoise 0.55)"))
                branch(app, g.name, res, *sel);
        }
    }

    if (ss.inspect) {
        inspectView(ui, g, ss);
        return;
    }
    if (tiles.empty()) {
        const float w = ImGui::GetContentRegionAvail().x, h = std::round(w * 0.36f);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), col(theme().cardBorder), 4, 0, 1.5f);
        const char* t = "Takes appear here as they finish";
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), col(theme().muted), t);
        ImGui::Dummy(ImVec2(w, h));
        return;
    }

    // ---- the grid
    const imagegen::PromotedTake pt = g.promotedTake();
    const int cols = gridFor((int)tiles.size());
    const float gap = 4, w = ImGui::GetContentRegionAvail().x;
    const float cell = std::floor((w - gap * (cols - 1)) / cols);
    float aspect = 1;  // the cells take the takes' shape (the first one's)
    if (tiles[0].w > 0 && tiles[0].h > 0) aspect = std::clamp((float)tiles[0].h / tiles[0].w, 0.4f, 1.6f);
    const float cellH = std::round(cell * aspect);
    const int rows = ((int)tiles.size() + cols - 1) / cols;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    bool anyHover = false;
    for (int k = 0; k < (int)tiles.size(); ++k) {
        const Tile& t = tiles[k];
        const ImVec2 c0(origin.x + (k % cols) * (cell + gap), origin.y + (k / cols) * (cellH + gap));
        const ImVec2 c1(c0.x + cell, c0.y + cellH);
        ImGui::SetCursorScreenPos(c0);
        ImGui::PushID(k);
        ImGui::InvisibleButton("##tile", ImVec2(cell, cellH));
        const bool hot = ImGui::IsItemHovered();
        anyHover |= hot;
        dl->AddRectFilled(c0, c1, col(theme().viewerBg), 3);
        ImVec2 p0, p1;
        fitRect(c0, c1, t.w, t.h, p0, p1);
        const Tex* tex = nullptr;
        if (t.kind == Tile::Done) tex = texture("thumb:" + t.key + "#" + std::to_string(t.index), g.takeImage(t.index, t.key), 320, true);
        else if (t.kind == Tile::Running) tex = texture("preview:" + t.key, g.livePreview(), 0, false);
        if (tex) dl->AddImage((ImTextureID)(intptr_t)tex->id, p0, p1);
        if (t.kind == Tile::Running) {  // the take being made: its preview, dimmed, with the step
            imagegen::Live l = g.live();
            dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, tex ? 60 : 0));
            char b[48];
            std::snprintf(b, sizeof(b), l.steps ? "step %d / %d" : "starting", l.step, l.steps);
            ImVec2 ts = ImGui::CalcTextSize(b);
            badge(dl, ImVec2((c0.x + c1.x - ts.x - 8) * 0.5f, (c0.y + c1.y - ts.y - 4) * 0.5f), b);
            const float fr = l.steps ? (float)l.step / l.steps : 0;
            dl->AddRectFilled(ImVec2(c0.x, c1.y - 3), ImVec2(c0.x + cell * fr, c1.y), themeAccent(255));
        } else if (t.kind == Tile::Waiting) {
            const char* b = "queued";
            ImVec2 ts = ImGui::CalcTextSize(b);
            dl->AddText(ImVec2((c0.x + c1.x - ts.x) * 0.5f, (c0.y + c1.y - ts.y) * 0.5f), col(theme().muted), b);
        }
        const bool isSel = sel && t.selectable && *sel == t.index;
        const bool isPending = !t.selectable && ss.pendingKey == t.key && ss.pendingIndex == t.index;
        const bool isSource = pt.active && pt.job == t.key && pt.index == t.index;
        if (isSel || isPending) dl->AddRect(ImVec2(c0.x - 1, c0.y - 1), ImVec2(c1.x + 1, c1.y + 1), themeAccent(isSel ? 255 : 140), 3, 0, 2.5f);
        else if (hot && t.kind == Tile::Done) dl->AddRect(c0, c1, withAlpha(col(theme().text), 90), 3, 0, 1.0f);
        if (isSource) badge(dl, ImVec2(c1.x - 4, c0.y + 4), "SOURCE", true);
        if (hot && t.kind == Tile::Done) {  // the take's number and seed
            badge(dl, ImVec2(c0.x + 4, c0.y + 4), ("#" + std::to_string(t.index + 1)).c_str());
            const std::string sd = seedText(t.seed);
            badge(dl, ImVec2(c0.x + 4, c1.y - ImGui::GetTextLineHeight() - 8), sd.c_str());
            ImGui::SetTooltip("Click: output this take  \xC2\xB7  Double-click / Space: 1:1  \xC2\xB7  Right-click: more");
        }
        if (t.kind == Tile::Done) {
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                if (t.selectable) selectTake(app, g.name, t.key, t.index);
                else {
                    ss.pendingKey = t.key;
                    ss.pendingIndex = t.index;
                }
            }
            if (hot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                ss.inspect = true;
                ss.inspectKey = t.key;
                ss.inspectIndex = t.index;
            }
            if (ImGui::BeginPopupContextItem("##tilemenu")) {
                if (ImGui::MenuItem("Set as Source")) promote(app, g.name, t.key, t.index);
                if (ImGui::MenuItem("Branch to New Node", nullptr, false, t.selectable)) branch(app, g.name, t.key, t.index);
                if (ImGui::MenuItem("Inspect 1:1")) {
                    ss.inspect = true;
                    ss.inspectKey = t.key;
                    ss.inspectIndex = t.index;
                }
                ImGui::EndPopup();
            }
        }
        // the selected tile carries its own two actions, small, along its bottom edge
        if (isSel && cell >= 150) {
            ImGui::SetCursorScreenPos(ImVec2(c0.x + 4, c1.y - ImGui::GetFrameHeight() - 2));
            if (pill("Source", isSource, "Set as Source")) promote(app, g.name, t.key, t.index);
            ImGui::SameLine(0, 4);
            if (pill("Branch", false, "Branch to New Node")) branch(app, g.name, t.key, t.index);
        }
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * cellH + (rows - 1) * gap));
    ImGui::Dummy(ImVec2(w, 4));
    if (anyHover) {
        ui.imageGen.sheetHoverFrame = ImGui::GetFrameCount();
        ui.imageGen.spaceNode = g.name;
    }
    if (live && ss.pendingIndex >= 0 && ss.pendingKey == job)
        ImGui::TextDisabled("Take #%d is selected when the batch finishes", ss.pendingIndex + 1);
}

// ------------------------------------------------------------------ Active Source chip
void sourceChip(App& app, ImageGenNode& g) {
    const imagegen::PromotedTake pt = g.promotedTake();
    if (!pt.active) return;
    char b[96];
    if (pt.found) std::snprintf(b, sizeof(b), "Active Source: Take #%d (Seed %u)", pt.index + 1, pt.seed);
    else std::snprintf(b, sizeof(b), "Active Source: Take #%d (gone from the cache)", pt.index + 1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 ts = ImGui::CalcTextSize(b);
    const float h = ImGui::GetFrameHeight() + 4, xw = h;
    const float w = ts.x + 20 + xw;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), pt.found ? themeAccent(46) : kErrBg, h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), pt.found ? themeAccent(220) : kErrText, h * 0.5f, 0, 1.5f);
    // its thumbnail would be nice but the chip stays one line: the sheet marks the tile SOURCE
    dl->AddText(ImVec2(p.x + 12, p.y + (h - ts.y) * 0.5f), pt.found ? col(theme().value) : kErrText, b);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - xw, p.y));
    ImGui::InvisibleButton("##clearsrc", ImVec2(xw, h));
    const bool hot = ImGui::IsItemHovered();
    const ImVec2 c(p.x + w - xw * 0.5f - 2, p.y + h * 0.5f);
    if (hot) dl->AddCircleFilled(c, h * 0.36f, withAlpha(col(theme().text), 50));
    const float r = h * 0.17f;
    dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col(theme().value), 1.6f);
    dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), col(theme().value), 1.6f);
    if (hot) ImGui::SetTooltip("Clear: back to the source input (or Txt2Img)");
    if (ImGui::IsItemClicked()) editGen(app, g.name, [](ImageGenNode& n, Comp&) { n.clearPromoted(); });
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(w, 4));
}

// ------------------------------------------------------------------ mode
void modeMenu(App& app, ImageGenNode& g) {
    const std::string cur = g.params.str("mode", "Auto");
    const std::string resolved = imagegen::modeName(g.effectiveMode());
    const std::string autoLabel = "Auto (" + resolved + ")";
    kit::rowLabel("mode", "Auto picks from what is connected: Inpaint with source + mask, Img2Img with a source or a promoted take, else Txt2Img");
    if (kit::beginCombo("##mode", cur == "Auto" ? autoLabel.c_str() : cur.c_str(), std::min(220.0f, ImGui::GetContentRegionAvail().x))) {
        for (const char* m : {"Auto", "Txt2Img", "Img2Img", "Inpaint"}) {
            std::string label = std::string(m) == "Auto" ? autoLabel : m;
            if (ImGui::Selectable(label.c_str(), cur == m) && cur != m) {
                std::string v = m;
                editGen(app, g.name, [v](ImageGenNode& n, Comp&) { n.params.data["mode"] = v; });
            }
        }
        ImGui::EndCombo();
    }
}

// ------------------------------------------------------------------ reference
// What FLUX.2 gets as reference images: the reference input, and the source when use_reference is on.
void referenceRow(ImageGenNode& g) {
    kit::rowLabel("reference", "A separate image FLUX.2 follows (a style, a character, a product): connect the node's "
                               "reference input. It never becomes the base image and doesn't change the mode.");
    std::string refs;
    if (Node* r = g.input(2)) refs = r->name;
    if (g.params.flag("use_reference", false) && (g.input(0) || g.promotedTake().active))
        refs += refs.empty() ? "the source" : " + the source";
    ImGui::AlignTextToFramePadding();
    if (refs.empty()) ImGui::TextDisabled("none (connect the reference input)");
    else ImGui::TextUnformatted(refs.c_str());
}

// ------------------------------------------------------------------ LoRA stack
void loraStack(App& app, ImageGenNode& g) {
    auto rows = imagegen::lorasOf(g.params);
    const auto choices = g.knobChoices("lora");
    const float rowH = ImGui::GetFrameHeightWithSpacing();
    const float h = std::max(1, std::min((int)rows.size(), 5)) * rowH + 6;
    bool changed = false, gesture = false;
    int remove = -1;
    if (!rows.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));  // the card shows through: tracks and boxes stay visible
        ImGui::BeginChild("##loras", ImVec2(0, h), ImGuiChildFlags_None, rows.size() > 5 ? 0 : ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorPos(ImVec2(0, 3));
        for (int i = 0; i < (int)rows.size(); ++i) {
            auto& r = rows[i];
            ImGui::PushID(i);
            ImGui::SetCursorPosX(0);
            bool en = r.enabled;
            if (ImGui::Checkbox("##on", &en)) {
                r.enabled = en;
                changed = gesture = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(en ? "On: applied" : "Off: kept in the stack, not applied");
            ImGui::SameLine(0, 4);
            const float avail = ImGui::GetContentRegionAvail().x;
            const float xw = ImGui::GetFrameHeight(), valW = 46;
            const float fileW = std::max(70.0f, (avail - xw - valW - 12) * 0.48f);
            const float sliderW = std::max(40.0f, avail - fileW - valW - xw - 12);
            const bool missing = !r.file.empty() && std::find(choices.begin(), choices.end(), r.file) == choices.end();
            std::string shown = r.file.empty() ? "pick a LoRA" : r.file;
            if (auto slash = shown.rfind('/'); slash != std::string::npos && shown.size() > 24) shown = shown.substr(slash + 1);
            ImGui::BeginDisabled(!en);
            if (missing) ImGui::PushStyleColor(ImGuiCol_Text, kErrText);
            if (kit::beginCombo("##file", shown.c_str(), fileW)) {
                if (missing) ImGui::PopStyleColor();
                if (choices.empty()) ImGui::TextDisabled("no .safetensors in the LoRAs folder");
                for (auto& f : choices)
                    if (ImGui::Selectable(f.c_str(), f == r.file)) {
                        r.file = f;
                        changed = gesture = true;
                    }
                ImGui::EndCombo();
            } else if (missing)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", missing ? ("missing: " + r.file).c_str() : r.file.empty() ? "choose a LoRA" : r.file.c_str());
            ImGui::SameLine(0, 4);
            double wv = r.weight;
            if (kit::valueField("##wv", &wv, valW, nullptr, false, 0.01, -2.0, 2.0)) {
                r.weight = std::clamp(std::round(wv * 100) / 100, -2.0, 2.0);
                changed = true;
            }
            gesture |= ImGui::IsItemActivated();
            ImGui::SameLine(0, 4);
            if (kit::tickSlider("##w", &wv, -2.0, 2.0, sliderW)) {
                r.weight = std::clamp(std::round(wv * 100) / 100, -2.0, 2.0);
                changed = true;
            }
            gesture |= ImGui::IsItemActivated();
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                r.weight = 1.0;
                changed = gesture = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine(0, 4);
            if (ImGui::Button("\xC3\x97##rm", ImVec2(xw, 0))) remove = i;  // ×
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove from the stack");
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    if (pill("+ Add LoRA", false, "Add a row to the stack (each row: on/off, file, weight -2..2)")) {
        std::string first = choices.empty() ? std::string() : choices.front();
        editGen(app, g.name, [first](ImageGenNode& n, Comp&) { n.addLora(first); });
    }
    if (!rows.empty()) {
        int on = (int)std::count_if(rows.begin(), rows.end(), [](const imagegen::LoraEntry& e) { return e.enabled && !e.file.empty(); });
        ImGui::SameLine();
        ImGui::TextDisabled("%d of %d on", on, (int)rows.size());
    }
    if (remove >= 0) editGen(app, g.name, [remove](ImageGenNode& n, Comp&) { n.removeLora(remove); });
    else if (changed)
        editGen(app, g.name, [rows](ImageGenNode& n, Comp&) { imagegen::setLoras(n.params, rows); }, gesture);
}

// ------------------------------------------------------------------ size
void sizeRow(App& app, ImageGenNode& g, Engine& e) {
    const int W = (int)g.params.num("width", 0, 1024), H = (int)g.params.num("height", 0, 1024);
    kit::rowLabel("aspect");
    const auto& presets = imagegen::aspectPresets();
    for (size_t i = 0; i < presets.size(); ++i) {
        const auto& a = presets[i];
        if (i) sameLineIfFits(a.label);
        char tip[48];
        std::snprintf(tip, sizeof(tip), "%d x %d", a.width, a.height);
        if (pill(a.label, W == a.width && H == a.height, tip)) {
            const int w = a.width, h = a.height;
            editGen(app, g.name, [w, h](ImageGenNode& n, Comp&) { n.setSize(w, h); });
        }
    }
    // Match Input: the promoted take's size, else the source input's (snapped to 16)
    int mw = 0, mh = 0;
    const imagegen::PromotedTake pt = g.promotedTake();
    if (pt.active && pt.found)
        for (auto& t : g.takes(pt.job))
            if (t.index == pt.index) mw = t.width, mh = t.height;
    if (!mw && g.input(0)) {
        Format f = g.input(0)->format(e, g.runFrame);
        mw = (int)std::lround(f.width * f.pixelAspect);
        mh = f.height;
    }
    kit::skipLabel();  // its own row, under the presets
    ImGui::BeginDisabled(!mw || !mh);
    char tip[96];
    std::snprintf(tip, sizeof(tip), mw ? "%d x %d, snapped to 16: %d x %d" : "connect a source or promote a take", mw, mh,
                  imagegen::snapSize(mw), imagegen::snapSize(mh));
    if (pill("Match Input", mw && imagegen::snapSize(mw) == W && imagegen::snapSize(mh) == H, tip)) {
        editGen(app, g.name, [mw, mh](ImageGenNode& n, Comp&) { n.setSize(mw, mh); });
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
}

}  // namespace

void drawImageGenPanel(App& app, UiState& ui, Node* n, const std::function<void(const char*)>& knob) {
    auto* g = dynamic_cast<ImageGenNode*>(n);
    if (!g) return;
    ImGui::PushID("imagegen");
    pruneTextures();
    Engine& e = app.uiEngine();
    const mlnodes::JobSpec s = g->spec("main", e);

    RunState rs;
    const std::string job = g->job("main"), res = g->result("main");
    rs.st = job.empty() || job == res ? ml::JobStatus{} : ml::jobStatus(job);
    const bool online = ml::workerInfo().online;
    rs.stale = rs.st.state == "running" && !online;
    MLExportStatus ex = app.mlExportStatus();
    rs.exporting = ex.running && ex.node == n->name;
    rs.busy = rs.exporting || rs.st.state == "queued" || (rs.st.state == "running" && !rs.stale);

    // ---- run: Generate / Cancel, progress, what went wrong
    generateBar(app, *g, s, rs);
    statusBanner(*g, s, rs);
    sourceChip(app, *g);
    contactSheet(app, ui, *g, rs);

    // ---- prompt and model
    if (kit::beginCard("##ig_prompt", "Prompt")) {
        knob("positive_prompt");
        knob("negative_prompt");
        modeMenu(app, *g);
        referenceRow(*g);
        ImGui::BeginDisabled(g->effectiveMode() == imagegen::Mode::Txt2Img);
        knob("denoise_strength");
        ImGui::EndDisabled();
    }
    kit::endCard();
    if (kit::beginCard("##ig_model", "Model")) {
        knob("checkpoint");
        if (auto m = ml::findCheckpoint(g->params.str("checkpoint", "")); m && !m->folder) knob("base");
    }
    kit::endCard();
    if (kit::beginCard("##ig_loras", "LoRAs")) loraStack(app, *g);
    kit::endCard();
    if (kit::beginCard("##ig_image", "Image")) {
        sizeRow(app, *g, e);
        knob("width");
        knob("height");
        knob("steps");
        knob("guidance_scale");
        knob("sampler");
        knob("seed");
        knob("seed_mode");
        knob("batch_count");
    }
    kit::endCard();

    // ---- the worker and the job's log (as every ML node)
    if (kit::beginCard("##ig_worker", "ML worker")) {
        drawMLWorker(app, "diffusers");
        const std::string shown = !job.empty() ? job : res;
        if (!shown.empty() && ImGui::TreeNode("log")) {
            std::string t = ml::logTail(shown, 14);
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", t.empty() ? "(no log yet)" : t.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TextDisabled("%s", ml::jobDir(shown).c_str());
            ImGui::TreePop();
        }
    }
    kit::endCard();
    ImGui::PopID();
}

}  // namespace strata::ui
