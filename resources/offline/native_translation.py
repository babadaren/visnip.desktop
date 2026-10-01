"""Pinned llama.cpp executable used only on loopback, with local weights and no downloads."""
from __future__ import annotations

import contextlib
import json
import hashlib
from collections import Counter, OrderedDict
import os
from pathlib import Path
import secrets
import socket
import re
import subprocess
import tempfile
import time
import threading

import httpx

from .layout import Unit, protected_token_counts, validate_translations


def clean_output(value: str, source: str, target: str) -> str:
    """Remove wrapper quotes the model added and join lines of a one-line source."""
    value, original = value.strip(), source.strip()
    for opening, closing in (('"', '"'), ('“', '”'), ("'", "'"), ('「', '」')):
        if (len(value) >= 2 and value.startswith(opening) and value.endswith(closing)
                and not original.startswith(opening)):
            value = value[len(opening):-len(closing)].strip()
    if '\n' not in original:
        value = ('' if target == 'zh-Hans' else ' ').join(line.strip() for line in value.splitlines() if line.strip())
    return value


class NativeHyMtAdapter:
    def __init__(self, executable: str, model: str, threads: int = 4, gpu_layers: int = 0):
        self.process = None
        self.client = None
        self.offloaded_layers = 0
        self.collect_startup = True
        executable, model = Path(executable).resolve(), Path(model).resolve()
        if not executable.is_file() or not model.is_file():
            raise ValueError('translation_runtime_missing')
        token = secrets.token_urlsafe(32)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        # Do not inherit proxy, model download or remote provider environment variables.
        env = {key: value for key, value in os.environ.items()
               if key.upper() in ('SYSTEMROOT', 'WINDIR', 'TEMP', 'TMP', 'USERPROFILE', 'LOCALAPPDATA')}
        env.update(PATH=str(executable.parent) + os.pathsep + os.path.join(env.get('SYSTEMROOT', 'C:\\Windows'), 'System32'),
                   HF_HUB_OFFLINE='1', TRANSFORMERS_OFFLINE='1', NO_PROXY='127.0.0.1,localhost')
        if os.name != 'nt':
            env['PATH'] = str(executable.parent) + ':/usr/local/bin:/usr/bin:/bin'
            env['LD_LIBRARY_PATH'] = str(executable.parent) + ':' + os.environ.get('LD_LIBRARY_PATH', '')
            for key in ('CUDA_VISIBLE_DEVICES', 'NVIDIA_VISIBLE_DEVICES', 'CUDA_MODULE_LOADING'):
                if key in os.environ: env[key] = os.environ[key]
        args = [str(executable), '--model', str(model), '--host', '127.0.0.1', '--port', str(port),
                '--ctx-size', '4096', '--parallel', '1', '--threads', str(threads),
                '--threads-batch', str(threads), '--gpu-layers', str(gpu_layers), '--jinja', '--offline',
                '--no-webui', '--poll', '0', '--api-key', token,
                '--log-verbosity', '4' if gpu_layers else '3']
        if gpu_layers > 0:
            # Interactive translation does not need a 2048-token prefill buffer.
            # Chunking preserves the complete 4096-token context and model weights.
            args.extend(['--batch-size', '512', '--ubatch-size', '128'])
        self.device_name = 'CPU'
        if gpu_layers > 0:
            try:
                devices=subprocess.run([str(executable),'--list-devices'],capture_output=True,timeout=20,env=env,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
            except (OSError,subprocess.SubprocessError) as exc:
                self.close()
                raise RuntimeError('gpu_device_query_failed') from exc
            text=(devices.stdout+devices.stderr).decode('utf-8',errors='replace')
            choices=re.findall(r'^\s*((?:Vulkan|CUDA)\d+):\s*([^\n]+)',text,re.M)
            selected=next((item for item in choices if 'NVIDIA' in item[1]),None)
            if selected:
                args.extend(['--device',selected[0]])
                self.device_name=selected[1].split('(')[0].strip()[:80]
        self.client = httpx.Client(base_url=f'http://127.0.0.1:{port}', trust_env=False,
                                   follow_redirects=False, timeout=300,
                                   headers={'Authorization': 'Bearer ' + token})
        try:
            self.process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            cwd=executable.parent, env=env,
                                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            def drain_runtime_output():
                # Consume without saving prompts, response text, tokens or keys.
                # Only startup layer-count telemetry is retained in memory.
                while True:
                    line=self.process.stdout.readline(65536)
                    if not line:break
                    if self.collect_startup:
                        match=re.search(rb'offloaded\s+(\d+)/(\d+)\s+layers',line)
                        if match:self.offloaded_layers=int(match.group(1))
            self.log_reader=threading.Thread(target=drain_runtime_output,name='local-translation-telemetry',daemon=True)
            self.log_reader.start()
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise RuntimeError('local_translator_start_failed')
                try:
                    if self.client.get('/health', timeout=1).status_code == 200:
                        break
                except httpx.HTTPError:
                    pass
                time.sleep(.15)
            else:
                raise RuntimeError('local_translator_start_timeout')
        except Exception:
            self.close()
            raise
        self.collect_startup=False
        if gpu_layers>0 and self.offloaded_layers==0:
            self.close()
            raise RuntimeError('gpu_not_actually_offloaded')
        self.warmup_ms=0
        if gpu_layers>0:
            # Vulkan may compile batch-size-specific kernels on the first prompt.
            # Exercise a representative batch before exposing the engine as ready.
            started=time.perf_counter()
            try:
                response=self.client.post('/completion',json={'prompt':'Local translation readiness test. '*64,
                    'n_predict':8,'temperature':0,'stream':False,'cache_prompt':False})
                response.raise_for_status()
            except httpx.HTTPError as exc:
                self.close()
                raise RuntimeError('gpu_warmup_failed') from exc
            self.warmup_ms=round((time.perf_counter()-started)*1000)

    def _request_translation(self, unit: Unit, prompt: str, *, target: str, retry=False) -> str:
        """Retain timing/counts even when inference or later validation fails."""
        started = time.perf_counter()
        trace = {'retry': int(retry), 'regions': 1, 'source_chars': len(unit.text)}
        calls = self.last_stats.setdefault('calls', [])
        if len(calls) < 32:
            calls.append(trace)
        try:
            result = self._request_translation_inner(unit, prompt, target=target, retry=retry, trace=trace)
            trace['outcome'] = 'response_received'
            return result
        except Exception as exc:
            code = str(exc)
            if isinstance(exc, httpx.TimeoutException):
                code = 'translation_timeout'
            elif isinstance(exc, httpx.HTTPError):
                code = 'translation_transport_error'
            trace['outcome'] = code if code in {
                'translation_truncated', 'translation_context_limit', 'translation_id_mismatch',
                'translation_timeout', 'translation_transport_error'
            } else 'translation_response_error'
            raise
        finally:
            trace['wall_ms'] = round((time.perf_counter()-started)*1000)

    def _request_translation_inner(self, unit: Unit, prompt: str, *, target, retry, trace) -> str:
        """One local inference call; neither prompts nor responses are logged."""
        if len(prompt) > 2400:
            # Only long prompts can approach the context limit. Short UI labels
            # skip the extra round trip; the shared page prefix stays cached.
            tokenizing = time.perf_counter()
            encoded = self.client.post('/tokenize', json={'content': prompt}, timeout=15)
            encoded.raise_for_status()
            trace['tokenize_ms'] = round((time.perf_counter()-tokenizing)*1000)
            if len(encoded.json().get('tokens', [])) > 2600:
                raise ValueError('translation_context_limit')
        self.last_stats['requests'] += 1
        infer_started = time.perf_counter()
        try:
            response = self.client.post('/v1/chat/completions', json={
                'messages': [{'role': 'user', 'content': prompt}], 'temperature': 0,
                'max_tokens': min(1400, 2*len(unit.text)+48), 'stream': False, 'cache_prompt': True,
            }, timeout=12 if retry else 120)
        finally:
            trace['inference_wall_ms'] = round((time.perf_counter()-infer_started)*1000)
        response.raise_for_status()
        answer = response.json()
        usage, timings = answer.get('usage', {}), answer.get('timings', {})
        self.last_stats['prompt_tokens'] += int(usage.get('prompt_tokens', 0))
        self.last_stats['completion_tokens'] += int(usage.get('completion_tokens', 0))
        self.last_stats['prompt_ms'] += float(timings.get('prompt_ms', 0))
        self.last_stats['generation_ms'] += float(timings.get('predicted_ms', 0))
        trace.update(prompt_tokens=int(usage.get('prompt_tokens', 0)),
                     completion_tokens=int(usage.get('completion_tokens', 0)),
                     prompt_ms=float(timings.get('prompt_ms', 0)),
                     generation_ms=float(timings.get('predicted_ms', 0)))
        choice = answer['choices'][0]
        if choice.get('finish_reason') != 'stop':
            raise ValueError('translation_truncated')
        content = choice['message'].get('content')
        if not isinstance(content, str):
            raise ValueError('invalid_translation')
        return clean_output(content, unit.text, target)

    def _record_validation_failure(self, unit: Unit, value, reason: str, *, retry=False):
        """Record counts, never the numbers/placeholders or screenshot text."""
        original = protected_token_counts(unit.text)
        returned = protected_token_counts(value) if isinstance(value, str) else Counter()
        missing, added = original-returned, returned-original
        numeric = lambda token: token[0].isdigit()
        row = {'id': unit.identifier, 'reason': reason, 'retry': int(retry),
               'source_chars': len(unit.text), 'target_chars': len(value) if isinstance(value, str) else 0,
               'missing_numbers': sum(n for t,n in missing.items() if numeric(t)),
               'added_numbers': sum(n for t,n in added.items() if numeric(t)),
               'missing_placeholders': sum(n for t,n in missing.items() if not numeric(t)),
               'added_placeholders': sum(n for t,n in added.items() if not numeric(t))}
        failures = self.last_stats.setdefault('validation_failures', [])
        if len(failures) < 128:
            failures.append(row)

    def _attempt(self, unit: Unit, prompt: str, target: str, *, retry=False):
        """Return (value, None) for a validated translation, else (value, reason)."""
        from .translation_policy import validate_language
        key = hashlib.sha256((target + '\0' + prompt).encode('utf-8')).digest()
        if key in self._cache:
            self.last_stats['cache_hits'] += 1
            self._cache.move_to_end(key)
            return self._cache[key], None
        try:
            value = self._request_translation(unit, prompt, target=target, retry=retry)
        except ValueError as exc:
            if str(exc) not in {'translation_truncated', 'translation_context_limit', 'invalid_translation'}:
                raise
            return None, str(exc)
        try:
            if len(value) > 4*len(unit.text)+40:
                raise ValueError('invalid_translation')  # runaway generation or echoed context
            validate_language([unit], validate_translations([unit], {unit.identifier: value}), target)
        except ValueError as exc:
            reason = str(exc)
            if reason not in {'sentence_not_translated', 'protected_token_mismatch', 'invalid_translation'}:
                raise
            return value, reason
        self._cache[key] = value
        if len(self._cache) > 512:
            self._cache.popitem(last=False)
        return value, None

    def translate(self, units: list[Unit], target: str) -> dict[str, str]:
        """Translate each region separately; a shared page prefix supplies context.

        One region per request makes key/value misalignment impossible. The
        page context and glossary form a common prompt prefix, so llama.cpp
        reuses its KV cache and only the short per-region suffix is prefilled.
        """
        if not hasattr(self, '_cache'):
            self._cache = OrderedDict()
        self.last_stats = {'cache_hits': 0, 'requests': 0, 'prompt_tokens': 0, 'completion_tokens': 0,
                           'prompt_ms': 0.0, 'generation_ms': 0.0,
                           'retry_requests': 0, 'retried_regions': 0, 'recovered_regions': 0, 'kept_names': 0,
                           'untranslated_regions': [], 'validation_failures': [], 'calls': []}
        self.unresolved_units = {}
        self.last_stats['offloaded_layers']=getattr(self,'offloaded_layers',0)
        self.last_stats['device']=getattr(self,'device_name','CPU')
        self.last_stats['startup_warmup_ms']=getattr(self,'warmup_ms',0)
        from .translation_policy import preserve_token, glossary_terms, kept_name, TranslationValidationError
        from .ui_structure import already_target
        target_name = {'zh-Hans': '中文', 'en': '英语'}.get(target)
        if target_name is None:
            raise ValueError('unsupported_local_language')
        result = {u.identifier:u.text for u in units
                  if u.role == 'identity' or preserve_token(u.text) or already_target(u.text,target)}
        self.last_stats['preserved_tokens'] = len(result)
        # Hy-MT's contextual and terminology templates. The page context is a
        # shared prefix (KV cache reuse); terms apply to the region they occur in.
        lines, size = [], 0
        for unit in units:
            if unit.role == 'identity':
                continue
            if size + len(unit.text[:160]) > 1200:
                break
            lines.append(unit.text[:160])
            size += len(unit.text[:160]) + 1
        context = '以下是软件界面截图中的文字：\n' + '\n'.join(lines) + '\n'
        def prompt_for(unit):
            terms = glossary_terms(unit, units, target)
            glossary = ('参考下面的翻译：\n' + '\n'.join(f'{s} 翻译成 {t}' for s, t in terms) + '\n') if terms else ''
            return (context + glossary + '参考上面的信息，把下面的文本翻译成' + target_name
                    + '，注意不需要翻译上文，也不要额外解释：\n' + unit.text)
        failed = []
        for unit in units:
            if unit.identifier in result:
                continue
            value, reason = self._attempt(unit, prompt_for(unit), target)
            if reason is None:
                result[unit.identifier] = value
            else:
                self._record_validation_failure(unit, value, reason)
                failed.append((unit, value, reason))
        for unit, first, reason in failed:
            # At most one bounded retry per region, without the page context,
            # so a context-induced echo or refusal is not simply repeated.
            self.last_stats['retry_requests'] += 1
            self.last_stats['retried_regions'] += 1
            retry_prompt = ('把下面的文本翻译成' + target_name + '，不要额外解释。'
                            '保留原文中的数字、占位符、网址和代码名称。\n\n' + unit.text)
            try:
                value, retry_reason = self._attempt(unit, retry_prompt, target, retry=True)
            except (ValueError, httpx.HTTPError) as exc:
                value, retry_reason = None, str(exc) if str(exc) in {
                    'translation_truncated', 'translation_context_limit', 'invalid_translation'
                } else reason
            if retry_reason is None:
                result[unit.identifier] = value
                self.last_stats['recovered_regions'] += 1
                # Serve the next identical request for the original prompt too.
                self._cache[hashlib.sha256((target + '\0' + prompt_for(unit)).encode('utf-8')).digest()] = value
                continue
            if (reason == retry_reason == 'sentence_not_translated' and kept_name(unit.text)
                    and (first or '').strip() == (value or '').strip() == unit.text.strip()):
                # Two independent prompts both kept a short capitalised label:
                # a product or proper name (e.g. Copilot). Unchanged pixels are correct.
                result[unit.identifier] = unit.text
                self.last_stats['kept_names'] += 1
                continue
            # Original pixels will remain untouched. This is marked as
            # incomplete later, never cached or labelled equivalent.
            result[unit.identifier] = unit.text
            self.unresolved_units[unit.identifier] = retry_reason
            self._record_validation_failure(unit, value, retry_reason, retry=True)
        self.last_stats['untranslated_regions'] = [
            {'id': identifier, 'reason': reason} for identifier, reason in self.unresolved_units.items()
        ]
        if self.unresolved_units and not any(result[u.identifier].strip() != u.text.strip() for u in units):
            identifier, reason = next(iter(self.unresolved_units.items()))
            error = TranslationValidationError(reason, identifier)
            error.region_outcomes = list(self.last_stats['untranslated_regions'])
            raise error
        return validate_translations(units, result)

    def close(self):
        if hasattr(self, '_cache'):
            self._cache.clear()
        if self.process is not None:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
            if hasattr(self,'log_reader'):self.log_reader.join(timeout=2)
            if self.process.stdout:self.process.stdout.close()
            self.process = None
        if self.client is not None:
            self.client.close()
            self.client = None
