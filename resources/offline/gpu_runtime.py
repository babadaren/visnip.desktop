"""Optional locally installed, hash-checked acceleration; never downloads at inference."""
import hashlib,json,os
from pathlib import Path


def verified_gpu_runtime():
    value=os.environ.get('VISLATE_GPU_RUNTIME','')
    if not value:return None,'not_installed'
    root=Path(value).resolve();manifest=root/'runtime-manifest.json'
    if not manifest.is_file() or manifest.stat().st_size>65536:return None,'manifest_missing'
    try:
        info=json.loads(manifest.read_text(encoding='utf-8'))
        if info['source']!='ggml-org/llama.cpp' or info['release']!='b10964' or info['backend'] not in ('vulkan','cuda'):
            return None,'unsupported_runtime'
        paths=[]
        for record in info['files']:
            path=root/'runtime'/record['path']
            if path.is_symlink() or not path.resolve().is_relative_to((root/'runtime').resolve()) or not path.is_file():return None,'file_missing'
            with path.open('rb') as stream:
                if hashlib.file_digest(stream,'sha256').hexdigest()!=record['sha256']:return None,'checksum_mismatch'
            paths.append(path)
        candidates=[p for p in paths if p.name=='llama-server.exe']
        if len(candidates)!=1:return None,'executable_missing'
        return str(candidates[0]),info['backend']
    except (ValueError,KeyError,OSError,TypeError):return None,'invalid_manifest'
