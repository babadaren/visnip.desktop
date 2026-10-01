"""Resident CUDA vision path shared by local validation and the GPU worker.

Real Hi-SAM segmentation and one LaMa pass per image. Only approved pixels
are copied back by the compositor. No CPU substitution when CUDA was requested.
"""
from __future__ import annotations
import time
from PIL import Image, ImageDraw
from .layout import Unit


class FullGpuVision:
    def __init__(self, config: dict, assets: dict, threads: int):
        from .adapters import HiSamAdapter, LamaJitAdapter
        if config.get('device') != 'cuda':
            raise ValueError('gpu_vision_requires_cuda')
        self.hisam = HiSamAdapter(config['hisam_source_directory'], assets['hisam'], 'cuda', threads,
                                  precision=config.get('vision_precision', 'fp16'),
                                  attention_rows=int(config.get('vision_attention_rows', 4)))
        self.lama = LamaJitAdapter(assets['lama_jit'], 'cuda')
        self.last_stats = {}
        self.fixed = None
        self.fixed_size = None

    def warmup(self):
        # Warm the encoder and TorchScript optimisation before readiness. This
        # does not cache any user screenshot or claim future sizes are warm.
        image = Image.new('RGB', (768, 320), 'white')
        draw = ImageDraw.Draw(image)
        draw.text((32, 32), 'Model readiness test', fill='black')
        units = [Unit('warm', 'Model readiness test', [(25, 25, 200, 60)])]
        mask = self.hisam.segment(image, units)
        for _ in range(3):
            self.lama.repair(image, mask)
        self.hisam._tile_cache.clear()
        self.hisam.predictor.reset_image()

    def segment(self, image, units):
        import torch
        self.fixed = None
        if not units:
            self.last_stats = {'vision_device': 'cuda', 'vision_strategy': 'gpu-full',
                               'heavy_regions': 0, 'tiles': 0, 'cache_hits': 0, 'heavy_ms': 0}
            return Image.new('L', image.size)
        torch.cuda.synchronize()
        start = time.perf_counter()
        result = self.hisam.segment(image, units)
        torch.cuda.synchronize()
        self.last_stats = {**self.hisam.last_stats, 'vision_device': 'cuda', 'vision_strategy': 'gpu-full',
                           'heavy_ms': round((time.perf_counter()-start)*1000),
                           'heavy_regions': len(units), 'simple_regions': 0,
                           'cuda_allocated_mib': round(torch.cuda.memory_allocated()/2**20, 1)}
        return result

    def prepare_translation(self):
        """Return unused CUDA scratch to the other resident (Vulkan) process.

        Weights stay on the GPU. Small consumer cards cannot afford two separate
        allocators both retaining their peak workspaces between stages.
        """
        import torch
        start = time.perf_counter()
        self.hisam.predictor.reset_image()
        torch.cuda.synchronize()
        allocated = torch.cuda.memory_allocated()
        before = torch.cuda.memory_reserved()
        capacity = torch.cuda.get_device_properties(torch.cuda.current_device()).total_memory
        released = capacity <= 6 * 2**30 and before - allocated >= 128 * 2**20
        if released:
            torch.cuda.empty_cache()
        after = torch.cuda.memory_reserved()
        free, total = torch.cuda.mem_get_info()
        self.translation_handoff = {
            'allocated_mib': round(allocated / 2**20, 1),
            'reserved_before_mib': round(before / 2**20, 1),
            'reserved_after_mib': round(after / 2**20, 1),
            'released_mib': round(max(0, before-after) / 2**20, 1),
            'device_free_mib': round(free / 2**20, 1),
            'device_total_mib': round(total / 2**20, 1),
            'wall_ms': round((time.perf_counter()-start) * 1000),
        }
        return self.translation_handoff

    def prepare_repair(self, source, mask):
        import torch
        self.fixed = None
        self.fixed_size = source.size
        if mask.getbbox() is None:
            self.last_stats.update(lama_ms=0, lama_repairs=0, lama_device='cuda')
            return
        torch.cuda.synchronize()
        start = time.perf_counter()
        self.fixed = self.lama.repair(source, mask)
        torch.cuda.synchronize()
        self.last_stats.update(lama_ms=round((time.perf_counter()-start)*1000), lama_repairs=1,
                               lama_device='cuda', cuda_reserved_mib=round(torch.cuda.memory_reserved()/2**20, 1))

    def repair_region(self, source, box, mask):
        if self.fixed is None or source.size != self.fixed_size:
            raise RuntimeError('gpu_repair_not_prepared')
        return self.fixed.crop(box)

    def repair(self, source, mask):
        return self.lama.repair(source, mask)

    def clear_request(self):
        self.fixed = None
