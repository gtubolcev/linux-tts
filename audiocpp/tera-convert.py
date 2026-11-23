#!/usr/bin/env python3
"""TeraTTS v2 (ONNX) -> audio.cpp Supertonic model directory.

TeraTTS has Supertonic's architecture with other tensor names, a smaller
character vocabulary and a distilled sampler (CFG baked in, one pass per step).

usage: tera-convert.py TERA_DIR OUT_DIR [ST_NAMES]
  TERA_DIR  models/*.onnx, styles/<voice>/style_{ttl,dp}.npy, unicode_indexer.json
  ST_NAMES  "name [shape]" lines of the Supertonic GGUF, for a shape check
"""
import json, re, sys
from pathlib import Path

import numpy as np
import onnx
from onnx import numpy_helper

src, out = Path(sys.argv[1]), Path(sys.argv[2])
VE = "vector_estimator.vector_estimator."
VF = VE + "tts.ttl.vector_field."
TE = "text_encoder.tts.ttl."
DP = "duration_predictor.tts.dp."
VOC = "vocoder.tts.ae.decoder."


def inits(graph):
    """initializers of a graph and its subgraphs, and the nodes using each"""
    found, users = {}, {}

    def walk(g):
        for i in g.initializer:
            found[i.name] = numpy_helper.to_array(i)
        for n in g.node:
            for x in n.input:
                users.setdefault(x, []).append(n)
            for a in n.attribute:
                if a.type == onnx.AttributeProto.GRAPH:
                    walk(a.g)
    walk(graph)
    return found, users


def convnext_norm(name):
    return re.sub(r"(convnext\.\d+)\.norm\.", r"\1.norm.norm.", name)


def matmul_name(users, name, prefix_map):
    """an onnx::MatMul_N weight: named after the node that uses it"""
    nodes = [n.name for n in users.get(name, []) if n.op_type == "MatMul"]
    if len(nodes) != 1:
        raise SystemExit(f"{name}: used by {nodes}")
    path = nodes[0].strip("/").split("/")[:-1]   # drop "MatMul"
    return prefix_map(path)


tensors = {}
transposed = []


def put(name, arr):
    if name in tensors:
        raise SystemExit(f"duplicate {name}")
    tensors[name] = np.ascontiguousarray(arr.astype(np.float32) if arr.dtype == np.float64 else arr)


# ---- text encoder
t, users = inits(onnx.load(src / "models/text_encoder.onnx").graph)
for n, a in t.items():
    if n == "style_key":
        put(TE + "style_encoder.style_token_layer.style_key", a)
    elif n == "char_embedder.weight":
        put(TE + "text_encoder.text_embedder.char_embedder.weight", a)
    elif n.startswith(("convnext.", "attn_encoder.")):
        put(TE + "text_encoder." + convnext_norm(n), a)
    elif m := re.fullmatch(r"(attention[12])\.(\w+)\.bias", n):
        put(TE + f"speech_prompted_text_encoder.{m[1]}.{m[2]}.linear.bias", a)
    elif n.startswith("final_norm."):
        put(TE + "speech_prompted_text_encoder.norm.norm." + n.split(".")[-1], a)
    elif n.startswith("onnx::MatMul_"):
        # e.g. /attention1/W_query/MatMul
        p = matmul_name(users, n, lambda p: p[-2:])
        put(TE + f"speech_prompted_text_encoder.{p[0]}.{p[1]}.linear.weight", a)
    else:
        raise SystemExit(f"text_encoder: unknown {n}")

# ---- duration predictor
t, users = inits(onnx.load(src / "models/duration_predictor.onnx").graph)
for n, a in t.items():
    if n == "sentence_encoder.char_embedder.weight":
        put(DP + "sentence_encoder.text_embedder.char_embedder.weight", a)
    elif n == "sentence_encoder.proj_out.weight":
        put(DP + "sentence_encoder.proj_out.net.weight", a)
    elif n.startswith("sentence_encoder."):
        put(DP + convnext_norm(n), a)
    elif n.startswith(("layers.", "activation.")):
        put(DP + "predictor." + n, a)
    else:
        raise SystemExit(f"duration_predictor: unknown {n}")

# ---- vocoder
t, users = inits(onnx.load(src / "models/vocoder.onnx").graph)
for n, a in t.items():
    if n == "normalizer_scale":
        put("vocoder.tts.ttl.normalizer.scale", a.reshape(1))
    elif n in ("latent_mean", "latent_std"):
        put("vocoder.tts.ae." + n, a)
    elif n.startswith("embed."):
        put(VOC + "embed.net." + n.split(".")[-1], a)
    elif m := re.fullmatch(r"convnext\.convnext\.(\d+)\.(.+)", n):
        rest = m[2]
        rest = re.sub(r"^dwconv\.", "dwconv.net.", rest)
        rest = re.sub(r"^norm\.", "norm.norm.", rest)
        put(VOC + f"convnext.{m[1]}.{rest}", a)
    elif n.startswith("final_norm."):
        put(VOC + "final_norm.norm." + n.split(".", 1)[1], a)
    elif n.startswith("head_layer1."):
        put(VOC + "head.layer1.net." + n.split(".")[-1], a)
    elif n == "head_layer2.weight":
        put(VOC + "head.layer2.weight", a)
    elif n.startswith("onnx::PRelu_"):
        put(VOC + "head.act.weight", a)
    else:
        raise SystemExit(f"vocoder: unknown {n}")

# ---- sampler (vector field in a Loop body)
g = onnx.load(src / "models/sampler_distilled_cfg3_8step.onnx").graph
t, users = inits(g)
dilations = {}
for n, a in t.items():
    if n in ("sampler_trip_count", "sampler_continue") or a.dtype != np.float32:
        continue
    if n == "vector_field.style_key":
        put(VE + "Expand_output_0", a)
    elif n in ("vector_field.proj_in.weight", "vector_field.proj_out.weight"):
        put(VF + n.split(".")[1] + ".net.weight", a)
    elif m := re.fullmatch(r"vector_field\.time_encoder\.mlp\.(\d)\.(\w+)", n):
        put(VF + f"time_encoder.mlp.{m[1]}.linear.{m[2]}", a)
    elif n.startswith("vector_field.last_convnext."):
        put(VF + convnext_norm(n[len("vector_field."):]), a)
    elif m := re.fullmatch(r"vector_field\.stages\.(\d)\.(\w+)\.(.+)", n):
        k, part, rest = int(m[1]), m[2], m[3]
        b = 6 * k
        if part.startswith("convnext_"):
            idx = {"convnext_0": b, "convnext_1": b + 2, "convnext_2": b + 4}[part]
            put(VF + f"main_blocks.{idx}." + convnext_norm(rest), a)
            if rest.endswith("dwconv.weight"):
                c = [u for u in users[n] if u.op_type == "Conv"]
                dilations[f"{part}.{rest}"] = [list(x.ints) for u in c for x in u.attribute
                                               if x.name == "dilations"]
        elif part == "time_linear":
            # Gemm(transB=1) [out,in]; Supertonic stores the MatMul layout [in,out]
            if rest == "weight":
                a = a.T
                transposed.append(n)
            put(VF + f"main_blocks.{b + 1}.linear.linear.{rest}", a)
        elif part == "text_condition":
            if rest == "attn.theta":
                if k == 0:
                    put(VF + "main_blocks.3.attn.theta", a)
            elif rest.startswith("norm."):
                put(VF + f"main_blocks.{b + 3}.{rest}", a)
            else:
                w, kind = rest.split(".")[1:]
                put(VF + f"main_blocks.{b + 3}.attn.{w}.linear.{kind}", a)
        elif part == "style_condition":
            if rest.startswith("norm."):
                put(VF + f"main_blocks.{b + 5}.{rest}", a)
            else:
                w, kind = rest.split(".")[1:]
                put(VF + f"main_blocks.{b + 5}.attention.{w}.linear.{kind}", a)
        else:
            raise SystemExit(f"sampler: unknown {n}")
    elif n.startswith("onnx::MatMul_"):
        # /vector_field/stages.0/text_condition/attn/W_query/MatMul
        def name(p):
            k = int(p[1].split(".")[1])
            if p[2] == "time_linear":
                return f"main_blocks.{6 * k + 1}.linear.linear.weight"
            if p[2] == "text_condition":
                return f"main_blocks.{6 * k + 3}.attn.{p[4]}.linear.weight"
            if p[2] == "style_condition":
                return f"main_blocks.{6 * k + 5}.attention.{p[4]}.linear.weight"
            raise SystemExit(f"{n}: {p}")
        put(VF + matmul_name(users, n, name), a)
    else:
        raise SystemExit(f"sampler: unknown {n}")

# ---- shape check against Supertonic
if len(sys.argv) > 3:
    ref = {}
    for line in open(sys.argv[3]):
        nm, _, shp = line.strip().partition(" ")
        if "." in nm and shp.startswith("[") and shp.endswith("]"):
            try:
                ref[nm] = json.loads(shp)
            except ValueError:
                pass
    for nm, a in sorted(tensors.items()):
        r = ref.get(nm)
        if r is None:
            print("NOT IN SUPERTONIC", nm, list(a.shape))
        elif list(a.shape) != r and "char_embedder" not in nm:
            print("SHAPE", nm, list(a.shape), "supertonic", r)
    for nm in sorted(set(ref) - set(tensors)):
        if not re.search(r"\.(Mul|Cast|Split|Constant|Gather|Expand|Slice)|webgpu|onnx|increments|uncond_masker", nm):
            print("MISSING", nm, ref[nm])

print("transposed:", transposed)
print("dilations:", {k: v for k, v in dilations.items() if ".stages" not in k and "convnext_0" in k} or
      sorted(set(str(v) for v in dilations.values())))
for k in sorted(dilations):
    if k.startswith("convnext_0"):
        print(" ", k, dilations[k])
        break

def save_file(tensors, path, metadata):
    """a safetensors file without the safetensors package: JSON header, then the
    tensors in name order, little endian, 8-byte aligned header"""
    dtypes = {"float32": "F32", "float16": "F16", "int64": "I64", "int32": "I32",
              "int8": "I8", "uint8": "U8", "bool": "BOOL", "float64": "F64"}
    header, offset, arrays = {"__metadata__": metadata}, 0, []
    for name in sorted(tensors):
        a = np.ascontiguousarray(tensors[name])
        a = a.astype(a.dtype.newbyteorder("<"), copy=False)
        header[name] = {"dtype": dtypes[a.dtype.name], "shape": list(a.shape),
                        "data_offsets": [offset, offset + a.nbytes]}
        offset += a.nbytes
        arrays.append(a)
    head = json.dumps(header, separators=(",", ":")).encode()
    head += b" " * (-len(head) % 8)
    tmp = Path(str(path) + ".part")
    with open(tmp, "wb") as f:
        f.write(len(head).to_bytes(8, "little"))
        f.write(head)
        for a in arrays:
            f.write(a.tobytes())
    tmp.replace(path)


# ---- model directory
(out / "ggml").mkdir(parents=True, exist_ok=True)
(out / "config").mkdir(exist_ok=True)
(out / "voice_styles").mkdir(exist_ok=True)
save_file(tensors, out / "ggml/supertonic.safetensors",
          metadata={"format": "minitts-supertonic-ggml", "source": "TeraTTS v2 (distilled sampler)",
                    "tensor_count": str(len(tensors))})
cfg = json.load(open(src / "config.json"))
(out / "config/tts.json").write_text(json.dumps({
    "ae": {"sample_rate": 44100, "base_chunk_size": 512},
    "ttl": {"chunk_compress_factor": 6, "latent_dim": 24},
    "text": {"duration_skip": "+"},
}, indent=1))
(out / "config/unicode_indexer.json").write_bytes((src / "unicode_indexer.json").read_bytes())
for v in cfg["voices"]:
    style = {}
    for key in ("style_ttl", "style_dp"):
        a = np.load(src / "styles" / v / f"{key}.npy", allow_pickle=False).astype(np.float32)
        if a.ndim == 2:
            a = a[None]
        style[key] = {"dims": list(a.shape), "data": a.tolist()}
    (out / "voice_styles" / f"{v}.json").write_text(json.dumps(style))
print(f"wrote {len(tensors)} tensors, {len(cfg['voices'])} voices to {out}")
