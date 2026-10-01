from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path
from typing import Any

from PIL import Image

from .fetch_external_screenshots import _safe_stem


CATEGORY_MAP = {
    "application": "application_ui",
    "game": "open_source_game",
    "document": "document_editor",
}


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"Expected an object in {path}")
    return value


def _normalized_record(
    candidate: dict[str, Any], image_path: Path, retrieved_at: str
) -> dict[str, Any]:
    with Image.open(image_path) as image:
        width, height = image.size
        image_format = image.format
    if image_format != "PNG":
        raise ValueError(f"Normalized screenshot is not PNG: {image_path}")
    normalized_sha = _sha256_file(image_path)
    category = CATEGORY_MAP.get(
        str(candidate["category"]), str(candidate["category"])
    )
    if "wikimedia conference" in str(candidate["title"]).casefold():
        category = "web_browser"
    return {
        **candidate,
        "category": category,
        "normalizedPath": image_path.name,
        "normalizedSha256": normalized_sha,
        "normalizedWidth": width,
        "normalizedHeight": height,
        "retrievedAt": retrieved_at,
        "lineage": "normalized screenshot retained; network-response SHA-256 was not checkpointed",
    }


def _expected_normalized_size(candidate: dict[str, Any]) -> tuple[int, int]:
    width = int(candidate["width"])
    height = int(candidate["height"])
    longest_side = max(width, height)
    if longest_side <= 1920:
        return width, height
    return round(width * 1920 / longest_side), round(height * 1920 / longest_side)


def consolidate(
    search_root: Path,
    curated_root: Path,
    output_root: Path,
) -> None:
    candidate_manifest = _read_json(search_root / "candidate_pool.json")
    candidates = candidate_manifest.get("candidates")
    if not isinstance(candidates, list):
        raise ValueError("Search candidate pool is malformed")
    retrieved_at = str(candidate_manifest.get("retrievedAt"))
    curated_manifest = _read_json(curated_root / "selection.partial.json")
    curated_selected = curated_manifest.get("selected")
    if not isinstance(curated_selected, list):
        raise ValueError("Curated selection checkpoint is malformed")

    output_images = output_root / "downloaded"
    output_images.mkdir(parents=True, exist_ok=True)
    final_records: list[dict[str, Any]] = []
    seen_hashes: set[str] = set()
    for record in curated_selected:
        source_path = curated_root / "downloaded" / record["normalizedPath"]
        if not source_path.is_file():
            raise ValueError(f"Curated image is missing: {source_path}")
        if _sha256_file(source_path) != record["normalizedSha256"]:
            raise ValueError(f"Curated image hash changed: {source_path}")
        destination = output_images / source_path.name
        shutil.copyfile(source_path, destination)
        value = dict(record)
        value["normalizedPath"] = destination.name
        value["lineage"] = "network-response and normalized SHA-256 checkpointed"
        final_records.append(value)
        seen_hashes.add(value["normalizedSha256"])

    search_images = sorted((search_root / "downloaded").glob("*.png"))
    for image_path in search_images:
        with Image.open(image_path) as image:
            normalized_size = image.size
        matches = [
            candidate
            for candidate in candidates
            if _safe_stem(
                str(candidate["category"]),
                int(image_path.stem.split("-", 2)[1]),
                str(candidate["title"]),
            )
            == image_path.stem
        ]
        if len(matches) > 1:
            matches = [
                candidate
                for candidate in matches
                if _expected_normalized_size(candidate) == normalized_size
            ]
        if len(matches) != 1:
            raise ValueError(
                f"Expected one candidate match for {image_path.name}, found {len(matches)}"
            )
        value = _normalized_record(matches[0], image_path, retrieved_at)
        if value["normalizedSha256"] in seen_hashes:
            continue
        destination = output_images / image_path.name
        shutil.copyfile(image_path, destination)
        value["normalizedPath"] = destination.name
        final_records.append(value)
        seen_hashes.add(value["normalizedSha256"])

    final_records.sort(key=lambda item: (str(item["category"]), str(item["title"])))
    manifest = {
        "formatVersion": 1,
        "seed": 20260729,
        "samplingMethod": "fixed-seed stratified random draw from two audited Commons candidate pools",
        "samplingNotes": (
            "The final set contains every unique successful draw available before Commons "
            "rate limiting. Failed or incomplete downloads are not represented as successes."
        ),
        "candidatePools": [
            {
                "path": str((search_root / "candidate_pool.json").resolve()),
                "sha256": _sha256_file(search_root / "candidate_pool.json"),
            },
            {
                "path": str((search_root / "agent_candidate_pool.json").resolve()),
                "sha256": _sha256_file(search_root / "agent_candidate_pool.json"),
            },
        ],
        "selected": final_records,
    }
    (output_root / "selection.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(
        json.dumps(
            {
                "selected": len(final_records),
                "categories": {
                    category: sum(
                        1 for item in final_records if item["category"] == category
                    )
                    for category in sorted({item["category"] for item in final_records})
                },
                "output": str(output_root.resolve()),
            },
            ensure_ascii=False,
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Consolidate completed external screenshot sampling runs."
    )
    parser.add_argument("--search-root", type=Path, required=True)
    parser.add_argument("--curated-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    consolidate(
        arguments.search_root.resolve(),
        arguments.curated_root.resolve(),
        arguments.output.resolve(),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
