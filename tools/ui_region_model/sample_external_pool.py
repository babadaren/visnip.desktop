from __future__ import annotations

import argparse
import hashlib
import json
import random
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit, urlunsplit

from .fetch_external_screenshots import _download, _safe_stem


DEFAULT_SEED = 20260729
DEFAULT_PER_CATEGORY = 4
THUMBNAIL_WIDTH = 1024
STANDARD_THUMBNAIL_WIDTHS = (1024, 800, 640, 320)
RATE_LIMIT_RETRY_SECONDS = 90


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _thumbnail_url(origin_url: str, width: int, source_width: int) -> str:
    thumbnail_width = next(
        (
            candidate
            for candidate in STANDARD_THUMBNAIL_WIDTHS
            if candidate <= width and candidate < source_width
        ),
        None,
    )
    if thumbnail_width is None:
        return origin_url
    parsed = urlsplit(origin_url)
    marker = "/wikipedia/commons/"
    if marker not in parsed.path:
        raise ValueError(f"Unsupported Wikimedia origin URL: {origin_url}")
    prefix, relative_path = parsed.path.split(marker, 1)
    filename = relative_path.rsplit("/", 1)[-1]
    thumb_path = (
        f"{prefix}{marker}thumb/{relative_path}/{thumbnail_width}px-{filename}"
    )
    return urlunsplit((parsed.scheme, parsed.netloc, thumb_path, "", ""))


def _load_pool(path: Path) -> list[dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    records = payload.get("records") if isinstance(payload, dict) else None
    if not isinstance(records, list) or not records:
        raise ValueError("Candidate pool records must be a non-empty array")
    required = {
        "category",
        "title",
        "width",
        "height",
        "mimeType",
        "originUrl",
        "descriptionUrl",
        "author",
        "licenseName",
        "licenseUrl",
        "query",
    }
    validated: list[dict[str, Any]] = []
    for index, record in enumerate(records):
        if not isinstance(record, dict):
            raise ValueError(f"Candidate {index} must be an object")
        missing = sorted(name for name in required if not record.get(name))
        if missing:
            raise ValueError(f"Candidate {index} misses: {', '.join(missing)}")
        if record["mimeType"] not in ("image/png", "image/jpeg"):
            raise ValueError(f"Candidate {index} has unsupported MIME type")
        validated.append(record)
    return validated


def _write_json(path: Path, value: object) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def sample_pool(
    pool_path: Path, output_root: Path, seed: int, per_category: int
) -> None:
    pool_sha = _sha256_file(pool_path)
    records = _load_pool(pool_path)
    output_root.mkdir(parents=True, exist_ok=True)
    images_root = output_root / "downloaded"
    images_root.mkdir(exist_ok=True)
    checkpoint_path = output_root / "selection.partial.json"
    retrieved_at = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
    checkpoint: dict[str, Any] = {
        "formatVersion": 1,
        "poolPath": str(pool_path.resolve()),
        "poolSha256": pool_sha,
        "seed": seed,
        "perCategory": per_category,
        "retrievedAt": retrieved_at,
        "selected": [],
        "failures": [],
    }
    if checkpoint_path.is_file():
        existing = json.loads(checkpoint_path.read_text(encoding="utf-8"))
        expected = (pool_sha, seed, per_category)
        actual = (
            existing.get("poolSha256"),
            existing.get("seed"),
            existing.get("perCategory"),
        )
        if actual != expected:
            raise ValueError("Existing checkpoint belongs to a different sampling run")
        checkpoint = existing

    selected = checkpoint["selected"]
    failures = checkpoint["failures"]
    selected_titles = {item["title"] for item in selected}
    normalized_hashes = {item["normalizedSha256"] for item in selected}
    categories = sorted({str(record["category"]) for record in records})
    random_source = random.Random(seed)
    queues: dict[str, list[dict[str, Any]]] = {}
    for category in categories:
        queue = sorted(
            (record for record in records if record["category"] == category),
            key=lambda item: str(item["title"]),
        )
        random_source.shuffle(queue)
        queues[category] = queue

    for category in categories:
        category_selected = sum(
            1 for item in selected if item["category"] == category
        )
        for candidate in queues[category]:
            if category_selected >= per_category:
                break
            if candidate["title"] in selected_titles:
                continue
            ordinal = category_selected + 1
            value = dict(candidate)
            value["downloadUrl"] = _thumbnail_url(
                str(value["originUrl"]), THUMBNAIL_WIDTH, int(value["width"])
            )
            output_path = images_root / (
                _safe_stem(category, ordinal, str(value["title"])) + ".png"
            )
            last_error: BaseException | None = None
            for attempt in range(2):
                try:
                    downloaded = _download(value, output_path)
                    break
                except (OSError, RuntimeError, ValueError) as error:
                    last_error = error
                    output_path.unlink(missing_ok=True)
                    rate_limited = "too many requests" in str(error).casefold()
                    if attempt == 0 and rate_limited:
                        print(
                            f"Rate limited for {ascii(value['title'])}; retrying in "
                            f"{RATE_LIMIT_RETRY_SECONDS}s"
                        )
                        time.sleep(RATE_LIMIT_RETRY_SECONDS)
                        continue
                    break
            else:
                raise AssertionError("unreachable download retry state")
            if not output_path.is_file():
                failures.append({"title": value["title"], "error": str(last_error)})
                _write_json(checkpoint_path, checkpoint)
                continue
            if downloaded["normalizedSha256"] in normalized_hashes:
                output_path.unlink(missing_ok=True)
                failures.append(
                    {"title": value["title"], "error": "normalized image duplicate"}
                )
                _write_json(checkpoint_path, checkpoint)
                continue
            downloaded["retrievedAt"] = checkpoint["retrievedAt"]
            for failure in failures:
                if failure.get("title") == downloaded["title"]:
                    failure["resolvedAt"] = datetime.now(timezone.utc).isoformat().replace(
                        "+00:00", "Z"
                    )
            selected.append(downloaded)
            selected_titles.add(downloaded["title"])
            normalized_hashes.add(downloaded["normalizedSha256"])
            category_selected += 1
            _write_json(checkpoint_path, checkpoint)
        if category_selected != per_category:
            raise RuntimeError(
                f"Only downloaded {category_selected}/{per_category} for {category}"
            )

    final_path = output_root / "selection.json"
    _write_json(final_path, checkpoint)
    print(
        json.dumps(
            {
                "candidateCount": len(records),
                "selectedCount": len(selected),
                "categoryCount": len(categories),
                "failures": len(failures),
                "output": str(output_root.resolve()),
            },
            ensure_ascii=False,
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Select and download a reproducible stratified screenshot pool."
    )
    parser.add_argument("--pool", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--per-category", type=int, default=DEFAULT_PER_CATEGORY)
    arguments = parser.parse_args()
    sample_pool(
        arguments.pool.resolve(),
        arguments.output.resolve(),
        arguments.seed,
        arguments.per_category,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
