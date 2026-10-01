"""Allowlisted, content-free failure telemetry for the local IPC boundary.

Never serialize exception repr/message, frame locals, prompt, screenshot pixels,
provider URLs or paths. Only our fixed validation codes and source locations leave
this process. Unknown errors remain explicitly unknown rather than guessed.
"""
from __future__ import annotations
import json
import math
from pathlib import Path
import re
import traceback

STAGES = frozenset({'initialization', 'loading_local_models', 'warming_vision_gpu',
    'decode', 'ocr', 'translation', 'segmentation', 'repair_gpu', 'composition', 'encode', 'output'})
CONTENT_CODES = frozenset({'protected_token_mismatch', 'sentence_not_translated',
    'translation_id_mismatch', 'invalid_translation', 'translation_truncated',
    'translation_context_limit', 'ocr_empty_or_too_many_units', 'no_safe_layout',
    'font_glyphs_missing', 'invalid_unit_box', 'invalid_image', 'image_too_large',
    'pixel_limit', 'image_format', 'language_not_qualified'})
OTHER_CODES = frozenset({'local_fonts_missing', 'translation_runtime_missing',
    'local_translator_start_failed', 'local_translator_start_timeout',
    'gpu_not_actually_offloaded', 'gpu_warmup_failed', 'gpu_repair_not_prepared',
    'outside_edit_region_changed', 'repair_size_mismatch', 'mask_size_mismatch',
    'invalid_job_id', 'invalid_session_filename', 'invalid_session_path',
    'existing_output', 'invalid_command_size', 'invalid_command', 'invalid_operation',
    'gpu_vision_requires_cuda', 'input_too_large'})


def safe_translation_stats(stats) -> dict:
    """Bounded allowlist for failed jobs; no text, token values or credentials."""
    if not isinstance(stats, dict):
        return {}
    def numbers(row, names):
        return {key: row[key] for key in names if key in row
                and type(row[key]) in (int, float)
                and 0 <= row[key] <= 1e10 and math.isfinite(row[key])}
    result = numbers(stats, ('cache_hits', 'requests', 'prompt_tokens', 'completion_tokens',
        'prompt_ms', 'generation_ms', 'retry_requests', 'retried_regions',
        'recovered_regions', 'offloaded_layers'))
    rows = stats.get('calls', [])
    calls = []
    for row in rows[:32] if isinstance(rows, list) else []:
        if not isinstance(row, dict):
            continue
        call = numbers(row, ('retry', 'regions', 'source_chars', 'tokenize_ms',
            'inference_wall_ms', 'wall_ms', 'prompt_tokens', 'completion_tokens',
            'prompt_ms', 'generation_ms'))
        if isinstance(row.get('outcome'), str) and row['outcome'] in {'response_received', 'translation_truncated',
                'translation_context_limit', 'translation_id_mismatch', 'translation_timeout',
                'translation_transport_error', 'translation_response_error'}:
            call['outcome'] = row['outcome']
        calls.append(call)
    if calls:
        result['calls'] = calls
    rows = stats.get('validation_failures', [])
    failures = []
    for row in rows[:128] if isinstance(rows, list) else []:
        if not isinstance(row, dict):
            continue
        identifier, reason = row.get('id'), row.get('reason')
        if (not isinstance(identifier, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', identifier)
                or not isinstance(reason, str) or reason not in CONTENT_CODES):
            continue
        item = {'id': identifier, 'reason': reason}
        item.update(numbers(row, ('retry', 'source_chars', 'target_chars', 'missing_numbers',
            'added_numbers', 'missing_placeholders', 'added_placeholders')))
        failures.append(item)
    if failures:
        result['validation_failures'] = failures
    return result


def describe_failure(exc: Exception, stage: str) -> dict:
    stage = stage if stage in STAGES else 'initialization'
    code = getattr(exc, 'code', None)
    if code not in CONTENT_CODES | OTHER_CODES:
        candidate = str(exc)
        if candidate in CONTENT_CODES | OTHER_CODES:
            code = candidate
        elif isinstance(exc, json.JSONDecodeError):
            code = 'translation_json_invalid' if stage == 'translation' else 'invalid_json'
        elif type(exc).__name__ == 'OutOfMemoryError':
            code = 'gpu_out_of_memory'
        elif isinstance(exc, MemoryError):
            code = 'memory_exhausted'
        elif isinstance(exc, ValueError):
            code = 'invalid_value'
        elif isinstance(exc, OSError):
            code = 'local_io_error'
        else:
            code = 'engine_internal_error'
    frames = []
    for frame in traceback.extract_tb(exc.__traceback__):
        path = Path(frame.filename)
        if path.parent.name != 'vislate_engine':
            continue
        if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*\.py', path.name):
            continue
        frames.append({'module': path.name, 'line': frame.lineno})
    result = {'code': code, 'stage': stage, 'frames': frames[-6:],
        'recoverable': code in CONTENT_CODES or code == 'translation_json_invalid'}
    identifier = getattr(exc, 'unit_id', '')
    if isinstance(identifier, str) and re.fullmatch(r'[A-Za-z0-9_-]{1,64}', identifier):
        result['unit_id'] = identifier
    outcomes = getattr(exc, 'region_outcomes', None)
    if isinstance(outcomes, list):
        allowed_reasons = CONTENT_CODES | {'equivalent', 'artwork_collision', 'neighbor_collision',
            'no_confident_text_mask', 'foreground_unavailable', 'font_unavailable',
            'protected_region', 'layout_unfit'}
        regions = []
        for row in outcomes[:64]:
            if not isinstance(row, dict):
                continue
            identifier, reason = row.get('id'), row.get('reason')
            if (isinstance(identifier, str) and re.fullmatch(r'[A-Za-z0-9_-]{1,64}', identifier)
                    and isinstance(reason, str) and reason in allowed_reasons):
                regions.append({'id': identifier, 'reason': reason})
        result['regions'] = regions
    return result
