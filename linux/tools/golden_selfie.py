#!/usr/bin/env python3
"""Golden masks for test_segment.c. Crops a portrait to each model's input
size, saves it, and runs the original model on it with MediaPipe.

Developer tool (Python 3.12): pip install mediapipe opencv-python numpy
Usage: golden_selfie.py PORTRAIT.jpg NAME MODEL_DIR OUT_DIR"""
import sys

import cv2
import mediapipe as mp
import numpy as np
from mediapipe.tasks.python import BaseOptions, vision

CROP_CENTER_Y = 0.4   # Portraits: keep the head and shoulders in the crop.
VARIANTS = {'landscape': ('selfie_segmenter_landscape.tflite', 256, 144),
            'square': ('selfie_segmenter.tflite', 256, 256)}


def crop(img, w, h):
    H, W = img.shape[:2]
    cw, ch = (round(H * w / h), H) if W / H > w / h else (W, round(W * h / w))
    x0 = (W - cw) // 2
    y0 = min(max(round(H * CROP_CENTER_Y - ch / 2), 0), H - ch)
    return cv2.resize(img[y0:y0 + ch, x0:x0 + cw], (w, h), interpolation=cv2.INTER_AREA)


def main(src, name, model_dir, out_dir):
    img = cv2.imread(src)
    if img is None:
        sys.exit(f'cannot read {src}')
    for variant, (model, w, h) in VARIANTS.items():
        small = crop(img, w, h)
        base = f'{out_dir}/{name}-{variant}'
        cv2.imwrite(base + '.png', small)
        rgb = np.ascontiguousarray(cv2.cvtColor(small, cv2.COLOR_BGR2RGB))
        opts = vision.ImageSegmenterOptions(base_options=BaseOptions(model_asset_path=f'{model_dir}/{model}'),
                                            output_confidence_masks=True)
        with vision.ImageSegmenter.create_from_options(opts) as seg:
            res = seg.segment(mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb))
        mask = res.confidence_masks[0].numpy_view().reshape(h, w, -1)[:, :, 0]
        cv2.imwrite(base + '-mask.png', np.round(np.clip(mask, 0, 1) * 255).astype(np.uint8))
        print(base, 'person fraction', round(float((mask >= 0.5).mean()), 3))


if __name__ == '__main__':
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
