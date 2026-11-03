# MediaPipe Selfie Segmentation

Google, Apache License 2.0 (see LICENSE and the model card:
https://storage.googleapis.com/mediapipe-assets/Model%20Card%20MediaPipe%20Selfie%20Segmentation.pdf).

| File | Source | SHA-256 |
|---|---|---|
| selfie_segmenter_landscape.tflite | https://storage.googleapis.com/mediapipe-models/image_segmenter/selfie_segmenter_landscape/float16/latest/selfie_segmenter_landscape.tflite | 490e9ea734313e0de10fa0cd9e3c6133e36ea4db2b7a49bde9ef019f72796b8e |
| selfie_segmenter.tflite | https://storage.googleapis.com/mediapipe-models/image_segmenter/selfie_segmenter/float16/latest/selfie_segmenter.tflite | 191ac9529ae506ee0beefa6b2c945a172dab9d07d1e802a290a4e4038226658b |

Downloaded 2026-11-01. The `.ysm` files are converted from these by
`tools/convert_selfie.py` (weights dequantized to float32, unchanged otherwise):

    python3 tools/convert_selfie.py vendor/selfie/selfie_segmenter_landscape.tflite vendor/selfie/selfie_landscape.ysm --norm unit
    python3 tools/convert_selfie.py vendor/selfie/selfie_segmenter.tflite vendor/selfie/selfie_square.ysm --norm unit
