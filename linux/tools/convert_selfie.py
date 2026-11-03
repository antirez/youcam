#!/usr/bin/env python3
"""Converts MediaPipe Selfie Segmentation (.tflite) to YouCam's .ysm, and
checks a numpy reference implementation of the model against a golden mask.

Developer tool: pip install tflite numpy opencv-python
  convert_selfie.py MODEL.tflite OUT.ysm --norm unit|signed [--invert]
  convert_selfie.py MODEL.tflite --check IMAGE.png MASK.png
The .ysm format is described in segment.c."""
import struct
import sys

import numpy as np
import tflite
from tflite.BuiltinOperator import BuiltinOperator as B

OPS = {'CONV': 1, 'DWCONV': 2, 'RELU': 3, 'HSWISH': 4, 'SIGMOID': 5, 'MEAN': 6,
       'MUL': 7, 'ADD': 8, 'RESIZE': 9, 'TCONV': 10}
NONE = 0xFFFFFFFF
NORMS = {'unit': (1 / 255, 0.0), 'signed': (2 / 255, -1.0)}
SAME, VALID = 0, 1   # tflite Padding enum
DTYPES = {tflite.TensorType.FLOAT16: np.float16, tflite.TensorType.FLOAT32: np.float32,
          tflite.TensorType.INT32: np.int32}


class Model:
    """The single subgraph, its operators as (name, inputs, output, op), and constants."""

    def __init__(self, path):
        self.m = tflite.Model.GetRootAsModel(open(path, 'rb').read(), 0)
        self.g = self.m.Subgraphs(0)
        self.consts = {}
        self.ops = []
        for i in range(self.g.OperatorsLength()):
            op = self.g.Operators(i)
            code = self.m.OperatorCodes(op.OpcodeIndex())
            b = max(code.BuiltinCode(), code.DeprecatedBuiltinCode())
            ins = [op.Inputs(j) for j in range(op.InputsLength())]
            out = op.Outputs(0)
            if b == B.DEQUANTIZE:
                self.consts[out] = self.raw(ins[0]).astype(np.float32)
                continue
            name = 'CUSTOM' if b == B.CUSTOM else next(k for k, v in B.__dict__.items() if v == b)
            self.ops.append((name, ins, out, op))
        self.input, self.output = self.g.Inputs(0), self.g.Outputs(0)

    def raw(self, t):
        tensor = self.g.Tensors(t)
        data = self.m.Buffers(tensor.Buffer()).DataAsNumpy()
        assert not isinstance(data, int), f'tensor {t} is not constant'
        return np.frombuffer(data.tobytes(), DTYPES[tensor.Type()]).reshape(tensor.ShapeAsNumpy())

    def const(self, t):
        return self.consts[t] if t in self.consts else self.raw(t).astype(np.float32)

    def shape(self, t):
        s = list(self.g.Tensors(t).ShapeAsNumpy())
        assert len(s) == 4 and s[0] == 1, f'tensor {t} shape {s}'
        return s[1:]


def options(op, cls):
    o = cls()
    t = op.BuiltinOptions()
    o.Init(t.Bytes, t.Pos)
    return o


def conv_params(op, cls, in_hw, k_hw, out_hw):
    """Stride and explicit top/left padding, checking what the C engine supports."""
    o = options(op, cls)
    assert o.FusedActivationFunction() == 0, 'fused activation'
    assert o.StrideH() == o.StrideW(), 'non-square stride'
    if cls is tflite.Conv2DOptions:
        assert o.DilationHFactor() == 1 and o.DilationWFactor() == 1, 'dilation'
    else:
        assert o.DepthMultiplier() == 1 and o.DilationHFactor() == 1, 'depth multiplier / dilation'
    s = o.StrideH()
    pads = [0, 0]
    if o.Padding() == SAME:
        pads = [max((out - 1) * s + k - size, 0) // 2 for size, k, out in zip(in_hw, k_hw, out_hw)]
    return s, pads


def tconv_params(op):
    padding, stride_w, stride_h = struct.unpack('<3i', bytes(op.CustomOptionsAsNumpy()))
    assert padding == VALID and stride_w == stride_h, 'transposed convolution options'
    return stride_h


# ---------------------------------------------------------------- converter

def convert(model, out_path, norm, invert):
    ids = {model.input: 0}
    def tid(t):
        return ids.setdefault(t, len(ids))
    layers = []
    for name, ins, out, op in model.ops:
        x = ins[0]
        hw_in, hw_out = model.shape(x)[:2], model.shape(out)[:2]
        rec = dict(op=None, in0=tid(x), in1=NONE, kh=0, kw=0, stride=0, pad=(0, 0), w=np.zeros(0), b=np.zeros(0))
        if name == 'CONV_2D':
            w = model.const(ins[1])                      # [O, KH, KW, I]
            s, pad = conv_params(op, tflite.Conv2DOptions, hw_in, w.shape[1:3], hw_out)
            rec.update(op='CONV', kh=w.shape[1], kw=w.shape[2], stride=s, pad=pad,
                       w=w.transpose(1, 2, 3, 0), b=model.const(ins[2]))
        elif name == 'DEPTHWISE_CONV_2D':
            w = model.const(ins[1])                      # [1, KH, KW, C]
            s, pad = conv_params(op, tflite.DepthwiseConv2DOptions, hw_in, w.shape[1:3], hw_out)
            rec.update(op='DWCONV', kh=w.shape[1], kw=w.shape[2], stride=s, pad=pad, w=w[0], b=model.const(ins[2]))
        elif name == 'CUSTOM':
            w = model.const(ins[1])                      # [O, KH, KW, I]
            s = tconv_params(op)
            assert w.shape[1] == w.shape[2] == s, 'transposed convolution kernel must equal its stride'
            rec.update(op='TCONV', kh=s, kw=s, stride=s, w=w.transpose(1, 2, 3, 0), b=model.const(ins[2]))
        elif name in ('RELU', 'HARD_SWISH', 'LOGISTIC'):
            rec['op'] = {'RELU': 'RELU', 'HARD_SWISH': 'HSWISH', 'LOGISTIC': 'SIGMOID'}[name]
        elif name in ('MEAN', 'AVERAGE_POOL_2D'):
            if name == 'AVERAGE_POOL_2D':
                o = options(op, tflite.Pool2DOptions)
                assert [o.FilterHeight(), o.FilterWidth()] == hw_in and o.FusedActivationFunction() == 0, 'not global'
            else:
                assert sorted(model.raw(ins[1]).tolist()) == [1, 2], 'mean axes'
            rec['op'] = 'MEAN'
        elif name in ('MUL', 'ADD'):
            o = options(op, tflite.MulOptions if name == 'MUL' else tflite.AddOptions)
            assert o.FusedActivationFunction() == 0, 'fused activation'
            rec.update(op=name, in1=tid(ins[1]))
        elif name == 'RESIZE_BILINEAR':
            o = options(op, tflite.ResizeBilinearOptions)
            assert not o.AlignCorners() and o.HalfPixelCenters(), 'resize mode'
            rec['op'] = 'RESIZE'
        else:
            raise SystemExit(f'unsupported operator {name}')
        rec['out'] = tid(out)
        layers.append(rec)
    assert model.output in ids
    in_h, in_w, _ = model.shape(model.input)
    scale, offset = NORMS[norm]
    blob = bytearray(struct.pack('<4s2I2f5I', b'YSM1', in_h, in_w, scale, offset, int(invert),
                                 len(ids), len(layers), ids[model.input], ids[model.output]))
    for t, _ in sorted(ids.items(), key=lambda kv: kv[1]):
        blob += struct.pack('<3I', *model.shape(t))
    for r in layers:
        w = np.ascontiguousarray(r['w'], np.float32).ravel()
        b = np.ascontiguousarray(r['b'], np.float32).ravel()
        blob += struct.pack('<11I', OPS[r['op']], r['in0'], r['in1'], r['out'], r['kh'], r['kw'],
                            r['stride'], r['pad'][0], r['pad'][1], w.size, b.size)
        blob += w.astype('<f4').tobytes() + b.astype('<f4').tobytes()
    open(out_path, 'wb').write(blob)
    print(f'{out_path}: {len(ids)} tensors, {len(layers)} layers, {len(blob)} bytes')


# ------------------------------------------------- numpy reference (tflite semantics)

def run_reference(model, rgb, norm, invert):
    scale, offset = NORMS[norm]
    v = {model.input: (rgb.astype(np.float32) * np.float32(scale) + np.float32(offset))[None]}
    for name, ins, out, op in model.ops:
        x = v[ins[0]]
        oh, ow = model.shape(out)[:2]
        if name in ('CONV_2D', 'DEPTHWISE_CONV_2D'):
            w, b = model.const(ins[1]), model.const(ins[2])
            cls = tflite.Conv2DOptions if name == 'CONV_2D' else tflite.DepthwiseConv2DOptions
            s, (pt, pl) = conv_params(op, cls, x.shape[1:3], w.shape[1:3], (oh, ow))
            kh, kw = w.shape[1:3]
            xp = np.pad(x, ((0, 0), (pt, kh + s * oh), (pl, kw + s * ow), (0, 0)))
            y = np.zeros((1, oh, ow, b.size), np.float32) + b
            for ky in range(kh):
                for kx in range(kw):
                    patch = xp[:, ky:ky + s * oh:s, kx:kx + s * ow:s, :]
                    if name == 'CONV_2D':
                        y += np.einsum('nhwi,oi->nhwo', patch, w[:, ky, kx, :])
                    else:
                        y += patch * w[0, ky, kx, :]
        elif name == 'CUSTOM':
            w, b = model.const(ins[1]), model.const(ins[2])
            s = tconv_params(op)
            y = np.zeros((1, oh, ow, b.size), np.float32) + b
            for ky in range(s):
                for kx in range(s):
                    y[:, ky::s, kx::s, :] += np.einsum('nhwi,oi->nhwo', x, w[:, ky, kx, :])
        elif name == 'RELU':
            y = np.maximum(x, 0)
        elif name == 'HARD_SWISH':
            y = x * np.clip(x + 3, 0, 6) / 6
        elif name == 'LOGISTIC':
            y = 1 / (1 + np.exp(-np.clip(x, -80, 80)))   # clip: no overflow warning
        elif name in ('MEAN', 'AVERAGE_POOL_2D'):
            y = x.mean(axis=(1, 2), keepdims=True)
        elif name == 'MUL':
            y = x * v[ins[1]]
        elif name == 'ADD':
            y = x + v[ins[1]]
        elif name == 'RESIZE_BILINEAR':
            def axis(n_out, n_in):
                f = np.maximum((np.arange(n_out) + 0.5) * (n_in / n_out) - 0.5, 0)
                i0 = np.minimum(f.astype(int), n_in - 1)
                return i0, np.minimum(i0 + 1, n_in - 1), (f - i0).astype(np.float32)
            y0, y1, dy = axis(oh, x.shape[1])
            x0, x1, dx = axis(ow, x.shape[2])
            dx = dx[None, None, :, None]
            top = x[:, y0][:, :, x0] * (1 - dx) + x[:, y0][:, :, x1] * dx
            bottom = x[:, y1][:, :, x0] * (1 - dx) + x[:, y1][:, :, x1] * dx
            y = top + (bottom - top) * dy[None, :, None, None]
        else:
            raise SystemExit(f'unsupported operator {name}')
        v[out] = y.astype(np.float32)
    mask = v[model.output][0, :, :, 0]
    return 1 - mask if invert else mask


def check(model, image_path, mask_path):
    import cv2
    rgb = cv2.cvtColor(cv2.imread(image_path), cv2.COLOR_BGR2RGB)
    golden = cv2.imread(mask_path, cv2.IMREAD_GRAYSCALE).astype(np.float32) / 255
    for norm in NORMS:
        for invert in (False, True):
            m = run_reference(model, rgb, norm, invert)
            a, g = m >= 0.5, golden >= 0.5
            iou = (a & g).sum() / max((a | g).sum(), 1)
            print(f'norm={norm:6} invert={int(invert)} max_diff={np.abs(m - golden).max():.4f} iou={iou:.4f}')


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) == 4 and a[1] == '--check':
        check(Model(a[0]), a[2], a[3])
    elif len(a) in (4, 5) and a[2] == '--norm' and a[3] in NORMS and a[4:] in ([], ['--invert']):
        convert(Model(a[0]), a[1], a[3], a[4:] == ['--invert'])
    else:
        sys.exit(__doc__)
