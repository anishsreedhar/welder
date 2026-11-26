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
#include <filesystem>
#include <fstream>
#include <set>

#include "GL.h"
#include "Panels.h"
#include "nodes/Cryptomatte.h"
#include "nodes/NodeUtils.h"
#include "color/Color.h"
#include "ViewerGL.h"
#include "imgui_internal.h"
#include "stb_image_write.h"

namespace strata::ui {

namespace {

constexpr char kFlipbookRequest[] = "\x01" "flipbook";
constexpr size_t kFlipbookBytes = 256 * 1024 * 1024;

struct ViewerCapture {
    App* app;
    ViewerCaptureState* state;
    ImGuiViewport* viewport;
};

void captureViewer(const ImDrawList*, const ImDrawCmd* cmd) {
    const auto& capture = *static_cast<const ViewerCapture*>(cmd->UserCallbackData);
    const std::string path = std::exchange(capture.state->framePath, {});
    const ImDrawData& draw = *capture.viewport->DrawData;
    GLint rect[4];
    glGetIntegerv(GL_VIEWPORT, rect);
    const auto pixel = [](float p, float origin, float scale, int limit) {
        return std::clamp((int)std::lround((p - origin) * scale), 0, limit);
    };
    const auto& c = cmd->ClipRect;
    const auto& o = draw.DisplayPos;
    const auto& s = draw.FramebufferScale;
    const int left = pixel(c.x, o.x, s.x, rect[2]), right = pixel(c.z, o.x, s.x, rect[2]);
    const int top = pixel(c.y, o.y, s.y, rect[3]), bottom = pixel(c.w, o.y, s.y, rect[3]);
    const int w = right - left, h = bottom - top;
    try {
        if (w <= 0 || h <= 0) throw std::runtime_error("Viewer is not visible");
        std::vector<unsigned char> pixels((size_t)w * h * 4);
        GLint pack = 0;
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(rect[0] + left, rect[1] + rect[3] - bottom, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glPixelStorei(GL_PACK_ALIGNMENT, pack);
        const size_t stride = (size_t)w * 4;
        for (int y = 0; y < h / 2; ++y)
            std::swap_ranges(pixels.data() + y * stride, pixels.data() + (y + 1) * stride,
                             pixels.data() + (h - 1 - y) * stride);
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
        std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
        const auto write = [](void* stream, void* data, int length) {
            static_cast<std::ofstream*>(stream)->write(static_cast<const char*>(data), length);
        };
        if (!out || !stbi_write_png_to_func(write, &out, w, h, 4, pixels.data(), w * 4))
            throw std::runtime_error("could not write PNG");
        out.close();
        if (!out) throw std::runtime_error("could not finish writing PNG");
        capture.state->wroteFrame = true;
    } catch (const std::exception& e) {
        capture.state->failed = true;
        capture.app->message(std::string("Viewer capture: ") + e.what());
    }
}

void queueViewerCapture(App& app, UiState& ui, ImDrawList* dl) {
    if (ui.viewerCapture.framePath.empty()) return;
    ViewerCapture capture{&app, &ui.viewerCapture, ImGui::GetWindowViewport()};
    dl->AddCallback(captureViewer, &capture, sizeof(capture));
}

const char* kChannelNames[] = {"RGBA", "R", "G", "B", "A", "Luminance"};

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
    if (layer == "rgba" || layer.empty()) {
        m = {img.ch("R"), img.ch("G"), img.ch("B"), img.ch("A")};
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

double pickScale(float zoom, int resolutionDivisor) {
    if (resolutionDivisor > 0) return 1.0 / resolutionDivisor;
    // Largest power-of-two reduction that still has at least one image pixel per screen pixel.
    double s = 1.0;
    while (s > 1.0 / 16 && zoom <= s * 0.5) s *= 0.5;
    return s;
}

std::array<int, 4> displayChannels(const Image& image, const ViewerState& v) {
    auto channels = layerChannels(image, v.layer);
    if (!v.alphaChannel.empty()) {
        const int alpha = image.ch(v.alphaChannel);
        if (alpha >= 0) channels[3] = alpha;
    }
    return channels;
}

float viewerPixelAspect(const ViewerState& v) {
    const double aspect = v.res.format.pixelAspect;
    return v.correctPixelAspect && std::isfinite(aspect) && aspect > 0 ? (float)aspect : 1.0f;
}

Box imageBox(ImVec2 a, ImVec2 b) {
    return Box((int)std::floor(std::min(a.x, b.x)), (int)std::floor(std::min(a.y, b.y)),
               (int)std::floor(std::max(a.x, b.x)) + 1, (int)std::floor(std::max(a.y, b.y)) + 1);
}

void viewerZoomCombo(ViewerState& v) {
    char current[24];
    std::snprintf(current, sizeof(current), "%.4g%%", v.zoom * 100.0);
    const bool open = ImGui::BeginCombo("##zoom", current, ImGuiComboFlags_HeightLarge);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Viewer zoom");
    if (!open) return;
    for (float percent : {6.25f, 12.5f, 25.0f, 50.0f, 75.0f, 100.0f, 150.0f, 200.0f, 400.0f, 800.0f}) {
        char label[24];
        std::snprintf(label, sizeof(label), "%g%%", percent);
        const float zoom = percent / 100.0f;
        const bool selected = v.zoom == zoom;
        if (ImGui::Selectable(label, selected)) {
            v.zoom = zoom;
            v.fitPending = false;
        }
        if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
}

void viewerResolutionCombo(ViewerState& v, const char* label) {
    char current[16];
    std::snprintf(current, sizeof(current), "1:%d", v.resolutionDivisor);
    const bool open = ImGui::BeginCombo(label, v.resolutionDivisor == 0 ? "Auto" : current);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Viewer render resolution. Auto follows zoom.\nProject proxy mode can lower the render resolution further.");
    if (!open) return;
    for (int divisor : {0, 1, 2, 4, 8, 16, 32}) {
        char ratio[16];
        std::snprintf(ratio, sizeof(ratio), "1:%d", divisor);
        const bool selected = v.resolutionDivisor == divisor;
        if (ImGui::Selectable(divisor == 0 ? "Auto" : ratio, selected)) v.resolutionDivisor = divisor;
        if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
}

}  // namespace

void viewerUpload(ViewerState& v) {
    v.dirtyTexture = false;
    v.hasB = false;
    const ImagePtr& img = v.res.image;
    if (!img || img->bounds.empty()) return;
    const int W = img->bounds.width(), H = img->bounds.height(), n = img->nch();
    std::array<int, 4> m = displayChannels(*img, v);
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
        std::array<int, 4> mb = displayChannels(*ib, v);
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
    const float w = (f.width + std::fabs(off.x)) * viewerPixelAspect(v), h = f.height + std::fabs(off.y);
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
        if (ImGui::Selectable("Raw (linear)", view == "Raw")) set(d, "Raw");
        for (auto& disp : color::displays(cfg)) {
            ImGui::SeparatorText(disp.c_str());
            ImGui::PushID(disp.c_str());
            for (auto& vv : color::views(cfg, disp))
                if (vv != "Raw" && ImGui::Selectable(vv.c_str(), disp == d && vv == view)) set(disp, vv);
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

namespace {

void viewerDisplayOptions(ViewerState& v) {
    ImGui::Checkbox("Format outline", &v.showFormat);
    ImGui::Checkbox("Bounding box", &v.showBounds);
    ImGui::Checkbox("Crop to format", &v.cropFormat);
    ImGui::Checkbox("Transparency checkerboard", &v.checkerboard);
    ImGui::Checkbox("Clipping warning", &v.clippingWarning);
    ImGui::Checkbox("Tool overlays (Q)", &v.overlays);
    if (ImGui::Checkbox("Correct pixel aspect ratio", &v.correctPixelAspect)) v.fitPending = true;
    ImGui::Combo("Guides", &v.guides, "None\0Title safe\0Action safe\0Center\0Format\0");
    ImGui::Combo("Mask", &v.maskMode, "None\0Lines\0Half\0Full\0");
    if (v.maskMode) ImGui::Combo("Mask ratio", &v.maskAspect, "4:3\0" "16:9\0" "1.85:1\0" "2.39:1\0");
}

void viewerInputProcess(App& app, Node* viewer) {
    if (!viewer) return;
    const bool enabled = viewer->params.flag("input_process");
    const std::string process = viewer->params.str("input_process_node", "VIEWER_INPUT");
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(enabled ? theme().segmentOn : theme().segment));
    if (ImGui::Button("IP")) app.setViewerParam(viewer->name, "input_process", !enabled);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Viewer input process: %s\nRight-click to choose a processing branch", process.c_str());
    if (ImGui::BeginPopupContextItem("input_process_menu")) {
        ImGui::TextDisabled("Choose a node downstream of the Viewer input");
        for (const auto& n : app.comp->nodes) {
            if (n->group != viewer->group || n->type == "Viewer" || n->type == "Write") continue;
            if (ImGui::Selectable(n->name.c_str(), n->name == process)) {
                app.setViewerParam(viewer->name, "input_process_node", n->name);
                app.setViewerParam(viewer->name, "input_process", true);
            }
        }
        ImGui::EndPopup();
    }
}

void drawViewerGuides(ImDrawList* dl, const ViewerState& v, const ViewXf& xf) {
    const auto& f = v.res.format;
    if (f.width <= 0 || f.height <= 0 || (!v.guides && !v.maskMode)) return;
    const ImVec2 a = xf.toScreen(0, f.height), b = xf.toScreen(f.width, 0);
    const ImVec2 center((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    const float width = b.x - a.x, height = b.y - a.y;
    const ImU32 color = IM_COL32(190, 215, 235, 210);
    if (v.maskMode) {
        constexpr double ratios[] = {4.0 / 3, 16.0 / 9, 1.85, 2.39};
        const double ratio = ratios[v.maskAspect] * xf.pixelAspect / std::max(0.001, f.pixelAspect);
        const float w = std::min(width, (float)(height * ratio)), h = std::min(height, (float)(width / ratio));
        const ImVec2 lo(center.x - w / 2, center.y - h / 2), hi(center.x + w / 2, center.y + h / 2);
        if (v.maskMode == 1) dl->AddRect(lo, hi, color);
        else {
            const ImU32 shade = IM_COL32(0, 0, 0, v.maskMode == 2 ? 128 : 255);
            dl->AddRectFilled(a, ImVec2(b.x, lo.y), shade);
            dl->AddRectFilled(ImVec2(a.x, hi.y), b, shade);
            dl->AddRectFilled(ImVec2(a.x, lo.y), ImVec2(lo.x, hi.y), shade);
            dl->AddRectFilled(ImVec2(hi.x, lo.y), ImVec2(b.x, hi.y), shade);
        }
    }
    if (v.guides == 3) {
        dl->AddLine(ImVec2(center.x - 14, center.y), ImVec2(center.x + 14, center.y), color);
        dl->AddLine(ImVec2(center.x, center.y - 14), ImVec2(center.x, center.y + 14), color);
    } else if (v.guides) {
        const float inset = v.guides == 1 ? 0.10f : v.guides == 2 ? 0.05f : 0.0f;
        dl->AddRect(ImVec2(a.x + width * inset, a.y + height * inset),
                    ImVec2(b.x - width * inset, b.y - height * inset), color);
    }
}

}  // namespace

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
        std::string label = (isB ? "B: " : "A: ") + (cur < 0 ? std::string("none") : pipeLabel(vn, cur));
        ImGui::SetNextItemWidth(isB ? 100 : 130);
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
    list("##bpipe", v.compare == "off" ? -1 : bPipe, true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("B: the pipe to compare against (Shift+number)");
    ImGui::SameLine(0, 4);
    static const char* modes[] = {"off", "wipe", "over", "under", "minus", "difference", "side by side", "above / below"};
    const std::string cur = vn->params.str("compare", "off");
    ImGui::SetNextItemWidth(84);
    if (ImGui::BeginCombo("##cmpmode", cur.c_str())) {
        for (const char* m : modes)
            if (ImGui::Selectable(m, cur == m)) {
                app.setViewerParam(vname, "compare", m);
                if (std::string(m) != "off" && bPipe < 0)  // no B yet: the next connected pipe
                    for (int step = 1; step < 10; ++step) {
                        const int i = (App::pipeIndex(aPipe) + step) % 10;
                        if (vn->input(i)) { app.setViewerParam(vname, "b_pipe", (i + 1) % 10); break; }
                    }
                v.fitPending = std::string(m) == "side by side" || std::string(m) == "above / below" || v.compare == "side by side" ||
                               v.compare == "above / below";
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How A and B are compared (W: wipe on / off)\nwipe: drag the line's centre to move it, its handle to turn it\n"
                          "over / under: A over B, B over A\nminus: A - B   difference: |A - B| (black: the same)\n"
                          "side by side / above / below: next to each other");
    if (cur == "wipe") {
        ImGui::SameLine(0, 4);
        float mix = (float)vn->params.num("wipe_mix", 0, 1);
        ImGui::SetNextItemWidth(60);
        if (ImGui::SliderFloat("##wmix", &mix, 0, 1, "mix %.2f")) app.setViewerParam(vname, "wipe_mix", mix);
    }
    if (cur != "off" && bPipe >= 0) {
        ImGui::SameLine(0, 4);
        if (ImGui::SmallButton("A<>B")) {  // swap the buffers
            app.setViewerParam(vname, "b_pipe", aPipe);
            app.setViewerParam(vname, "a_pipe", bPipe);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Swap A and B");
    }
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
        if (Node* x = app.viewerSource(vn, App::pipeIndex(aPipe))) nodeA = x->name;
        if (Node* x = bPipe >= 0 ? app.viewerSource(vn, App::pipeIndex(bPipe)) : nullptr) nodeB = x->name;
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
        if (ImGui::Button("2D")) {
            ui.v3d.on = false;
            ui.v3d.force2D = true;
        }
        ImGui::SameLine();
        drawViewer3DToolbar(app, ui);
        ImVec2 q0 = ImGui::GetCursorScreenPos();
        q0.y += 4;
        ImVec2 av = ImGui::GetContentRegionAvail();
        const ImVec2 area(av.x, std::max(50.0f, av.y - 4));
        drawViewer3D(app, ui, q0, area);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(q0, ImVec2(q0.x + area.x, q0.y + area.y), true);
        queueViewerCapture(app, ui, draw);
        draw->PopClipRect();
        ImGui::End();
        return;
    }

    // ---- toolbar: grouped segments (what's viewed | channels | display | view), status on the right
    ImGui::SetCursorPos(ImVec2(8, ImGui::GetCursorPosY() + 6));
    ImDrawList* tdl = ImGui::GetWindowDrawList();
    auto segBegin = [&] {
        tdl->ChannelsSplit(2);
        tdl->ChannelsSetCurrent(1);
        ImGui::BeginGroup();
    };
    auto segEnd = [&] {
        ImGui::EndGroup();
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        tdl->ChannelsSetCurrent(0);
        tdl->AddRectFilled(ImVec2(mn.x - 3, mn.y - 3), ImVec2(mx.x + 3, mx.y + 3), theme().card, 6);
        tdl->ChannelsMerge();
    };
    const float gap = 12;
    auto nextSeg = [&](float w) {  // on this row when it fits, otherwise the next
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (ImGui::GetItemRectMax().x + gap + w < right) ImGui::SameLine(0, gap);
        else ImGui::SetCursorPos(ImVec2(8, ImGui::GetCursorPosY() + 6));
    };
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(theme().segment));
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(theme().segment));

    segBegin();  // what's viewed
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(theme().medium, 0.0f);
    if (active) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(themeAccent()), " %s", vname.empty() ? "Viewer" : vname.c_str());
    else ImGui::TextDisabled(" %s", vname.empty() ? "Viewer" : vname.c_str());
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(active ? "The active viewer (number keys route here)" : "Click in this viewer to make it active");
    ImGui::SameLine(0, 8);
    pipeCombos(app, vn, aPipe, bPipe, v);
    segEnd();

    nextSeg(336);
    segBegin();  // channels
    ImGui::SetNextItemWidth(76);
    if (ImGui::BeginCombo("##layer", v.layer.c_str())) {
        for (auto& l : v.layers)
            if (ImGui::Selectable(l.c_str(), l == v.layer)) {
                v.layer = l;
                v.dirtyTexture = true;
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("layer");
    ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(106);
    const std::string alphaLabel = v.alphaChannel.empty() ? v.layer + ".alpha" : v.alphaChannel;
    if (ImGui::BeginCombo("##alpha_channel", alphaLabel.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("Layer alpha", v.alphaChannel.empty())) {
            v.alphaChannel.clear();
            v.dirtyTexture = true;
        }
        if (v.res.image)
            for (const auto& channel : v.res.image->channels)
                if (ImGui::Selectable(channel.c_str(), channel == v.alphaChannel)) {
                    v.alphaChannel = channel;
                    v.dirtyTexture = true;
                }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Alpha display source");
    {  // a segmented switch: RGB(A) R G B A Y
        static const char* shortNames[] = {"RGB", "R", "G", "B", "A", "Y"};
        static const ImU32 tints[] = {0, IM_COL32(255, 92, 92, 255), IM_COL32(110, 220, 110, 255), IM_COL32(100, 150, 255, 255),
                                      IM_COL32(230, 230, 230, 255), IM_COL32(200, 200, 200, 255)};
        ImGui::SameLine(0, 4);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1, 0));
        for (int i = 0; i < 6; ++i) {
            if (i) ImGui::SameLine();
            const bool on = v.channel == i;
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(on ? theme().segmentOn : theme().segment));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(on ? (i ? tints[i] : theme().value) : theme().muted));
            const float bw = ImGui::CalcTextSize(shortNames[i]).x + 12;
            if (ImGui::Button(shortNames[i], ImVec2(std::max(bw, 22.0f), 0))) {
                v.channel = on && i ? 0 : i;  // clicking the shown channel again goes back to colour
                v.dirtyTexture = true;
            }
            if (on) {
                const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                tdl->AddLine(ImVec2(mn.x + 4, mx.y - 1), ImVec2(mx.x - 4, mx.y - 1), i ? tints[i] : themeAccent(), 2);
            }
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kChannelNames[i]);
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
    }
    if (vn) {
        ImGui::SameLine(0, 4);
        viewerInputProcess(app, vn);
    }
    segEnd();

    // Double-click resets (#76). ImGui turns a double-click on a drag field into text entry, so
    // on that frame the field is drawn without it (Ctrl+click still types a value).
    auto resettingDrag = [](const char* id, float* val, float speed, float lo, float hi, const char* fmt, float def) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 q(p.x + ImGui::CalcItemWidth(), p.y + ImGui::GetFrameHeight());
        const bool dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(p, q) &&
                         ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        ImGui::DragFloat(id, val, speed, lo, hi, fmt, dbl ? ImGuiSliderFlags_NoInput : 0);
        if (dbl) {
            *val = def;
            ImGui::ClearActiveID();
        }
    };
    auto displayControl = [&](const char* label, float& value, float speed, float lo, float hi, const char* format,
                              float def, ImGuiSliderFlags sliderFlags) {
        constexpr float fieldWidth = 52, sliderWidth = 90;
        const float labelWidth = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
        const bool exposure = def == 0.0f;
        nextSeg(labelWidth + fieldWidth + sliderWidth + (exposure ? 42 : 8));
        segBegin();
        ImGui::PushID(label);
        if (exposure) {
            if (ImGui::ArrowButton("##less", ImGuiDir_Left)) value = std::max(lo, value - 1);
            ImGui::SameLine(0, 2);
        }
        if (ImGui::Button(label)) value = def;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset %s to %g", label, def);
        if (exposure) {
            ImGui::SameLine(0, 2);
            if (ImGui::ArrowButton("##more", ImGuiDir_Right)) value = std::min(hi, value + 1);
        }
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(fieldWidth);
        resettingDrag("##value", &value, speed, lo, hi, format, def);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Double-click to reset or Ctrl+click to enter a value");
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(sliderWidth);
        ImGui::SliderFloat("##slider", &value, lo, hi, "", sliderFlags | ImGuiSliderFlags_NoInput | ImGuiSliderFlags_NoRoundToFormat);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %g", label, value);
        ImGui::PopID();
        segEnd();
    };
    displayControl("f/stop", v.fstop, 0.05f, -10.0f, 10.0f, "%+.1f", 0.0f, 0);
    displayControl("gamma", v.gamma, 0.01f, 0.2f, 4.0f, "%.2f", 1.0f, ImGuiSliderFlags_Logarithmic);

    nextSeg(130);
    segBegin();
    viewerProcessCombo(app);
    segEnd();

    nextSeg(215);
    segBegin();  // the view
    if (ImGui::Button("Fit")) v.fitPending = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fit the image to the viewer (F)");
    ImGui::SameLine(0, 2);
    ImGui::SetNextItemWidth(84);
    viewerZoomCombo(v);
    ImGui::SameLine(0, 2);
    ImGui::SetNextItemWidth(62);
    viewerResolutionCombo(v, "##resolution");
    ImGui::SameLine(0, 2);
    if (ImGui::Button("3D") && active) {
        ui.v3d.on = true;
        ui.v3d.force2D = false;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("3D view (Tab)");
    segEnd();
    nextSeg(242);
    segBegin();
    if (active) {
        if (ImGui::Button("Refresh")) app.requestView(true);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Update the Viewer once (U)");
        ImGui::SameLine(0, 3);
        const bool paused = app.viewPaused;
        if (ImGui::Button(paused ? "Resume" : "Pause")) {
            app.viewPaused = !paused;
            if (paused) app.requestView();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pause Viewer updates (P)");
        ImGui::SameLine(0, 3);
    }
    const bool roiOn = v.roiEnabled || v.roiDraw;
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(roiOn ? theme().segmentOn : theme().segment));
    if (ImGui::Button("ROI")) {
        v.roiEnabled = !roiOn && !v.roi.empty();
        v.roiDraw = !roiOn && v.roi.empty();
        v.roiDragging = false;
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Render only a region of interest. Right-click or Alt+W to draw a new region.");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        v.roiDraw = true;
        v.roiDragging = false;
    }
    ImGui::SameLine(0, 3);
    if (ImGui::Button(v.sampleArmed ? "Picking" : "Pick")) v.sampleArmed = !v.sampleArmed;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sample a pixel or drag to sample an area. Ctrl+right-click clears the sample.");
    ImGui::SameLine(0, 3);
    if (ImGui::Button("...##view_options")) ImGui::OpenPopup("viewer_display_options");
    if (ImGui::BeginPopup("viewer_display_options")) {
        viewerDisplayOptions(v);
        ImGui::Combo("Pixel readout", &v.sampleDisplay, "HSVL\0" "8-bit\0" "8-bit hex\0");
        ImGui::EndPopup();
    }
    segEnd();
    ImGui::PopStyleColor(2);

    nextSeg(120);
    ImGui::AlignTextToFramePadding();
    if (v.res.image)
        ImGui::TextDisabled("Render 1:%d", (int)std::lround(1.0 / v.res.scale));
    if (app.comp->project.proxyMode) {
        if (v.res.image) ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1), "PROXY");
    }
    if (active && app.viewPaused) {
        if (v.res.image || app.comp->project.proxyMode) ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "PAUSED (U to update)");
    }
    if (active && app.rendering()) {
        if (v.res.image || app.comp->project.proxyMode || app.viewPaused) ImGui::SameLine(0, 12);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(themeAccent()), "rendering...");
    } else if (v.res.image) {
        ImGui::SameLine(0, 12);
        ImGui::TextDisabled("%.0f ms", active ? app.lastRenderMs() : v.res.ms);
    }
    ImGui::Dummy(ImVec2(0, 2));

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
    const bool wrapPixelInfo = ImGui::GetContentRegionAvail().x < ImGui::GetFontSize() * 56;
    const float infoH = ImGui::GetFrameHeight() + 4 + (wrapPixelInfo ? ImGui::GetTextLineHeightWithSpacing() : 0);
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
        const double hx = size.x * 0.5 / (v.zoom * viewerPixelAspect(v)), hy = size.y * 0.5 / v.zoom;
        Box seen((int)std::floor(v.pan.x - hx), (int)std::floor(v.pan.y - hy), (int)std::ceil(v.pan.x + hx), (int)std::ceil(v.pan.y + hy));
        Box shown = v.res.format.box();
        if (!v.res.bbox.empty()) shown = shown.unite(v.res.bbox);
        if (seen.intersect(shown).empty()) v.fitPending = true;
    }
    if (v.fitPending && v.res.format.width > 0) viewerFit(v, size);

    ImVec2 center(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
    auto viewXf = [&] { return ViewXf{center, v.pan, v.zoom, viewerPixelAspect(v)}; };
    auto toScreen = [&](double x, double y) { return viewXf().toScreen(x, y); };
    auto toImage = [&](ImVec2 s) { return viewXf().toImage(s); };
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
        v.pan.x -= io.MouseDelta.x / (v.zoom * viewerPixelAspect(v));
        v.pan.y += io.MouseDelta.y / v.zoom;
    }

    const ImVec2 imageMouse = toImage(io.MousePos);
    auto rectangle = [&](const Box& box, ImU32 color) {
        dl->AddRect(toScreen(box.x1, box.y2), toScreen(box.x2, box.y1), color, 0, 0, 1.5f);
    };
    const auto roiEdit = [&] {
        if (!v.roiDragEdges) return imageBox(v.roiStart, v.roiEnd);
        Box box = v.roi;
        const int dx = (int)std::lround(v.roiEnd.x - v.roiStart.x), dy = (int)std::lround(v.roiEnd.y - v.roiStart.y);
        if (v.roiDragEdges == 16) return Box(box.x1 + dx, box.y1 + dy, box.x2 + dx, box.y2 + dy);
        if (v.roiDragEdges & 1) box.x1 = std::min(box.x1 + dx, box.x2 - 1);
        if (v.roiDragEdges & 2) box.x2 = std::max(box.x2 + dx, box.x1 + 1);
        if (v.roiDragEdges & 4) box.y1 = std::min(box.y1 + dy, box.y2 - 1);
        if (v.roiDragEdges & 8) box.y2 = std::max(box.y2 + dy, box.y1 + 1);
        return box;
    };
    int roiHit = 0;
    if (v.roiEnabled && v.hovered && !v.roiDraw && !io.KeyCtrl && !io.KeyAlt) {
        const ImVec2 a = toScreen(v.roi.x1, v.roi.y2), b = toScreen(v.roi.x2, v.roi.y1);
        const auto nearEdge = [](float p, float q) { return std::fabs(p - q) <= 6; };
        const ImVec2 m = io.MousePos;
        if (m.y >= a.y - 6 && m.y <= b.y + 6) {
            if (nearEdge(m.x, a.x)) roiHit |= 1;
            else if (nearEdge(m.x, b.x)) roiHit |= 2;
        }
        if (m.x >= a.x - 6 && m.x <= b.x + 6) {
            if (nearEdge(m.y, a.y)) roiHit |= 8;
            else if (nearEdge(m.y, b.y)) roiHit |= 4;
        }
        if (!roiHit && nearEdge(m.x, (a.x + b.x) / 2) && nearEdge(m.y, (a.y + b.y) / 2)) roiHit = 16;
    }
    const bool leftClick = v.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool roiTool = v.roiDraw || v.roiDragging || roiHit != 0;
    if (leftClick && roiTool && !ui.picker.active) {
        v.roiDragging = true;
        v.roiDragEdges = roiHit;
        v.roiStart = v.roiEnd = imageMouse;
    }
    if (v.roiDragging) {
        if (ImGui::IsMousePosValid()) v.roiEnd = imageMouse;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            v.roi = roiEdit();
            v.roiEnabled = true;
            v.roiDraw = v.roiDragging = false;
        }
    }
    if (v.hovered && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        v.roiDraw = v.roiDragging = v.sampleArmed = v.sampleDragging = false;
    }

    const bool sampleTool = !ui.picker.active && !roiTool &&
                            (v.sampleArmed || v.sampleDragging || (io.KeyCtrl && (!active || !activeToolNode(app))));
    if (v.hovered && (io.KeyCtrl || v.sampleArmed) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        v.sampleBox = {};
        v.sampleArmed = false;
    }
    if (leftClick && sampleTool) {
        v.sampleDragging = true;
        v.sampleInput = io.KeyAlt && io.KeyCtrl;
        v.sampleStart = v.sampleEnd = imageMouse;
    }
    if (v.sampleDragging) {
        if (ImGui::IsMousePosValid() && (v.sampleArmed || io.KeyShift)) v.sampleEnd = imageMouse;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            v.sampleDragging = v.sampleArmed = false;
            const Box box = imageBox(v.sampleStart, v.sampleEnd);
            ImagePtr upstream;
            const Image* image = v.res.image.get();
            double scale = v.res.scale;
            if (v.sampleInput && vn && vn->input(App::pipeIndex(aPipe))) {
                const auto* source = vn->input(App::pipeIndex(aPipe));
                // Keep an interactive sample from requesting an unbounded full-frame render.
                if ((double)box.width() * box.height() <= 1024 * 1024)
                    upstream = app.pickImage(source->name, -1, box, v.res.frame);
                image = upstream.get();
                scale = 1;
            }
            if (image) {
                const Box mapped((int)std::floor(box.x1 * scale), (int)std::floor(box.y1 * scale),
                                 (int)std::ceil(box.x2 * scale), (int)std::ceil(box.y2 * scale));
                const Box pixels = mapped.intersect(image->bounds);
                if (!pixels.empty()) {
                    const auto channels = displayChannels(*image, v);
                    std::array<double, 4> sum{};
                    for (int y = pixels.y1; y < pixels.y2; ++y)
                        for (int x = pixels.x1; x < pixels.x2; ++x)
                            for (int c = 0; c < 4; ++c)
                                sum[c] += channels[c] >= 0 ? image->at(x, y, channels[c]) : (c == 3 ? 1 : 0);
                    const double count = (double)pixels.width() * pixels.height();
                    for (int c = 0; c < 4; ++c) v.pickedColor[c] = (float)(sum[c] / count);
                    v.sampleBox = box;
                }
            }
        }
    }
    const bool toolsHovered = (v.hovered || ImGui::IsItemActive()) && !ui.picker.active && !roiTool && !sampleTool;

    // Tell the renderer what is on screen (full-res pixels) and at which scale.
    // The request is padded so small pans don't need a new render.
    {
        double sc = pickScale(v.zoom, v.resolutionDivisor);
        if (app.comp->project.proxyMode) sc = std::min(sc, app.comp->project.proxyScale);
        ImVec2 a = toImage(p0), b = toImage(p1);
        Box visible((int)std::floor(std::min(a.x, b.x)), (int)std::floor(std::min(a.y, b.y)),
                    (int)std::ceil(std::max(a.x, b.x)), (int)std::ceil(std::max(a.y, b.y)));
        const ImVec2 off = sideOffset(v);  // side by side: what's on screen of B, in B's own pixels, too
        if (off.x != 0 || off.y != 0)
            visible = visible.unite(Box(visible.x1 - (int)off.x, visible.y1 - (int)off.y, visible.x2 - (int)off.x, visible.y2 - (int)off.y));
        const bool hasRoi = v.roiEnabled && !v.roi.empty();
        if (hasRoi) visible = v.roi;
        const bool covered = sc == v.reqScale && hasRoi == v.reqRoi && !v.reqRegion.empty() &&
                             (hasRoi ? v.reqRegion == visible : visible.intersect(v.reqRegion) == visible);
        if (!covered) {
            int px = std::max(64, visible.width() / 3), py = std::max(64, visible.height() / 3);
            v.reqRegion = hasRoi ? visible : visible.pad(px, py);
            v.reqScale = sc;
            v.reqRoi = hasRoi;
            if (active) app.setViewWindow(v.reqRegion, sc);
        }
        if (!active && vn) {
            if (!ui.linkViewerPlayback && !v.timelineFrame) v.timelineFrame = app.frame;
            app.requestAuxView(vname, nodeA, nodeB, ui.linkViewerPlayback ? app.frame : v.timelineFrame.value_or(app.frame),
                               v.reqRegion, v.reqScale);
        }
    }

    const ViewResult& res = v.res;
    DisplayParams dp;
    dp.gain = std::pow(2.0f, v.fstop);
    dp.gamma = v.gamma;
    dp.channel = v.channel;
    dp.matte = v.matte ? 1 : 0;
    const Node* source = app.node(nodeA);
    dp.raw = app.comp->project.viewerView == "Raw" || (source && source->outputIsData());
    dp.ocio = viewerTransform(app);
    dp.checkerboard = v.checkerboard;
    dp.clippingWarning = v.clippingWarning;
    if (res.image && v.tex && !v.texBounds.empty()) {
        // Texture pixels are at render scale; draw them in full-res image coordinates.
        const Box& b = v.texBounds;
        const double s = v.texScale;
        const auto drawImage = [&](unsigned int texture, ImVec2 offset) {
            const bool restrict = v.cropFormat || v.roiEnabled;
            if (restrict) {
                Box clip = v.roiEnabled ? v.roi : res.format.box();
                if (v.cropFormat) clip = clip.intersect(res.format.box());
                if (clip.empty()) return;
                dl->PushClipRect(toScreen(clip.x1 + offset.x, clip.y2 + offset.y),
                                 toScreen(clip.x2 + offset.x, clip.y1 + offset.y), true);
            }
            viewerDrawImage(dl, texture, toScreen(b.x1 / s + offset.x, b.y2 / s + offset.y),
                            toScreen(b.x2 / s + offset.x, b.y1 / s + offset.y), ImVec2(0, 1), ImVec2(1, 0), dp);
            if (restrict) dl->PopClipRect();
        };
        drawImage(v.tex, ImVec2(0, 0));
        if (v.hasB && v.texB) {  // side by side / above and below: B in its place, labelled
            const ImVec2 off = sideOffset(v);
            drawImage(v.texB, off);
            ImVec2 fa = toScreen(0, res.format.height), fb = toScreen(off.x, res.format.height + off.y);
            dl->AddText(ImVec2(fa.x + 4, fa.y - 18), themeAccent(230), ("A  " + nodeA).c_str());
            dl->AddText(ImVec2(fb.x + 4, fb.y - 18), IM_COL32(90, 170, 255, 230), ("B  " + nodeB).c_str());
            ImVec2 g0 = toScreen(off.x, res.format.height + off.y), g1 = toScreen(off.x + res.format.width, off.y);
            if (v.overlays && v.showFormat) dl->AddRect(g0, g1, IM_COL32(90, 170, 255, 160));
        } else if (v.compare == "wipe" && res.imageB && res.format.width > 0) {
            if (!roiTool && !sampleTool) wipeHandles(app, vn, v, dl, viewXf(), nodeA, nodeB);
        } else if (v.compare != "off" && res.imageB) {
            const std::string t = "A " + v.compare + " B";
            dl->AddText(ImVec2(p0.x + 10, p0.y + 8), IM_COL32(255, 255, 255, 170), t.c_str());
        }
        // Format box and bbox (O toggles them).
        if (v.overlays) {
        if (v.showFormat) {
            ImU32 fmtCol = IM_COL32(150, 150, 150, 200);
            ImVec2 f0 = toScreen(0, res.format.height), f1 = toScreen(res.format.width, 0);
            dl->AddRect(f0, f1, fmtCol);
            char label[64];
            std::snprintf(label, sizeof(label), "%dx%d", res.format.width, res.format.height);
            dl->AddText(ImVec2(f1.x - ImGui::CalcTextSize(label).x, f1.y + 3), fmtCol, label);
        }
        if (v.showBounds && !res.bbox.empty() && res.bbox != res.format.box()) {
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
        drawViewerGuides(dl, v, viewXf());
        }
    } else {
        const char* msg = nodeA.empty() ? "Select a node and press 1 to view it" : "";
        if (!res.error.empty()) msg = "";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(center.x - ts.x / 2, center.y - ts.y / 2), IM_COL32(120, 120, 120, 255), msg);
    }
    if (v.roiEnabled || v.roiDragging) {
        const Box box = v.roiDragging ? roiEdit() : v.roi;
        rectangle(box, themeAccent());
        const ImVec2 c = toScreen(((double)box.x1 + box.x2) / 2, ((double)box.y1 + box.y2) / 2);
        dl->AddRect(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), themeAccent());
    }
    if (v.roiDraw || v.sampleArmed)
        dl->AddText(ImVec2(p0.x + 10, p1.y - 22), themeAccent(),
                    v.roiDraw ? "Drag to draw the region of interest" : "Click a pixel or drag to sample an area");
    if (v.sampleDragging || !v.sampleBox.empty())
        rectangle(v.sampleDragging ? imageBox(v.sampleStart, v.sampleEnd) : v.sampleBox, IM_COL32(100, 205, 255, 235));
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
        const Node* pickNode = pk.mode == "color" ? app.node(pk.node) : nullptr;
        const bool gradeInput = pickNode && pickNode->type == "Grade" && io.KeyCtrl && io.KeyAlt;
        if (v.hovered && ImGui::IsKeyPressed(ImGuiKey_Escape)) pk.active = false;
        if (v.hovered && (!io.KeyAlt || gradeInput) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
            } else if (pk.fromInput || gradeInput) {
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
                           : pk.fromInput || gradeInput ? "PICK (input): click or drag over the screen   Esc: stop" : "PICK: click or drag   Esc: stop";
        dl->AddText(ImVec2(p0.x + 10, p1.y - 22), IM_COL32(255, 210, 120, 255), hint);
    }

    // Q hid the overlays, and with them the viewer tools: a click in the viewer while a tool node's
    // panel is showing (Roto, Tracker, Transform, any node with handles) brings them back, and the
    // click goes on to the tool (it was a roto that "wouldn't draw", a handle that wasn't there).
    if (active && !v.overlays && toolsHovered && v.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyAlt)
        if (Node* tn = activeToolNode(app)) {
            v.overlays = true;
            app.message("viewer overlays back on for " + tn->name + " (Q hides them)");
        }
    // Roto shapes and tools (when a Roto node's properties are open): in the active viewer.
    if (!active) {
    } else if (v.overlays) {
        const ViewXf xf = viewXf();
        ui.viewerImage = {res.image && v.tex && !v.texBounds.empty() ? v.tex : 0u, v.texBounds, v.texScale, dp};
        drawRotoViewer(app, ui, dl, p0, p1, xf, toolsHovered);
    }
    if (v.overlays) {
        const ViewXf xf = viewXf();
        drawTrackerViewer(app, ui, dl, p0, p1, xf, toolsHovered);
    }
    if (v.overlays) {
        const ViewXf xf = viewXf();
        drawPlanarViewer(app, ui, dl, p0, p1, xf, toolsHovered);
    }
    if (v.overlays) {
        const ViewXf xf = viewXf();
        drawTransformViewer(app, ui, dl, p0, p1, xf, toolsHovered);
        drawKnobHandles(app, ui, dl, p0, p1, xf, toolsHovered);
    }
    if (v.overlays) {
        const ViewXf xf = viewXf();
        drawViewer3DIn2D(app, ui, dl, p0, p1, xf, toolsHovered);
    }
    if (v.overlays && activeVanishCam(app)) {
        ViewXf xf{center, v.pan, v.zoom};
        drawVanishCamViewer(app, ui, dl, p0, p1, xf, (v.hovered || ImGui::IsItemActive()) && !pk.active);
    }
    if (v.overlays && activeFlowField(app)) {
        const ViewXf xf = viewXf();
        drawFlowPaintViewer(app, ui, dl, p0, p1, xf, toolsHovered);
    }
    if (active && v.overlays && activeMarkup(app)) {
        const ViewXf xf = viewXf();
        drawMarkupViewer(app, ui, dl, p0, p1, xf, toolsHovered);
    }
    if (v.overlays && activeTrackMask(app)) {
        const ViewXf xf = viewXf();
        drawTrackMaskViewer(app, ui, dl, p0, p1, xf, toolsHovered);
        drawTrackMaskHud(app, ui, p0, p1);
    }
    if (v.overlays) {
        const ViewXf xf = viewXf();
        drawCamTrackerViewer(app, ui, dl, p0, p1, xf, toolsHovered);
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
    if (active && v.res.node == nodeA && v.res.frame == app.frame && v.res.image)
        queueViewerCapture(app, ui, dl);
    dl->PopClipRect();

    // ---- info bar: format, bbox, pixel under the cursor
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6, p1.y + 4));
    if (res.image) {
        ImGui::TextDisabled("%dx%d  bbox %d %d %d %d  %d ch  frame %g", res.format.width, res.format.height,
                            res.bbox.x1, res.bbox.y1, res.bbox.x2, res.bbox.y2, (int)res.image->channels.size(),
                            res.frame);
        if (v.hovered || !v.sampleBox.empty()) {
            const bool picked = !v.sampleBox.empty();
            ImVec2 ip = picked ? ImVec2((float)v.sampleBox.x1, (float)v.sampleBox.y1) : toImage(io.MousePos);
            int x = (int)std::floor(ip.x), y = (int)std::floor(ip.y);
            const Image& im = *res.image;
            int sx = (int)std::floor(ip.x * res.scale), sy = (int)std::floor(ip.y * res.scale);
            const auto m = displayChannels(im, v);
            ImGui::SameLine(0, 20);
            ImGui::Text("%sx=%d y=%d", picked ? "Sample " : "", x, y);
            const ImVec4 cols[4] = {ImVec4(1, .45f, .45f, 1), ImVec4(.5f, 1, .5f, 1), ImVec4(.55f, .7f, 1, 1),
                                    ImVec4(.85f, .85f, .85f, 1)};
            float rgba[4];
            for (int c = 0; c < 4; ++c) {
                rgba[c] = picked ? v.pickedColor[c] : m[c] >= 0 ? im.at(sx, sy, m[c]) : c == 3 && im.bounds.contains(sx, sy) ? 1.0f : 0.0f;
                ImGui::SameLine(0, 12);
                const int byte = std::isfinite(rgba[c]) ? (int)std::lround(std::clamp(rgba[c], 0.0f, 1.0f) * 255) : 0;
                if (v.sampleDisplay == 1) ImGui::TextColored(cols[c], "%d", byte);
                else if (v.sampleDisplay == 2) ImGui::TextColored(cols[c], "%02X", (unsigned int)byte);
                else ImGui::TextColored(cols[c], "%.5f", rgba[c]);
            }
            if (!v.sampleDisplay && (picked || im.bounds.contains(sx, sy)) && m[0] >= 0 && m[1] >= 0 && m[2] >= 0) {
                float h, s, value;
                ImGui::ColorConvertRGBtoHSV(rgba[0], rgba[1], rgba[2], h, s, value);
                // Same RGB weights as the Viewer's Y channel.
                const float luminance = 0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2];
                if (!wrapPixelInfo) ImGui::SameLine(0, 12);
                ImGui::Text("H: %.1f  S: %.2f  V: %.2f  L: %.5f", h * 360.0f, s, value, luminance);
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
        ImGui::SetNextItemWidth(160);
        viewerResolutionCombo(v, "resolution");
        viewerDisplayOptions(v);
        ImGui::Checkbox("matte overlay  [M]", &v.matte);
        ImGui::Checkbox("deep samples under the cursor", &v.deepReadout);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Viewing a deep node: rest the cursor on a pixel to list its samples (depth, colour, alpha).");
        bool paused = app.viewPaused;
        if (active && ImGui::Checkbox("pause updates  [P]", &paused)) {
            app.viewPaused = paused;
            if (!paused) app.requestView();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

void openViewerCapture(App& app, UiState& ui) {
    if (app.viewed.empty() && !viewer3DActive(app, ui)) {
        app.message("Connect an input to the Viewer to capture it");
        return;
    }
    if (ui.viewerCapture.running) return;
    const auto [first, last] = app.playRange();
    auto& c = ui.viewerCapture;
    c.first = (int)std::lround(first);
    c.last = (int)std::lround(last);
    c.configure = true;
}

namespace {
void finishViewerCapture(App& app, ViewerCaptureState& c) {
    c.running = false;
    c.framePath.clear();
    if (app.activeViewer == c.viewer && app.editGeneration() == c.generation) {
        app.frame = c.originalFrame;
        app.requestView(true);
    }
}

void advanceViewerCapture(App& app, UiState& ui) {
    auto& c = ui.viewerCapture;
    if (!c.running) return;
    if (c.failed || app.activeViewer != c.viewer || app.viewed != c.node || app.editGeneration() != c.generation) {
        if (!c.failed) app.message("Capture stopped because the Viewer or comp changed");
        finishViewerCapture(app, c);
        return;
    }
    if (c.wroteFrame) {
        c.wroteFrame = false;
        if (c.next == c.last) {
            app.message("Viewer capture saved: " + c.destination);
            finishViewerCapture(app, c);
            return;
        }
        ++c.next;
    }
    const double frame = c.sequence ? c.next : c.originalFrame;
    if (app.frame != frame) {
        app.frame = frame;
        app.requestView(true);
    }
    if (!viewer3DActive(app, ui)) {
        const ViewResult r = app.result();
        if (app.rendering() || r.node != app.viewed || r.frame != frame) return;
        if (!r.image) {
            app.message("Capture stopped: " + (r.error.empty() ? std::string("no image at this frame") : r.error));
            finishViewerCapture(app, c);
            return;
        }
    }
    c.framePath = c.sequence ? expandFramePattern(c.destination + "/viewer.####.png", c.next) : c.destination;
}
}  // namespace

void drawViewerCaptureControls(App& app, UiState& ui) {
    auto& c = ui.viewerCapture;
    if (c.configure) {
        c.configure = false;
        ImGui::OpenPopup("Capture Viewer");
    }
    if (ImGui::BeginPopupModal("Capture Viewer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Checkbox("Capture a frame range", &c.sequence);
        if (c.sequence) {
            ImGui::SetNextItemWidth(100);
            ImGui::InputInt("First", &c.first, 0);
            ImGui::SetNextItemWidth(100);
            ImGui::InputInt("Last", &c.last, 0);
        }
        ImGui::BeginDisabled(c.sequence && c.last < c.first);
        if (ImGui::Button(c.sequence ? "Choose output folder" : "Save PNG")) {
            ImGui::CloseCurrentPopup();
            const auto start = [&app, &c](const std::string& path) {
                c.destination = path;
                if (c.sequence) {
                    namespace fs = std::filesystem;
                    const fs::path folder(std::u8string(path.begin(), path.end()));
                    std::error_code ec;
                    for (int i = 1; i <= 1000; ++i) {
                        const fs::path dest = folder / ("viewer-capture-" + std::to_string(i));
                        if (fs::create_directory(dest, ec)) {
                            c.destination = path + "/viewer-capture-" + std::to_string(i);
                            break;
                        }
                        if (ec) break;
                    }
                    if (c.destination == path) {
                        app.message("Could not create a Viewer capture folder");
                        return;
                    }
                }
                c.running = true;
                c.failed = c.wroteFrame = false;
                c.originalFrame = app.frame;
                c.viewer = app.activeViewer;
                c.node = app.viewed;
                c.generation = app.editGeneration();
                c.next = c.sequence ? c.first : c.last;
                app.playing = false;
                app.requestView(true);
            };
            if (c.sequence) openFolderDialog(ui, "Choose capture output folder", start);
            else {
                openFileDialog(ui, "Save Viewer capture", true, {".png"}, start);
                ui.fileDialog.filename = "viewer.png";
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (c.running) {
        ImGui::SetNextWindowSize(ImVec2(245, 85), ImGuiCond_Always);
        if (ImGui::Begin("Viewer capture", nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoResize)) {
            if (c.sequence) ImGui::Text("Capturing frame %d of %d", c.next, c.last);
            else ImGui::TextUnformatted("Capturing the current frame");
            if (ImGui::Button("Cancel capture")) finishViewerCapture(app, c);
        }
        ImGui::End();
    }
}

void openFlipbook(App& app, UiState& ui) {
    if (app.viewed.empty()) {
        app.message("Connect an image to the Viewer to make a flipbook");
        return;
    }
    const auto [first, last] = app.playRange();
    ui.flipbookSetup = std::pair{(int)std::lround(first), (int)std::lround(last)};
}

static void startFlipbook(App& app, UiState& ui, int first, int last) {
    auto& fb = ui.flipbook;
    app.clearAuxView(kFlipbookRequest);
    if (fb.display.tex) glDeleteTextures(1, &fb.display.tex);
    fb = {};
    fb.open = fb.loading = true;
    fb.node = app.viewed;
    fb.generation = app.editGeneration();
    fb.first = first;
    fb.last = last;
    fb.fps = std::max(1.0, app.comp->fps);
    fb.layer = ui.viewer.layer;
    fb.channel = ui.viewer.channel;
    fb.fstop = ui.viewer.fstop;
    fb.gamma = ui.viewer.gamma;
    fb.matte = ui.viewer.matte;
    fb.display.alphaChannel = ui.viewer.alphaChannel;
    fb.display.checkerboard = ui.viewer.checkerboard;
    fb.display.clippingWarning = ui.viewer.clippingWarning;
    fb.scale = ui.viewer.reqScale > 0 ? ui.viewer.reqScale : 1.0;
    const auto& image = ui.viewer.res;
    const double pixels = (double)std::max(1, image.format.width) * std::max(1, image.format.height);
    const double frameCount = (double)last - first + 1.0;
    const int channels = image.image ? std::max(4, image.image->nch()) : 4;
    while (fb.scale > 1.0 / 32 && pixels * fb.scale * fb.scale * frameCount * channels * sizeof(float) > kFlipbookBytes)
        fb.scale *= 0.5;
}

void drawFlipbook(App& app, UiState& ui) {
    if (ui.flipbookSetup && !ImGui::IsPopupOpen("Render Flipbook")) ImGui::OpenPopup("Render Flipbook");
    if (ImGui::BeginPopupModal("Render Flipbook", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto& [first, last] = *ui.flipbookSetup;
        ImGui::SetNextItemWidth(95);
        ImGui::InputInt("First", &first, 0);
        ImGui::SetNextItemWidth(95);
        ImGui::InputInt("Last", &last, 0);
        ImGui::BeginDisabled(first > last || app.viewed.empty());
        if (ImGui::Button("Render")) {
            startFlipbook(app, ui, first, last);
            ui.flipbookSetup.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ui.flipbookSetup.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    auto& fb = ui.flipbook;
    if (!fb.open) {
        if (!fb.node.empty()) {
            app.clearAuxView(kFlipbookRequest);
            if (fb.display.tex) glDeleteTextures(1, &fb.display.tex);
            fb = {};
        }
        return;
    }
    if (fb.loading) {
        if (app.editGeneration() != fb.generation || app.editsPending() || app.comp.get() != ui.timelineComp)
            fb.error = "The comp changed during rendering. Start the flipbook again.";
        const int64_t wanted = (int64_t)fb.first + (int64_t)fb.frames.size();
        const ViewResult result = app.auxResult(kFlipbookRequest);
        if (fb.error.empty() && result.serial > fb.lastSerial && result.node == fb.node && result.frame == wanted) {
            fb.lastSerial = result.serial;
            if (!result.error.empty()) fb.error = result.error;
            else if (!result.image) fb.error = "The Viewer input has no image at frame " + std::to_string(wanted);
            else {
                const size_t bytes = result.image->data.size() * sizeof(float);
                if (bytes > kFlipbookBytes - fb.bytes)
                    fb.error = "Flipbook filled its 256 MB preview cache. Try a shorter playback range.";
                else {
                    fb.bytes += bytes;
                    fb.frames.push_back(result);
                }
            }
        }
        const int64_t next = (int64_t)fb.first + (int64_t)fb.frames.size();
        if (!fb.error.empty() || next > fb.last) {
            fb.loading = false;
            app.clearAuxView(kFlipbookRequest);
            fb.playing = fb.error.empty() && fb.frames.size() > 1;
            fb.started = ImGui::GetTime();
        } else {
            app.requestAuxView(kFlipbookRequest, fb.node, "", (double)next, Box(), fb.scale);
        }
    }

    ImGui::SetNextWindowSize(ImVec2(640, 420), ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin("Flipbook", &fb.open, ImGuiWindowFlags_NoDocking);
    if (visible) {
        const int count = (int)fb.frames.size();
        if (fb.loading) ImGui::Text("Rendering frame %d of %d", fb.first + count, fb.last);
        if (!fb.error.empty()) ImGui::TextWrapped("%s", fb.error.c_str());
        if (count > 0) {
            const double now = ImGui::GetTime();
            if (fb.playing) fb.index = (int)std::fmod(std::floor((now - fb.started) * fb.fps), (double)count);
            if (ImGui::Button(fb.playing ? "Pause" : "Play")) {
                fb.playing = !fb.playing;
                fb.started = now - fb.index / fb.fps;
            }
            ImGui::SameLine();
            const int first = fb.first, last = first + count - 1;
            int frame = first + fb.index;
            ImGui::SetNextItemWidth(std::max(40.0f, ImGui::GetContentRegionAvail().x - 90));
            if (ImGui::SliderInt("##frame", &frame, first, last, "%d")) {
                fb.index = frame - first;
                fb.playing = false;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("1:%d  %g fps", (int)std::lround(1.0 / fb.scale), fb.fps);
            if (fb.uploaded != fb.index) {
                fb.display.res = fb.frames[fb.index];
                fb.display.layer = fb.layer;
                viewerUpload(fb.display);
                fb.uploaded = fb.index;
            }
            const ImVec2 area = ImGui::GetContentRegionAvail(), origin = ImGui::GetCursorScreenPos();
            const auto& format = fb.display.res.format;
            const Box& b = fb.display.texBounds;
            if (area.x > 0 && area.y > 0 && format.width > 0 && format.height > 0 && !b.empty()) {
                const float scale = std::min(area.x / format.width, area.y / format.height);
                const ImVec2 size(format.width * scale, format.height * scale);
                const ImVec2 lo(origin.x + (area.x - size.x) * 0.5f, origin.y + (area.y - size.y) * 0.5f);
                const ImVec2 hi(lo.x + size.x, lo.y + size.y);
                DisplayParams dp;
                dp.gain = std::pow(2.0f, fb.fstop);
                dp.gamma = fb.gamma;
                dp.channel = fb.channel;
                dp.matte = fb.matte;
                dp.ocio = viewerTransform(app);
                const Node* source = app.node(fb.node);
                dp.raw = app.comp->project.viewerView == "Raw" || (source && source->outputIsData());
                dp.checkerboard = fb.display.checkerboard;
                dp.clippingWarning = fb.display.clippingWarning;
                viewerDrawImage(ImGui::GetWindowDrawList(), fb.display.tex, lo, hi,
                                ImVec2((float)-b.x1 / b.width(), (float)(format.height * fb.scale - b.y1) / b.height()),
                                ImVec2((float)(format.width * fb.scale - b.x1) / b.width(), (float)-b.y1 / b.height()), dp);
            }
        }
    }
    ImGui::End();
}

void activateViewer(App& app, UiState& ui, const std::string& name) {
    if (name.empty() || !app.viewerNode(name)) return;
    if (name == ui.viewerOf && name == app.activeViewer) return;
    if (name != ui.viewerOf) {
        ui.viewer.timelinePin = app.pinnedRange();
        if (!ui.linkViewerPlayback) ui.viewer.timelineFrame = app.frame;
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
        app.pinRange(ui.viewer.timelinePin);
        if (!ui.linkViewerPlayback) {
            app.frame = ui.viewer.timelineFrame.value_or(app.frame);
            app.playing = false;
        }
        ui.viewer.shownSerial = 0;  // the main renderer's results now
        ui.viewer.reqRegion = Box();
        ui.viewer.reqScale = 0;
    }
    if (app.activeViewer != name) {
        app.activeViewer = name;
        app.syncViewer();
    }
    app.requestView();
}

// Every viewer panel: the first Viewer node in the main "Viewer" panel, the others (Ctrl+I) in
// panels of their own, docked beside it when they first appear.
void drawViewer(App& app, UiState& ui) {
    if (ui.timelineComp != app.comp.get()) {
        ui.timelineComp = app.comp.get();
        app.pinRange(std::nullopt);
        auto reset = [](ViewerState& v) {
            v.timelineFrame.reset();
            v.timelinePin.reset();
            v.timelineWindow.reset();
            v.alphaChannel.clear();
            v.dirtyTexture = true;
            v.roi = {};
            v.roiEnabled = v.roiDraw = v.roiDragging = false;
            v.sampleBox = {};
            v.sampleArmed = v.sampleDragging = false;
        };
        reset(ui.viewer);
        for (auto& [name, v] : ui.otherViewers) reset(v);
    }
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
    advanceViewerCapture(app, ui);
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
