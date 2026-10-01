"""Local PP-OCR ONNX detection + CTC recognition. No remote model resolver or downloads.

Supports horizontal screenshot text. PaddleOCR-compatible normalization and CTC,
with conservative axis-aligned boxes. Rotated/vertical text is not advertised.
"""
from __future__ import annotations
import math
from pathlib import Path
import cv2
import numpy as np
import onnxruntime as ort
from PIL import Image
from .layout import Unit


class OnnxOcrAdapter:
    def __init__(self, assets: dict, threads: int):
        options = ort.SessionOptions()
        options.log_severity_level = 3
        options.intra_op_num_threads = threads
        options.inter_op_num_threads = 1
        options.enable_cpu_mem_arena = False
        self.det = ort.InferenceSession(assets['ocr_det'], sess_options=options, providers=['CPUExecutionProvider'])
        self.rec = ort.InferenceSession(assets['ocr_rec'], sess_options=options, providers=['CPUExecutionProvider'])
        self.det_input = self.det.get_inputs()[0].name
        self.rec_input = self.rec.get_inputs()[0].name
        self.characters = Path(assets['ocr_dictionary']).read_text(encoding='utf-8-sig').splitlines()

    def detect(self, rgb):
        height, width = rgb.shape[:2]
        scale = min(1., 1280 / max(height, width))
        resized_h = max(32, round(height * scale / 32) * 32)
        resized_w = max(32, round(width * scale / 32) * 32)
        # The models bundled by Vislate use BGR channel order.
        bgr = cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)
        image = cv2.resize(bgr, (resized_w, resized_h)).astype('float32') / 255.
        image = (image - np.array([.485, .456, .406], dtype='float32')) / np.array([.229, .224, .225], dtype='float32')
        pred = self.det.run(None, {self.det_input: image.transpose(2, 0, 1)[None]})[0].squeeze()
        bitmap = (pred > .3).astype('uint8') * 255
        contours, _ = cv2.findContours(bitmap, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
        boxes = []
        for contour in contours[:1000]:
            if len(contour) < 3 or cv2.contourArea(contour) < 5:
                continue
            x, y, w, h = cv2.boundingRect(contour)
            local = np.zeros((h, w), dtype='uint8')
            cv2.fillPoly(local, [contour - np.array([[[x, y]]])], 1)
            score = cv2.mean(pred[y:y+h, x:x+w], local)[0]
            if score < .5:
                continue
            rect = cv2.minAreaRect(contour)
            corners = cv2.boxPoints(rect)
            # DB unclip approximation by expanding the rotated bounding rectangle.
            rw, rh = rect[1]
            delta = (rw * rh) * 1.5 / max(2 * (rw + rh), 1)
            expanded = cv2.boxPoints((rect[0], (rw + 2*delta, rh + 2*delta), rect[2]))
            lo, hi = expanded.min(axis=0), expanded.max(axis=0)
            left, right = max(0, int(lo[0] * width / resized_w)), min(width, int(math.ceil(hi[0] * width / resized_w)))
            top, bottom = max(0, int(lo[1] * height / resized_h)), min(height, int(math.ceil(hi[1] * height / resized_h)))
            if right-left < 3 or bottom-top < 5 or bottom-top > (right-left)*2:
                continue
            boxes.append((left, top, right, bottom))
        return sorted(boxes, key=lambda b: (b[1], b[0]))

    def read(self, crop, *, with_alignment=False):
        h, w = crop.shape[:2]
        target_w = min(2048, max(32, int(math.ceil(w * 48 / max(h, 1)))))
        resized = cv2.resize(cv2.cvtColor(crop, cv2.COLOR_RGB2BGR), (target_w, 48)).astype('float32')
        padded = np.zeros((3, 48, max(320, target_w)), dtype='float32')
        padded[:, :, :target_w] = ((resized / 255. - .5) / .5).transpose(2, 0, 1)
        pred = self.rec.run(None, {self.rec_input: padded[None]})[0][0]
        alphabet = [''] + self.characters
        if pred.shape[-1] == len(alphabet) + 1:
            alphabet.append(' ')
        if pred.shape[-1] != len(alphabet):
            raise ValueError('ocr_dictionary_model_mismatch')
        ids = pred.argmax(axis=1); scores = pred.max(axis=1)
        from .unified_elements import decode_ctc_anchors
        text, confidence, aligned = decode_ctc_anchors(ids, scores, alphabet,
            original_width=w, resized_width=target_w, padded_width=padded.shape[2])
        return (text, confidence, aligned) if with_alignment else (text, confidence)

    def recognize_aligned_line(self, image: Image.Image):
        return self.read(np.asarray(image.convert('RGB')), with_alignment=True)

    def recognize_line(self, image: Image.Image) -> tuple[str, float]:
        """Re-read one bounded label crop using the already loaded recognizer."""
        return self.read(np.asarray(image.convert('RGB')))

    def recognize(self, image: Image.Image) -> list[Unit]:
        rgb = np.asarray(image.convert('RGB'))
        units = []
        for index, box in enumerate(self.detect(rgb)):
            left, top, right, bottom = box
            text, confidence, aligned = self.read(rgb[top:bottom, left:right], with_alignment=True)
            if text and confidence >= .65:
                units.append(Unit(f'u{index}', text, [box], confidence,
                    alignment=[(ch,left+a,left+b) for ch,a,b in aligned]))
        if not units or len(units) > 256:
            raise ValueError('ocr_empty_or_too_many_units')
        return units
