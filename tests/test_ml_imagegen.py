"""Tests for the ImageGen task (ml/strata_ml/tasks/imagegen.py), run through the worker's job protocol.

Needs numpy, PyTorch (CPU is fine), diffusers, transformers, peft and opencv; skipped without them.
The models are tiny FLUX.2 [dev] and FLUX.2 [klein] pipelines with random weights (the real
architectures, a 4-level VAE so a latent token is 16 x 16 pixels as in the real models, and a
word-level tokenizer), saved as Diffusers folders, so loading, caching, LoRAs, img2img, inpaint,
batches, previews and cancel all go through the same code as with the real weights.

Run: python -m unittest tests/test_ml_imagegen.py
"""

import json
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "ml"))

try:
    import numpy as np
    import torch
    import diffusers  # noqa: F401
    import peft  # noqa: F401
    import transformers  # noqa: F401
    import cv2  # noqa: F401
    HAVE = True
except Exception:  # noqa: BLE001
    HAVE = False

FLUX2_SCHEDULER = {"base_image_seq_len": 256, "base_shift": 0.5, "max_image_seq_len": 4096, "max_shift": 1.15,
                   "num_train_timesteps": 1000, "shift": 3.0, "use_dynamic_shifting": True}
SIZE = 256


def tiny_tokenizer():
    from tokenizers import Tokenizer, models, pre_tokenizers
    from transformers import PreTrainedTokenizerFast
    words = ("a an the of on in with and red blue green dog cat house tree sky night day photo painting "
             "system user assistant : you are helpful image prompt").split()
    vocab = {"[PAD]": 0, "[UNK]": 1, "<s>": 2, "</s>": 3}
    for w in words:
        vocab.setdefault(w, len(vocab))
    t = Tokenizer(models.WordLevel(vocab, unk_token="[UNK]"))
    t.pre_tokenizer = pre_tokenizers.Whitespace()
    tok = PreTrainedTokenizerFast(tokenizer_object=t, pad_token="[PAD]", unk_token="[UNK]", bos_token="<s>", eos_token="</s>")
    tok.chat_template = ("{% for m in messages %}{% if m['content'] is string %}{{ m['role'] }} : {{ m['content'] }} "
                         "{% else %}{% for c in m['content'] %}{{ m['role'] }} : {{ c['text'] }} {% endfor %}{% endif %}"
                         "{% endfor %}{% if add_generation_prompt %}assistant :{% endif %}")
    return tok


def tiny_vae(groups=1, quant=False):
    from diffusers import AutoencoderKLFlux2
    torch.manual_seed(0)
    return AutoencoderKLFlux2(sample_size=32, in_channels=3, out_channels=3, down_block_types=("DownEncoderBlock2D",) * 4,
                              up_block_types=("UpDecoderBlock2D",) * 4, block_out_channels=(8, 8, 8, 8), layers_per_block=1,
                              latent_channels=4, norm_num_groups=groups, use_quant_conv=quant, use_post_quant_conv=quant)


def tiny_transformer(guidance):
    from diffusers import Flux2Transformer2DModel
    torch.manual_seed(0)
    return Flux2Transformer2DModel(patch_size=1, in_channels=16, num_layers=1, num_single_layers=2, attention_head_dim=16,
                                   num_attention_heads=2, joint_attention_dim=48, timestep_guidance_channels=256,
                                   axes_dims_rope=[4, 4, 4, 4], guidance_embeds=guidance)


def build_dev(path):
    from diffusers import FlowMatchEulerDiscreteScheduler, Flux2Pipeline
    from transformers import AutoModelForImageTextToText, Mistral3Config
    cfg = Mistral3Config(
        text_config={"model_type": "mistral", "vocab_size": 64, "hidden_size": 16, "intermediate_size": 32,
                     "max_position_embeddings": 1024, "num_attention_heads": 4, "num_hidden_layers": 31,
                     "num_key_value_heads": 2, "rms_norm_eps": 1e-5, "rope_theta": 1e6, "sliding_window": None,
                     "bos_token_id": 2, "eos_token_id": 3, "pad_token_id": 0},
        vision_config={"model_type": "pixtral", "hidden_size": 16, "num_hidden_layers": 1, "num_attention_heads": 4,
                       "intermediate_size": 32, "image_size": 30, "patch_size": 6, "num_channels": 3},
        bos_token_id=2, eos_token_id=3, pad_token_id=0, image_seq_length=4, vision_feature_layer=-1, image_token_index=1)
    torch.manual_seed(0)
    pipe = Flux2Pipeline(scheduler=FlowMatchEulerDiscreteScheduler(**FLUX2_SCHEDULER), vae=tiny_vae(),
                         text_encoder=AutoModelForImageTextToText.from_config(cfg), tokenizer=tiny_tokenizer(),
                         transformer=tiny_transformer(True))
    pipe.save_pretrained(path)
    return pipe


def build_klein(path, distilled=False, split_like=False):
    """split_like: the settings ComfyUI's split files imply (Qwen3's rope theta, a VAE with quant convs and
    as many groups as channels), so a folder and its split files give the same images."""
    from diffusers import FlowMatchEulerDiscreteScheduler, Flux2KleinPipeline
    from transformers import Qwen3Config, Qwen3ForCausalLM
    torch.manual_seed(0)
    extra = {"rope_parameters": {"rope_type": "default", "rope_theta": 1000000.0}} if split_like else {}
    te = Qwen3ForCausalLM(Qwen3Config(intermediate_size=16, hidden_size=16, num_hidden_layers=28, num_attention_heads=2,
                                      num_key_value_heads=2, vocab_size=64, max_position_embeddings=1024, **extra))
    vae = tiny_vae(groups=8, quant=True) if split_like else tiny_vae()
    pipe = Flux2KleinPipeline(scheduler=FlowMatchEulerDiscreteScheduler(**FLUX2_SCHEDULER), vae=vae, text_encoder=te,
                              tokenizer=tiny_tokenizer(), transformer=tiny_transformer(False), is_distilled=distilled)
    pipe.save_pretrained(path)
    return pipe


def make_lora(pipe, path, seed):
    """A random LoRA for the tiny transformer, saved the way Diffusers saves FLUX.2 LoRAs."""
    from peft import LoraConfig
    from peft.utils import get_peft_model_state_dict
    torch.manual_seed(seed)
    pipe.transformer.add_adapter(LoraConfig(r=4, lora_alpha=4, target_modules=["to_q", "to_k", "to_v", "to_out.0"],
                                            init_lora_weights=False), adapter_name="tmp")
    sd = get_peft_model_state_dict(pipe.transformer, adapter_name="tmp")
    sd = {k: v * 25.0 for k, v in sd.items()}  # strong enough to change a random model's output clearly
    type(pipe).save_lora_weights(os.path.dirname(path), transformer_lora_layers=sd, weight_name=os.path.basename(path))
    pipe.transformer.delete_adapters("tmp")


def to_bfl(sd):
    """A Diffusers FLUX.2 transformer state dict in the original BFL / ComfyUI naming (the inverse of
    Diffusers' convert_flux2_transformer_checkpoint_to_diffusers), for the single-file tests."""
    import re
    simple = {"x_embedder": "img_in", "context_embedder": "txt_in",
              "time_guidance_embed.timestep_embedder.linear_1": "time_in.in_layer",
              "time_guidance_embed.timestep_embedder.linear_2": "time_in.out_layer",
              "time_guidance_embed.guidance_embedder.linear_1": "guidance_in.in_layer",
              "time_guidance_embed.guidance_embedder.linear_2": "guidance_in.out_layer",
              "double_stream_modulation_img.linear": "double_stream_modulation_img.lin",
              "double_stream_modulation_txt.linear": "double_stream_modulation_txt.lin",
              "single_stream_modulation.linear": "single_stream_modulation.lin",
              "proj_out": "final_layer.linear"}
    double = {"attn.norm_q": "img_attn.norm.query_norm", "attn.norm_k": "img_attn.norm.key_norm",
              "attn.to_out.0": "img_attn.proj", "ff.linear_in": "img_mlp.0", "ff.linear_out": "img_mlp.2",
              "attn.norm_added_q": "txt_attn.norm.query_norm", "attn.norm_added_k": "txt_attn.norm.key_norm",
              "attn.to_add_out": "txt_attn.proj", "ff_context.linear_in": "txt_mlp.0", "ff_context.linear_out": "txt_mlp.2"}
    single = {"attn.to_qkv_mlp_proj": "linear1", "attn.to_out": "linear2", "attn.norm_q": "norm.query_norm",
              "attn.norm_k": "norm.key_norm"}
    out, qkv = {}, {}
    for k, v in sd.items():
        name, kind = k.rsplit(".", 1)
        norm = lambda n: kind if not n.endswith(("norm_q", "norm_k", "norm_added_q", "norm_added_k")) else "scale"  # noqa: E731
        if name == "norm_out.linear":
            sh, sc = v.chunk(2, dim=0)
            out["final_layer.adaLN_modulation.1." + kind] = torch.cat([sc, sh], 0)
        elif name in simple:
            out[simple[name] + "." + kind] = v
        elif (m := re.match(r"transformer_blocks\.(\d+)\.(.+)$", name)):
            i, rest = m.groups()
            q = re.match(r"attn\.(to|add)_(q|k|v)(_proj)?$", rest)
            if q:
                side = "img" if q.group(1) == "to" else "txt"
                qkv.setdefault((f"double_blocks.{i}.{side}_attn.qkv.{kind}"), {})["qkv".index(q.group(2))] = v
            else:
                out[f"double_blocks.{i}.{double[rest]}.{norm(rest)}"] = v
        elif (m := re.match(r"single_transformer_blocks\.(\d+)\.(.+)$", name)):
            i, rest = m.groups()
            out[f"single_blocks.{i}.{single[rest]}.{norm(rest)}"] = v
        else:
            raise KeyError(k)
    for k, parts in qkv.items():
        out[k] = torch.cat([parts[0], parts[1], parts[2]], 0)
    return out


def vae_to_ldm(sd, levels=4):
    """A Diffusers AutoencoderKLFlux2 state dict in BFL / LDM naming (flux2-vae.safetensors), for the split-file tests."""
    import re
    out = {}
    for k, v in sd.items():
        n = k
        if k.startswith("quant_conv."):
            n = "encoder." + k
        elif k.startswith("post_quant_conv."):
            n = "decoder." + k
        elif not k.startswith("bn."):
            n = re.sub(r"encoder\.down_blocks\.(\d+)\.resnets\.(\d+)", r"encoder.down.\1.block.\2", n)
            n = re.sub(r"encoder\.down_blocks\.(\d+)\.downsamplers\.0\.conv", r"encoder.down.\1.downsample.conv", n)
            n = re.sub(r"decoder\.up_blocks\.(\d+)\.resnets\.(\d+)", lambda m: f"decoder.up.{levels - 1 - int(m[1])}.block.{m[2]}", n)
            n = re.sub(r"decoder\.up_blocks\.(\d+)\.upsamplers\.0\.conv", lambda m: f"decoder.up.{levels - 1 - int(m[1])}.upsample.conv", n)
            n = re.sub(r"(en|de)coder\.mid_block\.resnets\.(\d)", lambda m: f"{m[1]}coder.mid.block_{int(m[2]) + 1}", n)
            if ".mid_block.attentions.0." in n:
                n = n.replace("mid_block.attentions.0.", "mid.attn_1.").replace("group_norm", "norm").replace("to_out.0", "proj_out")
                n = n.replace("to_q", "q").replace("to_k", "k").replace("to_v", "v")
                if v.ndim == 2:
                    v = v[:, :, None, None]
            n = n.replace("conv_norm_out", "norm_out").replace("conv_shortcut", "nin_shortcut")
        out[n] = v
    return out


def comfy_fp8(sd, prefix="model.diffusion_model.", per_channel=False):
    """ComfyUI-style "fp8 mixed": 2-D weights as float8_e4m3fn with a weight_scale, a comfy_quant marker
    and an input_scale; everything else bf16; all keys under `prefix`."""
    out = {}
    marker = torch.tensor(list(json.dumps({"format": "float8_e4m3fn"}).encode()), dtype=torch.uint8)
    for k, v in sd.items():
        if k.endswith(".weight") and v.ndim == 2 and not k.startswith(("img_in", "final_layer")):
            amax = v.abs().amax(dim=1) if per_channel else v.abs().max()
            scale = (amax / 448.0).clamp(min=1e-12).float()
            q = (v.float() / (scale.reshape(-1, 1) if per_channel else scale)).to(torch.float8_e4m3fn)
            base = k[: -len(".weight")]
            out[prefix + k] = q
            out[prefix + base + ".weight_scale"] = scale
            out[prefix + base + ".input_scale"] = torch.tensor(1.0)
            out[prefix + base + ".comfy_quant"] = marker.clone()
        else:
            out[prefix + k] = v.to(torch.bfloat16) if v.is_floating_point() else v
    return out


def write_job(home, key, params, inputs=None):
    jd = os.path.join(home, "jobs", key)
    os.makedirs(os.path.join(jd, "in"), exist_ok=True)
    job = {"task": "imagegen", "params": params, "inputs": {}}
    for name, arr in (inputs or {}).items():
        np.save(os.path.join(jd, "in", f"{name}.1.npy"), arr.astype(np.float16))
        job["inputs"][name] = {"frames": [1], "pattern": f"in/{name}.%d.npy", "width": arr.shape[1],
                               "height": arr.shape[0], "channels": ["R", "G", "B", "A"]}
    with open(os.path.join(jd, "job.json"), "w") as f:
        json.dump(job, f)
    os.makedirs(os.path.join(home, "queue"), exist_ok=True)
    open(os.path.join(home, "queue", key), "w").close()
    return jd


def read(jd, name):
    with open(os.path.join(jd, name)) as f:
        return json.load(f)


@unittest.skipUnless(HAVE, "needs numpy, torch, diffusers, transformers, peft and opencv")
class ImageGenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from strata_ml import worker as W
        from strata_ml.tasks import imagegen
        from strata_ml.tasks import imagegen_parts
        cls.W, cls.ig, cls.parts = W, imagegen, imagegen_parts
        W.load_tasks()
        cls.tmp = tempfile.mkdtemp(prefix="imagegen-test-")
        cls.home = os.path.join(cls.tmp, "home")
        cls.ckpts = os.path.join(cls.tmp, "checkpoints")
        cls.loras = os.path.join(cls.tmp, "loras")
        os.makedirs(cls.loras)
        cls.dev = os.path.join(cls.ckpts, "flux2-dev-tiny")
        cls.klein = os.path.join(cls.ckpts, "flux2-klein-tiny")
        dev = build_dev(cls.dev)
        build_klein(cls.klein)
        # a transformer-only single file (as finetunes are often shared) for the dev base
        from safetensors.torch import save_file
        cls.single = os.path.join(cls.ckpts, "flux2-dev-finetune.safetensors")
        cls.dev_transformer_sd = {k: v.detach().clone() for k, v in dev.transformer.state_dict().items()}
        save_file({k: v.contiguous() for k, v in cls.dev_transformer_sd.items()}, cls.single)
        cls.lora_a = os.path.join(cls.loras, "style_a.safetensors")
        cls.lora_b = os.path.join(cls.loras, "style_b.safetensors")
        make_lora(dev, cls.lora_a, 1)
        make_lora(dev, cls.lora_b, 2)
        cls.worker = W.Worker(cls.home)
        cls.n = 0

    @classmethod
    def tearDownClass(cls):
        cls.ig.unload()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_job(self, params, inputs=None, expect="done"):
        type(self).n += 1
        key = f"imagegen-{self.n}"
        p = {"checkpoint": self.dev, "width": SIZE, "height": SIZE, "steps": 4, "guidance": 4.0, "seed": 7,
             "batch_count": 1, "prompt": "a red house", "dtype": "fp32", "preview_every": 1}
        p.update(params)
        jd = write_job(self.home, key, p, inputs)
        self.worker.run_job(key)
        st = read(jd, "status.json")
        self.assertEqual(st["state"], expect, st.get("message"))
        return jd, (read(jd, "result.json") if expect == "done" else None)

    def take(self, jd, i=0):
        return np.load(os.path.join(jd, "out", f"take.{i}.npy")).astype(np.float32)

    # ------------------------------------------------------------ basics
    def test_txt2img_batch(self):
        jd, r = self.run_job({"batch_count": 3, "seed": 100})
        self.assertEqual([t["seed"] for t in r["takes"]], [100, 101, 102])
        self.assertEqual(r["outputs"]["take"]["frames"], [0, 1, 2])
        imgs = [self.take(jd, i) for i in range(3)]
        for im in imgs:
            self.assertEqual(im.shape, (SIZE, SIZE, 4))
            self.assertTrue(np.isfinite(im).all())
            self.assertTrue(np.allclose(im[..., 3], 1.0))
        self.assertGreater(np.abs(imgs[0] - imgs[1]).mean(), 1e-3, "different seeds, different images")
        st = read(jd, "status.json")
        self.assertEqual(st["detail"]["takes_done"], 3)
        self.assertEqual(st["detail"]["batch_size"], 3)
        self.assertGreater(st["detail"]["preview_seq"], 0)
        prev = np.load(os.path.join(jd, "out", "preview.npy"))
        self.assertEqual(prev.ndim, 3)
        self.assertEqual(prev.shape[2], 3)
        self.assertEqual(len(read(jd, "out/takes.json")), 3)

    def test_same_seed_same_image(self):
        a, _ = self.run_job({"seed": 5})
        b, _ = self.run_job({"seed": 5, "batch_count": 2})
        self.assertLess(np.abs(self.take(a) - self.take(b)).max(), 1e-4, "a take only depends on its own seed")

    def test_pipeline_stays_loaded(self):
        self.run_job({})
        pipe = self.ig._STATE["pipe"]
        self.run_job({"seed": 8})
        self.assertIs(self.ig._STATE["pipe"], pipe, "the same checkpoint reuses the loaded pipeline")
        self.run_job({"checkpoint": self.klein, "negative_prompt": "a blue sky", "guidance": 3.0})
        self.assertIsNot(self.ig._STATE["pipe"], pipe, "another checkpoint replaces it")
        self.assertEqual(self.ig._STATE["info"]["class"], "Flux2KleinPipeline")

    def test_negative_prompt_warning_on_dev(self):
        _, r = self.run_job({"negative_prompt": "a blue sky"})
        self.assertTrue(any("negative" in w for w in r["warnings"]))
        self.assertEqual(r["takes"][0]["negative_prompt"], "")

    def test_klein_negative_prompt_changes_result(self):
        a, ra = self.run_job({"checkpoint": self.klein, "guidance": 3.0, "negative_prompt": ""})
        b, rb = self.run_job({"checkpoint": self.klein, "guidance": 3.0, "negative_prompt": "a blue sky at night"})
        self.assertFalse(rb["warnings"])
        self.assertGreater(np.abs(self.take(a) - self.take(b)).mean(), 1e-5)

    def test_single_file_transformer_finds_its_base(self):
        jd, r = self.run_job({"checkpoint": self.single, "base": "auto", "base_candidates": [self.klein, self.dev], "seed": 3})
        self.assertEqual(self.ig._STATE["info"]["folder"], self.dev)
        ref, _ = self.run_job({"seed": 3})
        self.assertLess(np.abs(self.take(jd) - self.take(ref)).max(), 1e-4, "same weights as the folder's transformer")

    def test_comfy_fp8_single_file(self):
        """Comfy-Org / BFL single files: original naming, "model.diffusion_model." prefix, fp8 with scales."""
        from safetensors.torch import save_file
        ref_sd = {k: v.detach().float() for k, v in self.dev_transformer_sd.items()}
        for per_channel in (False, True):
            path = os.path.join(self.ckpts, f"flux2_dev_fp8mixed{'_pc' if per_channel else ''}.safetensors")
            save_file({k: v.contiguous() for k, v in comfy_fp8(to_bfl(ref_sd), per_channel=per_channel).items()}, path)
            self.assertEqual(self.ig.pick_base(path, [self.klein, self.dev]), self.dev)
            jd, r = self.run_job({"checkpoint": path, "base": "auto", "base_candidates": [self.klein, self.dev], "seed": 3})
            self.assertEqual(self.ig._STATE["info"]["folder"], self.dev)
            ref, _ = self.run_job({"seed": 3})
            other, _ = self.run_job({"seed": 4})
            err = np.abs(self.take(jd) - self.take(ref)).mean()
            self.assertLess(err, 0.25 * np.abs(self.take(other) - self.take(ref)).mean(),
                            f"fp8 weights are scaled back: close to the bf16 model ({err})")

    def test_klein_file_picks_distilled_or_base_folder(self):
        from safetensors.torch import save_file
        distilled = os.path.join(self.tmp, "klein-distilled")
        pipe = build_klein(distilled, distilled=True)
        sd = comfy_fp8(to_bfl({k: v.detach().float() for k, v in pipe.transformer.state_dict().items()}))
        for name, want in (("flux-2-klein-4b-fp8.safetensors", distilled), ("flux-2-klein-base-4b-fp8.safetensors", self.klein)):
            path = os.path.join(self.tmp, name)
            save_file({k: v.contiguous() for k, v in sd.items()}, path)
            self.assertEqual(self.ig.pick_base(path, [self.dev, self.klein, distilled]), want, name)

    def split_files(self):
        """ComfyUI's split files of a tiny [klein] base: models/diffusion_models/<transformer>,
        models/text_encoders/<qwen3> (+ its tokenizer folder) and models/vae/<vae>, plus a Diffusers folder
        with the same weights to compare against, and a text encoder that doesn't fit."""
        from safetensors.torch import save_file
        if getattr(type(self), "_split", None):
            return type(self)._split
        root = os.path.join(self.tmp, "comfy", "models")
        folder = os.path.join(self.tmp, "klein-split-ref")
        pipe = build_klein(folder, split_like=True)
        f32 = lambda sd: {k: v.detach().float().contiguous() for k, v in sd.items()}  # noqa: E731
        paths = {}
        for sub, name, sd in (("diffusion_models", "flux-2-klein-base-tiny.safetensors",
                               {"model.diffusion_model." + k: v for k, v in to_bfl(f32(pipe.transformer.state_dict())).items()}),
                              ("text_encoders", "qwen_3_tiny.safetensors",
                               {"model." + k: v for k, v in f32(pipe.text_encoder.model.state_dict()).items()}),
                              ("vae", "flux2-vae.safetensors", vae_to_ldm(f32(pipe.vae.state_dict())))):
            os.makedirs(os.path.join(root, sub), exist_ok=True)
            paths[sub] = os.path.join(root, sub, name)
            save_file(sd, paths[sub])
        pipe.tokenizer.save_pretrained(os.path.join(root, "text_encoders", "tokenizer"))
        from transformers import Qwen3Config, Qwen3Model
        decoy = os.path.join(root, "text_encoders", "qwen_3_small.safetensors")
        save_file({"model." + k: v.contiguous() for k, v in Qwen3Model(Qwen3Config(
            intermediate_size=8, hidden_size=8, num_hidden_layers=2, num_attention_heads=2, num_key_value_heads=2,
            vocab_size=64)).state_dict().items()}, decoy)
        cands = [{"path": decoy, "kind": "text_encoder"}, {"path": paths["text_encoders"], "kind": "text_encoder"},
                 {"path": paths["vae"], "kind": "vae"}]
        type(self)._split = (folder, paths, cands)
        return type(self)._split

    def test_split_files_load_without_a_diffusers_folder(self):
        folder, paths, cands = self.split_files()
        cfg = self.parts.transformer_config(self.parts.file_shapes(paths["diffusion_models"]))
        self.assertEqual((cfg["num_layers"], cfg["num_single_layers"], cfg["joint_attention_dim"], cfg["in_channels"]), (1, 2, 48, 16))
        self.assertEqual(self.parts.pick_parts(paths["diffusion_models"], "auto", "auto", cands),
                         (paths["text_encoders"], paths["vae"]), "the Qwen3 whose width fits, not the decoy")
        p = {"guidance": 3.0, "negative_prompt": "a blue sky", "seed": 5}
        jd, r = self.run_job(dict(p, checkpoint=paths["diffusion_models"], base="auto", base_candidates=[self.dev],
                                  text_encoder="auto", vae="auto", component_candidates=cands))
        info = self.ig._STATE["info"]
        self.assertEqual(info["class"], "Flux2KleinPipeline")
        self.assertFalse(info["distilled"], "a '...base...' file is the undistilled [klein]")
        ref, _ = self.run_job(dict(p, checkpoint=folder))
        self.assertLess(np.abs(self.take(jd) - self.take(ref)).max(), 1e-4, "the same images as the Diffusers folder")

    def test_split_files_picked_by_hand(self):
        folder, paths, cands = self.split_files()
        self.run_job({"checkpoint": paths["diffusion_models"], "text_encoder": paths["text_encoders"], "vae": paths["vae"],
                      "base_candidates": [], "component_candidates": cands, "seed": 1})
        jd, _ = self.run_job({"checkpoint": paths["diffusion_models"], "text_encoder": cands[0]["path"], "vae": paths["vae"],
                              "component_candidates": cands}, expect="error")
        self.assertIn("doesn't fit", read(jd, "status.json")["message"])

    def test_split_files_errors(self):
        folder, paths, cands = self.split_files()
        with self.assertRaisesRegex(RuntimeError, "no FLUX.2 VAE"):
            self.parts.pick_parts(paths["diffusion_models"], "auto", "auto", cands[:2])
        with self.assertRaisesRegex(RuntimeError, "no text encoder fits"):
            self.parts.pick_parts(paths["diffusion_models"], "auto", "auto", cands[:1] + cands[2:])
        with self.assertRaisesRegex(RuntimeError, r"\[dev\]"):
            self.parts.pick_parts(self.single, "auto", "auto", cands)

    def test_vae_conversion_round_trip(self):
        vae = tiny_vae(groups=8, quant=True)
        sd = {k: v.detach().float() for k, v in vae.state_dict().items()}
        ldm = vae_to_ldm(sd)
        cfg = self.parts.vae_config({k: tuple(v.shape) for k, v in ldm.items()})
        self.assertEqual((cfg["block_out_channels"], cfg["layers_per_block"], cfg["latent_channels"], cfg["norm_num_groups"],
                          cfg["use_quant_conv"], cfg["patch_size"]), ([8, 8, 8, 8], 1, 4, 8, True, [2, 2]))
        back = self.parts.convert_vae(ldm, cfg)
        for k, v in sd.items():
            if not k.endswith("num_batches_tracked"):
                self.assertTrue(torch.equal(back[k], v), k)

    def test_read_transformer_file_dequantises(self):
        from safetensors.torch import save_file
        sd = {"a.weight": torch.randn(8, 4), "a.bias": torch.randn(8)}
        path = os.path.join(self.tmp, "q.safetensors")
        save_file(comfy_fp8(sd, per_channel=True), path)
        out = self.ig.read_transformer_file(path, torch.float32)
        self.assertEqual(sorted(out), ["a.bias", "a.weight"], "prefix dropped, scales and markers gone")
        rel = (out["a.weight"] - sd["a.weight"]).abs().max() / sd["a.weight"].abs().max()
        self.assertLess(float(rel), 0.08)

    def test_packed_4bit_file_is_refused(self):
        from safetensors.torch import save_file
        path = os.path.join(self.tmp, "nvfp4.safetensors")
        marker = torch.tensor(list(json.dumps({"format": "nvfp4"}).encode()), dtype=torch.uint8)
        save_file({"model.diffusion_model.a.weight": torch.zeros(8, 2, dtype=torch.uint8),
                   "model.diffusion_model.a.comfy_quant": marker}, path)
        with self.assertRaisesRegex(RuntimeError, "nvfp4"):
            self.ig.read_transformer_file(path, torch.bfloat16)

    def test_offload_plan(self):
        gb = 2 ** 30
        plan = self.ig.plan_offload
        # FLUX.2 [dev] bf16 on 24 GB: neither the 32B transformer nor the 24B text encoder fits -> group
        self.assertEqual(plan("auto", {"transformer": 64 * gb, "text_encoder": 48 * gb, "vae": gb // 4}, 24 * gb), "group")
        # [klein] 4B on 24 GB: one component at a time fits -> model offload
        self.assertEqual(plan("auto", {"transformer": 8 * gb, "text_encoder": 8 * gb, "vae": gb // 4}, 24 * gb), "model")
        # "none" that can't fit falls back instead of running out of memory
        self.assertEqual(plan("none", {"transformer": 10 * gb, "text_encoder": 10 * gb}, 24 * gb), "model")
        self.assertEqual(plan("none", {"transformer": 4 * gb, "text_encoder": 2 * gb}, 24 * gb), "none")
        self.assertEqual(plan("sequential", {"transformer": 4 * gb}, 24 * gb), "sequential")
        self.assertEqual(plan("auto", {"transformer": 4 * gb}, 0), "none", "no GPU")

    def test_samplers(self):
        for s in self.ig.SAMPLERS:
            jd, r = self.run_job({"sampler": s, "seed": 11})
            self.assertTrue(np.isfinite(self.take(jd)).all(), s)
            self.assertEqual(r["takes"][0]["sampler"], s)

    # ------------------------------------------------------------ source image modes
    def source(self):
        y, x = np.mgrid[0:300, 0:400] / 300.0
        rgb = np.stack([x * 0.8, y * 0.6, 0.3 + 0 * x], 2)
        return np.concatenate([rgb, np.ones((300, 400, 1))], 2).astype(np.float32)

    def test_img2img_denoise(self):
        src = self.source()
        fitted = self.ig.fit_image(self.ig.srgb_encode(src[..., :3]), SIZE, SIZE, "crop")
        diffs = []
        for d in (0.1, 0.6, 1.0):
            jd, r = self.run_job({"mode": "img2img", "denoise": d, "steps": 10, "seed": 9}, {"src": src})
            out = self.ig.srgb_encode(self.take(jd)[..., :3])
            diffs.append(np.abs(out - fitted).mean())
            self.assertEqual(r["takes"][0]["denoise"], d)
        self.assertLess(diffs[0], diffs[1], f"less denoise stays closer to the source ({diffs})")
        self.assertLess(diffs[1], diffs[2] + 1e-3, f"({diffs})")

    def test_img2img_needs_source(self):
        jd, _ = self.run_job({"mode": "img2img"}, expect="error")
        self.assertIn("source", read(jd, "status.json")["message"])

    def test_inpaint_keeps_unmasked_pixels(self):
        src = self.source()
        mask = np.zeros_like(src)
        mask[100:200, 150:250, 3] = 1.0
        jd, r = self.run_job({"mode": "inpaint", "denoise": 1.0, "mask_grow": 0, "mask_feather": 0, "fit": "stretch"},
                             {"src": src, "mask": mask})
        out = self.take(jd)
        ref = self.ig.fit_image(src[..., :3], SIZE, SIZE, "stretch")  # linear, values above 1 kept
        m = self.ig.fit_image(mask[..., 3:4], SIZE, SIZE, "stretch")[..., 0]
        outside = m < 1e-3
        self.assertGreater(ref.max(), 1.0, "the test source has HDR values")
        err = np.abs(out[..., :3][outside] - ref[outside]) / np.maximum(1.0, ref[outside])
        self.assertLess(err.max(), 2e-3, "outside the mask = the source, HDR included")
        inside = m > 0.999
        self.assertGreater(np.abs(out[..., :3][inside] - ref[inside]).mean(), 1e-3, "inside the mask is generated")

    def test_inpaint_empty_mask(self):
        jd, _ = self.run_job({"mode": "inpaint"}, {"src": self.source(), "mask": np.zeros_like(self.source())}, expect="error")
        self.assertIn("empty", read(jd, "status.json")["message"])

    def test_promoted_take_as_source(self):
        a, _ = self.run_job({"seed": 21})
        jd, r = self.run_job({"mode": "img2img", "denoise": 0.2, "steps": 10,
                              "source_file": os.path.join(a, "out", "take.0.npy")})
        self.assertLess(np.abs(self.take(jd) - self.take(a)).mean(), 0.2)

    def test_reference_input(self):
        ref = self.source()
        ref[..., 0] = 1.5  # a different image (and HDR: clipped for the model)
        base, _ = self.run_job({"seed": 6})
        jd, r = self.run_job({"seed": 6}, {"ref": ref})
        self.assertEqual(r["takes"][0]["mode"], "txt2img", "a reference doesn't make it img2img")
        self.assertTrue(r["takes"][0]["reference"])
        self.assertEqual(r["takes"][0]["references"], 1)
        self.assertGreater(np.abs(self.take(jd) - self.take(base)).mean(), 1e-5, "the reference conditions the image")
        # with use_reference the source is a second reference, next to the base
        _, r2 = self.run_job({"mode": "img2img", "denoise": 0.6, "use_reference": True}, {"src": self.source(), "ref": ref})
        self.assertEqual(r2["takes"][0]["references"], 2)

    def test_reference_image(self):
        jd, r = self.run_job({"use_reference": True}, {"src": self.source()})
        self.assertTrue(r["takes"][0]["reference"])

    # ------------------------------------------------------------ LoRAs
    def test_lora_stack(self):
        base, _ = self.run_job({"seed": 4})
        a, ra = self.run_job({"seed": 4, "loras": [{"file": self.lora_a, "weight": 1.0}]})
        self.assertEqual(len(ra["takes"][0]["loras"]), 1)
        self.assertGreater(np.abs(self.take(a) - self.take(base)).mean(), 1e-3, "a LoRA changes the image")
        self.assertEqual(len(self.ig._STATE["adapters"]), 1)
        ab, _ = self.run_job({"seed": 4, "loras": [{"file": self.lora_a, "weight": 1.0}, {"file": self.lora_b, "weight": -0.5}]})
        self.assertEqual(len(self.ig._STATE["adapters"]), 2)
        self.assertGreater(np.abs(self.take(ab) - self.take(a)).mean(), 1e-4, "a second LoRA changes it again")
        # LoRA A off, B on: A is unloaded
        b, _ = self.run_job({"seed": 4, "loras": [{"file": self.lora_a, "weight": 1.0, "enabled": False},
                                                  {"file": self.lora_b, "weight": -0.5}]})
        self.assertEqual(list(self.ig._STATE["adapters"].values()), [self.lora_b])
        zero, _ = self.run_job({"seed": 4, "loras": [{"file": self.lora_b, "weight": 0.0}]})
        self.assertLess(np.abs(self.take(zero) - self.take(base)).max(), 1e-4, "weight 0 = no LoRA")
        again, _ = self.run_job({"seed": 4, "loras": []})
        self.assertEqual(self.ig._STATE["adapters"], {})
        self.assertLess(np.abs(self.take(again) - self.take(base)).max(), 1e-4, "an empty stack restores the base model")

    def test_missing_lora(self):
        jd, _ = self.run_job({"loras": [{"file": os.path.join(self.loras, "nope.safetensors"), "weight": 1}]}, expect="error")
        self.assertIn("LoRA not found", read(jd, "status.json")["message"])

    # ------------------------------------------------------------ cancel
    def test_cancel_mid_batch(self):
        type(self).n += 1
        key = f"imagegen-{self.n}"
        jd = write_job(self.home, key, {"checkpoint": self.dev, "width": 512, "height": 512, "steps": 200, "batch_count": 4,
                                        "prompt": "a cat", "dtype": "fp32", "seed": 1})

        def cancel_soon():
            for _ in range(600):
                try:
                    if read(jd, "status.json").get("detail", {}).get("step", 0) >= 2:
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(0.05)
            open(os.path.join(jd, "cancel"), "w").close()

        th = threading.Thread(target=cancel_soon)
        th.start()
        t0 = time.time()
        self.worker.run_job(key)
        th.join()
        self.assertEqual(read(jd, "status.json")["state"], "cancelled")
        self.assertFalse(os.path.exists(os.path.join(jd, "result.json")))
        self.assertLess(time.time() - t0, 60)
        for m in self.ig._STATE["pipe"].transformer.modules():  # the block hooks are gone again
            self.assertFalse(m._forward_pre_hooks, "cancel hooks removed")
        # and the worker still works afterwards
        self.run_job({"seed": 2})


if __name__ == "__main__":
    unittest.main()
