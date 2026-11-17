// ImageGen: all-in-one AI image generation in a single node (FLUX.2 [dev] / [klein] and FLUX 3
// [klein] through Diffusers on the ML worker: ml/strata_ml/tasks/imagegen.py, imagegen_flux3.py). Checkpoint, prompts, a LoRA
// stack, sampler and batch settings are all knobs of this one node; its output is an ordinary
// RGBA image (linear, converted to the comp's working space) that composites like a Read.
//
// Inputs: source    (optional RGBA: the base image for Img2Img / Inpaint; with use_reference also a
//                    reference image),
//         mask      (optional matte: Inpaint; its alpha, or luminance),
//         reference (optional RGBA: a separate reference image, e.g. a style or character to
//                    follow; it never becomes the base and doesn't change the mode).
//
// One press of Generate is one job: `batch_count` takes with seeds seed, seed+1, ... The node
// outputs the selected take; every take of every run stays on disk in the ML cache with its
// exact settings (result.json "takes"), and `history` lists the node's runs, newest first.
//
// Params (the data schema; defaults in core/Schema.cpp):
//   checkpoint        a name from ml::checkpoints() (a Diffusers folder or a single .safetensors)
//   base              "auto" | a Diffusers folder name: what a transformer-only file runs on
//   positive_prompt, negative_prompt
//   mode              "Auto" | "Txt2Img" | "Img2Img" | "Inpaint"   (Auto: Inpaint with source +
//                     mask, Img2Img with a source or a promoted take, else Txt2Img)
//   denoise_strength  0..1 (Img2Img / Inpaint)
//   loras             [{"enabled": bool, "file": name, "weight": -2..2}, ...]
//   width, height     multiples of 16, 256..4096
//   steps, guidance_scale, sampler, seed, seed_mode ("Fixed" | "Increment" | "Randomize"), batch_count 1..16
//   take              which take of the result the node outputs (0-based)
//   fit               "crop" | "pad" | "stretch": how the source meets width x height
//   mask_channel ("alpha" | "luminance"), invert_mask, mask_grow (px), mask_feather (px)
//   use_reference     also hand the source to the model as a reference image
//   prompt_upsampler  FLUX 3: rewrite the prompt with BFL's prompt upsampler (flux-3-image-klein-pup) first
//   dtype ("bf16" | "fp16" | "fp32"), offload ("auto" | "model" | "group" | "sequential" | "none"), vae_tiling
//   preview_every     steps between live previews (0 = none), preview_size (px)
//   promoted          {"job": key, "take": i} or null: a take used as the source
//   history           [job keys], newest first (the node's finished runs)
//   ml                {"main": {"job": key, "result": key}} (as every ML node)
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../ml/ML.h"
#include "MLNodes.h"

namespace strata {
class Comp;
}

namespace strata::imagegen {

enum class Mode { Txt2Img, Img2Img, Inpaint };
const char* modeName(Mode m);  // "Txt2Img", ...

struct AspectPreset {
    const char* label;  // "1:1", "16:9", ...
    int width, height;  // about one megapixel, multiples of 16
};
const std::vector<AspectPreset>& aspectPresets();

// The worker's samplers (the keys of SAMPLERS in ml/strata_ml/tasks/imagegen.py), first = default.
const std::vector<std::string>& samplers();

constexpr int kSizeStep = 16, kMinSize = 256, kMaxSize = 4096, kMaxBatch = 16, kHistory = 24;
int snapSize(double px);  // nearest multiple of 16 in [256, 4096]

struct LoraEntry {
    bool enabled = true;
    std::string file;  // a name from ml::loras() (or an absolute path)
    double weight = 1.0;
};
std::vector<LoraEntry> lorasOf(const Params& p);
void setLoras(Params& p, const std::vector<LoraEntry>& rows);

// One generated image of a finished (or running) job.
struct Take {
    int index = 0;
    uint32_t seed = 0;
    int width = 0, height = 0;
    json meta;  // everything it was made with: prompt, steps, sampler, guidance, denoise, loras, seconds, ...
};

// The take the node uses as its source ("Active Source: Take #X (Seed Y)").
struct PromotedTake {
    bool active = false;  // a take is promoted
    std::string job;      // its run
    int index = 0;        // 0-based (the UI shows index + 1)
    uint32_t seed = 0;
    bool found = false;   // its run is still in the ML cache (seed is only known then)
};

// What branchTakeToNewNode sets the new node's denoise to, and what promoting a take sets it to
// when it was still at 1.0 (a take is a finished image: refine it, don't replace it).
constexpr double kRefineDenoise = 0.55;

// A running job, for the node's panel: steps of the current image, and where the batch is.
struct Live {
    std::string state;  // "" (nothing running), "queued", "running", "done", "error", "cancelled"
    double progress = 0;
    std::string message;
    int step = 0, steps = 0;
    int batchIndex = 0, batchSize = 0;
    int takesDone = 0;
    int previewSeq = 0;  // goes up whenever livePreview() has a new picture
};

class ImageGenNode : public mlnodes::MLNode {
public:
    // ---------------------------------------------------------------- graph and pixels
    int maxInputs() const override { return 3; }
    std::string inputLabel(int i) const override { return i == 0 ? "source" : i == 1 ? "mask" : "reference"; }
    bool timeDependent() const override { return false; }
    std::string slotLabel(const std::string&) const override { return "Generate"; }
    Format format(Engine& e, double f) override;   // the selected take's size; before any, width x height
    Box dod(Engine& e, double f) override { return format(e, f).box(); }
    ChannelSet channels(Engine&, double) override { return rgbaChannels(); }
    void render(RenderArgs& a, Image& out) override;
    json info(Engine& e, double f) override;
    std::vector<std::string> knobChoices(const std::string& knob) const override;

    // ---------------------------------------------------------------- the job
    mlnodes::JobSpec spec(const std::string& slot, Engine& e) override;
    void runSubmitted(const std::string& slot, const std::string& key) override;   // seed_mode
    void resultAttached(const std::string& slot, const std::string& key) override;  // history, take

    // ---------------------------------------------------------------- state, for the UI
    Mode effectiveMode() const;                 // what Generate would run now (Auto resolved)
    std::optional<std::pair<std::string, int>> promoted() const;  // the take used as the source
    // A finished run's takes (default: the node's current result); empty if there's none.
    std::vector<Take> takes(const std::string& key = {}) const;
    // The takes the running job has finished so far.
    std::vector<Take> liveTakes() const;
    // A take's pixels (linear Rec.709 RGBA, top row first, full resolution), from the ML cache.
    std::shared_ptr<const ml::NpyImage> takeImage(int index, const std::string& key = {}) const;
    int takeIndex() const;                      // the take the node outputs (clamped)
    // The take of run `key` (default: the current result) the node outputs; nullopt when that run
    // isn't the node's result (or has no takes). For highlighting the selected tile.
    std::optional<int> selectedTakeIndex(const std::string& key = {}) const;
    PromotedTake promotedTake() const;          // the promoted take, with its seed
    std::vector<std::string> history() const;   // finished runs, newest first
    Live live() const;                          // the submitted job (if it isn't the result yet)
    std::shared_ptr<const ml::NpyImage> livePreview() const;  // display-referred RGB, small
    bool running() const;

    // ---------------------------------------------------------------- edits (call inside App::edit)
    void selectTake(int index, const std::string& key = {});   // a take of another run makes that run the result
    void promoteTake(const std::string& key, int index);       // use a take as the source (denoise 1.0 -> 0.55)
    void clearPromoted();
    void setSize(int width, int height);       // snapped
    bool matchInput(Engine& e);                // width x height = the source's (snapped); false without one
    void addLora(const std::string& file = {});
    void removeLora(int row);
    void advanceSeed();                        // what seed_mode does after each run

    // The params a branch starts from: everything but the node's runs, its take and its source.
    json branchParams(const std::string& key, int index) const;

private:
    std::string takesKey(const std::string& key) const { return key.empty() ? result("main") : key; }
};

// Editor action: a new ImageGen node below `from` in the graph whose source is take `index` of run
// `key` (promoted, so Auto runs Img2Img on it), with the same prompts, model, LoRA stack and
// settings, the same reference input, and denoise_strength 0.55. Call inside App::edit; returns the new node (null if `from`
// has no such take).
ImageGenNode* branchTakeToNewNode(Comp& c, ImageGenNode& from, const std::string& key, int index);

}  // namespace strata::imagegen
