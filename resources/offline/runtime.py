from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import time
from io import BytesIO
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

from vislate_server.errors import ServiceError
from vislate_server.schemas import ImageRequest
from .unified_composition import compose
from .unified_elements import normalize_elements, adopt_refinement, group_elements


def read_engine_configuration(filename: str | Path) -> tuple[dict, dict[str, str]]:
    config_file = Path(filename).resolve()
    config = json.loads(config_file.read_text(encoding="utf-8"))
    if config.get("schema_version") != 1 or config.get("quality") not in ("basic", "precise"):
        raise ValueError("unsupported_engine_configuration")
    required = ["ocr_det", "ocr_rec", "ocr_dictionary", "translator"]
    if config.get('ocr_backend') != 'onnx_direct':
        required.append('ocr_cls')
    if config.get("translator_backend") == "llama_cpp_native":
        required.append("translator_executable")
    if config["quality"] == "precise":
        required += ["hisam", "sam_backbone", "lama_jit"]
    assets = {}
    for name in required:
        record = config.get("assets", {}).get(name, {})
        expected = record.get("sha256", "")
        if not re.fullmatch(r"[a-f0-9]{64}", expected):
            raise ValueError(f"checksum_not_configured:{name}")
        relative = record.get("path")
        if not isinstance(relative, str) or not relative:
            raise ValueError(f"asset_path_missing:{name}")
        path = (config_file.parent / relative).resolve()
        if not path.is_file():
            raise ValueError(f"asset_missing:{name}")
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
        if digest.hexdigest() != expected:
            raise ValueError(f"asset_checksum_mismatch:{name}")
        assets[name] = str(path)
    config["fonts"] = [str((config_file.parent / p).resolve()) for p in config.get("fonts", [])]
    if config.get("use_windows_fonts") and os.name == 'nt':
        font_root = Path(os.environ.get('WINDIR', r'C:\Windows')) / 'Fonts'
        config['fonts'] += [str(font_root / name) for name in ('msyh.ttc', 'msyhbd.ttc', 'arial.ttf', 'arialbd.ttf', 'segoeui.ttf', 'segoeuib.ttf', 'simsun.ttc', 'simhei.ttf')
                           if (font_root / name).is_file()]
    if not config["fonts"] or not all(Path(p).is_file() for p in config["fonts"]):
        raise ValueError("local_fonts_missing")
    if config["quality"] == "precise":
        source = (config_file.parent / config.get("hisam_source_directory", "missing")).resolve()
        if (source / "pretrained_checkpoint" / "sam_vit_b_01ec64.pth").resolve() != Path(assets["sam_backbone"]):
            raise ValueError("hisam_backbone_layout_mismatch")
        config["hisam_source_directory"] = str(source)
    return config, assets


class LocalPipeline:
    """Experimental complete local pipeline; publish a package only after real-model acceptance tests."""

    def __init__(self, filename: str | Path, progress=None, vision_options: dict | None = None):
        loading_started = time.perf_counter()
        self.stage = 'initialization'
        report_progress = progress or (lambda _stage: None)
        def record_stage(stage):
            self.stage = stage
            report_progress(stage)
        self.progress = record_stage
        self.progress('loading_local_models')
        os.environ["HF_HUB_OFFLINE"] = "1"
        os.environ["TRANSFORMERS_OFFLINE"] = "1"
        self.config, assets = read_engine_configuration(filename)
        if vision_options:
            allowed_options = {'device', 'vision_precision', 'vision_attention_rows', 'vision_strategy', 'vision_prewarm'}
            if set(vision_options) - allowed_options:
                raise ValueError('unsupported_vision_option')
            self.config.update(vision_options)
        threads = max(1, min(8, int(self.config.get("threads", 2))))
        # Allocate physical cores, not all logical SMT threads. Retain spare CPU
        # capacity for the screenshot UI and other applications.
        try:
            import psutil
            physical = psutil.cpu_count(logical=False) or 4
        except ImportError:
            physical = min(4, os.cpu_count() or 1)
        vision_threads = max(1, min(6, physical - 2 if physical > 4 else physical))
        self.quality = self.config["quality"]
        device = self.config.get("device", "cpu")
        if device not in ("cpu", "cuda"):
            raise ValueError("unsupported_device")
        from fontTools.ttLib import TTFont
        self.font_coverage = {}
        for filename in self.config["fonts"]:
            with TTFont(filename, fontNumber=0, lazy=True) as font:
                self.font_coverage[filename] = set(font.getBestCmap() or {})
        from .adapters import (BasicMaskAdapter, HiSamAdapter, HyMtAdapter,
                               LamaJitAdapter, RapidOcrAdapter, basic_repair)
        if self.config.get('ocr_backend') == 'onnx_direct':
            from .onnx_ocr import OnnxOcrAdapter
            self.ocr = OnnxOcrAdapter(assets, threads)
        else:
            self.ocr = RapidOcrAdapter(assets, threads)
        if self.quality == "precise":
            if self.config.get('vision_strategy') == 'gpu-full':
                from .full_gpu_vision import FullGpuVision
                self.segmenter = FullGpuVision(self.config, assets, vision_threads)
            else:
                from .adaptive_vision import AdaptiveVision
                self.segmenter = AdaptiveVision(self.config, assets, vision_threads)
            self.inpainter = self.segmenter
            self.repair = self.inpainter.repair
        else:
            self.segmenter = BasicMaskAdapter()
            self.repair = basic_repair

        # Start the native model process only after all other dependencies load.
        # A failed precision module must not strand an orphan translator.
        if self.config.get('translator_backend') == 'llama_cpp_native':
            from .native_translation import NativeHyMtAdapter
            from .gpu_runtime import verified_gpu_runtime
            executable, backend = verified_gpu_runtime()
            if os.name != 'nt' and self.config.get('translator_device') == 'cuda':
                executable, backend = assets['translator_executable'], 'cuda'
            self.backend = 'cpu'
            self.backend_reason = backend
            if executable:
                try:
                    self.translator = NativeHyMtAdapter(executable, assets['translator'], threads, gpu_layers=99)
                    self.backend = backend
                    self.backend_reason = 'available'
                except (RuntimeError, OSError):
                    if self.config.get('require_gpu'):
                        raise
                    self.backend_reason = 'gpu_start_failed_using_local_cpu'
                    self.translator = NativeHyMtAdapter(assets['translator_executable'], assets['translator'], threads)
            else:
                self.translator = NativeHyMtAdapter(assets['translator_executable'], assets['translator'], threads)
        else:
            self.translator = HyMtAdapter(assets["translator"], threads, int(self.config.get("gpu_layers", 0)))
        try:
            if self.config.get('vision_prewarm') and hasattr(self.segmenter, 'warmup'):
                self.progress('warming_vision_gpu')
                self.segmenter.warmup()
                if hasattr(self.segmenter, 'prepare_translation'):
                    self.segmenter.prepare_translation()
        except Exception:
            self.translator.close()
            raise
        self.loading_ms = int((time.perf_counter()-loading_started)*1000)

    def close(self):
        self.translator.close()

    def process(self, request: ImageRequest, image: Image.Image) -> dict:
        if request.quality != self.quality:
            raise ServiceError(409, "quality_unavailable", "工作进程不提供所选质量，未自动降级。")
        if request.target_lang not in ("en", "zh-Hans"):
            raise ServiceError(422, "language_not_qualified", "此初始引擎仅开放中英文验证。")
        started = time.monotonic()
        timings = {'load': self.loading_ms}
        stage_started = time.perf_counter()
        self.progress('ocr')
        from .ui_structure import refine_ui_regions
        raw_units = self.ocr.recognize(image)
        # First trim already verified icon/label boundaries. Otherwise expanded
        # detector boxes can overlap across two genuinely different controls.
        preliminary = refine_ui_regions(image, raw_units,
            recognize_line=getattr(self.ocr, 'recognize_line', None))
        source_elements = adopt_refinement(raw_units, preliminary)
        elements, structure_stats = normalize_elements(image, source_elements,
            reader=getattr(self.ocr, 'recognize_aligned_line', None))
        units = group_elements(elements)
        timings['ocr'] = int((time.perf_counter()-stage_started)*1000)
        stage_started = time.perf_counter()
        self.progress('translation')
        handoff = (self.segmenter.prepare_translation()
                   if hasattr(self.segmenter, 'prepare_translation') else {})
        try:
            translations = self.translator.translate(units, request.target_lang)
        finally:
            statistics = getattr(self.translator, 'last_stats', None)
            if handoff and isinstance(statistics, dict):
                statistics['vision_handoff'] = handoff
        timings['translation'] = int((time.perf_counter()-stage_started)*1000)
        characters = {ord(c) for value in translations.values() for c in value if not c.isspace()}
        fonts = [filename for filename, cmap in self.font_coverage.items() if characters <= cmap]
        # Image-font failure must not discard valid text; affected rows go to
        # the persistent review panel, not a bitmap with missing-glyph squares.
        stage_started = time.perf_counter()
        self.progress('segmentation')
        changed_units = [u for u in units if translations[u.identifier].strip() != u.text.strip()]
        stroke = self.segmenter.segment(image, changed_units)
        # Finalise the erasure mask BEFORE LaMa runs. Completing glyph edges
        # only in compose would paste back pixels LaMa had never been asked to
        # erase, producing stale English fringes under otherwise valid Chinese.
        from .appearance import recover_source_marks, complete_readable_strokes
        for unit in changed_units:
            for text, box in zip(unit.source_lines, unit.boxes):
                local_stroke = recover_source_marks(image.crop(box), stroke.crop(box), text, self.config['fonts'])
                local_stroke = complete_readable_strokes(image.crop(box), local_stroke)
                stroke.paste(local_stroke, box[:2])
        timings['segmentation'] = int((time.perf_counter()-stage_started)*1000)
        allowed = Image.new("L", image.size)
        painter = ImageDraw.Draw(allowed)
        for unit in units:
            for left, top, right, bottom in unit.boxes:
                painter.rectangle((left, top, right - 1, bottom - 1), fill=255)
        stroke = ImageChops.multiply(stroke, allowed)
        protected = Image.new("L", image.size)
        painter = ImageDraw.Draw(protected)
        for region in request.protected_regions:
            if region.x + region.width > image.width or region.y + region.height > image.height:
                raise ServiceError(422, "invalid_region", "保护区域超出图片边界。")
            painter.rectangle((region.x, region.y, region.x + region.width - 1, region.y + region.height - 1), fill=255)
        stroke = ImageChops.subtract(stroke, protected)
        stage_started = time.perf_counter()
        self.progress('composition')
        prepare = getattr(self.segmenter, 'prepare_repair', None)
        def prepare_accepted(source, accepted_mask):
            self.progress('repair_gpu')
            prepare(source, accepted_mask)
            self.progress('composition')
        region_repair = self.inpainter.repair_region if self.quality == 'precise' else None
        composition_metrics = {}
        output, reports, _ = compose(image, units, translations, stroke, self.repair, fonts, protected,
            source_fonts=self.config['fonts'], repair_region=region_repair, readable=True,
            prepare_repair=prepare_accepted if prepare else None, metrics=composition_metrics)
        timings['repair'] = composition_metrics.get('repair', 0)
        unresolved = getattr(self.translator, 'unresolved_units', {})
        for report in reports:
            if report['id'] in unresolved:
                # Source pixels and text are retained after a failed bounded
                # retry. Never disguise this as an intentional equivalent token.
                report.update(status='preserved', reason=unresolved[report['id']],
                              translation='', translation_status='failed', display_mode='unavailable')
        timings['composition'] = int((time.perf_counter()-stage_started)*1000)
        if hasattr(self.segmenter, 'clear_request'):
            self.segmenter.clear_request()
        # The same-size bitmap may be unchanged while valid translations are
        # displayed in the panel. Model failure remains explicitly distinct.
        self.progress('encode')
        stream = BytesIO()
        output.save(stream, format="PNG")
        return {"provider": "vislate-local-v1", "quality": self.quality,
                "source_lang": request.source_lang, "target_lang": request.target_lang,
                "translated_image_mime_type": "image/png",
                "translated_image_base64": base64.b64encode(stream.getvalue()).decode("ascii"),
                "blocks": reports, "elapsed_ms": int((time.monotonic() - started) * 1000),
                "completion": 'partial' if any(r['status'] == 'preserved' and r['reason'] != 'equivalent' for r in reports) else 'complete',
                "timings_ms": timings, "segmentation": getattr(self.segmenter, 'last_stats', {}),
                "structure": structure_stats,
                "presentation": {"inline_regions": composition_metrics.get('inline_regions', 0),
                    "panel_regions": composition_metrics.get('panel_regions', 0),
                    "unavailable_regions": len(unresolved)},
                "translation_stats": getattr(self.translator, 'last_stats', {}),
                "compute_backend": getattr(self,'backend','cpu'),
                "backend_reason": getattr(self,'backend_reason','configured')}
