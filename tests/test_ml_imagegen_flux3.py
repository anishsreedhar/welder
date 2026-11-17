"""Tests for FLUX 3 [klein] in the ImageGen task (ml/strata_ml/tasks/imagegen.py + imagegen_flux3.py), run
through the worker's job protocol like tests/test_ml_imagegen.py (whose FLUX.2 models and helpers this reuses).

The models are tiny FLUX 3 [klein] (distilled, flow-map) and [klein] base folders with random weights: the real
Flux3Transformer2DModel, the FLUX.2 VAE, a 32-layer Qwen3-VL text encoder and a word-level tokenizer, laid out
the way the Hugging Face repos are (modular_model_index.json and one subfolder per component).

Run: python -m unittest tests/test_ml_imagegen_flux3.py
"""

import json
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "ml"))

from test_ml_imagegen import HAVE, SIZE, build_dev, comfy_fp8, read, tiny_tokenizer, tiny_vae, write_job  # noqa: E402

if HAVE:
    import numpy as np
    import torch
    try:
        from diffusers import Flux3Transformer2DModel  # noqa: F401
        from transformers import Qwen3VLForConditionalGeneration  # noqa: F401
    except Exception:  # noqa: BLE001
        HAVE = False


def tiny_flux3_transformer(flow_map):
    from diffusers import Flux3Transformer2DModel
    torch.manual_seed(0)
    return Flux3Transformer2DModel(in_channels=16, cond_in_channels=16, num_layers=1, num_single_layers=2,
                                   attention_head_dim=16, num_attention_heads=2, joint_attention_dim=128,
                                   axes_dims_rope=(4, 4, 4, 4), flow_map=flow_map)


def build_flux3(path, distilled):
    """A FLUX 3 [klein] (distilled) or [klein] base folder, laid out like the Hugging Face repos."""
    from diffusers import FlowMatchEulerDiscreteScheduler
    from transformers import Qwen3VLConfig, Qwen3VLForConditionalGeneration
    cfg = Qwen3VLConfig(
        text_config={"vocab_size": 64, "hidden_size": 16, "intermediate_size": 32, "num_hidden_layers": 32,
                     "num_attention_heads": 2, "num_key_value_heads": 2, "head_dim": 8, "max_position_embeddings": 1024,
                     "rope_scaling": {"rope_type": "default", "mrope_section": [2, 1, 1], "mrope_interleaved": True}},
        vision_config={"depth": 1, "hidden_size": 16, "intermediate_size": 32, "num_heads": 2, "out_hidden_size": 16,
                       "patch_size": 16, "spatial_merge_size": 2, "temporal_patch_size": 2, "num_position_embeddings": 16,
                       "deepstack_visual_indexes": [0]},
        image_token_id=1, video_token_id=1, vision_start_token_id=2, vision_end_token_id=3)
    torch.manual_seed(0)
    Qwen3VLForConditionalGeneration(cfg).save_pretrained(os.path.join(path, "text_encoder"))
    tiny_tokenizer().save_pretrained(os.path.join(path, "tokenizer"))
    tiny_vae().save_pretrained(os.path.join(path, "vae"))
    t = tiny_flux3_transformer(distilled)
    t.save_pretrained(os.path.join(path, "transformer"))
    FlowMatchEulerDiscreteScheduler(shift=6.93).save_pretrained(os.path.join(path, "scheduler"))
    name = "Flux3DistilledModularPipeline" if distilled else "Flux3ModularPipeline"
    with open(os.path.join(path, "modular_model_index.json"), "w") as f:
        json.dump({"_class_name": name, "_diffusers_version": "0.43.0"}, f)
    return t


def to_bfl_flux3(sd):
    """A Diffusers FLUX 3 transformer state dict in BFL's original naming (the inverse of Diffusers'
    convert_flux3_transformer_checkpoint_to_diffusers), for the single-file tests."""
    exact = {"timestep_embedder.linear_1.weight": "time_in.in_layer.weight",
             "timestep_embedder.linear_2.weight": "time_in.out_layer.weight",
             "target_timestep_embedder.linear_1.weight": "ref_time_in.in_layer.weight",
             "target_timestep_embedder.linear_2.weight": "ref_time_in.out_layer.weight",
             "proj_out.weight": "final_layer.image.linear.weight",
             "norm_out.linear.weight": "final_layer.image.adaLN_modulation.1.weight"}
    prefixes = {"x_embedders.": "emb_in.", "context_embedder.": "txt_in.", "mode_blocks.": "content_mode_blocks.",
                "single_transformer_blocks.": "single_blocks."}
    block = {"attn.to_q.weight": "q_proj.weight", "attn.to_k.weight": "k_proj.weight", "attn.to_v.weight": "v_proj.weight",
             "attn.to_out.0.weight": "attn_out.weight", "attn.norm_q.weight": "norm.query_norm.scale",
             "attn.norm_k.weight": "norm.key_norm.scale", "ff.linear_in.weight": "mlp_in.weight",
             "ff.linear_out.weight": "mlp_out.weight"}
    out = {}
    for k, v in sd.items():
        if k in exact:
            if k == "norm_out.linear.weight":
                a, b = v.chunk(2, dim=0)
                v = torch.cat([b, a], 0)
            out[exact[k]] = v
            continue
        nk = k
        for a, b in prefixes.items():
            if nk.startswith(a):
                nk = b + nk[len(a):]
                break
        nk = nk.replace(".linear.weight", ".lin.weight")
        for a, b in block.items():
            if nk.endswith(a):
                nk = nk[: -len(a)] + b
                break
        out[nk] = v
    return out


def make_lora(pipe_cls, transformer, path, seed):
    from peft import LoraConfig
    from peft.utils import get_peft_model_state_dict
    torch.manual_seed(seed)
    transformer.add_adapter(LoraConfig(r=4, lora_alpha=4, target_modules=["to_q", "to_k", "to_v", "to_out.0"],
                                       init_lora_weights=False), adapter_name="tmp")
    sd = {k: v * 25.0 for k, v in get_peft_model_state_dict(transformer, adapter_name="tmp").items()}
    pipe_cls.save_lora_weights(os.path.dirname(path), transformer_lora_layers=sd, weight_name=os.path.basename(path))
    transformer.delete_adapters("tmp")


@unittest.skipUnless(HAVE, "needs numpy, torch, diffusers (with FLUX 3), transformers, peft and opencv")
class ImageGenFlux3Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from safetensors.torch import save_file
        from strata_ml import worker as W
        from strata_ml.tasks import imagegen, imagegen_flux3
        cls.W, cls.ig, cls.f3 = W, imagegen, imagegen_flux3
        W.load_tasks()
        cls.tmp = tempfile.mkdtemp(prefix="imagegen-flux3-test-")
        cls.home = os.path.join(cls.tmp, "home")
        cls.ckpts = os.path.join(cls.tmp, "checkpoints")
        cls.klein = os.path.join(cls.ckpts, "flux-3-image-klein")
        cls.base = os.path.join(cls.ckpts, "flux-3-image-klein-base")
        cls.dev = os.path.join(cls.ckpts, "flux2-dev-tiny")
        t = build_flux3(cls.klein, distilled=True)
        tb = build_flux3(cls.base, distilled=False)
        dev = build_dev(cls.dev)
        cls.klein_sd = {k: v.detach().clone() for k, v in t.state_dict().items()}
        cls.base_sd = {k: v.detach().clone() for k, v in tb.state_dict().items()}
        cls.dev_file = os.path.join(cls.tmp, "flux2-dev-finetune.safetensors")
        save_file({k: v.detach().contiguous() for k, v in dev.transformer.state_dict().items()}, cls.dev_file)
        cls.lora = os.path.join(cls.tmp, "loras", "flux3_style.safetensors")
        os.makedirs(os.path.dirname(cls.lora))
        make_lora(imagegen_flux3.pipe_class(), t, cls.lora, 1)
        cls.worker = W.Worker(cls.home)
        cls.n = 0

    @classmethod
    def tearDownClass(cls):
        cls.ig.unload()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_job(self, params, inputs=None, expect="done"):
        type(self).n += 1
        key = f"imagegen-f3-{self.n}"
        p = {"checkpoint": self.klein, "width": SIZE, "height": SIZE, "steps": 4, "guidance": 1.0, "seed": 7,
             "batch_count": 1, "prompt": "a red house", "dtype": "fp32", "preview_every": 1}
        p.update(params)
        jd = write_job(self.home, key, p, inputs)
        self.worker.run_job(key)
        st = read(jd, "status.json")
        self.assertEqual(st["state"], expect, st.get("message"))
        return jd, (read(jd, "result.json") if expect == "done" else None)

    def take(self, jd, i=0):
        return np.load(os.path.join(jd, "out", f"take.{i}.npy")).astype(np.float32)

    def source(self):
        y, x = np.mgrid[0:300, 0:400] / 300.0
        rgb = np.stack([x * 0.8, y * 0.6, 0.3 + 0 * x], 2)
        return np.concatenate([rgb, np.ones((300, 400, 1))], 2).astype(np.float32)

    # ------------------------------------------------------------ what a folder / file is
    def test_folder_detection(self):
        self.assertTrue(self.f3.is_flux3_folder(self.klein))
        self.assertTrue(self.f3.is_flux3_folder(self.base))
        self.assertFalse(self.f3.is_flux3_folder(self.dev))
        self.assertTrue(self.f3.folder_flow_map(self.klein))
        self.assertFalse(self.f3.folder_flow_map(self.base))

    def test_component_from_modular_index(self):
        """A modular repo may take a component from another repository (modular_model_index.json)."""
        other = os.path.join(self.tmp, "split-repo")
        os.makedirs(other, exist_ok=True)
        shutil.copytree(os.path.join(self.klein, "transformer"), os.path.join(other, "transformer"), dirs_exist_ok=True)
        with open(os.path.join(other, "modular_model_index.json"), "w") as f:
            json.dump({"_class_name": "Flux3DistilledModularPipeline",
                       "text_encoder": ["transformers", "Qwen3VLForConditionalGeneration",
                                        {"pretrained_model_name_or_path": self.klein, "subfolder": "text_encoder"}]}, f)
        self.assertEqual(self.f3.component_dir(other, "transformer"), os.path.join(other, "transformer"))
        self.assertEqual(self.f3.component_dir(other, "text_encoder"), os.path.join(self.klein, "text_encoder"))
        with self.assertRaisesRegex(RuntimeError, "has no vae"):
            self.f3.component_dir(other, "vae")

    # ------------------------------------------------------------ generation
    def test_txt2img_batch_distilled(self):
        jd, r = self.run_job({"batch_count": 2, "seed": 100})
        self.assertEqual(r["model"], "Flux3")
        self.assertEqual(self.ig._STATE["info"]["family"], "flux3")
        self.assertTrue(self.ig._STATE["info"]["distilled"])
        self.assertEqual([t["seed"] for t in r["takes"]], [100, 101])
        a, b = self.take(jd, 0), self.take(jd, 1)
        self.assertEqual(a.shape, (SIZE, SIZE, 4))
        self.assertTrue(np.isfinite(a).all())
        self.assertGreater(np.abs(a - b).mean(), 1e-3, "different seeds, different images")
        st = read(jd, "status.json")
        self.assertEqual(st["detail"]["takes_done"], 2)
        self.assertGreater(st["detail"]["preview_seq"], 0)
        self.assertFalse(r["warnings"])

    def test_same_seed_same_image(self):
        a, _ = self.run_job({"seed": 5})
        b, _ = self.run_job({"seed": 5, "batch_count": 2})
        self.assertLess(np.abs(self.take(a) - self.take(b)).max(), 1e-4)

    def test_distilled_warnings(self):
        _, r = self.run_job({"guidance": 4.0, "negative_prompt": "a blue sky", "steps": 20})
        w = " ".join(r["warnings"])
        self.assertIn("ignores guidance", w)
        self.assertIn("4 to 16 steps", w)
        self.assertIn("negative", w)
        self.assertEqual(r["takes"][0]["negative_prompt"], "")

    def test_base_cfg_and_negative_prompt(self):
        a, ra = self.run_job({"checkpoint": self.base, "guidance": 3.5, "negative_prompt": ""})
        self.assertFalse(self.ig._STATE["info"]["distilled"])
        self.assertTrue(self.ig._STATE["info"]["negative"])
        b, rb = self.run_job({"checkpoint": self.base, "guidance": 3.5, "negative_prompt": "a blue sky at night"})
        self.assertFalse(rb["warnings"])
        self.assertEqual(rb["takes"][0]["negative_prompt"], "a blue sky at night")
        self.assertGreater(np.abs(self.take(a) - self.take(b)).mean(), 1e-5, "the negative prompt is used")
        c, _ = self.run_job({"checkpoint": self.base, "guidance": 1.0, "negative_prompt": "a blue sky at night"})
        self.assertGreater(np.abs(self.take(c) - self.take(b)).mean(), 1e-5, "guidance changes the image")

    def test_switching_between_flux2_and_flux3(self):
        self.run_job({})
        p3 = self.ig._STATE["pipe"]
        self.run_job({"checkpoint": self.dev, "guidance": 4.0})
        self.assertEqual(self.ig._STATE["info"]["class"], "Flux2Pipeline")
        self.assertEqual(self.ig._STATE["info"]["family"], "flux2")
        self.run_job({})
        self.assertIsNot(self.ig._STATE["pipe"], p3, "reloaded after FLUX.2 replaced it")
        self.assertEqual(self.ig._STATE["info"]["class"], "Flux3")

    def test_samplers(self):
        for s in self.ig.SAMPLERS:
            jd, r = self.run_job({"sampler": s, "seed": 11})
            self.assertTrue(np.isfinite(self.take(jd)).all(), s)

    # ------------------------------------------------------------ source image modes
    def test_img2img_denoise(self):
        src = self.source()
        fitted = self.ig.fit_image(self.ig.srgb_encode(src[..., :3]), SIZE, SIZE, "crop")
        diffs = []
        for d in (0.1, 1.0):
            jd, _ = self.run_job({"mode": "img2img", "denoise": d, "steps": 10, "seed": 9}, {"src": src})
            diffs.append(np.abs(self.ig.srgb_encode(self.take(jd)[..., :3]) - fitted).mean())
        self.assertLess(diffs[0], diffs[1], f"less denoise stays closer to the source ({diffs})")

    def test_inpaint_keeps_unmasked_pixels(self):
        src = self.source()
        mask = np.zeros_like(src)
        mask[100:200, 150:250, 3] = 1.0
        jd, _ = self.run_job({"mode": "inpaint", "denoise": 1.0, "mask_grow": 0, "mask_feather": 0, "fit": "stretch"},
                             {"src": src, "mask": mask})
        out = self.take(jd)
        ref = self.ig.fit_image(src[..., :3], SIZE, SIZE, "stretch")
        m = self.ig.fit_image(mask[..., 3:4], SIZE, SIZE, "stretch")[..., 0]
        outside = m < 1e-3
        err = np.abs(out[..., :3][outside] - ref[outside]) / np.maximum(1.0, ref[outside])
        self.assertLess(err.max(), 2e-3)
        self.assertGreater(np.abs(out[..., :3][m > 0.999] - ref[m > 0.999]).mean(), 1e-3)

    def test_reference_images(self):
        ref = self.source()
        ref[..., 0] = 1.5
        base, _ = self.run_job({"seed": 6})
        jd, r = self.run_job({"seed": 6}, {"ref": ref})
        self.assertEqual(r["takes"][0]["references"], 1)
        self.assertGreater(np.abs(self.take(jd) - self.take(base)).mean(), 1e-5, "the reference conditions the image")
        jd2, r2 = self.run_job({"mode": "img2img", "denoise": 0.6, "use_reference": True}, {"src": self.source(), "ref": ref})
        self.assertEqual(r2["takes"][0]["references"], 2)
        self.assertTrue(np.isfinite(self.take(jd2)).all())

    # ------------------------------------------------------------ LoRAs
    def test_lora(self):
        base, _ = self.run_job({"seed": 4})
        a, ra = self.run_job({"seed": 4, "loras": [{"file": self.lora, "weight": 1.0}]})
        self.assertEqual(len(ra["takes"][0]["loras"]), 1)
        self.assertGreater(np.abs(self.take(a) - self.take(base)).mean(), 1e-3, "a LoRA changes the image")
        again, _ = self.run_job({"seed": 4, "loras": []})
        self.assertEqual(self.ig._STATE["adapters"], {})
        self.assertLess(np.abs(self.take(again) - self.take(base)).max(), 1e-4, "an empty stack restores the model")

    # ------------------------------------------------------------ single files
    def test_bfl_single_file_finds_its_folder(self):
        from safetensors.torch import save_file
        cands = [self.dev, self.base, self.klein]
        for name, sd, want in (("flux-3-image-klein.safetensors", self.klein_sd, self.klein),
                               ("flux-3-image-klein-base.safetensors", self.base_sd, self.base)):
            path = os.path.join(self.tmp, name)
            save_file({k: v.contiguous() for k, v in to_bfl_flux3(sd).items()}, path)
            self.assertEqual(self.ig.pick_base(path, cands), want, name)
            jd, _ = self.run_job({"checkpoint": path, "base": "auto", "base_candidates": cands, "seed": 3})
            self.assertEqual(self.ig._STATE["info"]["folder"], want)
            ref, _ = self.run_job({"checkpoint": want, "seed": 3})
            self.assertLess(np.abs(self.take(jd) - self.take(ref)).max(), 1e-4, f"{name}: the folder's weights")

    def test_comfy_prefixed_fp8_single_file(self):
        from safetensors.torch import save_file
        path = os.path.join(self.tmp, "flux3_klein_fp8_scaled.safetensors")
        sd = comfy_fp8({k: v.float() for k, v in to_bfl_flux3(self.klein_sd).items()}, per_channel=True)
        save_file({k: v.contiguous() for k, v in sd.items()}, path)
        cands = [self.dev, self.base, self.klein]
        self.assertEqual(self.ig.pick_base(path, cands), self.klein)
        jd, _ = self.run_job({"checkpoint": path, "base": "auto", "base_candidates": cands, "seed": 3})
        ref, _ = self.run_job({"seed": 3})
        other, _ = self.run_job({"seed": 4})
        err = np.abs(self.take(jd) - self.take(ref)).mean()
        self.assertLess(err, 0.25 * np.abs(self.take(other) - self.take(ref)).mean(), f"fp8 scaled back ({err})")

    def test_diffusers_named_single_file(self):
        from safetensors.torch import save_file
        path = os.path.join(self.tmp, "flux3-klein-diffusers-names.safetensors")
        save_file({k: v.contiguous() for k, v in self.klein_sd.items()}, path)
        self.assertEqual(self.ig.pick_base(path, [self.base, self.klein]), self.klein)
        jd, _ = self.run_job({"checkpoint": path, "base": "auto", "base_candidates": [self.base, self.klein], "seed": 3})
        ref, _ = self.run_job({"seed": 3})
        self.assertLess(np.abs(self.take(jd) - self.take(ref)).max(), 1e-4)

    def test_flux2_file_ignores_flux3_folders(self):
        self.assertEqual(self.ig.pick_base(self.dev_file, [self.klein, self.base, self.dev]), self.dev)
        with self.assertRaisesRegex(RuntimeError, "none of the Diffusers model folders"):
            self.ig.pick_base(self.dev_file, [self.klein, self.base])

    def test_flux3_file_without_flux3_folder(self):
        from safetensors.torch import save_file
        path = os.path.join(self.tmp, "lonely-flux3.safetensors")
        save_file({k: v.contiguous() for k, v in to_bfl_flux3(self.klein_sd).items()}, path)
        with self.assertRaisesRegex(RuntimeError, "FLUX 3"):
            self.ig.pick_base(path, [self.dev])

    # ------------------------------------------------------------ prompt upsampler
    def test_upsampler_missing(self):
        jd, _ = self.run_job({"prompt_upsampler": True}, expect="error")
        self.assertIn("klein-pup", read(jd, "status.json")["message"])

    def test_upsampler_caption_is_used(self):
        pup = os.path.join(self.ckpts, "flux-3-image-klein-pup")
        os.makedirs(pup, exist_ok=True)
        with open(os.path.join(pup, "config.json"), "w") as f:
            json.dump({"model_type": "qwen3_5"}, f)
        calls = []

        def fake_upsample(model, proc, request, images=None, boxes=None, max_new_tokens=0):
            calls.append((request, len(images or [])))
            return "a red house on a green hill at night"

        import transformers
        from diffusers.modular_pipelines.flux3 import prompt_upsampler as pu
        try:
            with mock.patch.object(pu, "upsample_prompt", fake_upsample), \
                    mock.patch.object(transformers, "Qwen3_5ForConditionalGeneration", create=True) as qm, \
                    mock.patch.object(transformers.AutoProcessor, "from_pretrained"):
                qm.from_pretrained.return_value.to.return_value = object()
                plain, _ = self.run_job({"seed": 12, "prompt": "a red house on a green hill at night"})
                jd, r = self.run_job({"seed": 12, "prompt": "house", "prompt_upsampler": True})
                self.run_job({"seed": 13, "prompt": "house", "prompt_upsampler": True})
        finally:
            shutil.rmtree(pup, ignore_errors=True)
        self.assertEqual(calls, [("house", 0)], "the caption of an unchanged prompt is cached")
        self.assertEqual(r["takes"][0]["prompt"], "house")
        self.assertEqual(r["takes"][0]["caption"], "a red house on a green hill at night")
        self.assertLess(np.abs(self.take(jd) - self.take(plain)).max(), 1e-4, "the image follows the caption")

    # ------------------------------------------------------------ cancel
    def test_cancel(self):
        type(self).n += 1
        key = f"imagegen-f3-{self.n}"
        jd = write_job(self.home, key, {"checkpoint": self.klein, "width": 512, "height": 512, "steps": 200,
                                        "batch_count": 3, "prompt": "a cat", "dtype": "fp32", "seed": 1})

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
        self.worker.run_job(key)
        th.join()
        self.assertEqual(read(jd, "status.json")["state"], "cancelled")
        for m in self.ig._STATE["pipe"].transformer.modules():
            self.assertFalse(m._forward_pre_hooks, "cancel hooks removed")
        self.run_job({"seed": 2})


if __name__ == "__main__":
    unittest.main()
