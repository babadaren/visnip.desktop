from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from collections import Counter
from pathlib import Path
from typing import Any

from PIL import Image

from . import SCHEMA_VERSION
from .export_diagnostics import _draft_annotation, _proposal_id
from .schema import (
    COORDINATE_SYSTEM,
    RECT_ENCODING,
    atomic_write_json,
    canonical_split_identifier,
    perceptual_hash_file,
    read_json,
    sha256_file,
    utc_timestamp,
    validate_dataset_metadata,
    validate_sample,
)


def _slug(value: str) -> str:
    slug = canonical_split_identifier(value.replace("_", "-"))
    allowed = "abcdefghijklmnopqrstuvwxyz0123456789-"
    slug = "".join(character if character in allowed else "-" for character in slug)
    return "-".join(part for part in slug.split("-") if part) or "unknown"


def _load_results(path: Path) -> dict[str, dict[str, Any]]:
    payload = read_json(path)
    if payload.get("formatVersion") != 1 or not isinstance(
        payload.get("results"), list
    ):
        raise ValueError("OCR results must use formatVersion 1")
    results: dict[str, dict[str, Any]] = {}
    for index, result in enumerate(payload["results"]):
        if not isinstance(result, dict) or not isinstance(result.get("path"), str):
            raise ValueError(f"OCR result {index} is malformed")
        filename = Path(result["path"]).name.casefold()
        if filename in results:
            raise ValueError(f"Duplicate OCR result filename: {filename}")
        if result.get("error"):
            raise ValueError(f"OCR failed for {filename}: {result['error']}")
        if not isinstance(result.get("lines"), list):
            raise ValueError(f"OCR lines are missing for {filename}")
        results[filename] = result
    return results


def _metadata(dataset_root: Path) -> dict[str, Any]:
    metadata_path = dataset_root / "dataset.json"
    if metadata_path.is_file():
        value = read_json(metadata_path)
        validate_dataset_metadata(value)
        return value
    now = utc_timestamp()
    return {
        "schemaVersion": SCHEMA_VERSION,
        "datasetId": "visnip-ui-regions-external-v2",
        "createdAt": now,
        "updatedAt": now,
        "splitSeed": "visnip-ui-v1",
        "coordinateSystem": COORDINATE_SYSTEM,
        "rectEncoding": RECT_ENCODING,
    }


def _line_box(line: dict[str, Any], name: str) -> dict[str, int]:
    value = line.get(name)
    if not isinstance(value, dict):
        raise ValueError(f"OCR line {name} must be an object")
    box = {field: value.get(field) for field in ("x", "y", "width", "height")}
    if any(not isinstance(item, int) or isinstance(item, bool) for item in box.values()):
        raise ValueError(f"OCR line {name} must contain integer xywh values")
    return box


def _proposal(
    sample_id: str, source_id: str, line: dict[str, Any]
) -> dict[str, Any]:
    line_index = line.get("index")
    if not isinstance(line_index, int) or isinstance(line_index, bool) or line_index < 0:
        raise ValueError("OCR line index must be a non-negative integer")
    text_box = _line_box(line, "textBox")
    detected_box = _line_box(line, "detectedBox")
    recognized_text = line.get("recognizedText")
    score = line.get("recognitionScore")
    if not isinstance(recognized_text, str):
        raise ValueError("OCR recognizedText must be a string")
    if not isinstance(score, (int, float)) or isinstance(score, bool):
        raise ValueError("OCR recognitionScore must be numeric")
    proposal_id = _proposal_id(sample_id, source_id, line_index, text_box)
    observation: dict[str, Any] = {
        "diagnosticId": source_id,
        "sourceLineIndex": line_index,
        "textBox": text_box,
        "detectedBox": detected_box,
        "recognizedText": recognized_text,
        "recognitionScore": float(score),
        "pipelineDecision": {
            "acceptedForTranslation": None,
            "rejectedFragmentByOcrFilter": None,
            "rejectionReason": None,
            "preservationReason": None,
        },
    }
    if isinstance(line.get("leadingIconSeparated"), bool):
        observation["leadingIconSeparated"] = line["leadingIconSeparated"]
    if isinstance(line.get("refinementReason"), str) and line[
        "refinementReason"
    ].strip():
        observation["refinementReason"] = line["refinementReason"]
    return {
        "id": proposal_id,
        "textBox": text_box,
        "observations": [observation],
        "anchorObservation": {
            "diagnosticId": source_id,
            "sourceLineIndex": line_index,
        },
    }


def import_selection(
    selection_path: Path,
    images_root: Path,
    ocr_results_path: Path,
    dataset_root: Path,
) -> dict[str, Any]:
    selection = read_json(selection_path)
    selected = selection.get("selected")
    if not isinstance(selected, list) or not selected:
        raise ValueError("Selection must contain at least one selected screenshot")
    results = _load_results(ocr_results_path)
    dataset_root.mkdir(parents=True, exist_ok=True)
    dataset_images = dataset_root / "images"
    dataset_samples = dataset_root / "samples"
    dataset_images.mkdir(exist_ok=True)
    dataset_samples.mkdir(exist_ok=True)
    metadata = _metadata(dataset_root)
    now = utc_timestamp()
    imported = 0
    proposals_total = 0
    categories: Counter[str] = Counter()
    for index, item in enumerate(selected):
        if not isinstance(item, dict):
            raise ValueError(f"Selection item {index} must be an object")
        normalized_path = item.get("normalizedPath")
        if not isinstance(normalized_path, str) or Path(normalized_path).name != normalized_path:
            raise ValueError(f"Selection item {index} has an unsafe normalizedPath")
        source_image = images_root / normalized_path
        if not source_image.is_file():
            raise ValueError(f"Selected image is missing: {source_image}")
        sample_id = sha256_file(source_image)
        if sample_id != item.get("normalizedSha256"):
            raise ValueError(f"Selected image hash changed: {normalized_path}")
        with Image.open(source_image) as image:
            width, height = image.size
            image_format = image.format
        if image_format != "PNG":
            raise ValueError(f"Normalized image is not PNG: {normalized_path}")
        if (width, height) != (
            item.get("normalizedWidth"),
            item.get("normalizedHeight"),
        ):
            raise ValueError(f"Selected image dimensions changed: {normalized_path}")
        result = results.get(normalized_path.casefold())
        if result is None:
            raise ValueError(f"OCR result is missing for {normalized_path}")
        if (result.get("width"), result.get("height")) != (width, height):
            raise ValueError(f"OCR dimensions differ for {normalized_path}")

        destination_image = dataset_images / f"{sample_id}.png"
        if destination_image.is_file() and sha256_file(destination_image) != sample_id:
            raise ValueError(f"Dataset image path has conflicting content: {sample_id}")
        if not destination_image.is_file():
            shutil.copyfile(source_image, destination_image)

        source_id = f"external-{sample_id[:20]}"
        proposals = [
            _proposal(sample_id, source_id, line) for line in result["lines"]
        ]
        category = str(item.get("category", "unknown"))
        split_slug = _slug(category)
        title = str(item.get("title", normalized_path))
        sample = {
            "schemaVersion": SCHEMA_VERSION,
            "sampleId": sample_id,
            "revision": 0,
            "createdAt": item.get("retrievedAt") or now,
            "updatedAt": now,
            "splitGroup": f"external/{split_slug}",
            "splitGroupMethod": "manual",
            "splitKey": {
                "appFamily": f"external-{split_slug}",
                "pageSession": _slug(title.removeprefix("File:")),
                "theme": None,
                "dpiScale": None,
                "locale": None,
                "independenceReviewed": True,
            },
            "coverage": {
                "textComplete": False,
                "anchorsComplete": False,
                "protectedRegionsComplete": False,
            },
            "image": {
                "path": f"images/{sample_id}.png",
                "width": width,
                "height": height,
                "sha256": sample_id,
                "perceptualHash": perceptual_hash_file(destination_image),
                "mimeType": "image/png",
                "coordinateSpace": COORDINATE_SYSTEM,
                "rectEncoding": RECT_ENCODING,
            },
            "sources": [
                {
                    "kind": "external_screenshot",
                    "diagnosticId": source_id,
                    "createdAt": item.get("retrievedAt"),
                    "status": "ocr_completed",
                    "selectionCoordinateSpace": COORDINATE_SYSTEM,
                    "selection": {"x": 0, "y": 0, "width": width, "height": height},
                    "requestedOcrPackId": result.get("packId"),
                    "resolvedOcrPackId": result.get("packId"),
                    "originUrl": item.get("originUrl"),
                    "descriptionUrl": item.get("descriptionUrl"),
                    "sourceTitle": title,
                    "author": item.get("author"),
                    "licenseName": item.get("licenseName"),
                    "licenseUrl": item.get("licenseUrl"),
                    "retrievedAt": item.get("retrievedAt"),
                    "category": category,
                    "query": item.get("query"),
                    **(
                        {"originalSha256": item["originalSha256"]}
                        if item.get("originalSha256")
                        else {}
                    ),
                }
            ],
            "ocrProposals": proposals,
            "layoutContainers": [],
            "annotations": [_draft_annotation(proposal) for proposal in proposals],
        }
        validate_sample(sample, dataset_root)
        sample_path = dataset_samples / f"{sample_id}.json"
        if sample_path.is_file():
            existing = read_json(sample_path)
            if existing != sample:
                raise ValueError(f"Sample already exists with different data: {sample_id}")
        else:
            atomic_write_json(sample_path, sample)
            imported += 1
        proposals_total += len(proposals)
        categories[category] += 1

    metadata["updatedAt"] = now
    atomic_write_json(dataset_root / "dataset.json", metadata)
    summary = {
        "selected": len(selected),
        "imported": imported,
        "proposals": proposals_total,
        "categories": dict(sorted(categories.items())),
        "dataset": str(dataset_root.resolve()),
    }
    print(json.dumps(summary, ensure_ascii=False))
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Import licensed screenshots and raw Visnip OCR proposals."
    )
    parser.add_argument("--selection", type=Path, required=True)
    parser.add_argument("--images", type=Path, required=True)
    parser.add_argument("--ocr-results", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, required=True)
    arguments = parser.parse_args()
    import_selection(
        arguments.selection.resolve(),
        arguments.images.resolve(),
        arguments.ocr_results.resolve(),
        arguments.dataset.resolve(),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
