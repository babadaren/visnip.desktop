from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Any

from PIL import Image

from . import SCHEMA_VERSION
from .schema import (
    COORDINATE_SYSTEM,
    RECT_ENCODING,
    SPLIT_IDENTIFIER_PATTERN,
    DatasetValidationError,
    atomic_write_json,
    canonical_split_identifier,
    dataset_lock,
    iter_sample_paths,
    perceptual_hash_file,
    read_json,
    sample_path,
    sha256_file,
    utc_timestamp,
    validate_dataset_metadata,
    validate_sample,
)


def default_diagnostics_root() -> Path:
    local_app_data = os.environ.get("LOCALAPPDATA")
    if not local_app_data:
        raise RuntimeError("LOCALAPPDATA is unavailable; pass --diagnostics explicitly")
    return Path(local_app_data) / "Visnip" / "Visnip" / "fast-translation-diagnostics"


def default_dataset_root() -> Path:
    local_app_data = os.environ.get("LOCALAPPDATA")
    if not local_app_data:
        raise RuntimeError("LOCALAPPDATA is unavailable; pass --dataset explicitly")
    return Path(local_app_data) / "Visnip" / "Visnip" / "ui-region-dataset"


def _rect(value: Any, context: str) -> dict[str, int]:
    if not isinstance(value, dict):
        raise DatasetValidationError([f"{context}: rectangle is missing"])
    result: dict[str, int] = {}
    for name in ("x", "y", "width", "height"):
        coordinate = value.get(name)
        if not isinstance(coordinate, int) or isinstance(coordinate, bool):
            raise DatasetValidationError([f"{context}.{name}: expected integer"])
        result[name] = coordinate
    return result


def _hamming(first: str, second: str) -> int:
    return (int(first, 16) ^ int(second, 16)).bit_count()


def _observation(
    line: dict[str, Any], diagnostic_id: str, text_box: dict[str, int]
) -> dict[str, Any]:
    value: dict[str, Any] = {
        "diagnosticId": diagnostic_id,
        "sourceLineIndex": line.get("index"),
        "textBox": text_box,
        "recognizedText": line.get("text"),
        "recognitionScore": line.get("score"),
        "detectedBox": _rect(line["detectedBox"], f"{diagnostic_id}.detectedBox")
        if "detectedBox" in line
        else None,
        "pipelineDecision": {
            "acceptedForTranslation": line.get("acceptedForTranslation"),
            "rejectedFragmentByOcrFilter": line.get("rejectedFragmentByOcrFilter"),
            "rejectionReason": line.get("rejectionReason"),
            "preservationReason": line.get("preservationReason"),
        },
    }
    for field in (
        "leadingIconSeparated",
        "refinementReason",
        "inkHeight",
        "backgroundLuma",
        "foregroundLuma",
        "contrast",
    ):
        if field in line:
            value[field] = line[field]
    return value


def _proposal_id(
    sample_id: str, diagnostic_id: str, source_line_index: Any, box: dict[str, int]
) -> str:
    coordinates = ",".join(str(box[name]) for name in ("x", "y", "width", "height"))
    digest = hashlib.sha256(
        f"{sample_id}\0{diagnostic_id}\0{source_line_index}\0{coordinates}".encode(
            "utf-8"
        )
    ).hexdigest()
    return f"ocr-{digest[:16]}"


def _draft_annotation(proposal: dict[str, Any]) -> dict[str, Any]:
    return {
        "id": f"ann-{proposal['id']}",
        "proposalIds": [proposal["id"]],
        "textness": "unknown",
        "role": None,
        "transcription": None,
        "translationPolicy": "unknown",
        "labelStatus": "unreviewed",
        "relation": "unknown",
        "patchMode": "unknown",
        "textBox": dict(proposal["textBox"]),
        "maskBox": None,
        "layoutBox": None,
        "containerId": None,
        "groupId": None,
        "parentAnnotationId": None,
        "readingOrder": None,
        "attributes": {"illegible": False, "truncated": False},
    }


def _merge_proposal(
    sample: dict[str, Any], line: dict[str, Any], diagnostic_id: str
) -> tuple[bool, bool]:
    box = _rect(
        line.get("box"), f"{diagnostic_id}.ocrLines[{line.get('index', '?')}].box"
    )
    observation = _observation(line, diagnostic_id, box)
    for proposal in sample["ocrProposals"]:
        for observation_index, existing in enumerate(proposal["observations"]):
            if existing.get("diagnosticId") == diagnostic_id and existing.get(
                "sourceLineIndex"
            ) == line.get("index"):
                if existing.get("textBox") != box:
                    raise DatasetValidationError(
                        [
                            f"{diagnostic_id}: OCR line {line.get('index')} changed geometry after export"
                        ]
                    )
                if existing == observation:
                    return False, False
                proposal["observations"][observation_index] = observation
                return True, False
    proposal_id = _proposal_id(
        sample["sampleId"], diagnostic_id, line.get("index"), box
    )
    existing_ids = {proposal["id"] for proposal in sample["ocrProposals"]}
    if proposal_id in existing_ids:
        raise DatasetValidationError(
            [f"proposal ID collision for {diagnostic_id} line {line.get('index')}"]
        )
    proposal = {
        "id": proposal_id,
        "textBox": box,
        "anchorObservation": {
            "diagnosticId": diagnostic_id,
            "sourceLineIndex": line.get("index"),
        },
        "observations": [observation],
    }
    sample["ocrProposals"].append(proposal)
    sample["annotations"].append(_draft_annotation(proposal))
    return True, True


def _load_diagnostic(directory: Path) -> dict[str, Any]:
    analysis_path = directory / "analysis.json"
    if not analysis_path.is_file():
        raise DatasetValidationError([f"{directory.name}: analysis.json is missing"])
    analysis = read_json(analysis_path)
    original_name = analysis.get("originalFile", "original.png")
    if not isinstance(original_name, str) or Path(original_name).name != original_name:
        raise DatasetValidationError(
            [f"{directory.name}: originalFile must be a file name"]
        )
    image_path = directory / original_name
    if not image_path.is_file():
        raise DatasetValidationError([f"{directory.name}: {original_name} is missing"])
    with Image.open(image_path) as image:
        image.load()
        width, height = image.size
        image_format = image.format
    format_metadata = {
        "PNG": (".png", "image/png"),
        "JPEG": (".jpg", "image/jpeg"),
    }.get(image_format)
    if format_metadata is None:
        raise DatasetValidationError(
            [f"{directory.name}: unsupported source image format {image_format}"]
        )
    ocr_lines = analysis.get("ocrLines")
    if not isinstance(ocr_lines, list):
        raise DatasetValidationError([f"{directory.name}: ocrLines must be an array"])
    line_indices: set[int] = set()
    for line_position, line in enumerate(ocr_lines):
        if not isinstance(line, dict):
            raise DatasetValidationError(
                [f"{directory.name}: ocrLines[{line_position}] must be an object"]
            )
        source_line_index = line.get("index")
        if (
            not isinstance(source_line_index, int)
            or isinstance(source_line_index, bool)
            or source_line_index < 0
        ):
            raise DatasetValidationError(
                [
                    f"{directory.name}: ocrLines[{line_position}].index must be a non-negative integer"
                ]
            )
        if source_line_index in line_indices:
            raise DatasetValidationError(
                [f"{directory.name}: duplicate OCR line index {source_line_index}"]
            )
        line_indices.add(source_line_index)
    return {
        "directory": directory,
        "analysis": analysis,
        "imagePath": image_path,
        "width": width,
        "height": height,
        "sha256": sha256_file(image_path),
        "perceptualHash": perceptual_hash_file(image_path),
        "imageExtension": format_metadata[0],
        "mimeType": format_metadata[1],
    }


def _dataset_metadata(dataset_root: Path) -> dict[str, Any]:
    metadata_path = dataset_root / "dataset.json"
    if metadata_path.is_file():
        metadata = read_json(metadata_path)
        validate_dataset_metadata(metadata)
        return metadata
    now = utc_timestamp()
    return {
        "schemaVersion": SCHEMA_VERSION,
        "datasetId": "visnip-ui-regions-v2",
        "createdAt": now,
        "updatedAt": now,
        "splitSeed": "visnip-ui-v1",
        "coordinateSystem": COORDINATE_SYSTEM,
        "rectEncoding": RECT_ENCODING,
    }


def _migrate_split_identifiers(sample: dict[str, Any]) -> bool:
    updates: list[tuple[dict[str, Any], str, str]] = []
    split_group = sample.get("splitGroup")
    if isinstance(split_group, str):
        canonical_group = canonical_split_identifier(split_group)
        if not SPLIT_IDENTIFIER_PATTERN.fullmatch(canonical_group):
            raise DatasetValidationError(
                [
                    f"{sample.get('sampleId')}: splitGroup must be replaced with a lowercase ASCII identifier"
                ]
            )
        if canonical_group != split_group:
            updates.append((sample, "splitGroup", canonical_group))
    split_key = sample.get("splitKey")
    if isinstance(split_key, dict):
        for name in ("appFamily", "pageSession", "theme", "locale"):
            value = split_key.get(name)
            if not isinstance(value, str):
                continue
            canonical_value = canonical_split_identifier(value)
            if not SPLIT_IDENTIFIER_PATTERN.fullmatch(canonical_value):
                raise DatasetValidationError(
                    [
                        f"{sample.get('sampleId')}: splitKey.{name} must be replaced with a lowercase ASCII identifier"
                    ]
                )
            if canonical_value != value:
                updates.append((split_key, name, canonical_value))
    if not updates:
        return False
    has_human_labels = any(
        annotation.get("labelStatus") != "unreviewed"
        or not annotation.get("proposalIds")
        for annotation in sample.get("annotations", [])
        if isinstance(annotation, dict)
    )
    has_human_layout = bool(sample.get("layoutContainers")) or any(
        sample.get("coverage", {}).get(name) is True
        for name in ("textComplete", "anchorsComplete", "protectedRegionsComplete")
    )
    if has_human_labels or has_human_layout:
        raise DatasetValidationError(
            [
                f"{sample.get('sampleId')}: canonical split migration requires manual review because labels already exist"
            ]
        )
    for target, name, value in updates:
        target[name] = value
    return True


def _new_sample(item: dict[str, Any], now: str) -> dict[str, Any]:
    sample_id = item["sha256"]
    return {
        "schemaVersion": SCHEMA_VERSION,
        "sampleId": sample_id,
        "revision": 0,
        "createdAt": now,
        "updatedAt": now,
        "splitGroup": sample_id,
        "splitGroupMethod": "perceptual_hash",
        "splitKey": {
            "appFamily": None,
            "pageSession": None,
            "theme": None,
            "dpiScale": None,
            "locale": None,
            "independenceReviewed": False,
        },
        "coverage": {
            "textComplete": False,
            "anchorsComplete": False,
            "protectedRegionsComplete": False,
        },
        "image": {
            "path": f"images/{sample_id}{item['imageExtension']}",
            "width": item["width"],
            "height": item["height"],
            "sha256": sample_id,
            "perceptualHash": item["perceptualHash"],
            "coordinateSpace": COORDINATE_SYSTEM,
            "rectEncoding": RECT_ENCODING,
            "mimeType": item["mimeType"],
        },
        "sources": [],
        "ocrProposals": [],
        "annotations": [],
        "layoutContainers": [],
    }


def _group_similar_default_samples(samples: list[dict[str, Any]]) -> None:
    parents = list(range(len(samples)))

    def find(index: int) -> int:
        while parents[index] != index:
            parents[index] = parents[parents[index]]
            index = parents[index]
        return index

    def union(first: int, second: int) -> None:
        first_root = find(first)
        second_root = find(second)
        if first_root != second_root:
            parents[max(first_root, second_root)] = min(first_root, second_root)

    for first in range(len(samples)):
        first_image = samples[first]["image"]
        for second in range(first + 1, len(samples)):
            second_image = samples[second]["image"]
            if (
                _hamming(first_image["perceptualHash"], second_image["perceptualHash"])
                <= 4
            ):
                union(first, second)
    clusters: dict[int, list[dict[str, Any]]] = {}
    for index, sample in enumerate(samples):
        clusters.setdefault(find(index), []).append(sample)
    for cluster in clusters.values():
        group = min(sample["sampleId"] for sample in cluster)
        for sample in cluster:
            if sample["splitGroupMethod"] == "perceptual_hash":
                sample["splitGroup"] = group


def _remove_stale_proposals(
    sample: dict[str, Any], diagnostic_id: str, current_line_indices: set[int]
) -> int:
    stale_ids = {
        proposal["id"]
        for proposal in sample["ocrProposals"]
        if proposal["observations"][0]["diagnosticId"] == diagnostic_id
        and proposal["observations"][0]["sourceLineIndex"] not in current_line_indices
    }
    if not stale_ids:
        return 0
    linked_annotations = [
        annotation
        for annotation in sample["annotations"]
        if any(
            proposal_id in stale_ids
            for proposal_id in annotation.get("proposalIds", [])
        )
    ]
    reviewed = [
        annotation["id"]
        for annotation in linked_annotations
        if annotation.get("labelStatus") != "unreviewed"
    ]
    if reviewed:
        raise DatasetValidationError(
            [
                f"{diagnostic_id}: removed OCR lines are already reviewed: {', '.join(sorted(reviewed))}"
            ]
        )
    stale_annotation_ids = {annotation["id"] for annotation in linked_annotations}
    if any(
        annotation.get("parentAnnotationId") in stale_annotation_ids
        for annotation in sample["annotations"]
    ):
        raise DatasetValidationError(
            [f"{diagnostic_id}: removed OCR lines still own child annotations"]
        )
    sample["ocrProposals"] = [
        proposal
        for proposal in sample["ocrProposals"]
        if proposal["id"] not in stale_ids
    ]
    sample["annotations"] = [
        annotation
        for annotation in sample["annotations"]
        if annotation["id"] not in stale_annotation_ids
    ]
    return len(stale_ids)


def export_diagnostics(
    diagnostics_root: Path, dataset_root: Path, limit: int | None = None
) -> dict[str, int]:
    directories = sorted(
        (path for path in diagnostics_root.iterdir() if path.is_dir()),
        key=lambda path: path.name,
    )
    if limit is not None:
        directories = directories[-limit:]
    loaded = [_load_diagnostic(directory) for directory in directories]
    dataset_root.mkdir(parents=True, exist_ok=True)
    with dataset_lock(dataset_root):
        metadata_path = dataset_root / "dataset.json"
        metadata_is_new = not metadata_path.is_file()
        metadata = _dataset_metadata(dataset_root)
        samples: dict[str, dict[str, Any]] = {}
        source_images: dict[str, Path] = {}
        source_owners: dict[str, str] = {}
        changed_samples: set[str] = set()
        added_candidates = 0
        removed_candidates = 0
        now = utc_timestamp()

        for json_path in iter_sample_paths(dataset_root):
            sample = read_json(json_path)
            if "splitGroupMethod" not in sample:
                sample["splitGroupMethod"] = "manual"
                changed_samples.add(sample["sampleId"])
            if _migrate_split_identifiers(sample):
                changed_samples.add(sample["sampleId"])
            validate_sample(sample, dataset_root)
            samples[sample["sampleId"]] = sample
            for source in sample["sources"]:
                diagnostic_id = source["diagnosticId"]
                previous_owner = source_owners.setdefault(
                    diagnostic_id, sample["sampleId"]
                )
                if previous_owner != sample["sampleId"]:
                    raise DatasetValidationError(
                        [
                            f"diagnostic {diagnostic_id} is attached to multiple source images"
                        ]
                    )

        for item in loaded:
            sample_id = item["sha256"]
            diagnostic_id = item["directory"].name
            previous_owner = source_owners.get(diagnostic_id)
            if previous_owner is not None and previous_owner != sample_id:
                raise DatasetValidationError(
                    [
                        f"diagnostic {diagnostic_id} changed source image after it was exported"
                    ]
                )
            if sample_id not in samples:
                samples[sample_id] = _new_sample(item, now)
                changed_samples.add(sample_id)
            source_owners[diagnostic_id] = sample_id
            source_images[sample_id] = item["imagePath"]
            sample = samples[sample_id]
            analysis = item["analysis"]
            removed = _remove_stale_proposals(
                sample,
                diagnostic_id,
                {line["index"] for line in analysis["ocrLines"]},
            )
            if removed:
                removed_candidates += removed
                changed_samples.add(sample_id)
            source = {
                "kind": "fast_translation_diagnostic",
                "diagnosticId": item["directory"].name,
                "createdAt": analysis.get("createdAt"),
                "status": analysis.get("status"),
                "selection": analysis.get("selection"),
                "selectionCoordinateSpace": "virtual-screen-pixels",
                "requestedOcrPackId": analysis.get("requestedOcrPackId"),
                "resolvedOcrPackId": analysis.get("resolvedOcrPackId"),
                "ocrPackVersionKey": analysis.get("ocrPackVersionKey"),
            }
            existing_source = next(
                (
                    existing
                    for existing in sample["sources"]
                    if existing.get("diagnosticId") == source["diagnosticId"]
                ),
                None,
            )
            if existing_source is None:
                sample["sources"].append(source)
                changed_samples.add(sample_id)
            elif existing_source != source:
                existing_source.clear()
                existing_source.update(source)
                changed_samples.add(sample_id)
            for line in analysis["ocrLines"]:
                if not isinstance(line, dict):
                    raise DatasetValidationError(
                        [f"{item['directory'].name}: ocrLines entries must be objects"]
                    )
                proposal_changed, proposal_added = _merge_proposal(
                    sample, line, item["directory"].name
                )
                if proposal_changed:
                    added_candidates += int(proposal_added)
                    changed_samples.add(sample_id)

        sample_values = list(samples.values())
        previous_groups = {
            sample["sampleId"]: sample["splitGroup"] for sample in sample_values
        }
        _group_similar_default_samples(sample_values)
        for sample in sample_values:
            if sample["splitGroup"] != previous_groups[sample["sampleId"]]:
                changed_samples.add(sample["sampleId"])
            if sample["sampleId"] in changed_samples:
                if sample_path(dataset_root, sample["sampleId"]).is_file():
                    sample["revision"] += 1
                sample["updatedAt"] = now
            validate_sample(sample)

        (dataset_root / "images").mkdir(exist_ok=True)
        (dataset_root / "samples").mkdir(exist_ok=True)
        for sample_id in sorted(changed_samples):
            sample = samples[sample_id]
            destination = dataset_root / sample["image"]["path"]
            if destination.is_file():
                if sha256_file(destination) != sample_id:
                    raise DatasetValidationError(
                        [f"existing dataset image has the wrong hash: {destination}"]
                    )
            else:
                source_image = source_images.get(sample_id)
                if source_image is None:
                    raise DatasetValidationError(
                        [f"source image is unavailable for sample {sample_id}"]
                    )
                shutil.copy2(source_image, destination)
            validate_sample(sample, dataset_root)
            atomic_write_json(sample_path(dataset_root, sample_id), sample)
        if metadata_is_new or changed_samples:
            metadata["updatedAt"] = now
            atomic_write_json(metadata_path, metadata)
        return {
            "diagnostics": len(loaded),
            "samples": len(sample_values),
            "updatedSamples": len(changed_samples),
            "addedCandidates": added_candidates,
            "removedCandidates": removed_candidates,
        }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Export Visnip OCR diagnostics into a reviewable dataset"
    )
    parser.add_argument("--diagnostics", type=Path, default=default_diagnostics_root())
    parser.add_argument("--dataset", type=Path, default=default_dataset_root())
    parser.add_argument("--limit", type=int)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if not args.diagnostics.is_dir():
        print(
            f"Diagnostics directory does not exist: {args.diagnostics}", file=sys.stderr
        )
        return 2
    try:
        summary = export_diagnostics(args.diagnostics, args.dataset, args.limit)
    except (DatasetValidationError, OSError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    print(
        json.dumps(
            {"dataset": str(args.dataset), **summary}, ensure_ascii=False, indent=2
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
