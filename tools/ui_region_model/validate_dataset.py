from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from .schema import (
    DatasetValidationError,
    deterministic_split,
    iter_sample_paths,
    read_json,
    sha256_file,
    strict_json_loads,
    validate_dataset_metadata,
    validate_sample,
)


def canonical_sha256(value: object) -> str:
    payload = json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _is_sha256(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value)
    )


def _is_nonnegative_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def validate_dataset_manifest(manifest: object) -> None:
    if not isinstance(manifest, dict):
        raise DatasetValidationError(["dataset manifest must be an object"])
    errors: list[str] = []
    if manifest.get("manifestVersion") != 1:
        errors.append("dataset manifestVersion must equal 1")
    if not isinstance(manifest.get("datasetId"), str) or not manifest["datasetId"]:
        errors.append("dataset manifest datasetId must be a non-empty string")
    if not _is_sha256(manifest.get("metadataSha256")):
        errors.append("dataset manifest metadataSha256 is invalid")
    samples = manifest.get("samples")
    if not isinstance(samples, list):
        errors.append("dataset manifest samples must be an array")
        samples = []
    sample_ids: set[str] = set()
    valid_entries: list[dict[str, object]] = []
    for index, entry in enumerate(samples):
        prefix = f"dataset manifest samples[{index}]"
        if not isinstance(entry, dict):
            errors.append(f"{prefix} must be an object")
            continue
        sample_id = entry.get("sampleId")
        split_group = entry.get("splitGroup")
        split = entry.get("split")
        if not _is_sha256(sample_id):
            errors.append(f"{prefix}.sampleId must be a lowercase SHA-256 digest")
        elif sample_id in sample_ids:
            errors.append(f"{prefix}.sampleId is duplicated")
        else:
            sample_ids.add(sample_id)
        if not isinstance(split_group, str) or not split_group:
            errors.append(f"{prefix}.splitGroup must be a non-empty string")
        if split not in ("train", "validation", "test"):
            errors.append(f"{prefix}.split is invalid")
        for name in ("sampleJsonSha256", "imageSha256"):
            if not _is_sha256(entry.get(name)):
                errors.append(f"{prefix}.{name} is invalid")
        if not _is_nonnegative_int(entry.get("trainingUnitCount")):
            errors.append(f"{prefix}.trainingUnitCount is invalid")
        if (
            _is_sha256(sample_id)
            and isinstance(split_group, str)
            and split_group
            and split in ("train", "validation", "test")
            and _is_sha256(entry.get("sampleJsonSha256"))
            and _is_sha256(entry.get("imageSha256"))
            and _is_nonnegative_int(entry.get("trainingUnitCount"))
        ):
            valid_entries.append(entry)
    ordered_ids = [
        entry.get("sampleId") for entry in samples if isinstance(entry, dict)
    ]
    if all(isinstance(value, str) for value in ordered_ids) and ordered_ids != sorted(
        ordered_ids
    ):
        errors.append("dataset manifest samples must be sorted by sampleId")

    splits = manifest.get("splits")
    if not isinstance(splits, dict) or set(splits) != {"train", "validation", "test"}:
        errors.append(
            "dataset manifest splits must contain train, validation, and test"
        )
        splits = {}
    for split in ("train", "validation", "test"):
        value = splits.get(split)
        if not isinstance(value, dict):
            errors.append(f"dataset manifest splits.{split} must be an object")
            continue
        split_entries = [entry for entry in valid_entries if entry["split"] == split]
        expected = {
            "sha256": canonical_sha256(split_entries),
            "sampleCount": len(split_entries),
            "splitGroupCount": len(
                {str(entry["splitGroup"]) for entry in split_entries}
            ),
            "trainingUnitCount": sum(
                int(entry["trainingUnitCount"]) for entry in split_entries
            ),
        }
        if value != expected:
            errors.append(
                f"dataset manifest splits.{split} differs from its sample entries"
            )
    if errors:
        raise DatasetValidationError(errors)


def load_manifest_sample(
    dataset_root: Path, entry: dict[str, Any]
) -> tuple[dict[str, Any], bytes]:
    sample_id = entry["sampleId"]
    sample_path = dataset_root / "samples" / f"{sample_id}.json"
    sample_bytes = sample_path.read_bytes()
    if hashlib.sha256(sample_bytes).hexdigest() != entry["sampleJsonSha256"]:
        raise DatasetValidationError(
            [f"sample changed after manifest creation: {sample_id}"]
        )
    sample = strict_json_loads(sample_bytes)
    if not isinstance(sample, dict):
        raise DatasetValidationError([f"sample root must be an object: {sample_id}"])
    validate_sample(sample, dataset_root)
    if sample.get("sampleId") != sample_id:
        raise DatasetValidationError([f"sample ID differs from manifest: {sample_id}"])

    image_bytes = (dataset_root / sample["image"]["path"]).read_bytes()
    image_sha = hashlib.sha256(image_bytes).hexdigest()
    if image_sha != entry["imageSha256"] or image_sha != sample["image"]["sha256"]:
        raise DatasetValidationError(
            [f"image changed after manifest creation: {sample_id}"]
        )
    training_unit_count = sum(
        len(annotation["proposalIds"])
        for annotation in sample["annotations"]
        if annotation["labelStatus"] == "verified"
    )
    if training_unit_count != entry["trainingUnitCount"]:
        raise DatasetValidationError(
            [f"training-unit count differs from manifest: {sample_id}"]
        )
    return sample, image_bytes


def build_dataset_manifest(dataset_root: Path) -> dict[str, object]:
    metadata_path = dataset_root / "dataset.json"
    metadata = read_json(metadata_path)
    validate_dataset_metadata(metadata)
    entries: list[dict[str, object]] = []
    for path in iter_sample_paths(dataset_root):
        sample = read_json(path)
        validate_sample(sample, dataset_root)
        if path.stem != sample["sampleId"]:
            raise DatasetValidationError(
                [f"{path.name}: file name must equal sampleId"]
            )
        split = deterministic_split(sample["splitGroup"], metadata["splitSeed"])
        entries.append(
            {
                "sampleId": sample["sampleId"],
                "splitGroup": sample["splitGroup"],
                "split": split,
                "sampleJsonSha256": sha256_file(path),
                "imageSha256": sample["image"]["sha256"],
                "trainingUnitCount": sum(
                    len(annotation["proposalIds"])
                    for annotation in sample["annotations"]
                    if annotation["labelStatus"] == "verified"
                ),
            }
        )
    entries.sort(key=lambda value: str(value["sampleId"]))
    splits: dict[str, dict[str, object]] = {}
    for split in ("train", "validation", "test"):
        split_entries = [entry for entry in entries if entry["split"] == split]
        splits[split] = {
            "sha256": canonical_sha256(split_entries),
            "sampleCount": len(split_entries),
            "splitGroupCount": len(
                {str(entry["splitGroup"]) for entry in split_entries}
            ),
            "trainingUnitCount": sum(
                int(entry["trainingUnitCount"]) for entry in split_entries
            ),
        }
    manifest = {
        "manifestVersion": 1,
        "datasetId": metadata["datasetId"],
        "metadataSha256": sha256_file(metadata_path),
        "samples": entries,
        "splits": splits,
    }
    validate_dataset_manifest(manifest)
    return manifest


def validate_dataset(
    dataset_root: Path, require_reviewed: bool = False
) -> dict[str, object]:
    metadata = read_json(dataset_root / "dataset.json")
    validate_dataset_metadata(metadata)
    split_seed = metadata["splitSeed"]
    class_counts: Counter[str] = Counter()
    sample_counts: Counter[str] = Counter()
    group_counts: Counter[str] = Counter()
    application_groups: dict[str, set[str]] = defaultdict(set)
    session_groups: dict[tuple[str, str], set[str]] = defaultdict(set)
    source_owners: dict[str, str] = {}
    loaded_samples: list[tuple[dict[str, object], str]] = []
    errors: list[str] = []
    unreviewed = 0
    annotations = 0
    manual_text = 0
    proposals = 0
    for path in iter_sample_paths(dataset_root):
        sample = read_json(path)
        validate_sample(sample, dataset_root)
        if path.stem != sample["sampleId"]:
            raise DatasetValidationError(
                [f"{path.name}: file name must equal sampleId"]
            )
        split = deterministic_split(sample["splitGroup"], split_seed)
        sample_counts[split] += 1
        group_counts[sample["splitGroup"]] += 1
        loaded_samples.append((sample, split))
        for source in sample["sources"]:
            diagnostic_id = source["diagnosticId"]
            previous_owner = source_owners.setdefault(diagnostic_id, sample["sampleId"])
            if previous_owner != sample["sampleId"]:
                errors.append(
                    f"diagnostic source is attached to multiple samples: {diagnostic_id}"
                )
        app_family = sample["splitKey"].get("appFamily")
        page_session = sample["splitKey"].get("pageSession")
        if app_family:
            application_groups[app_family].add(sample["splitGroup"])
            if page_session:
                session_groups[(app_family, page_session)].add(sample["splitGroup"])
        proposals += len(sample["ocrProposals"])
        for annotation in sample["annotations"]:
            annotations += 1
            if annotation["labelStatus"] == "unreviewed":
                unreviewed += 1
                continue
            if annotation["labelStatus"] == "ignored":
                continue
            class_counts[
                f"{annotation['textness']}:{annotation['role'] or '-'}:{annotation['translationPolicy']}"
            ] += 1
            if annotation["textness"] == "text" and not annotation["proposalIds"]:
                manual_text += 1
        if require_reviewed:
            if not sample["coverage"]["textComplete"]:
                errors.append(f"{sample['sampleId']}: text coverage is incomplete")
            if not sample["coverage"]["anchorsComplete"]:
                errors.append(f"{sample['sampleId']}: anchor coverage is incomplete")
            if not sample["coverage"]["protectedRegionsComplete"]:
                errors.append(
                    f"{sample['sampleId']}: protected-region coverage is incomplete"
                )
            split_key = sample["splitKey"]
            stable_key_complete = bool(
                split_key.get("appFamily") and split_key.get("pageSession")
            )
            if not stable_key_complete and not split_key["independenceReviewed"]:
                errors.append(
                    f"{sample['sampleId']}: app/page split keys are incomplete and split isolation was not reviewed"
                )
            if not stable_key_complete and sample["splitGroupMethod"] != "manual":
                errors.append(
                    f"{sample['sampleId']}: samples without complete app/page keys require a manually reviewed split group"
                )

    for first_index, (first, _) in enumerate(loaded_samples):
        first_hash = int(first["image"]["perceptualHash"], 16)
        for second, _ in loaded_samples[first_index + 1 :]:
            distance = (
                first_hash ^ int(second["image"]["perceptualHash"], 16)
            ).bit_count()
            if distance <= 4 and first["splitGroup"] != second["splitGroup"]:
                errors.append(
                    f"near-duplicate samples use different split groups: {first['sampleId']} and {second['sampleId']}"
                )
    for app_family, groups in application_groups.items():
        if len(groups) > 1:
            errors.append(f"application family must use one split group: {app_family}")
    for (app_family, page_session), groups in session_groups.items():
        if len(groups) > 1:
            errors.append(
                f"page session must use one split group: {app_family}/{page_session}"
            )
    if require_reviewed and unreviewed:
        errors.append(f"{unreviewed} annotations are still unreviewed")
    if errors:
        raise DatasetValidationError(errors)
    return {
        "samples": sum(sample_counts.values()),
        "splitGroups": len(group_counts),
        "ocrProposals": proposals,
        "annotations": annotations,
        "unreviewed": unreviewed,
        "missedTextAnnotations": manual_text,
        "samplesBySplit": dict(sorted(sample_counts.items())),
        "verifiedClasses": dict(sorted(class_counts.items())),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate a Visnip UI region dataset")
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--require-reviewed", action="store_true")
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)
    try:
        summary = validate_dataset(args.dataset, args.require_reviewed)
    except (DatasetValidationError, OSError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
