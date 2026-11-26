// Viewer: shows the viewed node at the current frame.
//
// Speed comes from three things:
//  * only the part of the image that is on screen is rendered, at a render
//    scale matched to the zoom (zoomed out to 40% renders at 1/2 resolution);
//  * renders are progressive (App), so a slow comp shows a quick preview first;
//  * exposure, gamma and channel views run in a shader, never on the CPU.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "GL.h"
#include "Panels.h"
#include "nodes/Cryptomatte.h"
#include "color/Color.h"
#include "ViewerGL.h"
#include "imgui_internal.h"

namespace strata::ui {

namespace {


// Layers present in a channel list: "rgba" first, then "<layer>" for "<layer>.<ch>".
std::vector<std::string> layersOf(const ChannelSet& cs) {
    std::vector<std::string> r = {"rgba"};
    std::set<std::string> seen;
    for (auto& c : cs) {
        auto dot = c.find('.');
        if (dot == std::string::npos) continue;
        std::string l = c.substr(0, dot);
        if (seen.insert(l).second) r.push_back(l);
    }
    return r;
}

// Channels of `layer` mapped to display R, G, B, A. A single-channel layer
// (depth.Z) shows as grey.
std::array<int, 4> layerChannels(const Image& img, const std::string& layer) {
    std::array<int, 4> m = {-1, -1, -1, -1};
    if (layer == "rgba" || layer == "rgb" || layer.empty()) {
        m = {img.ch("R"), img.ch("G"), img.ch("B"), layer == "rgb" ? -1 : img.ch("A")};
        return m;
    }
    std::vector<int> found;
    const char* names[4][3] = {{"R", "red", "X"}, {"G", "green", "Y"}, {"B", "blue", "Z"}, {"A", "alpha", "W"}};
    for (int k = 0; k < 4; ++k)
        for (auto* nm : names[k]) {
            int i = img.ch(layer + "." + nm);
            if (i >= 0) {
                m[k] = i;
                break;
            }
        }
    if (m[0] < 0 && m[1] < 0 && m[2] < 0) {  // e.g. depth.Z only, or oddly named channels
        for (size_t i = 0; i < img.channels.size(); ++i)
            if (img.channels[i].rfind(layer + ".", 0) == 0) found.push_back((int)i);
        if (!found.empty()) m = {found[0], found[0], found[0], found.size() > 1 ? found[1] : -1};
    }
    return m;
}

double pickScale(float zoom, bool fullRes) {
    if (fullRes || zoom > 0.5f) return 1.0;
    // Largest power-of-two reduction that still has at least one image pixel per screen pixel.
    double s = 1.0;
    while (s > 1.0 / 16 && zoom <= s * 0.5) s *= 0.5;
    return s;
}

}  // namespace

void viewerUpload(ViewerState& v) {
    v.dirtyTexture = false;
    v.hasB = false;
    const ImagePtr& img = v.res.image;
    if (!img || img->bounds.empty()) return;
    const int W = img->bounds.width(), H = img->bounds.height(), n = img->nch();
    std::array<int, 4> m = layerChannels(*img, v.layer);
    if (v.alphaChannel != "A") m[3] = img->ch(v.alphaChannel);
    const bool direct = n == 4 && m[0] == 0 && m[1] == 1 && m[2] == 2 && m[3] == 3;
    v.texBounds = img->bounds;
    v.texScale = v.res.scale;
    // A playback frame from the half-float cache (no compare): its stored half floats go to the
    // GPU, directly when they are rgba. At 4.6K the float copy and upload of every frame
    // (233 MB) was what held cached playback under 24 fps whenever the whole frame was at full
    // size (#96); this is half the bytes and no copy.
    const auto& hf = v.res.half;
    const bool wantB = v.compare != "off" && v.res.imageB && v.res.imageB->bounds == img->bounds;
    if (!wantB && hf && hf->bounds == img->bounds && hf->channels == img->channels && hf->data.size() == (size_t)W * H * n) {
        if (direct) {
            viewerUploadHalf(v.tex, v.texW, v.texH, W, H, hf->data.data());
            return;
        }
        static std::vector<uint16_t> hb;  // RGB plates, layers: gathered to rgba, still as halves
        hb.resize((size_t)W * H * 4);
        const uint16_t* hs = hf->data.data();
        uint16_t* hd = hb.data();
        const uint16_t one = 0x3c00, zero = 0;
        halfbits::split((size_t)W * H, [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i)
                for (int k = 0; k < 4; ++k) hd[i * 4 + k] = m[k] >= 0 ? hs[i * n + m[k]] : (k == 3 ? one : zero);
        });
        viewerUploadHalf(v.tex, v.texW, v.texH, W, H, hb.data());
        return;
    }
    static std::vector<float> buf, bb;
    buf.resize((size_t)W * H * 4);
    const float* src0 = img->data.data();
    float* d0 = buf.data();
    halfbits::split((size_t)W * H, [&](size_t b, size_t e) {  // over a few cores: a 4K frame is a lot of bytes
        if (direct) {
            std::copy(src0 + b * 4, src0 + e * 4, d0 + b * 4);
            return;
        }
        const float* src = src0 + b * n;
        float* d = d0 + b * 4;
        for (size_t i = b; i < e; ++i, src += n, d += 4)
            for (int k = 0; k < 4; ++k) d[k] = m[k] >= 0 ? src[m[k]] : (k == 3 ? 1.0f : 0.0f);
    });
    // the B buffer, over the same pixels, in the same layer
    const ImagePtr& ib = v.res.imageB;
    const bool haveB = v.compare != "off" && ib && ib->bounds == img->bounds;
    if (haveB) {
        std::array<int, 4> mb = layerChannels(*ib, v.layer);
        if (v.alphaChannel != "A") mb[3] = ib->ch(v.alphaChannel);
        bb.resize(buf.size());
        const int nb = ib->nch();
        const float* sb = ib->data.data();
        for (size_t i = 0; i < (size_t)W * H; ++i, sb += nb)
            for (int k = 0; k < 4; ++k) bb[i * 4 + k] = mb[k] >= 0 ? sb[mb[k]] : (k == 3 ? 1.0f : 0.0f);
        const std::string& c = v.compare;
        if (c == "side by side" || c == "above / below") {
            viewerUploadFloat(v.texB, v.texBW, v.texBH, W, H, bb.data());
            v.hasB = true;
        } else {
            // combined into the A texture
            const double s = v.res.scale, ang = v.wipeAngle * 3.14159265358979 / 180;
            const double nx = std::cos(ang), ny = std::sin(ang);  // the normal: B's side
            const float mix = std::clamp(v.wipeMix, 0.0f, 1.0f);
            const Box& bx = img->bounds;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const size_t i = ((size_t)y * W + x) * 4;
                    float* a = &buf[i];
                    const float* q = &bb[i];
                    if (c == "wipe") {
                        const double X = (bx.x1 + x + 0.5) / s - v.wipeC.x, Y = (bx.y1 + y + 0.5) / s - v.wipeC.y;
                        if (X * nx + Y * ny > 0)
                            for (int k = 0; k < 4; ++k) a[k] += (q[k] - a[k]) * mix;
                    } else if (c == "over") {
                        const float t = 1 - a[3];
                        for (int k = 0; k < 4; ++k) a[k] += q[k] * t;
                    } else if (c == "under") {
                        const float t = 1 - q[3];
                        for (int k = 0; k < 4; ++k) a[k] = q[k] + a[k] * t;
                    } else if (c == "minus") {
                        for (int k = 0; k < 3; ++k) a[k] -= q[k];
                    } else if (c == "onion skin") {
                        for (int k = 0; k < 4; ++k) a[k] = (a[k] + q[k]) * .5f;
                    } else if (c == "difference") {
                        for (int k = 0; k < 3; ++k) a[k] = std::fabs(a[k] - q[k]);
                        a[3] = 1;
                    }
                }
        }
    }
    viewerUploadFloat(v.tex, v.texW, v.texH, W, H, buf.data());
}

// The gap between A and B side by side, and where B sits (full-res pixels).
static ImVec2 sideOffset(const ViewerState& v) {
    const Format& f = v.res.format;
    if (v.compare == "side by side") return ImVec2((float)(f.width + std::max(8, f.width / 50)), 0);
    if (v.compare == "above / below") return ImVec2(0, -(float)(f.height + std::max(8, f.height / 50)));
    return ImVec2(0, 0);
}

void viewerFit(ViewerState& v, ImVec2 size) {
    const Format& f = v.res.format;
    if (f.width <= 0 || f.height <= 0 || size.x < 10 || size.y < 10) return;
    const ImVec2 off = sideOffset(v);
    const float w = f.width + std::fabs(off.x), h = f.height + std::fabs(off.y);
    v.zoom = std::min((size.x - 40) / w, (size.y - 40) / h);
    v.zoom = std::max(v.zoom, 0.01f);
    v.pan = ImVec2(f.width * 0.5f + off.x * 0.5f, f.height * 0.5f + off.y * 0.5f);
    v.fitPending = false;
}

// The viewer's display transform: the comp's working space through the
// display / view chosen in the toolbar (stored with the comp, as is standard viewer process).
std::shared_ptr<const color::GpuShader> viewerTransform(App& app) {
    if (!app.comp || !color::haveOCIO()) return nullptr;
    const auto& pr = app.comp->project;
    std::string cfg = color::configOf(app.comp.get());
    std::string d = pr.viewerDisplay.empty() ? color::defaultDisplay(cfg) : pr.viewerDisplay;
    std::string view = pr.viewerView.empty() ? color::defaultView(cfg, d) : pr.viewerView;
    if (view == "Raw") return nullptr;
    std::string err;
    auto sh = color::gpuDisplay(cfg, color::workingOf(app.comp.get()), d, view, &err);
    static std::string lastErr;
    if (!sh && err != lastErr) {
        lastErr = err;
        app.message("viewer colour: " + err);
    }
    return sh;
}

void viewerProcessCombo(App& app) {
    if (!app.comp) return;
    std::string cfg = color::configOf(app.comp.get());
    auto& pr = app.comp->project;
    std::string d = pr.viewerDisplay.empty() ? color::defaultDisplay(cfg) : pr.viewerDisplay;
    std::string view = pr.viewerView.empty() ? color::defaultView(cfg, d) : pr.viewerView;
    ImGui::SetNextItemWidth(130);
    if (ImGui::BeginCombo("##viewproc", view.c_str(), ImGuiComboFlags_HeightLarge)) {
        auto set = [&](const std::string& dd, const std::string& vv) {
            app.edit([dd, vv](Comp& c) {
                c.project.viewerDisplay = dd;
                c.project.viewerView = vv;
            });
        };
        for (auto& disp : color::displays(cfg)) {
            ImGui::SeparatorText(disp.c_str());
            ImGui::PushID(disp.c_str());
            for (auto& vv : color::views(cfg, disp))
                if (ImGui::Selectable(vv.c_str(), disp == d && vv == view)) set(disp, vv);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Viewer transform (OCIO): %s, %s\nThe image is in %s", d.c_str(), view.c_str(),
                          color::workingOf(app.comp.get()).c_str());
}

ImVec2 gTestDeepHover(-1, -1);

// A deep node's samples at full-resolution pixel (x, y): their number on the info bar, and the
// list in a tooltip. They're rendered once the cursor has rested on the pixel for a moment.
static void drawDeepReadout(App& app, ViewerState& v, int x, int y) {
    const ViewResult& res = v.res;
    const double now = ImGui::GetTime();
    if (x != v.deepX || y != v.deepY || res.frame != v.deepFrame || res.content != v.deepContent || res.node != v.deepNode) {
        v.deepX = x, v.deepY = y, v.deepFrame = res.frame, v.deepContent = res.content, v.deepNode = res.node;
        v.deepSince = now;
        v.deepAsked = false;
        v.deepAt.reset();
        return;
    }
    if (!v.deepAsked) {
        if (now - v.deepSince < 0.3 || ImGui::IsAnyMouseDown()) return;
        v.deepAsked = true;
        v.deepAt = app.deepSamplesAt(res.node, x, y, res.frame);
    }
    if (!v.deepAt) return;
    const DeepImage& d = *v.deepAt;
    const int n = d.count(x, y);
    ImGui::SameLine(0, 16);
    ImGui::TextDisabled("deep: %d sample%s", n, n == 1 ? "" : "s");
    if (!n) return;
    const int R = d.ch("R"), G = d.ch("G"), B = d.ch("B");
    ImGui::BeginTooltip();
    ImGui::TextDisabled("%d deep sample%s at %d, %d (front to back)", n, n == 1 ? "" : "s", x, y);
    if (ImGui::BeginTable("deep_samples", 7, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        for (const char* h : {"Z", "ZBack", "R", "G", "B", "A", "shows"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        const float* s = d.sample(x, y);
        double T = 1;
        const int shown = std::min(n, 32);
        for (int k = 0; k < shown; ++k, s += d.nch()) {
            const float a = s[d.ai];
            const double vis = T * std::clamp((double)a, 0.0, 1.0);
            T *= 1 - std::clamp((double)a, 0.0, 1.0);
            const float vals[6] = {s[d.zi], s[d.zbi], R >= 0 ? s[R] : 0.0f, G >= 0 ? s[G] : 0.0f, B >= 0 ? s[B] : 0.0f, a};
            ImGui::TableNextRow();
            for (int c = 0; c < 6; ++c) {
                ImGui::TableSetColumnIndex(c);
                if (c < 2 && std::isinf(vals[c])) ImGui::TextUnformatted("inf");
                else ImGui::Text(c < 2 ? "%.4g" : "%.4f", vals[c]);
            }
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%.1f%%", vis * 100);
        }
        ImGui::EndTable();
    }
    if (n > 32) ImGui::TextDisabled("... and %d more", n - 32);
    ImGui::EndTooltip();
}

// Cryptomatte / focus picking: one click in the viewer, read from the node's input.
static void pickSpecial(App& app, PickerState& pk, int x, int y) {
    Node* n = app.node(pk.node);
    if (!n) return;
    std::string name = pk.node;
    ImagePtr im = app.pickImage(name, 0, Box(x, y, x + 1, y + 1), app.frame);
    if (!im || !im->bounds.contains(x, y)) return;
    const float* px = im->px(x, y);
    if (pk.mode == "focus") {
        std::string dch = n->params.str("depth", "depth.Z");
        int zc = im->ch(dch);
        if (zc < 0) { app.message(name + ": no " + dch + " at that pixel"); return; }
        float v = px[zc];
        std::string math = n->params.str("math", "1/z");
        double d = math == "z" ? v : v > 0 ? 1.0 / v : 0;
        if (math == "direct" || d <= 0) { app.message(name + ": nothing to focus on there"); return; }
        app.checkpoint();
        app.edit([name, d](Comp& c) {
            if (Node* m = c.find(name)) {
                m->params.data["focus_plane"] = d;
                m->params.data["use_focus_point"] = false;
                m->paramsChanged();
            }
        });
        char msg[128];
        std::snprintf(msg, sizeof msg, "%s: focus plane %.4g", name.c_str(), d);
        app.message(msg);
        return;
    }
    crypto::Layer L = crypto::findLayer(im->channels, n->params.str("layer", "cryptoobject"));
    float id;
    if (L.id.empty() || !crypto::topId(px, L, id)) { app.message(name + ": no cryptomatte id there"); return; }
    std::string entry = crypto::nameFor(id);
    bool remove = pk.mode == "crypto remove" || ImGui::GetIO().KeyCtrl;
    app.checkpoint();
    app.edit([name, entry, id, remove](Comp& c) {
        Node* m = c.find(name);
        if (!m) return;
        auto items = crypto::splitList(m->params.str("matte_list", ""));
        auto same = [&](const std::string& e) {
            float o;
            return crypto::entryId(e, o) && crypto::bitsOf(o) == crypto::bitsOf(id);
        };
        items.erase(std::remove_if(items.begin(), items.end(), same), items.end());
        if (!remove) items.push_back(entry);
        m->params.data["matte_list"] = crypto::joinList(items);
        m->paramsChanged();
    });
    app.message(name + (remove ? ": removed " : ": added ") + entry);
}

// The viewer's pipes in the toolbar: A (shown), B (compared) and how they're compared.
static std::string pipeLabel(Node* vn, int pipe) {
    Node* x = vn && pipe >= 0 ? vn->input(App::pipeIndex(pipe)) : nullptr;
    return std::to_string(pipe) + "  " + (x ? x->name : std::string("-"));
}

static void pipeCombos(App& app, Node* vn, int aPipe, int bPipe, ViewerState& v) {
    if (!vn) {
        ImGui::TextDisabled("(no viewer: press 1)");
        return;
    }
    const std::string vname = vn->name;
    auto list = [&](const char* id, int cur, bool isB) {
        std::string label = (isB ? "B  " : "A  ") + (cur < 0 ? std::string("-") : vn->input(App::pipeIndex(cur)) ? vn->input(App::pipeIndex(cur))->name : std::string("-"));
        ImGui::SetNextItemWidth(110);
        if (ImGui::BeginCombo(id, label.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (isB && ImGui::Selectable("none", cur < 0)) app.setViewerParam(vname, "b_pipe", -1);
            for (int k = 1; k <= 10; ++k) {
                const int pipe = k % 10;
                if (!vn->input(App::pipeIndex(pipe))) continue;
                if (ImGui::Selectable(pipeLabel(vn, pipe).c_str(), cur == pipe)) {
                    if (isB) {
                        app.setViewerParam(vname, "b_pipe", pipe);
                        if (v.compare == "off") app.setViewerParam(vname, "compare", "wipe");
                    } else {
                        app.setViewerParam(vname, "a_pipe", pipe);
                    }
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled(isB ? "Shift+1-9, 0 over the viewer or graph" : "1-9, 0: select a node, press its pipe number");
            ImGui::EndCombo();
        }
    };
    list("##apipe", aPipe, false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A: the pipe shown (number keys)");
    ImGui::SameLine(0, 4);
    static const char* modes[] = {"off", "wipe", "over", "under", "minus", "onion skin", "difference", "side by side", "above / below"};
    const std::string cur = vn->params.str("compare", "off");
    ImGui::SetNextItemWidth(84);
    if (ImGui::BeginCombo("##cmpmode", cur == "off" ? "-" : cur == "above / below" ? "stack" : cur.c_str())) {
        for (const char* m : modes)
            if (ImGui::Selectable(std::string(m) == "off" ? "-" : std::string(m) == "above / below" ? "stack" : m, cur == m)) {
                app.setViewerParam(vname, "compare", m);
                if (std::string(m) != "off" && bPipe < 0)  // no B yet: the next connected pipe
                    for (int step = 1; step < 10; ++step) {
                        const int i = (App::pipeIndex(aPipe) + step) % 10;
                        if (vn->input(i)) { app.setViewerParam(vname, "b_pipe", (i + 1) % 10); break; }
                    }
                v.fitPending = std::string(m) == "side by side" || std::string(m) == "above / below" || v.compare == "side by side" ||
                               v.compare == "above / below";
            }
        ImGui::Separator();
        if (bPipe >= 0 && ImGui::Selectable("Swap A / B")) {
            app.setViewerParam(vname, "a_pipe", bPipe);
            app.setViewerParam(vname, "b_pipe", aPipe);
        }
        if (cur == "wipe") {
            float mix = (float)vn->params.num("wipe_mix", 0, 1);
            if (ImGui::SliderFloat("wipe mix", &mix, 0, 1)) app.setViewerParam(vname, "wipe_mix", mix);
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How A and B are compared (W: wipe on / off)\nwipe: drag the line's centre to move it, its handle to turn it\n"
                          "over / under: A over B, B over A\nminus: A - B   difference: |A - B| (black: the same)\n"
                          "side by side / above / below: next to each other");
    ImGui::SameLine(0, 2);
    list("##bpipe", bPipe, true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("B: comparison input (Shift+number)");

}

// The wipe: a line through its centre (drag it), a handle to turn it, A on one side, B the other.
static void wipeHandles(App& app, Node* vn, ViewerState& v, ImDrawList* dl, const ViewXf& xf, const std::string& nodeA,
                        const std::string& nodeB) {
    if (!vn) return;
    ImGuiIO& io = ImGui::GetIO();
    const double a = v.wipeAngle * 3.14159265358979 / 180;
    const ImVec2 c = xf.toScreen(v.wipeC.x, v.wipeC.y);
    const ImVec2 n((float)std::cos(a), -(float)std::sin(a));  // B's side, on screen
    const ImVec2 d(-n.y, n.x);                                 // along the line
    const ImVec2 rh(c.x + n.x * 60, c.y + n.y * 60);
    const float dc = std::hypot(io.MousePos.x - c.x, io.MousePos.y - c.y), dr = std::hypot(io.MousePos.x - rh.x, io.MousePos.y - rh.y);
    if (v.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        if (dr < 9) v.wipeDrag = 2;
        else if (dc < 10) v.wipeDrag = 1;
    }
    if (v.wipeDrag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) v.wipeDrag = 0;
    if (v.wipeDrag == 1) {
        const ImVec2 mi = xf.toImage(io.MousePos);
        app.setViewerParam(vn->name, "wipe_center", json::array({std::round(mi.x), std::round(mi.y)}));
    } else if (v.wipeDrag == 2) {
        const ImVec2 mi = xf.toImage(io.MousePos);
        double deg = std::atan2(mi.y - v.wipeC.y, mi.x - v.wipeC.x) * 180 / 3.14159265358979;
        if (io.KeyShift) deg = std::round(deg / 15) * 15;  // Shift: 15 degree steps
        app.setViewerParam(vn->name, "wipe_angle", std::round(deg * 10) / 10);
    }
    const ImU32 w = IM_COL32(255, 255, 255, 190), hot = IM_COL32(255, 230, 40, 255);
    dl->AddLine(ImVec2(c.x - d.x * 5000, c.y - d.y * 5000), ImVec2(c.x + d.x * 5000, c.y + d.y * 5000), w, 1.5f);
    dl->AddCircle(c, 7, v.wipeDrag == 1 || (dc < 10 && !v.wipeDrag) ? hot : w, 0, 2);
    dl->AddLine(c, rh, w, 1.0f);
    dl->AddCircleFilled(rh, 5, v.wipeDrag == 2 || (dr < 9 && !v.wipeDrag) ? hot : w);
    const std::string la = "A " + nodeA, lb = "B " + nodeB;
    const ImVec2 sa = ImGui::CalcTextSize(la.c_str());
    dl->AddText(ImVec2(c.x - n.x * 24 - sa.x * (n.x > 0 ? 1.0f : 0.0f) + d.x * 40, c.y - n.y * 24 + d.y * 40 - 8), themeAccent(230), la.c_str());
    dl->AddText(ImVec2(c.x + n.x * 24 - ImGui::CalcTextSize(lb.c_str()).x * (n.x < 0 ? 1.0f : 0.0f) + d.x * 40, c.y + n.y * 24 + d.y * 40 - 8),
                IM_COL32(90, 170, 255, 230), lb.c_str());
}

// Small line icons and two tightly packed rows, matching the reference Viewer strip.
static bool viewerToolIcon(const char* id, int shape, bool on, const char* tip) {
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##icon", ImVec2(20, 20));
    const ImVec2 p = ImGui::GetItemRectMin();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImU32 c = on ? IM_COL32(245, 183, 77, 255) : IM_COL32(158, 158, 158, 255);
    if (ImGui::IsItemHovered()) d->AddRectFilled(p, ImVec2(p.x + 20, p.y + 20), IM_COL32(78, 78, 78, 255));
    auto pt = [&](float x, float y) { return ImVec2(p.x + x, p.y + y); };
    auto line = [&](float x, float y, float X, float Y) { d->AddLine(pt(x,y), pt(X,Y), c, 1.4f); };
    if (shape == 0) { d->AddRect(pt(3,5), pt(17,15),c); d->AddRect(pt(5,7),pt(15,13),c); }
    else if (shape == 1) { d->AddRect(pt(5,6),pt(16,15),c); line(3,3,3,13); line(3,3,14,3); }
    else if (shape == 2) { for (int i=0;i<3;++i) line(4+i*4,15,8+i*4,5); }
    else if (shape == 3) { d->AddRect(pt(3,8),pt(13,16),c); d->AddRect(pt(7,4),pt(17,12),c); }
    else if (shape == 4) { for(int i=0;i<4;++i) line(3,5+i*3,17,5+i*3); }
    else if (shape == 5) { d->AddCircle(pt(10,10),6,c,16,1.5f); line(9,3,12,6); line(12,6,14,2); line(10,6,8,14); }
    else if (shape == 6) { d->AddRect(pt(5,5),pt(15,15),c); line(10,2,10,6); line(10,14,10,18); line(2,10,6,10); line(14,10,18,10); }
    else if (shape == 7) { d->AddRectFilled(pt(5,4),pt(8,16),c); d->AddRectFilled(pt(12,4),pt(15,16),c); }
    else if (shape == 8) { line(5,6,10,11); line(10,11,15,6); line(5,11,10,16); line(10,16,15,11); }
    else if(shape==9) { line(6,3,16,8); line(8,5,6,11); line(14,8,11,13); line(6,11,11,13); line(9,12,4,18); }
    else { for(int i=3;i<17;i+=4){line(i,4,i+2,4);line(i,16,i+2,16);line(3,i,3,i+2);line(17,i,17,i+2);} }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",tip);
    ImGui::PopID();
    return clicked;
}

// Labeled ruler with a narrow rectangular handle. Numeric entry and double-click reset
// use the same value as dragging the scale; all adjustments stay in the display shader.
static void viewerRuler(const char* label, float& value, float width, int kind) {
    ImGui::PushID(label);
    const float startX = ImGui::GetCursorScreenPos().x;
    auto stepArrow = [&](bool up) {
        if (ImGui::InvisibleButton(up ? "##up" : "##down", ImVec2(10,20))) value=std::clamp(value*(up?2.f:.5f),1.f/64,64.f);
        ImVec2 p=ImGui::GetItemRectMin();
        ImDrawList* d=ImGui::GetWindowDrawList();
        d->AddTriangleFilled(ImVec2(p.x+(up?8:2),p.y+10),ImVec2(p.x+(up?2:8),p.y+5),ImVec2(p.x+(up?2:8),p.y+15),IM_COL32(170,170,170,255));
        if(ImGui::IsItemHovered())ImGui::SetTooltip(up?"Increase exposure by one stop":"Decrease exposure by one stop");
        ImGui::SameLine(0,2);
    };
    if(kind==0)stepArrow(false);
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label);
    if(kind==0){ImGui::SameLine(0,2);stepArrow(true);}
    ImGui::SameLine(0,3); ImGui::SetNextItemWidth(44);
    ImGui::InputFloat("##value", &value, 0, 0, "%.3g");
    value = std::clamp(value, kind == 0 ? 1.f/64 : kind == 1 ? .01f : -4.f, kind == 0 ? 64.f : 8.f);
    ImGui::SameLine(0,3);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = std::max(80.f,width - (p.x-startX) - 8);
    ImGui::InvisibleButton("##scale",ImVec2(w,24));
    const std::vector<float> values = kind == 0 ? std::vector<float>{1.f/64,.1f,1,10,64} :
        kind == 1 ? std::vector<float>{.2f,.3f,.5f,.7f,1,2,3,4,5} : std::vector<float>{0,.1f,.4f,.7f,1,2,3,4};
    const std::vector<float> positions = kind == 0 ? std::vector<float>{0,.22f,.5f,.82f,1} :
        kind == 1 ? std::vector<float>{0,.1f,.2f,.3f,.5f,.67f,.79f,.9f,1} : std::vector<float>{0,.17f,.32f,.43f,.5f,.68f,.84f,1};
    auto map = [](float x, const std::vector<float>& from, const std::vector<float>& to) {
        x=std::clamp(x,from.front(),from.back());
        size_t i=1; while(i+1<from.size() && x>from[i])++i;
        return to[i-1]+(to[i]-to[i-1])*(x-from[i-1])/(from[i]-from[i-1]);
    };
    auto fromT = [&](float t) {return map(t,positions,values);};
    auto toT = [&](float x) {return map(x,values,positions);};
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(0)) value = fromT(std::clamp((ImGui::GetIO().MousePos.x-p.x)/w,0.f,1.f));
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) value=1;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: drag ruler or type a value; double-click resets to 1",kind==0?"Brightness (gain)":kind==1?"Gamma":"Saturation");
    ImDrawList* d=ImGui::GetWindowDrawList();
    d->AddLine(ImVec2(p.x,p.y+10),ImVec2(p.x+w,p.y+10),IM_COL32(12,12,12,255));
    for(int i=0;i<=32;++i) {
        const float x=p.x+w*i/32.f;
        d->AddLine(ImVec2(x,p.y+(i%4?5:2)),ImVec2(x,p.y+10),IM_COL32(19,19,19,255));
    }
    for(float tick:values) {
        char b[16]; std::snprintf(b,sizeof b,"%g",tick);
        const float x=p.x+w*toT(tick);
        const float tw=ImGui::CalcTextSize(b).x*.75f;
        d->AddText(ImGui::GetFont(),9,ImVec2(std::clamp(x-tw*.5f,p.x,p.x+w-tw),p.y+12),IM_COL32(17,17,17,255),b);
    }
    const float x=p.x+w*std::clamp(toT(value),0.f,1.f);
    d->AddRectFilled(ImVec2(x-2,p.y+3),ImVec2(x+2,p.y+14),IM_COL32(124,111,58,255));
    d->AddRect(ImVec2(x-2,p.y+3),ImVec2(x+2,p.y+14),IM_COL32(12,12,12,255));
    ImGui::PopID();
}

static void drawReferenceToolbar(App& app, UiState& ui, ViewerState& v, Node* vn, int aPipe, int bPipe, bool active) {
    ImGui::PushFont(theme().regular,14);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(4,2));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(2,1));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(2,1));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,IM_COL32(49,49,49,255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,IM_COL32(43,43,43,255));
    ImGui::PushStyleColor(ImGuiCol_Button,IM_COL32(49,49,49,255));
    ImGui::PushStyleColor(ImGuiCol_Border,IM_COL32(25,25,25,255));
    const float width=std::max(1120.f,ImGui::GetContentRegionAvail().x-4);
    ImGui::SetNextWindowContentSize(ImVec2(width,51));
    ImGui::BeginChild("##viewerTop",ImVec2(0,ImGui::GetContentRegionAvail().x<1124?72:55),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetNextItemWidth(64);
    if(ImGui::BeginCombo("##layer",v.layer.c_str())) {
        for(const char* layer:{"none","rgb","rgba"}) if(ImGui::Selectable(layer,v.layer==layer)) {v.layer=layer;v.dirtyTexture=true;}
        ImGui::SeparatorText("other layers");
        for(auto& layer:v.layers) if(layer!="rgba" && ImGui::Selectable(layer.c_str(),v.layer==layer)){v.layer=layer;v.dirtyTexture=true;}
        ImGui::EndCombo();
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(99);
    const std::string alphaLabel=v.alphaChannel=="A"?"rgba.alpha":v.alphaChannel;
    if(ImGui::BeginCombo("##alpha",alphaLabel.c_str())) {
        for(auto c:std::vector<std::string>{"R","G","B","A"}) {
            const std::string label=c=="R"?"rgba.red":c=="G"?"rgba.green":c=="B"?"rgba.blue":"rgba.alpha";
            if(ImGui::Selectable(label.c_str(),v.alphaChannel==c)){v.alphaChannel=c;v.dirtyTexture=true;}
        }
        if(v.res.image) for(auto& c:v.res.image->channels) if(c.find('.')!=std::string::npos && ImGui::Selectable(c.c_str(),v.alphaChannel==c)){v.alphaChannel=c;v.dirtyTexture=true;}
        ImGui::EndCombo();
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Channel used for alpha display and matte overlay");
    ImGui::SameLine(); ImGui::SetNextItemWidth(57);
    const char* names[]={"RGB","R","G","B","A","Y"};
    if(ImGui::BeginCombo("##channels",names[v.channel])){for(int i=0;i<6;++i)if(ImGui::Selectable(names[i],v.channel==i))v.channel=i;ImGui::EndCombo();}
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text,v.inputProcess?IM_COL32(185,185,185,255):IM_COL32(90,90,90,255));
    if(ImGui::Button("IP"))v.inputProcess=!v.inputProcess;
    ImGui::PopStyleColor();
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Enable / bypass the Viewer display process");
    ImGui::SameLine(); ImGui::SetNextItemWidth(76);
    const char* transfers[]={"sRGB","None","rec709","rec1886"};
    if(ImGui::BeginCombo("##transfer",transfers[v.displayTransfer])) {
        for(int i=0;i<4;++i)if(ImGui::Selectable(transfers[i],v.displayTransfer==i))v.displayTransfer=i;
        if(color::haveOCIO()){ImGui::Separator();viewerProcessCombo(app);}
        ImGui::EndCombo();
    }
    ImGui::SameLine(std::max(365.f,(width-310)*.5f));
    pipeCombos(app,vn,aPipe,bPipe,v);
    ImGui::SameLine(width-349);
    if(viewerToolIcon("safe",0,v.safeZone,"Safe zones: 90% action and 80% title"))v.safeZone=!v.safeZone; ImGui::SameLine();
    if(viewerToolIcon("crop",1,v.cropToFormat,"Crop display to project format"))v.cropToFormat=!v.cropToFormat; ImGui::SameLine();
    if(viewerToolIcon("proxy",2,v.proxy,"Proxy rendering: use the chosen 1:N resolution")){v.proxy=!v.proxy;if(v.proxy&&v.resolution==1)v.resolution=2;v.reqScale=0;} ImGui::SameLine();
    if(viewerToolIcon("full",3,v.fullBuffer,"Full buffer: render and cache the complete frame")){v.fullBuffer=!v.fullBuffer;v.reqScale=0;} ImGui::SameLine();
    if(viewerToolIcon("cache",4,app.precaching(),"Cache the playback range in the background"))app.setPrecache(!app.precaching()); ImGui::SameLine();
    if(viewerToolIcon("refresh",5,app.rendering(),"Refresh Viewer: clear cached buffers and render again")){app.clearCaches();app.requestView(true);} ImGui::SameLine();
    if(viewerToolIcon("roi",6,v.roi,"Region of interest: click to toggle; right-click to edit bounds")){v.roi=!v.roi;if(v.roiBox.empty()){const auto f=v.res.format;v.roiBox=Box(f.width/4,f.height/4,f.width*3/4,f.height*3/4);}v.reqScale=0;}
    if(ImGui::BeginPopupContextItem("##roiBounds")){
        int xy[2]={v.roiBox.x1,v.roiBox.y1}, wh[2]={v.roiBox.width(),v.roiBox.height()};
        if(ImGui::InputInt2("origin",xy)|ImGui::InputInt2("size",wh)){v.roiBox=Box(xy[0],xy[1],xy[0]+std::max(1,wh[0]),xy[1]+std::max(1,wh[1]));v.reqScale=0;}
        ImGui::EndPopup();
    } ImGui::SameLine();
    if(viewerToolIcon("pause",7,app.viewPaused,"Pause / resume Viewer updates"))app.viewPaused=!app.viewPaused; ImGui::SameLine();
    float zoom=v.zoom*100; ImGui::SetNextItemWidth(58);
    if(ImGui::DragFloat("##zoom",&zoom,.5f,2,6400,"%.1f%%")){v.zoom=zoom/100;v.fitPending=false;}
    if(ImGui::IsItemHovered()&&ImGui::IsMouseDoubleClicked(0))v.fitPending=true;
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Viewer zoom; double-click to fit (F)");
    ImGui::SameLine();ImGui::SetNextItemWidth(65);
    char r[12];std::snprintf(r,sizeof r,"1:%d",v.resolution);
    if(ImGui::BeginCombo("##res",r)){for(int i:{1,2,4,8,16,32}){char b[12];std::snprintf(b,sizeof b,"1:%d",i);if(ImGui::Selectable(b,v.resolution==i)){v.resolution=i;v.proxy=i>1;v.fullRes=i==1;v.reqScale=0;}}ImGui::EndCombo();}
    ImGui::SameLine();if(viewerToolIcon("settings",8,v.openSettings,"Viewer settings"))v.openSettings=true;
    ImGui::SetCursorPos(ImVec2(2,26));
    const float rulerWidth=(width-125)/3;
    float gain=std::pow(2.f,v.fstop);viewerRuler("f/8",gain,rulerWidth,0);v.fstop=std::log2(gain);
    ImGui::SameLine(rulerWidth+2);viewerRuler("\xCE\xB3",v.gamma,rulerWidth,1);
    ImGui::SameLine(rulerWidth*2+2);viewerRuler("S",v.saturation,rulerWidth,2);
    ImGui::SameLine(width-118);
    if(viewerToolIcon("overlays",10,v.overlays,"Toggle Viewer overlays (Q)"))v.overlays=!v.overlays;
    ImGui::SameLine();ImGui::SetNextItemWidth(54);
    if(ImGui::BeginCombo("##dimension","2D")){if(ImGui::Selectable("2D",true)){}if(ImGui::Selectable("3D")&&active){ui.v3d.on=true;ui.v3d.force2D=false;}ImGui::EndCombo();}
    ImGui::SameLine();
    ImGui::BeginDisabled(!vn);
    if(viewerToolIcon("pin",9,vn && vn->params.flag("lock_inputs",false),"Pin Viewer inputs: prevent number keys from replacing connected nodes") && vn)
        app.setViewerParam(vn->name,"lock_inputs",!vn->params.flag("lock_inputs",false));
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::PopStyleColor(4);ImGui::PopStyleVar(4);ImGui::PopFont();
}

// One viewer panel: `vn` is its Viewer node (may be null: nothing to look through yet); `active`:
// it is the active viewer (main renderer, overlays, tools).
static void drawViewerPanel(App& app, UiState& ui, ViewerState& v, Node* vn, bool active, const std::string& winName) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(winName.c_str(), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ui.noteHover("Viewer");
    const std::string vname = vn ? vn->name : "";
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        ui.hoverViewer = vname;
        // a click anywhere in another viewer's panel makes it the active one
        if (!active && vn && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
                              ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
            ui.activateNext = vname;
    }
    // what this viewer looks at: its A pipe, and B when comparing
    std::string nodeA, nodeB;
    int aPipe = 1, bPipe = -1;
    if (vn) {
        aPipe = (int)vn->params.num("a_pipe", 0, 1);
        bPipe = (int)vn->params.num("b_pipe", 0, -1);
        if (Node* x = vn->input(App::pipeIndex(aPipe))) nodeA = x->name;
        if (Node* x = bPipe >= 0 ? vn->input(App::pipeIndex(bPipe)) : nullptr) nodeB = x->name;
        const std::string cmp = nodeB.empty() ? "off" : vn->params.str("compare", "off");
        auto wc = vn->params.vec<2>("wipe_center", 0, {v.res.format.width / 2.0, v.res.format.height / 2.0});
        const ImVec2 c((float)wc[0], (float)wc[1]);
        const float ang = (float)vn->params.num("wipe_angle", 0, 0), mix = (float)vn->params.num("wipe_mix", 0, 1);
        char key[160];
        std::snprintf(key, sizeof key, "%s|%.2f|%.2f|%.3f|%.3f", cmp.c_str(), c.x, c.y, ang, mix);
        v.compare = cmp, v.wipeC = c, v.wipeAngle = ang, v.wipeMix = mix;
        if (key != v.builtWith) {
            v.builtWith = key;
            v.dirtyTexture = true;
        }
        if (cmp == "off") nodeB.clear();
    }
    if (!active && vn) {
        // (the request is sent below, once the region on screen is known)
    }

    // Pick up a finished render (the active viewer's from the main renderer, others' from their own).
    ViewResult r = active ? app.result() : app.auxResult(vname);
    if (r.serial > v.shownSerial && (r.node == nodeA || nodeA.empty())) {
        const bool hadImage = v.res.image && v.res.format.width > 0;
        bool firstOfNode = r.node != v.res.node || v.res.format.width != r.format.width ||
                           v.res.format.height != r.format.height;
        v.shownSerial = r.serial;
        if (r.image || !r.error.empty() || r.node.empty()) {
            v.res = r;
            v.dirtyTexture = true;
            if (r.image) v.layers = layersOf(r.image->channels);
        }
        // Switching what's viewed (viewer inputs 1 / 2 ..., another node) keeps the zoom and pan, so
        // plate and comp can be flipped at the same spot; only the very first image fits, or one
        // that would land entirely off screen (checked once the viewport size is known).
        if (firstOfNode && r.image) {
            if (!hadImage) v.fitPending = true;
            else v.checkOnScreen = true;
            if (std::getenv("STRATA_DEBUG_AHEAD"))
                std::fprintf(stderr, "refit: %s %dx%d -> %s %dx%d (frame %g)\n", v.res.node.c_str(), v.res.format.width,
                             v.res.format.height, r.node.c_str(), r.format.width, r.format.height, r.frame);
        }
    }
    if (v.dirtyTexture) viewerUpload(v);

    // ---- 3D mode (Tab): its own toolbar and viewport
    if (active && viewer3DActive(app, ui)) {
        ImGui::SetCursorPos(ImVec2(6, ImGui::GetCursorPosY() + 4));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(app.viewed.empty() ? "(nothing viewed)" : app.viewed.c_str());
        ImGui::SameLine(0, 16);
        drawViewer3DToolbar(app, ui);
        ImVec2 q0 = ImGui::GetCursorScreenPos();
        q0.y += 4;
        ImVec2 av = ImGui::GetContentRegionAvail();
        drawViewer3D(app, ui, q0, ImVec2(av.x, std::max(50.0f, av.y - 4)));
        ImGui::End();
        return;
    }

    drawReferenceToolbar(app, ui, v, vn, aPipe, bPipe, active);

    // ---- a viewer tool's own row (the Tracker's toolbar)
    if (active && activeTracker(app)) {
        ImGui::Spacing();
        drawTrackerToolbar(app, ui);
    }
    if (active && activeRoto(app)) {
        ImGui::SetCursorPosX(8);
        drawRotoToolbar(app, ui);
    }
    if (active && activeMarkup(app)) {
        ImGui::SetCursorPosX(8);
        drawMarkupToolbar(app, ui);
    }

    // ---- image area
    const float infoH = ImGui::GetFrameHeight() + 4;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    p0.y += 4;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 size(avail.x, std::max(50.0f, avail.y - infoH - 4));
    v.viewSize = size;
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##viewport", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    v.hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, theme().viewerBg);
    dl->PushClipRect(p0, p1, true);

    if (v.checkOnScreen && !v.fitPending && v.res.format.width > 0) {
        v.checkOnScreen = false;
        // the part of the image plane on screen, against the new image's format and bounding box
        const double hx = size.x * 0.5 / v.zoom, hy = size.y * 0.5 / v.zoom;
        Box seen((int)std::floor(v.pan.x - hx), (int)std::floor(v.pan.y - hy), (int)std::ceil(v.pan.x + hx), (int)std::ceil(v.pan.y + hy));
        Box shown = v.res.format.box();
        if (!v.res.bbox.empty()) shown = shown.unite(v.res.bbox);
        if (seen.intersect(shown).empty()) v.fitPending = true;
    }
    if (v.fitPending && v.res.format.width > 0) viewerFit(v, size);

    ImVec2 center(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
    auto toScreen = [&](double x, double y) {
        return ImVec2(center.x + (float)((x - v.pan.x) * v.zoom), center.y - (float)((y - v.pan.y) * v.zoom));
    };
    auto toImage = [&](ImVec2 s) {
        return ImVec2(v.pan.x + (s.x - center.x) / v.zoom, v.pan.y - (s.y - center.y) / v.zoom);
    };
    if (static const char* tdh = std::getenv("STRATA_TEST_DEEPHOVER"); tdh && *tdh) {
        int tx = 0, ty = 0;
        if (std::sscanf(tdh, "%d,%d", &tx, &ty) == 2) gTestDeepHover = toScreen(tx + 0.5, ty + 0.5);
    }

    // Navigation: wheel zooms about the cursor, middle / alt+left drag pans.
    ImGuiIO& io = ImGui::GetIO();
    if (v.hovered && io.MouseWheel != 0) {
        ImVec2 before = toImage(io.MousePos);
        v.zoom = std::clamp(v.zoom * std::pow(1.15f, io.MouseWheel), 0.02f, 64.0f);
        ImVec2 after = toImage(io.MousePos);
        v.pan.x += before.x - after.x;
        v.pan.y += before.y - after.y;
    }
    const bool midZoom = app.middleDragZooms || io.KeyAlt;  // middle-drag zoom: tablets without a wheel (issue #23)
    if (ImGui::IsItemActive() && midZoom && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const ImVec2 anchor = io.MouseClickedPos[ImGuiMouseButton_Middle];  // about where the drag started
        const ImVec2 before = toImage(anchor);
        v.zoom = std::clamp(v.zoom * std::pow(1.006f, io.MouseDelta.x - io.MouseDelta.y), 0.02f, 64.0f);
        const ImVec2 after = toImage(anchor);
        v.pan.x += before.x - after.x;
        v.pan.y += before.y - after.y;
    } else if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                                  (io.KeyAlt && !io.KeyCtrl && ImGui::IsMouseDragging(ImGuiMouseButton_Left)))) {
        v.pan.x -= io.MouseDelta.x / v.zoom;
        v.pan.y += io.MouseDelta.y / v.zoom;
    }

    // Tell the renderer what is on screen (full-res pixels) and at which scale.
    // The request is padded so small pans don't need a new render.
    {
        double sc = v.proxy ? 1.0 / v.resolution : pickScale(v.zoom, v.fullRes);
        if (app.comp->project.proxyMode) sc = std::min(sc, app.comp->project.proxyScale);
        ImVec2 a = toImage(p0), b = toImage(p1);
        Box visible((int)std::floor(std::min(a.x, b.x)), (int)std::floor(std::min(a.y, b.y)),
                    (int)std::ceil(std::max(a.x, b.x)), (int)std::ceil(std::max(a.y, b.y)));
        const ImVec2 off = sideOffset(v);  // side by side: what's on screen of B, in B's own pixels, too
        if (off.x != 0 || off.y != 0)
            visible = visible.unite(Box(visible.x1 - (int)off.x, visible.y1 - (int)off.y, visible.x2 - (int)off.x, visible.y2 - (int)off.y));
        if (v.fullBuffer) visible = v.res.format.box();
        if (v.roi && !v.roiBox.empty()) visible = v.roiBox;
        bool covered = sc == v.reqScale && !v.reqRegion.empty() && visible.intersect(v.reqRegion) == visible;
        if (!covered && !(active && app.viewPaused)) {
            int px = std::max(64, visible.width() / 3), py = std::max(64, visible.height() / 3);
            v.reqRegion = (v.fullBuffer || v.roi) ? visible : visible.pad(px, py);
            v.reqScale = sc;
            if (active) app.setViewWindow(v.reqRegion, sc);
        }
        if (!active && vn) app.requestAuxView(vname, nodeA, nodeB, app.frame, v.reqRegion, v.reqScale);
    }

    const ViewResult& res = v.res;
    DisplayParams dp;
    dp.gain = std::pow(2.0f, v.fstop);
    dp.gamma = v.gamma;
    dp.saturation = v.saturation;
    dp.transfer = v.inputProcess ? v.displayTransfer : 1;
    dp.channel = v.channel;
    dp.matte = v.matte ? 1 : 0;
    if (vn)
        if (Node* x = vn->input(App::pipeIndex(aPipe)); x && x->outputIsData()) dp.raw = 1;
    dp.ocio = v.inputProcess && v.displayTransfer == 0 ? viewerTransform(app) : nullptr;
    if (res.image && v.tex && !v.texBounds.empty()) {
        // Texture pixels are at render scale; draw them in full-res image coordinates.
        const Box& b = v.texBounds;
        const double s = v.texScale;
        if (v.cropToFormat) dl->PushClipRect(toScreen(0, res.format.height), toScreen(res.format.width, 0), true);
        viewerDrawImage(dl, v.tex, toScreen(b.x1 / s, b.y2 / s), toScreen(b.x2 / s, b.y1 / s), ImVec2(0, 1), ImVec2(1, 0), dp);
        if (v.cropToFormat) dl->PopClipRect();
        if (v.hasB && v.texB) {  // side by side / above and below: B in its place, labelled
            const ImVec2 off = sideOffset(v);
            ImVec2 a0 = toScreen(b.x1 / s + off.x, b.y2 / s + off.y), a1 = toScreen(b.x2 / s + off.x, b.y1 / s + off.y);
            viewerDrawImage(dl, v.texB, a0, a1, ImVec2(0, 1), ImVec2(1, 0), dp);
            ImVec2 fa = toScreen(0, res.format.height), fb = toScreen(off.x, res.format.height + off.y);
            dl->AddText(ImVec2(fa.x + 4, fa.y - 18), themeAccent(230), ("A  " + nodeA).c_str());
            dl->AddText(ImVec2(fb.x + 4, fb.y - 18), IM_COL32(90, 170, 255, 230), ("B  " + nodeB).c_str());
            ImVec2 g0 = toScreen(off.x, res.format.height + off.y), g1 = toScreen(off.x + res.format.width, off.y);
            dl->AddRect(g0, g1, IM_COL32(90, 170, 255, 160));
        } else if (v.compare == "wipe" && res.imageB && res.format.width > 0) {
            wipeHandles(app, vn, v, dl, ViewXf{center, v.pan, v.zoom}, nodeA, nodeB);
        } else if (v.compare != "off" && res.imageB) {
            const std::string t = "A " + v.compare + " B";
            dl->AddText(ImVec2(p0.x + 10, p0.y + 8), IM_COL32(255, 255, 255, 170), t.c_str());
        }
        if (v.safeZone) {
            for (float inset : {.05f, .10f})
                dl->AddRect(toScreen(res.format.width * inset, res.format.height * (1 - inset)),
                            toScreen(res.format.width * (1 - inset), res.format.height * inset), IM_COL32(220, 220, 160, 180));
        }
        if (v.roi && !v.roiBox.empty())
            dl->AddRect(toScreen(v.roiBox.x1, v.roiBox.y2), toScreen(v.roiBox.x2, v.roiBox.y1), IM_COL32(230, 180, 65, 230));
        // Format box and bbox (O toggles them).
        if (v.overlays) {
        ImU32 fmtCol = IM_COL32(150, 150, 150, 200);
        ImVec2 f0 = toScreen(0, res.format.height), f1 = toScreen(res.format.width, 0);
        dl->AddRect(f0, f1, fmtCol);
        char label[64];
        std::snprintf(label, sizeof(label), "%dx%d", res.format.width, res.format.height);
        dl->AddText(ImVec2(f1.x - ImGui::CalcTextSize(label).x, f1.y + 3), fmtCol, label);
        if (!res.bbox.empty() && res.bbox != res.format.box()) {
            ImVec2 b0 = toScreen(res.bbox.x1, res.bbox.y2), b1 = toScreen(res.bbox.x2, res.bbox.y1);
            ImU32 bc = IM_COL32(110, 160, 220, 160);
            for (float x = b0.x; x < b1.x; x += 8) {
                dl->AddLine(ImVec2(x, b0.y), ImVec2(std::min(x + 4, b1.x), b0.y), bc);
                dl->AddLine(ImVec2(x, b1.y), ImVec2(std::min(x + 4, b1.x), b1.y), bc);
            }
            for (float y = b0.y; y < b1.y; y += 8) {
                dl->AddLine(ImVec2(b0.x, y), ImVec2(b0.x, std::min(y + 4, b1.y)), bc);
                dl->AddLine(ImVec2(b1.x, y), ImVec2(b1.x, std::min(y + 4, b1.y)), bc);
            }
        }
        }
    } else {
        const char* msg = nodeA.empty() ? "Select a node and press 1 to view it" : "";
        if (!res.error.empty()) msg = "";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(center.x - ts.x / 2, center.y - ts.y / 2), IM_COL32(120, 120, 120, 255), msg);
    }
    // Colour picker (a Color knob's "pick" button): click samples a pixel,
    // drag averages a box. Takes the mouse from the roto tools while active.
    PickerState& pk = ui.picker;
    // The picker belongs to its node's panel: closing or folding that panel, or going on to work in
    // another one, puts it down. Left armed, it took every click meant for a Roto or a handle.
    if (pk.active && (!app.node(pk.node) || !app.panelShown(pk.node))) pk.active = false;
    // So does another node's viewer tool taking over (its panel opened, or clicked in: see nodePanel).
    {
        Node* tn = activeToolNode(app);
        const std::string tool = tn ? tn->name : "";
        if (tool != ui.pickerToolSeen) {
            if (pk.active && !tool.empty() && tool != pk.node) pk.active = false;
            ui.pickerToolSeen = tool;
        }
    }
    if (active && pk.active) {
        ImVec2 mi = toImage(io.MousePos);
        if (v.hovered && ImGui::IsKeyPressed(ImGuiKey_Escape)) pk.active = false;
        if (v.hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            pk.dragging = true;
            pk.startImg = mi;
        }
        if (pk.dragging) {
            ImVec2 a = toScreen(pk.startImg.x, pk.startImg.y);
            dl->AddRect(ImVec2(std::min(a.x, io.MousePos.x), std::min(a.y, io.MousePos.y)),
                        ImVec2(std::max(a.x, io.MousePos.x), std::max(a.y, io.MousePos.y)), IM_COL32(255, 210, 120, 220));
        }
        if (pk.dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            pk.dragging = false;
            int x1 = (int)std::floor(std::min(pk.startImg.x, mi.x)), y1 = (int)std::floor(std::min(pk.startImg.y, mi.y));
            int x2 = std::max(x1 + 1, (int)std::ceil(std::max(pk.startImg.x, mi.x)));
            int y2 = std::max(y1 + 1, (int)std::ceil(std::max(pk.startImg.y, mi.y)));
            float rgba[4] = {0, 0, 0, 1};
            bool ok = false;
            if (pk.mode == "point") {  // an XY knob's crosshair: the clicked spot, in image pixels
                std::string node = pk.node, knob = pk.knob;
                const double x = std::round(mi.x * 10) / 10, y = std::round(mi.y * 10) / 10, fr = app.frame;
                app.checkpoint();
                app.edit([node, knob, x, y, fr](Comp& c) {
                    if (Node* n = c.find(node)) {
                        json& v = n->params.data[knob];
                        const std::vector<double> defs = {x, y};
                        setParamComp(v, 2, 0, x, fr, defs);
                        setParamComp(v, 2, 1, y, fr, defs);
                        n->paramsChanged();
                    }
                });
                char msg[160];
                std::snprintf(msg, sizeof msg, "%s.%s = %g %g", node.c_str(), knob.c_str(), x, y);
                app.message(msg);
                pk.active = false;
            } else if (pk.mode != "color") {
                pickSpecial(app, pk, (int)std::floor(mi.x), (int)std::floor(mi.y));
            } else if (pk.fromInput) {
                ok = app.samplePixels(pk.node, 0, Box(x1, y1, x2, y2), app.frame, rgba);
            } else if (res.image) {
                // What the viewer shows, at the scale it was rendered.
                const Image& im = *res.image;
                const double s = res.scale;
                int idx[4] = {im.ch("R"), im.ch("G"), im.ch("B"), im.ch("A")};
                double acc[4] = {0, 0, 0, 0};
                long count = 0;
                for (int y = (int)std::floor(y1 * s); y < std::max((int)std::floor(y1 * s) + 1, (int)std::ceil(y2 * s)); ++y)
                    for (int x = (int)std::floor(x1 * s); x < std::max((int)std::floor(x1 * s) + 1, (int)std::ceil(x2 * s)); ++x) {
                        if (!im.bounds.contains(x, y)) continue;
                        for (int k = 0; k < 4; ++k) acc[k] += idx[k] >= 0 ? im.at(x, y, idx[k]) : (k == 3 ? 1.0 : 0.0);
                        ++count;
                    }
                if (count) {
                    for (int k = 0; k < 4; ++k) rgba[k] = (float)(acc[k] / count);
                    ok = true;
                }
            }
            if (ok) {
                std::string node = pk.node, knob = pk.knob;
                json val = json::array({rgba[0], rgba[1], rgba[2], rgba[3]});
                app.checkpoint();
                app.edit([node, knob, val](Comp& c) {
                    if (Node* n = c.find(node)) {
                        n->params.data[knob] = val;
                        n->paramsChanged();
                    }
                });
                char msg[160];
                std::snprintf(msg, sizeof(msg), "%s.%s = %.4f %.4f %.4f", node.c_str(), knob.c_str(), rgba[0], rgba[1], rgba[2]);
                app.message(msg);
            }
        }
        const char* hint = pk.mode == "crypto add"      ? "PICK MATTES: click objects to add them (Ctrl: remove)   Esc: stop"
                           : pk.mode == "crypto remove" ? "PICK MATTES: click objects to remove them   Esc: stop"
                           : pk.mode == "focus"         ? "PICK FOCUS: click what should be sharp   Esc: stop"
                           : pk.mode == "point"         ? "PICK POINT: click to place it   Esc: stop"
                           : pk.fromInput ? "PICK (input): click or drag over the screen   Esc: stop" : "PICK: click or drag   Esc: stop";
        dl->AddText(ImVec2(p0.x + 10, p1.y - 22), IM_COL32(255, 210, 120, 255), hint);
    }

    // Q hid the overlays, and with them the viewer tools: a click in the viewer while a tool node's
    // panel is showing (Roto, Tracker, Transform, any node with handles) brings them back, and the
    // click goes on to the tool (it was a roto that "wouldn't draw", a handle that wasn't there).
    if (active && !v.overlays && v.hovered && !pk.active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyAlt)
        if (Node* tn = activeToolNode(app)) {
            v.overlays = true;
            app.message("viewer overlays back on for " + tn->name + " (Q hides them)");
        }
    // Roto shapes and tools (when a Roto node's properties are open): in the active viewer.
    if (!active) {
    } else if (v.overlays) {
        ViewXf xf{center, v.pan, v.zoom};
        ui.viewerImage = {res.image && v.tex && !v.texBounds.empty() ? v.tex : 0u, v.texBounds, v.texScale, dp};
        drawRotoViewer(app, ui, dl, p0, p1, xf, (v.hovered || ImGui::IsItemActive()) && !pk.active);
    }
    if (v.overlays) {
        ViewXf xf{center, v.pan, v.zoom};
        drawTrackerViewer(app, ui, dl, p0, p1, xf, (v.hovered || ImGui::IsItemActive()) && !pk.active);
    }
    if (v.overlays) {
        ViewXf xf{center, v.pan, v.zoom};
        drawPlanarViewer(app, ui, dl, p0, p1, xf, (v.hovered || ImGui::IsItemActive()) && !pk.active);
    }
    if (v.overlays) {
        ViewXf xf{center, v.pan, v.zoom};
        drawTransformViewer(app, ui, dl, p0, p1, xf, (v.hovered || ImGui::IsItemActive()) && !pk.active);
        drawKnobHandles(app, ui, dl, p0, p1, xf, (v.hovered || ImGui::IsItemActive()) && !pk.active);
    }
    if (v.overlays) {
        ViewXf xf{center, v.pan, v.zoom};
        drawViewer3DIn2D(app, ui, dl, p0, p1, xf, (v.hovered || ImGui::IsItemActive()) && !pk.active);
    }
    if (v.overlays && activeVanishCam(app)) {
        ViewXf xf{center, v.pan, v.zoom};
        drawVanishCamViewer(app, ui, dl, p0, p1, xf, (v.hovered || ImGui::IsItemActive()) && !pk.active);
    }
    if (v.overlays && activeFlowField(app)) {
        ViewXf xf{center, v.pan, v.zoom};
        drawFlowPaintViewer(app, ui, dl, p0, p1, xf, (v.hovered || ImGui::IsItemActive()) && !pk.active);
    }
    if (active && v.overlays && activeMarkup(app)) {
        ViewXf xf{center, v.pan, v.zoom};
        drawMarkupViewer(app, ui, dl, p0, p1, xf, (v.hovered || ImGui::IsItemActive()) && !pk.active);
    }
    if (v.overlays && activeTrackMask(app)) {
        ViewXf xf{center, v.pan, v.zoom};
        drawTrackMaskViewer(app, ui, dl, p0, p1, xf, (v.hovered || ImGui::IsItemActive()) && !pk.active);
        drawTrackMaskHud(app, ui, p0, p1);
    }
    if (v.overlays) {
        ViewXf xf{center, v.pan, v.zoom};
        drawCamTrackerViewer(app, ui, dl, p0, p1, xf, (v.hovered || ImGui::IsItemActive()) && !pk.active);
    }
    if (!v.overlays) {  // Q hid the shapes and handles: say so, so a tool that ignores clicks isn't a mystery
        const char* t = "overlays off [Q]";
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(p1.x - ts.x - 10, p1.y - ts.y - 8), IM_COL32(200, 200, 200, 150), t);
    }
    if (!res.error.empty()) {
        std::string e = "ERROR: " + res.error;
        dl->AddRectFilled(ImVec2(p0.x + 10, p0.y + 10), ImVec2(p0.x + 20 + ImGui::CalcTextSize(e.c_str()).x, p0.y + 32),
                          IM_COL32(120, 20, 20, 230), 3);
        dl->AddText(ImVec2(p0.x + 15, p0.y + 13), IM_COL32(255, 220, 220, 255), e.c_str());
    }
    dl->PopClipRect();

    // ---- info bar: format, bbox, pixel under the cursor
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6, p1.y + 4));
    if (res.image) {
        ImGui::TextDisabled("%dx%d  bbox %d %d %d %d  %d ch  frame %g", res.format.width, res.format.height,
                            res.bbox.x1, res.bbox.y1, res.bbox.x2, res.bbox.y2, (int)res.image->channels.size(),
                            res.frame);
        if (v.hovered) {
            ImVec2 ip = toImage(io.MousePos);
            int x = (int)std::floor(ip.x), y = (int)std::floor(ip.y);
            const Image& im = *res.image;
            int sx = (int)std::floor(ip.x * res.scale), sy = (int)std::floor(ip.y * res.scale);
            std::array<int, 4> m = layerChannels(im, v.layer);
            if (v.alphaChannel != "A") m[3] = im.ch(v.alphaChannel);
            ImGui::SameLine(0, 20);
            ImGui::Text("x=%d y=%d", x, y);
            const ImVec4 cols[4] = {ImVec4(1, .45f, .45f, 1), ImVec4(.5f, 1, .5f, 1), ImVec4(.55f, .7f, 1, 1),
                                    ImVec4(.85f, .85f, .85f, 1)};
            for (int c = 0; c < 4; ++c) {
                ImGui::SameLine(0, 12);
                ImGui::TextColored(cols[c], "%.5f", im.at(sx, sy, m[c]));
            }
            if (v.deepReadout) drawDeepReadout(app, v, x, y);
        }
    }
    ImGui::Dummy(ImVec2(0, 0));

    // ---- S: viewer settings
    if (v.openSettings) {
        v.openSettings = false;
        ImGui::OpenPopup("viewer_settings");
    }
    if (ImGui::BeginPopup("viewer_settings")) {
        ImGui::TextDisabled("Viewer settings");
        ImGui::Separator();
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("f/stop", &v.fstop, -6.0f, 6.0f, "%.1f");
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("gamma", &v.gamma, 0.2f, 4.0f, "%.2f");
        ImGui::Checkbox("full resolution (no automatic downscale)", &v.fullRes);
        ImGui::Checkbox("overlays (shapes, handles, format, bbox)  [Q]", &v.overlays);
        ImGui::Checkbox("matte overlay  [M]", &v.matte);
        ImGui::Checkbox("deep samples under the cursor", &v.deepReadout);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Viewing a deep node: rest the cursor on a pixel to list its samples (depth, colour, alpha).");
        bool paused = app.viewPaused;
        if (ImGui::Checkbox("pause updates  [P]", &paused)) app.viewPaused = paused;
        ImGui::EndPopup();
    }
    ImGui::End();
}

void activateViewer(App& app, UiState& ui, const std::string& name) {
    if (name.empty() || !app.viewerNode(name)) return;
    if (name != ui.viewerOf) {
        if (!ui.viewerOf.empty()) ui.otherViewers[ui.viewerOf] = ui.viewer;
        auto it = ui.otherViewers.find(name);
        if (it != ui.otherViewers.end()) {
            ui.viewer = it->second;
            ui.otherViewers.erase(it);
        } else {
            ViewerState fresh;
            fresh.layer = ui.viewer.layer;
            ui.viewer = fresh;
        }
        ui.viewerOf = name;
        ui.viewer.shownSerial = 0;  // the main renderer's results now
        ui.viewer.reqRegion = Box();
        ui.viewer.reqScale = 0;
    }
    if (app.activeViewer != name) {
        app.activeViewer = name;
        app.syncViewer();
    }
}

// Every viewer panel: the first Viewer node in the main "Viewer" panel, the others (Ctrl+I) in
// panels of their own, docked beside it when they first appear.
void drawViewer(App& app, UiState& ui) {
    app.syncViewer();
    auto viewers = app.viewerNodes();
    if (!ui.activateNext.empty()) {
        activateViewer(app, ui, ui.activateNext);
        ui.activateNext.clear();
    }
    if (!app.viewerNode() && !viewers.empty()) app.activeViewer = viewers.front()->name;
    if (ui.viewerOf != app.activeViewer && app.viewerNode()) {
        if (ui.viewerOf.empty()) ui.viewerOf = app.activeViewer;  // the first frame: no swap
        else activateViewer(app, ui, app.activeViewer);
    }
    // forget the states of viewers that are gone
    for (auto it = ui.otherViewers.begin(); it != ui.otherViewers.end();)
        it = app.viewerNode(it->first) ? std::next(it) : ui.otherViewers.erase(it);
    ui.hoverViewer.clear();
    const bool maxed = ui.maximized == "Viewer";
    // the main panel (maximized: the active viewer takes the screen)
    Node* primary = viewers.empty() ? nullptr : viewers.front();
    Node* mainNode = maxed && app.viewerNode() ? app.viewerNode() : primary;
    const bool mainActive = !mainNode || mainNode->name == app.activeViewer;
    drawViewerPanel(app, ui, mainActive ? ui.viewer : ui.otherViewers[mainNode->name], mainNode, mainActive, ui.win("Viewer"));
    if (maxed) return;
    ImGuiWindow* mainWin = ImGui::FindWindowByName("Viewer");
    // Viewer menu > Split: the viewer area divided evenly between the viewers, side by side or stacked
    if (ui.splitViewers && mainWin && mainWin->DockId && viewers.size() > 1) {
        const ImGuiDir dir = ui.splitViewers == 1 ? ImGuiDir_Right : ImGuiDir_Down;
        const size_t n = viewers.size();
        ImGuiID cur = mainWin->DockId;
        for (size_t i = 1; i < n; ++i) {
            ImGuiID at = 0, rest = 0;  // `at`: the side the rest of the viewers go; `rest`: this slot
            ImGui::DockBuilderSplitNode(cur, dir, (float)(n - i) / (float)(n - i + 1), &at, &rest);
            ImGui::DockBuilderDockWindow(i == 1 ? "Viewer" : (viewers[i - 1]->name + "###viewer_" + viewers[i - 1]->name).c_str(), rest);
            cur = at;
        }
        ImGui::DockBuilderDockWindow((viewers[n - 1]->name + "###viewer_" + viewers[n - 1]->name).c_str(), cur);
        ImGui::DockBuilderFinish(mainWin->DockId);
        ui.viewer.fitPending = true;
        for (auto& [k, st] : ui.otherViewers) st.fitPending = true;
    }
    ui.splitViewers = 0;
    for (size_t i = 1; i < viewers.size(); ++i) {
        Node* vn = viewers[i];
        const std::string title = vn->name + "###viewer_" + vn->name;
        if (!ImGui::FindWindowByName(title.c_str()) || ui.newViewer == vn->name) {  // first time: a tab beside the main viewer
            if (mainWin && mainWin->DockId) ImGui::SetNextWindowDockID(mainWin->DockId, ImGuiCond_Always);
            ImGui::SetNextWindowFocus();
            if (ui.newViewer == vn->name) ui.newViewer.clear();
        }
        const bool act = vn->name == app.activeViewer;
        drawViewerPanel(app, ui, act ? ui.viewer : ui.otherViewers[vn->name], vn, act, title);
    }
}

}  // namespace strata::ui
