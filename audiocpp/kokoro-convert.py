#!/usr/bin/env python3
"""Kokoro checkpoint (.pth) -> audio.cpp GGUF, with numpy only.

A port of audio.cpp's tools/models/prepare_kokoro_gguf.py (default resources,
no embedded multilingual data) that needs neither torch nor gguf-py: the
.pth zip is read by a restricted unpickler that builds only tensors, and the
GGUF is written by hand. For the same input the output is byte-identical to
the upstream script (kokoro-ru: checked against the gguf-py build).

usage: kokoro-convert.py [--type q8_0|bf16] [--name NAME] SPEC SRC OUT.gguf VOICE=FILE.pt...
  SPEC  audio.cpp model_specs/kokoro_tts.json
  SRC   the checkpoint directory: config.json and the .pth (the only *.pth there)
  VOICE voice name in the GGUF (af_* is a female American voice for audio.cpp)

Run with python3 -I; the input is downloaded data.
"""
import json, pickle, struct, sys, zipfile
from collections import OrderedDict
from pathlib import Path

import numpy as np

ALIGN = 32
T_F32, T_Q8_0, T_BF16 = 0, 8, 30
V_UINT8, V_INT32, V_UINT32, V_STRING, V_ARRAY, V_UINT64, V_INT64 = 0, 5, 4, 8, 9, 10, 11
LANGS = ['en-us', 'en-gb', 'es', 'fr-fr', 'hi', 'it', 'ja', 'pt-br', 'zh']
MAX_FILE = 2 << 30

STORAGES = {'FloatStorage': np.float32, 'HalfStorage': np.float16,
            'BFloat16Storage': 'bf16', 'DoubleStorage': np.float64,
            'LongStorage': np.int64, 'IntStorage': np.int32}


class Tensor:
    __slots__ = ('array',)

    def __init__(self, array):
        self.array = array


def load_pth(path):
    """the state of a torch zip checkpoint; tensors become Tensor(np.ndarray)"""
    z = zipfile.ZipFile(path)
    pkl = [n for n in z.namelist() if n.endswith('/data.pkl') and n.count('/') == 1]
    if len(pkl) != 1:
        raise SystemExit(f'{path}: not a torch zip checkpoint')
    root = pkl[0][:-len('data.pkl')]
    if z.read(root + 'byteorder').strip() not in (b'little', b''):
        raise SystemExit(f'{path}: big-endian checkpoint')
    cache = {}

    def storage(dtype, key, numel):
        if key not in cache:
            info = z.getinfo(root + 'data/' + key)
            if info.file_size > MAX_FILE:
                raise SystemExit(f'{path}: storage {key} too large')
            raw = z.read(info)
            if dtype == 'bf16':
                arr = (np.frombuffer(raw, '<u2').astype(np.uint32) << 16).view(np.float32)
            else:
                arr = np.frombuffer(raw, np.dtype(dtype).newbyteorder('<'))
            if arr.size < numel:
                raise SystemExit(f'{path}: storage {key} short')
            cache[key] = arr
        return cache[key]

    def rebuild_tensor(st, offset, size, stride, *_):
        size, stride = tuple(int(x) for x in size), tuple(int(x) for x in stride)
        need = offset + sum((s - 1) * t for s, t in zip(size, stride)) + 1 if all(size) else 0
        if offset < 0 or any(t < 0 for t in stride) or need > st.size:
            raise SystemExit(f'{path}: tensor outside its storage')
        it = st.itemsize
        a = np.lib.stride_tricks.as_strided(st[offset:], size, tuple(t * it for t in stride))
        return Tensor(np.ascontiguousarray(a))

    def rebuild_parameter(t, *_):
        return t

    class U(pickle.Unpickler):
        def find_class(self, mod, name):
            if (mod, name) == ('collections', 'OrderedDict'):
                return OrderedDict
            if (mod, name) == ('torch._utils', '_rebuild_tensor_v2'):
                return rebuild_tensor
            if (mod, name) == ('torch._utils', '_rebuild_parameter'):
                return rebuild_parameter
            if mod == 'torch' and name in STORAGES:
                return name
            raise SystemExit(f'{path}: forbidden pickle global {mod}.{name}')

        def persistent_load(self, pid):
            if not (isinstance(pid, tuple) and len(pid) == 5 and pid[0] == 'storage'
                    and pid[1] in STORAGES and isinstance(pid[2], str) and pid[2].isdigit()):
                raise SystemExit(f'{path}: bad storage reference')
            return storage(STORAGES[pid[1]], pid[2], int(pid[4]))

    import io
    return U(io.BytesIO(z.read(pkl[0]))).load()


def flatten(prefix, value, out):
    """as flatten_torch_state upstream"""
    if isinstance(value, dict):
        for key, item in value.items():
            key = str(key)
            if key == 'module' and prefix:
                name = prefix
            else:
                if prefix and key.startswith('module.'):
                    key = key[len('module.'):]
                name = key if not prefix else prefix + '.' + key
            flatten(name, item, out)
        return
    if not isinstance(value, Tensor):
        raise SystemExit(f'unsupported checkpoint value at {prefix}: {type(value)!r}')
    out[prefix] = value.array


def np_roundf(n):
    a = abs(n)
    f = np.floor(a)
    return np.sign(n) * (f + np.floor(2 * (a - f)))


def q8_0(x):
    rows = x.reshape(-1, x.shape[-1])
    out = []
    for g in np.array_split(rows, (rows.shape[0] // 16) or 1):
        b = g.reshape(-1, 32)
        d = abs(b).max(axis=1, keepdims=True) / 127
        with np.errstate(divide='ignore'):
            inv = np.where(d == 0, 0, 1 / d)
        qs = np_roundf(b * inv)
        out.append(np.concatenate([d.astype(np.float16).view(np.uint8),
                                   qs.astype(np.int8).view(np.uint8)], axis=1).ravel())
    return np.concatenate(out)


def bf16(x):
    n = x.reshape(-1).view(np.uint32)
    n = np.where((n & 0x7fffffff) > 0x7f800000, (n & np.uint32(0xffff0000)) | np.uint32(64 << 16), n)
    n = (n.astype(np.uint64) + (0x7fff + ((n >> 16) & 1))) >> 16
    return n.astype(np.uint16).view(np.uint8)


def s(v):
    b = v.encode('utf-8')
    return struct.pack('<Q', len(b)) + b


def kv(key, vtype, val, sub=None):
    out = s(key) + struct.pack('<I', vtype)
    if vtype == V_STRING:
        return out + s(val)
    if vtype == V_UINT32:
        return out + struct.pack('<I', val)
    if sub == V_UINT8:
        return out + struct.pack('<IQ', V_UINT8, len(val)) + val
    if sub == V_STRING:
        return out + struct.pack('<IQ', V_STRING, len(val)) + b''.join(s(x) for x in val)
    fmt = {V_INT32: '<i4', V_UINT64: '<u8', V_INT64: '<i8'}[sub]
    return out + struct.pack('<IQ', sub, len(val)) + np.asarray(val, fmt).tobytes()


def pad(n):
    return (n + ALIGN - 1) // ALIGN * ALIGN


def main(argv):
    precision, name = 'q8_0', None
    while argv and argv[0].startswith('--'):
        opt = argv.pop(0)
        if opt == '--type' and argv:
            precision = argv.pop(0)
        elif opt == '--name' and argv:
            name = argv.pop(0)
        else:
            sys.exit(__doc__.strip())
    if len(argv) < 4 or precision not in ('q8_0', 'bf16'):
        sys.exit(__doc__.strip())
    spec_path, src, out = Path(argv[0]), Path(argv[1]), Path(argv[2])
    spec = json.loads(spec_path.read_text(encoding='utf-8'))
    if spec.get('family') != 'kokoro_tts' or spec.get('schema_version') != 1:
        sys.exit(f'not a Kokoro schema-v1 model spec: {spec_path}')
    pths = sorted(src.glob('*.pth'))
    if len(pths) != 1:
        sys.exit(f'{src}: need exactly one .pth')
    config_bytes = (src / 'config.json').read_bytes()
    config = json.loads(config_bytes.decode('utf-8'))
    vocab = config.get('vocab')
    if not isinstance(vocab, dict):
        sys.exit('config.json is missing the vocab object')

    voices, files = {}, {}
    for arg in argv[3:]:
        vname, _, vfile = arg.partition('=')
        if not vname or not vfile or not vname.replace('_', '').isalnum():
            sys.exit(f'bad voice {arg!r}')
        t = load_pth(Path(vfile))
        if not isinstance(t, Tensor):
            sys.exit(f'{vfile}: voice file is not a tensor')
        a = t.array.astype(np.float32)
        if a.ndim == 3 and a.shape[1] == 1:
            a = a[:, 0, :]
        if a.ndim != 2:
            sys.exit(f'{vfile}: voice tensor must be rank 2 or [rows,1,cols]')
        a = np.ascontiguousarray(a, dtype='<f4')
        files['voices/' + vname + '.bin'] = a.tobytes()
        voices[vname] = {'rows': int(a.shape[0]), 'cols': int(a.shape[1]), 'path': vname + '.bin'}
    voices = dict(sorted(voices.items()))
    files['config.json'] = config_bytes
    files['voices.json'] = json.dumps(voices, ensure_ascii=False, separators=(',', ':')).encode()
    files['vocab.tsv'] = ''.join(f'{k}\t{int(v)}\n' for k, v in
                                 sorted(vocab.items(), key=lambda i: int(i[1]))).encode()

    weights = {}
    flatten('', load_pth(pths[0]), weights)
    names = sorted(weights)

    meta = [kv('general.architecture', V_STRING, 'kokoro_tts'),
            kv('general.name', V_STRING, name or 'Kokoro v1.0 multilingual ' + precision),
            kv('kokoro.languages', V_ARRAY, LANGS, V_STRING),
            kv('kokoro.voices', V_ARRAY, sorted(voices), V_STRING),
            kv('audiocpp.model_spec.version', V_UINT32, 1),
            kv('audiocpp.model_spec.family', V_STRING, 'kokoro_tts'),
            kv('audiocpp.model_spec.json', V_STRING,
               json.dumps(spec, ensure_ascii=False, separators=(',', ':')))]
    fnames, offsets, data = [], [0], bytearray()
    for f in sorted(files):
        fnames.append(f)
        data += files[f]
        offsets.append(len(data))
    meta += [kv('audiocpp.embedded_files.names', V_ARRAY, fnames, V_STRING),
             kv('audiocpp.embedded_files.offsets', V_ARRAY, offsets, V_UINT64),
             kv('audiocpp.embedded_files.data', V_ARRAY, bytes(data), V_UINT8),
             kv('kokoro.tensor_names', V_ARRAY, names, V_STRING),
             kv('audiocpp.tensor_names', V_ARRAY, ['kokoro/' + n for n in names], V_STRING)]
    ranks, shapes, infos, blobs = [], [], [], []
    offset = 0
    for i, n in enumerate(names):
        a = np.ascontiguousarray(weights[n], dtype=np.float32)
        ranks.append(a.ndim)
        shapes.extend(int(d) for d in a.shape)
        meta.append(kv('kokoro.tensor_shape.' + n, V_ARRAY, [int(d) for d in a.shape], V_INT64))
        kind = T_F32
        if a.ndim >= 2 and all(d > 1 for d in a.shape):
            kind = T_BF16
            if precision == 'q8_0' and a.ndim == 2 and a.shape[-1] % 32 == 0:
                kind = T_Q8_0
        blob = (a.astype('<f4').reshape(-1).view(np.uint8) if kind == T_F32
                else bf16(a) if kind == T_BF16 else q8_0(a))
        dims = a.shape[::-1]
        infos.append(s('kokoro.' + str(i)) + struct.pack('<I', len(dims))
                     + b''.join(struct.pack('<Q', d) for d in dims)
                     + struct.pack('<IQ', kind, offset))
        offset += pad(blob.nbytes)
        blobs.append(blob)
    meta += [kv('audiocpp.tensor_ranks', V_ARRAY, ranks, V_INT32),
             kv('audiocpp.tensor_shapes', V_ARRAY, shapes, V_INT64)]

    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + '.partial')
    with open(tmp, 'wb') as f:
        f.write(struct.pack('<IIQQ', 0x46554747, 3, len(names), len(meta)))
        for m in meta:
            f.write(m)
        for t in infos:
            f.write(t)
        f.write(b'\0' * (pad(f.tell()) - f.tell()))
        for b in blobs:
            b.tofile(f)
            f.write(b'\0' * (pad(b.nbytes) - b.nbytes))
    tmp.replace(out)
    print(json.dumps({'path': str(out), 'bytes': out.stat().st_size, 'tensors': len(names),
                      'voices': sorted(voices)}))


if __name__ == '__main__':
    main(sys.argv[1:])
