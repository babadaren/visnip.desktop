"""Resident local worker: one model load, serialized jobs over inherited pipes.

No listening socket is exposed by this worker. Only the native translator uses
an authenticated loopback endpoint. Image/job files are confined to a session
workspace supplied by the desktop, and are removed after each response.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
from pathlib import Path
import re
import sys
import threading
import time

from .desktop_runner import contain_process_tree, restrict_python_network

PROTOCOL = 2
MAX_COMMAND_BYTES = 4096


def safe_job_paths(workspace: Path, command: dict) -> tuple[Path, Path]:
    identifier = command.get('id', '')
    if not isinstance(identifier, str) or not re.fullmatch(r'[a-f0-9]{32}', identifier):
        raise ValueError('invalid_job_id')
    paths = []
    for key, suffix in [('input', '.png'), ('output', '.json')]:
        name = command.get(key)
        if name != identifier + suffix:
            raise ValueError('invalid_session_filename')
        path = workspace / name
        if path.is_symlink() or path.resolve().parent != workspace.resolve():
            raise ValueError('invalid_session_path')
        paths.append(path)
    if paths[1].exists():
        raise ValueError('existing_output')
    return paths[0], paths[1]


def execute_job(pipeline, workspace, command):
    """Complete cleanup before signalling failure; keep reusable models alive."""
    from vislate_server.config import Settings
    from vislate_server.engine import decode_image
    from vislate_server.schemas import ImageRequest
    from .failure import describe_failure, safe_translation_stats
    source = output = None
    outcome = None
    started = time.perf_counter()
    try:
        pipeline.stage = 'decode'
        source, output = safe_job_paths(workspace, command)
        if source.stat().st_size > 8 * 1024 * 1024:
            raise ValueError('input_too_large')
        request = ImageRequest(image_base64=base64.b64encode(source.read_bytes()).decode(),
                               target_lang=command.get('target'), quality='precise')
        _, image = decode_image(request.image_base64, Settings())
        result = pipeline.process(request, image)
        result['timings_ms']['load'] = 0
        result['worker_pid'] = os.getpid()
        result['elapsed_ms'] = int((time.perf_counter()-started)*1000)
        pipeline.stage = 'output'
        with output.open('x', encoding='utf-8') as stream:
            json.dump(result, stream, ensure_ascii=False)
        outcome = {'event': 'done'}
    except Exception as exc:
        outcome = {'event': 'error', **describe_failure(exc, getattr(pipeline, 'stage', 'initialization')),
                   'job_ms': int((time.perf_counter()-started)*1000)}
        # Only current translation failures have fresh per-request statistics.
        # Never attach the previous job's telemetry to a decode/OCR failure.
        if getattr(pipeline, 'stage', '') == 'translation':
            stats = safe_translation_stats(getattr(getattr(pipeline, 'translator', None), 'last_stats', {}))
            if stats:
                outcome['translation_stats'] = stats
        if output is not None:
            output.unlink(missing_ok=True)
    finally:
        segmenter = getattr(pipeline, 'segmenter', None)
        if hasattr(segmenter, 'clear_request'):
            segmenter.clear_request()
        if source is not None:
            source.unlink(missing_ok=True)
    return outcome


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', required=True)
    parser.add_argument('--workspace', required=True)
    parser.add_argument('--gpu-profile')
    args = parser.parse_args()
    workspace = Path(args.workspace).resolve()
    if not workspace.is_dir() or workspace.is_symlink():
        raise SystemExit(2)
    contain_process_tree()
    restrict_python_network()
    os.environ.update(HF_HUB_OFFLINE='1', TRANSFORMERS_OFFLINE='1', NO_PROXY='127.0.0.1,localhost')
    current = {'id': ''}
    def emit(event, **data):
        print(json.dumps({'event': event, 'id': current['id'], **data}, ensure_ascii=True), flush=True)
    # Avoid holding the CRT stdin lock across Windows NumPy/OpenCV DLL imports.
    # Parent death is detected through a HANDLE, not a concurrent stdin reader.
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        parent = kernel.OpenProcess(0x100000, False, os.getppid())
        if not parent:
            raise SystemExit(2)
        def parent_watch():
            kernel.WaitForSingleObject(parent, 0xFFFFFFFF)
            os._exit(0)  # Closing the worker's Job Object also stops its models.
        threading.Thread(target=parent_watch, name='desktop-parent-watch', daemon=True).start()
    pipeline = None
    try:
        from vislate_server.config import Settings
        from vislate_server.engine import decode_image
        from vislate_server.schemas import ImageRequest
        from .runtime import LocalPipeline
        started = time.perf_counter()
        vision_options = None
        if args.gpu_profile:
            from .device_profile import activate_local_profile
            vision_options = activate_local_profile(args.gpu_profile)
        pipeline = LocalPipeline(args.config, progress=lambda stage: emit('stage', stage=stage), vision_options=vision_options)
        emit('ready', protocol=PROTOCOL, quality=pipeline.quality,
             loading_ms=int((time.perf_counter()-started)*1000), pid=os.getpid(),
             backend=getattr(pipeline,'backend','cpu'),backend_reason=getattr(pipeline,'backend_reason','configured'),
             offloaded_layers=getattr(pipeline.translator,'offloaded_layers',0))
        emit('vision_ready', device=pipeline.config.get('device', 'cpu'),
             strategy=pipeline.config.get('vision_strategy', 'adaptive'))
        while True:
            raw = sys.stdin.buffer.readline(MAX_COMMAND_BYTES + 1)
            if not raw:
                break
            if len(raw) > MAX_COMMAND_BYTES or not raw.endswith(b'\n'):
                raise ValueError('invalid_command_size')
            command = json.loads(raw)
            if not isinstance(command, dict):
                raise ValueError('invalid_command')
            if command.get('op') == 'shutdown':
                break
            if command.get('op') != 'translate':
                raise ValueError('invalid_operation')
            current['id'] = command.get('id', '')
            outcome = execute_job(pipeline, workspace, command)
            event = outcome.pop('event')
            emit(event, **outcome)
            current['id'] = ''
    except Exception as exc:
        from .failure import describe_failure
        emit('fatal', **describe_failure(exc, getattr(pipeline, 'stage', 'initialization')))
        return 1
    finally:
        if pipeline is not None:
            pipeline.close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
