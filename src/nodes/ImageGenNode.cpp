#include "ImageGenNode.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "../core/Comp.h"
#include "../core/Engine.h"

namespace strata::imagegen {

using mlnodes::JobSpec;

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Img2Img: return "Img2Img";
        case Mode::Inpaint: return "Inpaint";
        default: return "Txt2Img";
    }
}

const std::vector<AspectPreset>& aspectPresets() {
    static const std::vector<AspectPreset> v = {
        {"1:1", 1024, 1024}, {"4:3", 1184, 880}, {"3:2", 1248, 832}, {"16:9", 1360, 768}, {"21:9", 1568, 672},
        {"3:4", 880, 1184},  {"2:3", 832, 1248}, {"9:16", 768, 1360},
    };
    return v;
}

const std::vector<std::string>& samplers() {
    static const std::vector<std::string> v = {"euler", "euler (stochastic)", "euler (karras)", "euler (beta)", "lcm"};
    return v;
}

int snapSize(double px) {
    int s = (int)std::lround(px / kSizeStep) * kSizeStep;
    return std::clamp(s, kMinSize, kMaxSize);
}

std::vector<LoraEntry> lorasOf(const Params& p) {
    std::vector<LoraEntry> v;
    if (!p.has("loras") || !p.data["loras"].is_array()) return v;
    for (auto& j : p.data["loras"]) {
        if (!j.is_object()) continue;
        LoraEntry e;
        e.enabled = j.value("enabled", true);
        e.file = j.value("file", std::string());
        e.weight = std::clamp(j.value("weight", 1.0), -2.0, 2.0);
        v.push_back(e);
    }
    return v;
}

void setLoras(Params& p, const std::vector<LoraEntry>& rows) {
    json a = json::array();
    for (auto& e : rows) a.push_back({{"enabled", e.enabled}, {"file", e.file}, {"weight", std::clamp(e.weight, -2.0, 2.0)}});
    p.data["loras"] = a;
}

namespace {

std::vector<Take> takesFrom(const json& arr) {
    std::vector<Take> v;
    if (!arr.is_array()) return v;
    for (auto& t : arr) {
        if (!t.is_object()) continue;
        Take k;
        k.index = t.value("index", (int)v.size());
        k.seed = (uint32_t)t.value("seed", 0LL);
        k.width = t.value("width", 0);
        k.height = t.value("height", 0);
        k.meta = t;
        v.push_back(std::move(k));
    }
    return v;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

}  // namespace

// ================================================================= state
Mode ImageGenNode::effectiveMode() const {
    const std::string m = params.str("mode", "Auto");
    if (m == "Txt2Img") return Mode::Txt2Img;
    if (m == "Img2Img") return Mode::Img2Img;
    if (m == "Inpaint") return Mode::Inpaint;
    const bool src = input(0) != nullptr || promoted().has_value();
    if (src && input(1)) return Mode::Inpaint;
    return src ? Mode::Img2Img : Mode::Txt2Img;
}

std::optional<std::pair<std::string, int>> ImageGenNode::promoted() const {
    if (!params.has("promoted") || !params.data["promoted"].is_object()) return std::nullopt;
    const json& p = params.data["promoted"];
    std::string key = p.value("job", std::string());
    if (key.empty()) return std::nullopt;
    return std::make_pair(key, p.value("take", 0));
}

std::vector<Take> ImageGenNode::takes(const std::string& key) const {
    auto r = ml::jobResult(takesKey(key));
    return r ? takesFrom(r->value("takes", json())) : std::vector<Take>{};
}

std::vector<Take> ImageGenNode::liveTakes() const {
    std::string k = job("main");
    if (k.empty() || k == result("main")) return {};
    return takesFrom(ml::jobFile(k, "out/takes.json"));
}

std::shared_ptr<const ml::NpyImage> ImageGenNode::takeImage(int index, const std::string& key) const {
    std::string k = takesKey(key);
    if (k.empty() || index < 0) return nullptr;
    return ml::outputFrame(k, "take", index);
}

int ImageGenNode::takeIndex() const {
    int n = (int)takes().size();
    int t = (int)params.num("take", 0, 0);
    return n > 0 ? std::clamp(t, 0, n - 1) : 0;
}

std::vector<std::string> ImageGenNode::history() const { return params.strList("history"); }

bool ImageGenNode::running() const {
    std::string k = job("main");
    if (k.empty() || k == result("main")) return false;
    std::string st = ml::jobStatus(k).state;
    return st == "queued" || st == "running";
}

Live ImageGenNode::live() const {
    Live l;
    std::string k = job("main");
    if (k.empty() || k == result("main")) return l;
    ml::JobStatus st = ml::jobStatus(k);
    l.state = st.state;
    l.progress = st.progress;
    l.message = st.message;
    const json& d = st.detail;
    if (d.is_object()) {
        l.step = d.value("step", 0);
        l.steps = d.value("steps", 0);
        l.batchIndex = d.value("batch_index", 0);
        l.batchSize = d.value("batch_size", 0);
        l.takesDone = d.value("takes_done", 0);
        l.previewSeq = d.value("preview_seq", 0);
    }
    return l;
}

std::shared_ptr<const ml::NpyImage> ImageGenNode::livePreview() const {
    std::string k = job("main");
    if (k.empty() || k == result("main")) return nullptr;
    return ml::liveFrame(k, "out/preview.npy");
}

// ================================================================= edits
void ImageGenNode::selectTake(int index, const std::string& key) {
    if (!key.empty() && key != result("main") && ml::jobResult(key)) {
        json& m = params.data["ml"];
        if (!m.is_object()) m = json::object();
        if (!m.contains("main") || !m["main"].is_object()) m["main"] = json::object();
        m["main"]["result"] = key;
        if (job("main").empty()) m["main"]["job"] = key;
    }
    params.data["take"] = std::max(0, index);
}

void ImageGenNode::promoteTake(const std::string& key, int index) {
    if (key.empty()) return;
    params.data["promoted"] = {{"job", key}, {"take", std::max(0, index)}};
}

void ImageGenNode::clearPromoted() { params.data.erase("promoted"); }

void ImageGenNode::setSize(int width, int height) {
    params.data["width"] = snapSize(width);
    params.data["height"] = snapSize(height);
}

bool ImageGenNode::matchInput(Engine& e) {
    if (auto p = promoted()) {
        for (auto& t : takes(p->first))
            if (t.index == p->second) {
                setSize(t.width, t.height);
                return true;
            }
    }
    Node* in = input(0);
    if (!in) return false;
    Format f = in->format(e, runFrame);
    setSize(f.width * f.pixelAspect, f.height);
    return true;
}

void ImageGenNode::addLora(const std::string& file) {
    auto rows = lorasOf(params);
    LoraEntry e;
    e.file = file;
    rows.push_back(e);
    setLoras(params, rows);
}

void ImageGenNode::removeLora(int row) {
    auto rows = lorasOf(params);
    if (row < 0 || row >= (int)rows.size()) return;
    rows.erase(rows.begin() + row);
    setLoras(params, rows);
}

void ImageGenNode::advanceSeed() {
    const std::string mode = params.str("seed_mode", "Increment");
    const int64_t seed = (int64_t)params.num("seed", 0, 0);
    if (mode == "Increment") {
        int64_t n = std::clamp((int)params.num("batch_count", 0, 1), 1, kMaxBatch);
        params.data["seed"] = (seed + n) % 2147483648LL;
    } else if (mode == "Randomize") {
        static std::mt19937 rng{std::random_device{}()};
        params.data["seed"] = (int64_t)std::uniform_int_distribution<int64_t>(0, 2147483647LL)(rng);
    }
}

void ImageGenNode::runSubmitted(const std::string&, const std::string&) { advanceSeed(); }

void ImageGenNode::resultAttached(const std::string&, const std::string& key) {
    std::vector<std::string> h = history();
    h.erase(std::remove(h.begin(), h.end(), key), h.end());
    h.insert(h.begin(), key);
    if ((int)h.size() > kHistory) h.resize(kHistory);
    params.data["history"] = h;
    int n = (int)takes(key).size();
    if ((int)params.num("take", 0, 0) >= n) params.data["take"] = 0;
}

// ================================================================= pixels
Format ImageGenNode::format(Engine&, double) {
    Format f;
    auto ts = takes();
    int t = takeIndex();
    if (t < (int)ts.size() && ts[t].width > 0 && ts[t].height > 0) {
        f.width = ts[t].width;
        f.height = ts[t].height;
    } else {
        f.width = snapSize(params.num("width", 0, 1024));
        f.height = snapSize(params.num("height", 0, 1024));
    }
    f.pixelAspect = 1.0;
    return f;
}

void ImageGenNode::render(RenderArgs& a, Image& out) {
    std::fill(out.data.begin(), out.data.end(), 0.0f);
    if (takes().empty()) return;  // nothing generated yet: transparent black at width x height
    int r = out.ch("R"), g = out.ch("G"), b = out.ch("B"), al = out.ch("A");
    fillFromOutput(a, out, "main", "take", {{r, 0}, {g, 1}, {b, 2}, {al, 3}}, takeIndex());
}

json ImageGenNode::info(Engine&, double) {
    json j = {{"mode", modeName(effectiveMode())}};
    auto ts = takes();
    if (!ts.empty()) {
        const Take& t = ts[takeIndex()];
        j["take"] = std::to_string(takeIndex() + 1) + " of " + std::to_string(ts.size());
        j["seed"] = t.seed;
        j["size"] = std::to_string(t.width) + "x" + std::to_string(t.height);
    }
    if (auto r = resultJson("main"); r && r->contains("warnings") && !(*r)["warnings"].empty()) j["warnings"] = (*r)["warnings"];
    return j;
}

std::vector<std::string> ImageGenNode::knobChoices(const std::string& knob) const {
    std::vector<std::string> v;
    if (knob == "checkpoint") {
        for (auto& m : ml::checkpoints())
            if (!m.folder || m.family.rfind("Flux2", 0) == 0) v.push_back(m.name);
    } else if (knob == "base") {
        v.push_back("auto");
        for (auto& m : ml::checkpoints())
            if (m.folder && m.family.rfind("Flux2", 0) == 0) v.push_back(m.name);
    } else if (knob == "lora") {
        for (auto& m : ml::loras()) v.push_back(m.name);
    } else if (knob == "sampler") {
        v = samplers();
    }
    return v;
}

// ================================================================= the job
JobSpec ImageGenNode::spec(const std::string&, Engine&) {
    JobSpec s;
    s.task = "imagegen";
    const int frame = (int)std::lround(runFrame);
    s.first = s.last = frame;
    const Mode mode = effectiveMode();
    auto fail = [&](const std::string& why) {
        if (s.error.empty()) s.error = why;
    };

    json& p = s.params;
    p = json::object();
    // ---- the model
    const std::string ck = params.str("checkpoint", "");
    auto model = ml::findCheckpoint(ck);
    if (ck.empty()) fail("pick a checkpoint (Diffusers FLUX.2 folders and .safetensors in the checkpoints folder)");
    else if (!model) fail("checkpoint '" + ck + "' isn't in the checkpoints folder any more");
    else {
        if (model->folder && model->family.rfind("Flux2", 0) != 0)
            fail(ck + " is a " + (model->family.empty() ? std::string("unknown") : model->family) + " model; ImageGen runs FLUX.2");
        p["checkpoint"] = model->path;
        p["checkpoint_stamp"] = model->stamp();
        if (!model->folder) {
            const std::string base = params.str("base", "auto");
            if (base == "auto") {
                json cands = json::array();
                for (auto& m : ml::checkpoints())
                    if (m.folder && m.family.rfind("Flux2", 0) == 0) cands.push_back(m.path);
                if (cands.empty()) fail(ck + " is a transformer-only file: it needs the full Diffusers FLUX.2 model it was made from in the checkpoints folder");
                p["base"] = "auto";
                p["base_candidates"] = cands;
            } else if (auto b = ml::findCheckpoint(base); b && b->folder) {
                p["base"] = b->path;
            } else {
                fail("base model '" + base + "' isn't in the checkpoints folder");
            }
        }
    }
    p["dtype"] = params.str("dtype", "bf16");
    p["offload"] = params.str("offload", "model");
    p["vae_tiling"] = params.flag("vae_tiling", false);

    // ---- prompts and sampling
    p["prompt"] = params.str("positive_prompt", "");
    p["negative_prompt"] = params.str("negative_prompt", "");
    p["mode"] = lower(modeName(mode));
    p["width"] = snapSize(params.num("width", frame, 1024));
    p["height"] = snapSize(params.num("height", frame, 1024));
    p["steps"] = std::clamp((int)params.num("steps", frame, 28), 1, 200);
    p["guidance"] = std::clamp(params.num("guidance_scale", frame, 4.0), 0.0, 30.0);
    std::string sampler = params.str("sampler", samplers().front());
    if (std::find(samplers().begin(), samplers().end(), sampler) == samplers().end()) sampler = samplers().front();
    p["sampler"] = sampler;
    p["seed"] = (int64_t)params.num("seed", frame, 0) & 0xFFFFFFFFLL;
    p["batch_count"] = std::clamp((int)params.num("batch_count", frame, 1), 1, kMaxBatch);
    p["denoise"] = std::clamp(params.num("denoise_strength", frame, 0.75), 0.0, 1.0);

    // ---- LoRAs (enabled rows only, resolved to files)
    json loras = json::array();
    for (auto& e : lorasOf(params)) {
        if (!e.enabled || e.file.empty()) continue;
        auto f = ml::findLora(e.file);
        if (!f) {
            fail("LoRA '" + e.file + "' isn't in the LoRAs folder any more");
            continue;
        }
        loras.push_back({{"file", f->path}, {"weight", e.weight}, {"stamp", f->stamp()}});
    }
    p["loras"] = loras;

    // ---- source and mask
    p["fit"] = params.str("fit", "crop");
    p["use_reference"] = params.flag("use_reference", false);
    p["preview_every"] = std::clamp((int)params.num("preview_every", frame, 1), 0, 50);
    p["preview_size"] = std::clamp((int)params.num("preview_size", frame, 512), 64, 1024);
    const bool needSource = mode != Mode::Txt2Img || p["use_reference"].get<bool>();
    if (auto pr = promoted(); pr && needSource) {
        p["source_file"] = ml::jobDir(pr->first) + "/out/take." + std::to_string(pr->second) + ".npy";
        p["source_take"] = pr->first + "#" + std::to_string(pr->second);  // (the file's contents never change)
        if (!ml::outputFrame(pr->first, "take", pr->second)) fail("the promoted take is gone from the ML cache");
    } else if (needSource) {
        s.inputs.push_back({"src", 0, mode != Mode::Txt2Img});
        if (!input(0)) fail(std::string(modeName(mode)) + " needs a source: connect the source input or promote a take");
    }
    if (mode == Mode::Inpaint) {
        s.inputs.push_back({"mask", 1, true});
        p["mask_channel"] = params.str("mask_channel", "alpha");
        p["invert_mask"] = params.flag("invert_mask", false);
        p["mask_grow"] = params.num("mask_grow", frame, 8);
        p["mask_feather"] = params.num("mask_feather", frame, 8);
        if (!input(1)) fail("Inpaint needs a mask: connect the mask input");
    }
    return s;
}

}  // namespace strata::imagegen

namespace strata {
void registerImageGenNode() {
    NodeRegistry::instance().add("ImageGen", [] { return std::make_unique<imagegen::ImageGenNode>(); });
}
}  // namespace strata
