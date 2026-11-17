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


def tiny_vae():
    from diffusers import AutoencoderKLFlux2
    torch.manual_seed(0)
    return AutoencoderKLFlux2(sample_size=32, in_channels=3, out_channels=3, down_block_types=("DownEncoderBlock2D",) * 4,
                              up_block_types=("UpDecoderBlock2D",) * 4, block_out_channels=(8, 8, 8, 8), layers_per_block=1,
                              latent_channels=4, norm_num_groups=1, use_quant_conv=False, use_post_quant_conv=False)


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


def build_klein(path, distilled=False):
    from diffusers import FlowMatchEulerDiscreteScheduler, Flux2KleinPipeline
    from transformers import Qwen3Config, Qwen3ForCausalLM
    torch.manual_seed(0)
    te = Qwen3ForCausalLM(Qwen3Config(intermediate_size=16, hidden_size=16, num_hidden_layers=28, num_attention_heads=2,
                                      num_key_value_heads=2, vocab_size=64, max_position_embeddings=1024))
    pipe = Flux2KleinPipeline(scheduler=FlowMatchEulerDiscreteScheduler(**FLUX2_SCHEDULER), vae=tiny_vae(), text_encoder=te,
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
        cls.W, cls.ig = W, imagegen
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
        save_file({k: v.contiguous() for k, v in dev.transformer.state_dict().items()}, cls.single)
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
