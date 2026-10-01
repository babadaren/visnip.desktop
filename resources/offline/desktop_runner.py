"""One-shot desktop IPC runner. No public listener, provider fallback or automatic model fetch."""
from __future__ import annotations

import argparse
import base64
import ctypes
import json
import ipaddress
import os
from pathlib import Path
import sys
import time

_JOB = None


def contain_process_tree():
    """Kill model child processes when the desktop cancels or the runner crashes."""
    global _JOB
    if os.name != 'nt':
        return
    from ctypes import wintypes as w
    class IO(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in ('ReadOps', 'WriteOps', 'OtherOps', 'ReadBytes', 'WriteBytes', 'OtherBytes')]
    class BASIC(ctypes.Structure):
        _fields_ = [('ProcessTime', ctypes.c_longlong), ('JobTime', ctypes.c_longlong), ('Flags', w.DWORD),
                    ('MinWS', ctypes.c_size_t), ('MaxWS', ctypes.c_size_t), ('ActiveProcesses', w.DWORD),
                    ('Affinity', ctypes.c_size_t), ('Priority', w.DWORD), ('Scheduling', w.DWORD)]
    class EXTENDED(ctypes.Structure):
        _fields_ = [('Basic', BASIC), ('Io', IO), ('ProcessMemory', ctypes.c_size_t), ('JobMemory', ctypes.c_size_t),
                    ('PeakProcess', ctypes.c_size_t), ('PeakJob', ctypes.c_size_t)]
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.CreateJobObjectW.restype = w.HANDLE
    k.CreateJobObjectW.argtypes = [ctypes.c_void_p, w.LPCWSTR]
    k.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD]
    k.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
    k.GetCurrentProcess.restype = w.HANDLE
    handle = k.CreateJobObjectW(None, None)
    limits = EXTENDED(); limits.Basic.Flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not handle or not k.SetInformationJobObject(handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
        raise RuntimeError('process_isolation_failed')
    if not k.AssignProcessToJobObject(handle, k.GetCurrentProcess()):
        k.CloseHandle(handle)
        raise RuntimeError('process_isolation_failed')
    # Keep handle until OS tears down this process. Do not close it mid-inference.
    _JOB = handle


def progress(stage):
    print(json.dumps({'stage': stage}), flush=True)


def restrict_python_network():
    def audit(event, args):
        if event == 'socket.connect':
            address = args[1]
            if not isinstance(address, tuple):
                raise RuntimeError('offline_network_boundary')
            try:
                allowed = ipaddress.ip_address(address[0]).is_loopback
            except ValueError:
                allowed = False
            if not allowed:
                raise RuntimeError('offline_network_boundary')
    sys.addaudithook(audit)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', required=True)
    parser.add_argument('--input', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--target', choices=['zh-Hans', 'en'], default='zh-Hans')
    parser.add_argument('--quality', choices=['basic', 'precise'], default='basic')
    args = parser.parse_args()
    pipeline = None
    started = time.monotonic()
    try:
        contain_process_tree()
        restrict_python_network()
        os.environ.update(HF_HUB_OFFLINE='1', TRANSFORMERS_OFFLINE='1', NO_PROXY='127.0.0.1,localhost')
        from vislate_server.config import Settings
        from vislate_server.engine import decode_image
        from vislate_server.schemas import ImageRequest
        from .runtime import LocalPipeline
        source, destination = Path(args.input), Path(args.output)
        if destination.exists() or source.stat().st_size > 8 * 1024 * 1024:
            raise ValueError('invalid_input_or_existing_output')
        request = ImageRequest(image_base64=base64.b64encode(source.read_bytes()).decode(),
                               target_lang=args.target, quality=args.quality)
        _, image = decode_image(request.image_base64, Settings())
        progress('loading_local_models')
        pipeline = LocalPipeline(args.config, progress=progress)
        progress('local_image_translation')
        result = pipeline.process(request, image)
        result['elapsed_ms'] = int((time.monotonic() - started) * 1000)
        with destination.open('x', encoding='utf-8') as stream:
            json.dump(result, stream, ensure_ascii=False)
        progress('finished')
        return 0
    except Exception as exc:
        code = getattr(exc, 'code', type(exc).__name__)
        # Error names are useful for diagnostics, but no image/text/token is printed.
        print(json.dumps({'error': str(code)[:80]}), flush=True)
        return 1
    finally:
        if pipeline is not None:
            pipeline.close()


if __name__ == '__main__':
    raise SystemExit(main())
