"""Explicit operator-only local CUDA validation profile; never downloaded implicitly."""
from __future__ import annotations
import json
from pathlib import Path
import sys


def activate_local_profile(filename: str | Path) -> dict:
    profile = Path(filename).resolve()
    data = json.loads(profile.read_text(encoding='utf-8'))
    if (data.get('schema_version') != 1 or data.get('mode') != 'cuda-validation'
            or data.get('packages') != 'cuda-py313' or sys.version_info[:2] != (3, 13)):
        raise ValueError('invalid_local_gpu_profile')
    packages = profile.parent / data['packages']
    if packages.is_symlink() or packages.resolve().parent != profile.parent:
        raise ValueError('invalid_cuda_package_path')
    for relative in ('torch/version.py', 'torch/lib/torch_cuda.dll', 'torchvision/__init__.py'):
        if not (packages / relative).is_file():
            raise ValueError('local_cuda_packages_missing')
    # Only the designated, locally prepared validation directory is added. No
    # URL, download instruction or arbitrary module name comes from a request.
    sys.path.insert(0, str(packages))
    import torch
    if torch.__version__ != '2.8.0+cu126' or not torch.cuda.is_available():
        raise RuntimeError('local_cuda_runtime_unavailable')
    return {'device': 'cuda', 'vision_precision': 'fp16', 'vision_attention_rows': 4,
            'vision_strategy': 'gpu-full', 'vision_prewarm': True}
