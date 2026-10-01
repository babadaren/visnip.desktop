from __future__ import annotations

import argparse
import hashlib
import html
import json
import os
import random
import re
import subprocess
import sys
import time
import urllib.parse
from datetime import datetime, timezone
from html.parser import HTMLParser
from pathlib import Path
from typing import Any

from PIL import Image, ImageOps


COMMONS_API = "https://commons.wikimedia.org/w/api.php"
USER_AGENT = "VisnipDatasetBuilder/1.0 (UI-region research dataset)"
DEFAULT_SEED = 20260729
DEFAULT_PER_CATEGORY = 4
MAX_SIDE = 1920
MIN_WIDTH = 640
MIN_HEIGHT = 360
MAX_PIXELS = 24_000_000
API_REQUEST_INTERVAL_SECONDS = 4.0
DOWNLOAD_REQUEST_INTERVAL_SECONDS = 15.0
ALLOWED_LICENSE_PREFIXES = (
    "cc0",
    "cc by",
    "public domain",
    "gpl",
    "lgpl",
    "agpl",
    "mit",
    "bsd",
)
LICENSE_URL_FALLBACKS = {
    "cc0": "https://creativecommons.org/publicdomain/zero/1.0/",
    "public domain": "https://creativecommons.org/publicdomain/mark/1.0/",
    "gpl": "https://www.gnu.org/licenses/gpl-3.0.html",
    "gplv2": "https://www.gnu.org/licenses/old-licenses/gpl-2.0.html",
    "gplv3": "https://www.gnu.org/licenses/gpl-3.0.html",
    "lgpl": "https://www.gnu.org/licenses/lgpl-3.0.html",
    "agpl": "https://www.gnu.org/licenses/agpl-3.0.html",
}

POWERSHELL_FETCH_COMMAND = (
    '$ProgressPreference="SilentlyContinue"; '
    'Invoke-WebRequest -Uri $env:VISNIP_FETCH_URL '
    '-OutFile $env:VISNIP_FETCH_OUTPUT -TimeoutSec 120 '
    '-Headers @{"User-Agent"=$env:VISNIP_FETCH_USER_AGENT}'
)

CATEGORY_QUERIES = {
    "application": (
        "open source software screenshot user interface",
        "application settings screenshot",
        "desktop application screenshot interface",
        "mobile application user interface screenshot",
    ),
    "game": (
        "open source video game screenshot",
        "strategy game screenshot GPL",
        "puzzle game screenshot GPL",
        "role-playing game screenshot open source",
    ),
    "document": (
        "LibreOffice Writer screenshot",
        "LibreOffice Calc screenshot",
        "LibreOffice Impress screenshot",
        "PDF viewer screenshot open source",
    ),
    "web": (
        "web browser website screenshot",
        "web application screenshot open source",
        "wiki website screenshot",
        "admin web interface screenshot",
    ),
    "desktop_window": (
        "Linux desktop screenshot",
    ),
}


class _TextExtractor(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.parts: list[str] = []

    def handle_data(self, data: str) -> None:
        self.parts.append(data)


def _fetch_to_file(url: str, output_path: Path) -> None:
    environment = os.environ.copy()
    environment.update(
        {
            "VISNIP_FETCH_URL": url,
            "VISNIP_FETCH_OUTPUT": str(output_path),
            "VISNIP_FETCH_USER_AGENT": USER_AGENT,
        }
    )
    completed = subprocess.run(
        [
            "pwsh.exe",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            POWERSHELL_FETCH_COMMAND,
        ],
        check=False,
        env=environment,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        details = " ".join(completed.stderr.split())
        raise RuntimeError(details[-500:] or "HTTP request failed")


def _plain_text(value: object) -> str:
    if not isinstance(value, str):
        return ""
    parser = _TextExtractor()
    parser.feed(html.unescape(value))
    return " ".join("".join(parser.parts).split())


def _metadata_value(image_info: dict[str, Any], name: str) -> str:
    metadata = image_info.get("extmetadata")
    if not isinstance(metadata, dict):
        return ""
    entry = metadata.get(name)
    return _plain_text(entry.get("value")) if isinstance(entry, dict) else ""


def _license_url(image_info: dict[str, Any], license_name: str) -> str:
    direct = _metadata_value(image_info, "LicenseUrl")
    if direct.startswith("https://"):
        return direct
    normalized = license_name.casefold().strip()
    for prefix, url in LICENSE_URL_FALLBACKS.items():
        if normalized.startswith(prefix):
            return url
    return ""


def _api_query(query: str, limit: int, cache_root: Path) -> list[dict[str, Any]]:
    parameters = {
        "action": "query",
        "generator": "search",
        "gsrsearch": f"filetype:bitmap {query}",
        "gsrnamespace": "6",
        "gsrlimit": str(limit),
        "prop": "imageinfo|info",
        "iiprop": "url|size|mime|extmetadata",
        "iiurlwidth": str(MAX_SIDE),
        "inprop": "url",
        "format": "json",
        "formatversion": "2",
        "origin": "*",
    }
    cache_key = hashlib.sha256(
        json.dumps(parameters, sort_keys=True).encode("utf-8")
    ).hexdigest()
    cache_path = cache_root / f"{cache_key}.json"
    if cache_path.is_file():
        payload = json.loads(cache_path.read_text(encoding="utf-8"))
    else:
        url = f"{COMMONS_API}?{urllib.parse.urlencode(parameters)}"
        temporary = cache_path.with_suffix(".download")
        last_error: BaseException | None = None
        for attempt in range(1, 4):
            try:
                _fetch_to_file(url, temporary)
                payload = json.loads(temporary.read_text(encoding="utf-8"))
                temporary.replace(cache_path)
                time.sleep(API_REQUEST_INTERVAL_SECONDS)
                break
            except (OSError, RuntimeError, json.JSONDecodeError) as error:
                last_error = error
                temporary.unlink(missing_ok=True)
                print(
                    f"Commons query attempt {attempt}/3 failed for {query!r}: {error}",
                    file=sys.stderr,
                )
                if attempt < 3:
                    time.sleep(15 * attempt)
        else:
            raise RuntimeError(
                f"Commons query failed after three attempts: {query!r}"
            ) from last_error
    query_value = payload.get("query")
    pages = query_value.get("pages", []) if isinstance(query_value, dict) else []
    return [page for page in pages if isinstance(page, dict)]


def _candidate(page: dict[str, Any], category: str, query: str) -> dict[str, Any] | None:
    image_infos = page.get("imageinfo")
    if not isinstance(image_infos, list) or not image_infos:
        return None
    image_info = image_infos[0]
    if not isinstance(image_info, dict):
        return None
    mime_type = image_info.get("mime")
    width = image_info.get("width")
    height = image_info.get("height")
    if mime_type not in ("image/png", "image/jpeg"):
        return None
    if not isinstance(width, int) or not isinstance(height, int):
        return None
    if width < MIN_WIDTH or height < MIN_HEIGHT or width * height > MAX_PIXELS:
        return None
    license_name = _metadata_value(image_info, "LicenseShortName")
    if not license_name.casefold().startswith(ALLOWED_LICENSE_PREFIXES):
        return None
    author = _metadata_value(image_info, "Artist") or _metadata_value(
        image_info, "Credit"
    )
    license_url = _license_url(image_info, license_name)
    origin_url = image_info.get("url")
    description_url = page.get("canonicalurl") or page.get("fullurl")
    if not author or not license_url:
        return None
    if not isinstance(origin_url, str) or not origin_url.startswith("https://"):
        return None
    if not isinstance(description_url, str) or not description_url.startswith(
        "https://"
    ):
        return None
    download_url = image_info.get("thumburl") or origin_url
    if not isinstance(download_url, str) or not download_url.startswith("https://"):
        return None
    return {
        "pageId": page.get("pageid"),
        "title": page.get("title"),
        "category": category,
        "query": query,
        "width": width,
        "height": height,
        "mimeType": mime_type,
        "originUrl": origin_url,
        "downloadUrl": download_url,
        "descriptionUrl": description_url,
        "author": author,
        "licenseName": license_name,
        "licenseUrl": license_url,
        "usageTerms": _metadata_value(image_info, "UsageTerms"),
        "attributionRequired": _metadata_value(
            image_info, "AttributionRequired"
        ),
    }


def _safe_stem(category: str, ordinal: int, title: str) -> str:
    base = re.sub(r"[^a-z0-9]+", "-", title.casefold()).strip("-")
    return f"{category}-{ordinal:02d}-{base[:64]}"


def _download(candidate: dict[str, Any], output_path: Path) -> dict[str, Any]:
    temporary = output_path.with_suffix(".download")
    try:
        _fetch_to_file(candidate["downloadUrl"], temporary)
        time.sleep(DOWNLOAD_REQUEST_INTERVAL_SECONDS)
        original_bytes = temporary.read_bytes()
        original_sha = hashlib.sha256(original_bytes).hexdigest()
        with Image.open(temporary) as source:
            if getattr(source, "n_frames", 1) != 1:
                raise ValueError("animated or multi-frame images are not accepted")
            image = ImageOps.exif_transpose(source).convert("RGB")
            if image.width * image.height > MAX_PIXELS:
                raise ValueError("decoded image exceeds the pixel limit")
            if max(image.size) > MAX_SIDE:
                scale = MAX_SIDE / max(image.size)
                image = image.resize(
                    (round(image.width * scale), round(image.height * scale)),
                    Image.Resampling.LANCZOS,
                )
            image.save(output_path, "PNG", optimize=True)
    finally:
        temporary.unlink(missing_ok=True)
    normalized_bytes = output_path.read_bytes()
    with Image.open(output_path) as normalized:
        normalized_size = normalized.size
    return {
        **candidate,
        "originalSha256": original_sha,
        "normalizedSha256": hashlib.sha256(normalized_bytes).hexdigest(),
        "normalizedPath": output_path.name,
        "normalizedWidth": normalized_size[0],
        "normalizedHeight": normalized_size[1],
    }


def fetch(output_root: Path, seed: int, per_category: int, query_limit: int) -> None:
    output_root.mkdir(parents=True, exist_ok=True)
    images_root = output_root / "downloaded"
    images_root.mkdir(exist_ok=True)
    cache_root = output_root / "query-cache"
    cache_root.mkdir(exist_ok=True)
    retrieved_at = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
    random_source = random.Random(seed)
    candidates: list[dict[str, Any]] = []
    by_category_query: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for category, queries in CATEGORY_QUERIES.items():
        for query in queries:
            values: list[dict[str, Any]] = []
            for page in _api_query(query, query_limit, cache_root):
                value = _candidate(page, category, query)
                if value is not None:
                    values.append(value)
            values.sort(key=lambda item: (str(item["title"]), int(item["pageId"])))
            by_category_query[(category, query)] = values
            candidates.extend(values)

    deduplicated_candidates: dict[int, dict[str, Any]] = {}
    for candidate in candidates:
        page_id = candidate.get("pageId")
        if isinstance(page_id, int):
            deduplicated_candidates.setdefault(page_id, candidate)
    candidate_manifest = {
        "formatVersion": 1,
        "retrievedAt": retrieved_at,
        "seed": seed,
        "perCategory": per_category,
        "queryLimit": query_limit,
        "categories": CATEGORY_QUERIES,
        "candidates": sorted(
            deduplicated_candidates.values(),
            key=lambda item: (item["category"], item["query"], item["title"]),
        ),
    }
    (output_root / "candidate_pool.json").write_text(
        json.dumps(candidate_manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    selected: list[dict[str, Any]] = []
    selected_page_ids: set[int] = set()
    normalized_hashes: set[str] = set()
    failures: list[dict[str, str]] = []
    for category, queries in CATEGORY_QUERIES.items():
        queues: list[list[dict[str, Any]]] = []
        for query in queries:
            queue = list(by_category_query[(category, query)])
            random_source.shuffle(queue)
            queues.append(queue)
        category_selected = 0
        while category_selected < per_category and any(queues):
            made_progress = False
            for queue in queues:
                while queue:
                    candidate = queue.pop()
                    page_id = candidate.get("pageId")
                    if not isinstance(page_id, int) or page_id in selected_page_ids:
                        continue
                    ordinal = category_selected + 1
                    output_path = images_root / (
                        _safe_stem(category, ordinal, str(candidate["title"])) + ".png"
                    )
                    try:
                        value = _download(candidate, output_path)
                    except (OSError, ValueError, subprocess.CalledProcessError) as error:
                        output_path.unlink(missing_ok=True)
                        failures.append(
                            {"title": str(candidate["title"]), "error": str(error)}
                        )
                        continue
                    if value["normalizedSha256"] in normalized_hashes:
                        output_path.unlink(missing_ok=True)
                        continue
                    value["retrievedAt"] = retrieved_at
                    selected.append(value)
                    selected_page_ids.add(page_id)
                    normalized_hashes.add(value["normalizedSha256"])
                    category_selected += 1
                    made_progress = True
                    break
                if category_selected >= per_category:
                    break
            if not made_progress:
                break
        if category_selected != per_category:
            raise RuntimeError(
                f"Only downloaded {category_selected}/{per_category} images for {category}"
            )

    selection_manifest = {
        "formatVersion": 1,
        "retrievedAt": retrieved_at,
        "seed": seed,
        "perCategory": per_category,
        "selected": selected,
        "failures": failures,
    }
    (output_root / "selection.json").write_text(
        json.dumps(selection_manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            {
                "candidateCount": len(deduplicated_candidates),
                "selectedCount": len(selected),
                "failures": len(failures),
                "output": str(output_root.resolve()),
            },
            ensure_ascii=False,
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fetch a reproducible, licensed screenshot sample from Wikimedia Commons."
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--per-category", type=int, default=DEFAULT_PER_CATEGORY)
    parser.add_argument("--query-limit", type=int, default=20)
    arguments = parser.parse_args()
    fetch(
        arguments.output.resolve(),
        arguments.seed,
        arguments.per_category,
        arguments.query_limit,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
