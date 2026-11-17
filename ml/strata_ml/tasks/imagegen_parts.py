"""FLUX.2 from ComfyUI's split files: a transformer-only .safetensors (models/diffusion_models or
checkpoints), a text encoder file (models/text_encoders) and a VAE file (models/vae), with no Diffusers
folder. The configs come from the tensor shapes; only the tokenizer (a few MB) is fetched from
huggingface.co, once, unless a tokenizer folder sits beside the text encoder file.

FLUX.2 [klein] (Qwen3 text encoder) is supported. FLUX.2 [dev]'s Mistral text encoder file isn't: a
[dev] transformer file still needs the Diffusers [dev] folder (see imagegen.pick_base)."""

import json
import os

from .imagegen import read_transformer_file, strip_prefix, _safetensors_shapes

QWEN3_TOKENIZER_REPO = "Qwen/Qwen3-8B"     # the same tokenizer for every Qwen3 size (and FLUX.2 [klein]'s)
TOKENIZER_FILES = ("tokenizer.json", "tokenizer_config.json", "vocab.json", "merges.txt")
SCHEDULER = {"base_image_seq_len": 256, "base_shift": 0.5, "max_image_seq_len": 4096, "max_shift": 1.15,
             "num_train_timesteps": 1000, "shift": 3.0, "use_dynamic_shifting": True}
QWEN3_ROPE_THETA = 1000000.0
_VAE_PREFIXES = ("first_stage_model.", "vae.", "model.")


def _base(path):
    return os.path.basename(path.rstrip(os.sep))


def file_shapes(path):
    try:
        return {strip_prefix(k): v for k, v in _safetensors_shapes(path).items()}
    except Exception as e:  # noqa: BLE001
        raise RuntimeError(f"can't read {_base(path)}: {e}") from e


# ================================================================= transformer
def transformer_config(shapes):
    """Flux2Transformer2DModel's config from a transformer file's tensor shapes (BFL / ComfyUI or Diffusers naming)."""
    def get(*names):
        for n in names:
            if n in shapes:
                return shapes[n]
        raise RuntimeError("not a FLUX.2 transformer (no " + names[0] + ")")

    def count(prefixes):
        idx = set()
        for k in shapes:
            for p in prefixes:
                if k.startswith(p) and k[len(p):].split(".", 1)[0].isdigit():
                    idx.add(int(k[len(p):].split(".", 1)[0]))
        return len(idx)

    inner, in_ch = get("img_in.weight", "x_embedder.weight")
    joint = get("txt_in.weight", "context_embedder.weight")[1]
    head = get("double_blocks.0.img_attn.norm.query_norm.scale", "transformer_blocks.0.attn.norm_q.weight")[0]
    out_ch = get("final_layer.linear.weight", "proj_out.weight")[0]
    tgc = get("time_in.in_layer.weight", "time_guidance_embed.timestep_embedder.linear_1.weight")[1]
    mlp = get("double_blocks.0.img_mlp.0.weight", "transformer_blocks.0.ff.linear_in.weight")[0] // 2  # SwiGLU: gate + up
    guidance = any(k.startswith(("guidance_in.", "time_guidance_embed.guidance_embedder.")) for k in shapes)
    return {"patch_size": 1, "in_channels": in_ch, "out_channels": out_ch,
            "num_layers": count(("double_blocks.", "transformer_blocks.")),
            "num_single_layers": count(("single_blocks.", "single_transformer_blocks.")),
            "attention_head_dim": head, "num_attention_heads": inner // head, "joint_attention_dim": joint,
            "timestep_guidance_channels": tgc, "mlp_ratio": mlp / inner, "axes_dims_rope": [head // 4] * 4,
            "guidance_embeds": guidance}


def load_transformer(path, dtype):
    import diffusers
    from accelerate import init_empty_weights
    from diffusers.loaders.single_file_utils import convert_flux2_transformer_checkpoint_to_diffusers
    sd = read_transformer_file(path, dtype)
    cfg = transformer_config({k: tuple(v.shape) for k, v in sd.items()})
    if not any(k.startswith(("transformer_blocks.", "single_transformer_blocks.")) for k in sd):
        sd = convert_flux2_transformer_checkpoint_to_diffusers(sd)
    with init_empty_weights():
        m = diffusers.Flux2Transformer2DModel.from_config(cfg)
    try:
        m.load_state_dict(sd, strict=True, assign=True)
    except RuntimeError as e:
        raise RuntimeError(f"can't load {_base(path)} as a FLUX.2 transformer: {e}") from e
    return m.to(dtype)


# ================================================================= text encoder (Qwen3)
def _te_prefix(keys):
    """The prefix before "layers.N." in a text encoder file ("model.", "", "qwen3_8b.transformer.model.", ...)."""
    for k in keys:
        i = k.find("layers.0.self_attn.q_proj.weight")
        if i >= 0:
            return k[:i]
    return None


def text_encoder_info(shapes):
    """(family, hidden size) of a text encoder file: ("qwen3", n), or ("other", n) (e.g. [dev]'s Mistral)."""
    pre = _te_prefix(shapes)
    if pre is None:
        raise RuntimeError("not a text encoder")
    hidden = shapes[pre + "embed_tokens.weight"][1] if pre + "embed_tokens.weight" in shapes else None
    qwen = pre + "layers.0.self_attn.q_norm.weight" in shapes
    return ("qwen3" if qwen else "other"), hidden


def qwen3_config(shapes, pre):
    from transformers import Qwen3Config
    vocab, hidden = shapes[pre + "embed_tokens.weight"]
    head = shapes[pre + "layers.0.self_attn.q_norm.weight"][0]
    layers = len({int(k[len(pre) + 7:].split(".", 1)[0]) for k in shapes if k.startswith(pre + "layers.")})
    return Qwen3Config(vocab_size=vocab, hidden_size=hidden, intermediate_size=shapes[pre + "layers.0.mlp.gate_proj.weight"][0],
                       num_hidden_layers=layers, num_attention_heads=shapes[pre + "layers.0.self_attn.q_proj.weight"][0] // head,
                       num_key_value_heads=shapes[pre + "layers.0.self_attn.k_proj.weight"][0] // head, head_dim=head,
                       max_position_embeddings=40960, rms_norm_eps=1e-6, tie_word_embeddings=True,
                       rope_parameters={"rope_type": "default", "rope_theta": QWEN3_ROPE_THETA})


def load_text_encoder(path, dtype):
    """A Qwen3 text encoder file as Qwen3ForCausalLM. Only its hidden states are used, so the LM head is
    dropped and tied to the embeddings (no extra memory)."""
    from accelerate import init_empty_weights
    from transformers import Qwen3ForCausalLM
    sd = read_transformer_file(path, dtype)
    pre = _te_prefix(sd)
    if pre is None or pre + "layers.0.self_attn.q_norm.weight" not in sd:
        raise RuntimeError(f"{_base(path)} isn't a Qwen3 text encoder: FLUX.2 [klein] split files need Qwen3 "
                           "(qwen_3_4b / qwen_3_8b); for FLUX.2 [dev] use the Diffusers [dev] folder")
    cfg = qwen3_config({k: tuple(v.shape) for k, v in sd.items()}, pre)
    sd = {"model." + k[len(pre):]: v for k, v in sd.items() if k.startswith(pre)}
    with init_empty_weights():
        m = Qwen3ForCausalLM(cfg)
    missing, unexpected = m.load_state_dict(sd, strict=False, assign=True)
    missing = [k for k in missing if k != "lm_head.weight"]
    if missing or unexpected:
        raise RuntimeError(f"can't load {_base(path)} as a Qwen3 text encoder: missing {missing[:4]}, unexpected {unexpected[:4]}")
    m.tie_weights()
    return m.to(dtype).eval()


def load_tokenizer(te_path, override=""):
    """The Qwen3 tokenizer: `override` (a folder), a folder beside the text encoder file (named after it,
    or "tokenizer"), else Qwen3's from huggingface.co (a few MB, cached by huggingface_hub)."""
    from transformers import AutoTokenizer
    here = os.path.dirname(te_path)
    stem = os.path.splitext(os.path.basename(te_path))[0]
    for d in (override, os.path.join(here, stem), os.path.join(here, stem + "_tokenizer"), os.path.join(here, "tokenizer")):
        if d and os.path.isfile(os.path.join(d, "tokenizer_config.json")):
            return AutoTokenizer.from_pretrained(d)
    try:
        from huggingface_hub import hf_hub_download
        d = None
        for f in TOKENIZER_FILES:
            d = os.path.dirname(hf_hub_download(QWEN3_TOKENIZER_REPO, f))
        return AutoTokenizer.from_pretrained(d)
    except Exception as e:  # noqa: BLE001
        raise RuntimeError(f"couldn't get the Qwen3 tokenizer from huggingface.co ({e}). Offline: put "
                           f"{', '.join(TOKENIZER_FILES)} from {QWEN3_TOKENIZER_REPO} in a folder named 'tokenizer' "
                           f"next to {os.path.basename(te_path)}") from e


# ================================================================= VAE
def _vae_sd(sd):
    for p in _VAE_PREFIXES:
        if any(k.startswith(p + "decoder.") for k in sd):
            return {k[len(p):]: v for k, v in sd.items() if k.startswith(p)}
    return sd


def vae_config(shapes):
    """AutoencoderKLFlux2's config from a VAE file's shapes, in LDM / BFL naming or Diffusers naming
    (Comfy-Org's flux2-vae.safetensors is the latter)."""
    diffusers_names = is_diffusers_vae(shapes)
    down, res = ("encoder.down_blocks.", "resnets") if diffusers_names else ("encoder.down.", "block")
    levels = sorted({int(k.split(".")[2]) for k in shapes if k.startswith(down) and k.split(".")[2].isdigit()})
    if not levels:
        raise RuntimeError("not a VAE (no encoder down blocks)")
    blocks = [shapes[f"{down}{i}.{res}.0.conv1.weight"][0] for i in levels]
    per = len({k.split(".")[4] for k in shapes if k.startswith(f"{down}0.{res}.")})
    latent = shapes["decoder.conv_in.weight"][1]
    patch = 2
    bn = shapes.get("bn.running_mean")
    if bn and bn[0] % latent == 0:
        patch = int(round((bn[0] // latent) ** 0.5))
    groups = 32 if all(c % 32 == 0 for c in blocks) else min(blocks)
    return {"in_channels": shapes["encoder.conv_in.weight"][1], "out_channels": shapes["decoder.conv_out.weight"][0],
            "down_block_types": ["DownEncoderBlock2D"] * len(blocks), "up_block_types": ["UpDecoderBlock2D"] * len(blocks),
            "block_out_channels": blocks, "layers_per_block": per, "latent_channels": latent, "norm_num_groups": groups,
            "use_quant_conv": "quant_conv.weight" in shapes or "encoder.quant_conv.weight" in shapes,
            "use_post_quant_conv": "post_quant_conv.weight" in shapes or "decoder.post_quant_conv.weight" in shapes,
            "mid_block_add_attention": any(k.startswith(("encoder.mid.attn_1.", "encoder.mid_block.attentions.0.")) for k in shapes),
            "patch_size": [patch, patch]}


def is_diffusers_vae(keys):
    return any(k.startswith(("decoder.up_blocks.", "encoder.down_blocks.")) for k in keys)


def convert_vae(sd, cfg):
    """LDM / BFL VAE naming -> Diffusers' AutoencoderKLFlux2 (the batch norm passes through)."""
    from diffusers.loaders.single_file_utils import convert_ldm_vae_checkpoint
    sd = dict(sd)
    for a, b in (("encoder.quant_conv.", "quant_conv."), ("decoder.post_quant_conv.", "post_quant_conv.")):
        for k in [k for k in sd if k.startswith(a)]:
            sd[b + k[len(a):]] = sd.pop(k)
    out = convert_ldm_vae_checkpoint(sd, cfg)
    for k, v in sd.items():
        if k.startswith("bn."):
            out[k] = v
    return out


def load_vae(path, dtype):
    import diffusers
    from accelerate import init_empty_weights
    sd = _vae_sd(read_transformer_file(path, dtype))
    cfg = vae_config({k: tuple(v.shape) for k, v in sd.items()})
    if not is_diffusers_vae(sd):
        sd = convert_vae(sd, cfg)
    with init_empty_weights():
        m = diffusers.AutoencoderKLFlux2.from_config(cfg)
    want = set(m.state_dict())
    sd = {k: v for k, v in sd.items() if k in want or not k.endswith("num_batches_tracked")}
    if "bn.num_batches_tracked" in want and "bn.num_batches_tracked" not in sd:
        import torch
        sd["bn.num_batches_tracked"] = torch.tensor(0)
    try:
        m.load_state_dict(sd, strict=True, assign=True)
    except RuntimeError as e:
        raise RuntimeError(f"can't load {_base(path)} as a FLUX.2 VAE: {e}") from e
    return m.to(dtype).eval()


# ================================================================= choosing the parts
def vae_latent_features(shapes):
    """The channels of a FLUX.2 VAE's (patched) latent, from its batch norm; None for other VAEs."""
    for p in ("",) + _VAE_PREFIXES:
        if p + "bn.running_mean" in shapes:
            return shapes[p + "bn.running_mean"][0]
    return None


def pick_parts(transformer, te, vae, candidates):
    """The text encoder and VAE files for a transformer-only file: the ones the user picked, else the
    candidates that fit it (a Qwen3 whose hidden size x 3 is the transformer's text width; a VAE whose
    latent has the transformer's input channels). Returns (te_path, vae_path)."""
    cfg = transformer_config(file_shapes(transformer))
    if cfg["guidance_embeds"]:
        raise RuntimeError(f"{_base(transformer)} is FLUX.2 [dev]: its text encoder (Mistral) only loads from the "
                           "Diffusers [dev] folder; put that folder in the checkpoints folder")
    te_ok, vae_ok, why = [], [], []
    for c in candidates:
        path, kind = c.get("path", ""), c.get("kind", "")
        try:
            shapes = file_shapes(path)
        except RuntimeError:
            continue
        if kind == "text_encoder":
            fam, hidden = text_encoder_info(shapes)
            if fam == "qwen3" and hidden and hidden * 3 == cfg["joint_attention_dim"]:
                te_ok.append(path)
            else:
                why.append(f"{_base(path)} ({fam}, width {hidden})")
        elif kind == "vae" and vae_latent_features(shapes) == cfg["in_channels"]:
            vae_ok.append(path)
    if te not in ("", "auto", None) and te not in te_ok:
        fam, hidden = text_encoder_info(file_shapes(te))
        raise RuntimeError(f"{_base(te)} doesn't fit {_base(transformer)}: it wants a Qwen3 text encoder with width "
                           f"{cfg['joint_attention_dim'] // 3}, this one is {fam} with width {hidden}")
    if te in ("", "auto", None):
        if not te_ok:
            raise RuntimeError(f"no text encoder fits {_base(transformer)} (it wants Qwen3 with width "
                               f"{cfg['joint_attention_dim'] // 3}, e.g. qwen_3_{'8b' if cfg['joint_attention_dim'] > 9000 else '4b'}"
                               ".safetensors in models/text_encoders)" + (": found " + ", ".join(why) if why else ""))
        te = te_ok[0]
    if vae in ("", "auto", None):
        if not vae_ok:
            raise RuntimeError(f"no FLUX.2 VAE for {_base(transformer)}: put flux2-vae.safetensors in models/vae")
        vae = vae_ok[0]
    return te, vae


def is_distilled(transformer):
    """[klein] and [klein] base share an architecture: a file named "...base..." is the undistilled one."""
    return "base" not in os.path.basename(transformer).lower()


def build_pipeline(transformer, te, vae, dtype, tokenizer=""):
    import diffusers
    pipe = diffusers.Flux2KleinPipeline(
        scheduler=diffusers.FlowMatchEulerDiscreteScheduler(**SCHEDULER), vae=load_vae(vae, dtype),
        text_encoder=load_text_encoder(te, dtype), tokenizer=load_tokenizer(te, tokenizer),
        transformer=load_transformer(transformer, dtype), is_distilled=is_distilled(transformer))
    return pipe


def describe(te, vae):
    return json.dumps({"text_encoder": te, "vae": vae})
