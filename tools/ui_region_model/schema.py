from __future__ import annotations

import hashlib
import json
import math
import os
import re
import time
import unicodedata
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterable
from urllib.parse import urlsplit

from PIL import Image

from . import SCHEMA_VERSION
from .geometry import box_inside_bounds, context_crop_bounds


TEXT_ROLES = (
    "title",
    "heading",
    "body",
    "menu_item",
    "label",
    "value",
    "button",
    "link",
    "tab",
    "badge",
    "code",
    "date_number",
    "caption",
    "metadata",
    "identifier",
    "other_text",
)

NON_TEXT_ROLES = (
    "icon",
    "logo",
    "avatar",
    "artwork",
    "photo",
    "chart_cell",
    "divider",
    "decoration",
    "other_non_text",
)

CONTAINER_ROLES = (
    "window",
    "panel",
    "section",
    "menu",
    "menu_item",
    "button",
    "card",
    "table",
    "table_cell",
    "dialog",
    "other_container",
)

TEXTNESS_VALUES = ("unknown", "text", "non_text", "mixed")
TRANSLATION_POLICIES = ("unknown", "translate", "preserve", "review")
LABEL_STATUSES = ("unreviewed", "verified", "ignored")
RELATIONS = ("unknown", "single", "split_required", "merge_required", "none")
PATCH_MODES = ("unknown", "rect_safe", "mask_required", "none")
COORDINATE_SYSTEM = "original-image-pixels"
RECT_ENCODING = "xywh-half-open"
IMAGE_FORMATS = {
    "image/png": (".png", "PNG"),
    "image/jpeg": (".jpg", "JPEG"),
}
SOURCE_KINDS = ("fast_translation_diagnostic", "external_screenshot")
SPLIT_IDENTIFIER_PATTERN = re.compile(r"^[a-z0-9][a-z0-9._:/-]*$")
MIN_PROPOSAL_ANCHOR_OVERLAP = 0.5


class DatasetValidationError(ValueError):
    def __init__(self, errors: Iterable[str]):
        self.errors = tuple(errors)
        super().__init__("\n".join(self.errors))


def _reject_json_constant(value: str) -> None:
    raise DatasetValidationError([f"non-standard JSON number is forbidden: {value}"])


def _json_tree_errors(
    value: Any, prefix: str = "$", seen: set[int] | None = None
) -> list[str]:
    if value is None or isinstance(value, (bool, int)):
        return []
    if isinstance(value, float):
        return (
            [] if math.isfinite(value) else [f"{prefix} contains a non-finite number"]
        )
    if isinstance(value, str):
        try:
            value.encode("utf-8", errors="strict")
        except UnicodeEncodeError:
            return [f"{prefix} contains an unpaired Unicode surrogate"]
        return []
    if not isinstance(value, (dict, list)):
        return [f"{prefix} contains a non-JSON value"]
    if seen is None:
        seen = set()
    identity = id(value)
    if identity in seen:
        return [f"{prefix} contains a cyclic value"]
    seen.add(identity)
    errors: list[str] = []
    if isinstance(value, list):
        for index, item in enumerate(value):
            errors.extend(_json_tree_errors(item, f"{prefix}[{index}]", seen))
    else:
        for key, item in value.items():
            if not isinstance(key, str):
                errors.append(f"{prefix} contains a non-string object key")
                continue
            errors.extend(_json_tree_errors(item, f"{prefix}.{key}", seen))
    seen.remove(identity)
    return errors


def strict_json_loads(value: str | bytes) -> Any:
    result = json.loads(value, parse_constant=_reject_json_constant)
    errors = _json_tree_errors(result)
    if errors:
        raise DatasetValidationError(errors)
    return result


def utc_timestamp() -> str:
    from datetime import datetime, timezone

    return (
        datetime.now(timezone.utc)
        .isoformat(timespec="milliseconds")
        .replace("+00:00", "Z")
    )


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream, parse_constant=_reject_json_constant)
    tree_errors = _json_tree_errors(value)
    if tree_errors:
        raise DatasetValidationError([f"{path}: {error}" for error in tree_errors])
    if not isinstance(value, dict):
        raise DatasetValidationError([f"{path}: JSON root must be an object"])
    return value


def atomic_write_json(path: Path, value: dict[str, Any]) -> None:
    tree_errors = _json_tree_errors(value)
    if tree_errors:
        raise DatasetValidationError(tree_errors)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(
            value, stream, ensure_ascii=False, allow_nan=False, indent=2, sort_keys=True
        )
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def perceptual_hash_file(path: Path) -> str:
    with Image.open(path) as image:
        resized = image.convert("L").resize((9, 8), Image.Resampling.LANCZOS)
        pixels = (
            list(resized.get_flattened_data())
            if hasattr(resized, "get_flattened_data")
            else list(resized.getdata())
        )
    bits = 0
    for row in range(8):
        for column in range(8):
            bits = (bits << 1) | int(
                pixels[row * 9 + column] > pixels[row * 9 + column + 1]
            )
    return f"{bits:016x}"


@contextmanager
def dataset_lock(dataset_root: Path, timeout_seconds: float = 10.0):
    dataset_root.mkdir(parents=True, exist_ok=True)
    lock_path = dataset_root / ".dataset.lock"
    with lock_path.open("a+b") as stream:
        stream.seek(0, os.SEEK_END)
        if stream.tell() == 0:
            stream.write(b"\0")
            stream.flush()
        deadline = time.monotonic() + timeout_seconds
        if os.name == "nt":
            import msvcrt

            while True:
                try:
                    stream.seek(0)
                    msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError(
                            f"Timed out waiting for dataset lock: {lock_path}"
                        )
                    time.sleep(0.05)
            try:
                yield
            finally:
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl

            while True:
                try:
                    fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError(
                            f"Timed out waiting for dataset lock: {lock_path}"
                        )
                    time.sleep(0.05)
            try:
                yield
            finally:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def sample_path(dataset_root: Path, sample_id: str) -> Path:
    return dataset_root / "samples" / f"{sample_id}.json"


def iter_sample_paths(dataset_root: Path) -> list[Path]:
    samples_root = dataset_root / "samples"
    if not samples_root.exists():
        return []
    return sorted(samples_root.glob("*.json"))


def deterministic_split(split_group: str, seed: str = "visnip-ui-v1") -> str:
    digest = hashlib.sha256(f"{seed}\0{split_group}".encode("utf-8")).digest()
    bucket = int.from_bytes(digest[:4], byteorder="big") % 100
    if bucket < 70:
        return "train"
    if bucket < 85:
        return "validation"
    return "test"


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value: Any) -> bool:
    if isinstance(value, bool):
        return False
    if isinstance(value, int):
        return True
    return isinstance(value, float) and math.isfinite(value)


def _optional_string_errors(value: Any, prefix: str) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, str) or not value.strip():
        return [f"{prefix} must be a non-empty string or null"]
    return []


def canonical_split_identifier(value: str) -> str:
    return unicodedata.normalize("NFKC", value).strip().casefold()


def _box_errors(
    box: Any, prefix: str, width: int, height: int, *, nullable: bool = False
) -> list[str]:
    if box is None and nullable:
        return []
    if not isinstance(box, dict):
        return [f"{prefix} must be an object"]
    errors: list[str] = []
    for name in ("x", "y", "width", "height"):
        if not _is_int(box.get(name)):
            errors.append(f"{prefix}.{name} must be an integer")
    if errors:
        return errors
    x = box["x"]
    y = box["y"]
    box_width = box["width"]
    box_height = box["height"]
    if x < 0 or y < 0:
        errors.append(f"{prefix} origin must be non-negative")
    if box_width <= 0 or box_height <= 0:
        errors.append(f"{prefix} size must be positive")
    if x + box_width > width or y + box_height > height:
        errors.append(f"{prefix} exceeds image bounds {width}x{height}")
    return errors


def _screen_selection_errors(
    box: Any, prefix: str, image_width: int, image_height: int
) -> list[str]:
    if not isinstance(box, dict):
        return [f"{prefix} must be an object"]
    errors: list[str] = []
    for name in ("x", "y", "width", "height"):
        if not _is_int(box.get(name)):
            errors.append(f"{prefix}.{name} must be an integer")
    if errors:
        return errors
    if box["width"] <= 0 or box["height"] <= 0:
        errors.append(f"{prefix} size must be positive")
    if (box["width"], box["height"]) != (image_width, image_height):
        errors.append(f"{prefix} size must equal the source image dimensions")
    return errors


def _contains(outer: dict[str, int], inner: dict[str, int]) -> bool:
    return (
        outer["x"] <= inner["x"]
        and outer["y"] <= inner["y"]
        and outer["x"] + outer["width"] >= inner["x"] + inner["width"]
        and outer["y"] + outer["height"] >= inner["y"] + inner["height"]
    )


def _intersects(first: dict[str, int], second: dict[str, int]) -> bool:
    return max(first["x"], second["x"]) < min(
        first["x"] + first["width"], second["x"] + second["width"]
    ) and max(first["y"], second["y"]) < min(
        first["y"] + first["height"], second["y"] + second["height"]
    )


def _meaningfully_overlaps(first: dict[str, int], second: dict[str, int]) -> bool:
    overlap_width = max(
        0,
        min(first["x"] + first["width"], second["x"] + second["width"])
        - max(first["x"], second["x"]),
    )
    overlap_height = max(
        0,
        min(first["y"] + first["height"], second["y"] + second["height"])
        - max(first["y"], second["y"]),
    )
    overlap_area = overlap_width * overlap_height
    smaller_area = min(
        first["width"] * first["height"],
        second["width"] * second["height"],
    )
    return overlap_area >= smaller_area * MIN_PROPOSAL_ANCHOR_OVERLAP


def _cycle_errors(
    items: dict[str, dict[str, Any]], parent_field: str, prefix: str
) -> list[str]:
    errors: list[str] = []
    for item_id in items:
        visited: set[str] = set()
        current_id: str | None = item_id
        while current_id is not None and current_id in items:
            if current_id in visited:
                errors.append(f"{prefix} hierarchy contains a cycle at {current_id}")
                break
            visited.add(current_id)
            parent = items[current_id].get(parent_field)
            current_id = parent if isinstance(parent, str) else None
    return sorted(set(errors))


def _required_string_errors(value: object, prefix: str) -> list[str]:
    if not isinstance(value, str) or not value.strip():
        return [f"{prefix} must be a non-empty string"]
    return []


def _required_https_url_errors(value: object, prefix: str) -> list[str]:
    errors = _required_string_errors(value, prefix)
    if errors:
        return errors
    parsed = urlsplit(value)
    if parsed.scheme != "https" or not parsed.netloc:
        return [f"{prefix} must be an absolute HTTPS URL"]
    return []


def validate_dataset_metadata(metadata: dict[str, Any]) -> None:
    errors: list[str] = _json_tree_errors(metadata)
    if metadata.get("schemaVersion") != SCHEMA_VERSION:
        errors.append(f"dataset.schemaVersion must equal {SCHEMA_VERSION}")
    if (
        not isinstance(metadata.get("datasetId"), str)
        or not metadata["datasetId"].strip()
    ):
        errors.append("dataset.datasetId must be a non-empty string")
    if (
        not isinstance(metadata.get("splitSeed"), str)
        or not metadata["splitSeed"].strip()
    ):
        errors.append("dataset.splitSeed must be a non-empty string")
    if metadata.get("coordinateSystem") != COORDINATE_SYSTEM:
        errors.append(f"dataset.coordinateSystem must equal {COORDINATE_SYSTEM}")
    if metadata.get("rectEncoding") != RECT_ENCODING:
        errors.append(f"dataset.rectEncoding must equal {RECT_ENCODING}")
    if errors:
        raise DatasetValidationError(errors)


def validate_sample(sample: dict[str, Any], dataset_root: Path | None = None) -> None:
    errors: list[str] = _json_tree_errors(sample)
    if sample.get("schemaVersion") != SCHEMA_VERSION:
        errors.append(f"schemaVersion must equal {SCHEMA_VERSION}")
    sample_id = sample.get("sampleId")
    if not isinstance(sample_id, str) or not sample_id.strip():
        errors.append("sampleId must be a non-empty string")
    revision = sample.get("revision")
    if not _is_int(revision) or revision < 0:
        errors.append("revision must be a non-negative integer")
    split_group = sample.get("splitGroup")
    if not isinstance(split_group, str) or not split_group.strip():
        errors.append("splitGroup must be a non-empty string")
    elif split_group != canonical_split_identifier(split_group):
        errors.append("splitGroup must be NFKC-normalized, trimmed, and case-folded")
    elif not SPLIT_IDENTIFIER_PATTERN.fullmatch(split_group):
        errors.append("splitGroup must use lowercase ASCII identifier characters")
    if sample.get("splitGroupMethod") not in ("perceptual_hash", "manual"):
        errors.append("splitGroupMethod must be perceptual_hash or manual")
    coverage = sample.get("coverage")
    if not isinstance(coverage, dict):
        errors.append("coverage must be an object")
    else:
        for name in ("textComplete", "anchorsComplete", "protectedRegionsComplete"):
            if not isinstance(coverage.get(name), bool):
                errors.append(f"coverage.{name} must be boolean")
    split_key = sample.get("splitKey")
    if not isinstance(split_key, dict):
        errors.append("splitKey must be an object")
        split_key = {}
    for name in ("appFamily", "pageSession", "theme", "locale"):
        split_value = split_key.get(name)
        errors.extend(_optional_string_errors(split_value, f"splitKey.{name}"))
        if isinstance(split_value, str) and split_value != canonical_split_identifier(
            split_value
        ):
            errors.append(
                f"splitKey.{name} must be NFKC-normalized, trimmed, and case-folded"
            )
        elif isinstance(split_value, str) and not SPLIT_IDENTIFIER_PATTERN.fullmatch(
            split_value
        ):
            errors.append(
                f"splitKey.{name} must use lowercase ASCII identifier characters"
            )
    dpi_scale = split_key.get("dpiScale")
    if dpi_scale is not None and (not _is_number(dpi_scale) or dpi_scale <= 0):
        errors.append("splitKey.dpiScale must be a positive number or null")
    if not isinstance(split_key.get("independenceReviewed"), bool):
        errors.append("splitKey.independenceReviewed must be boolean")
    if split_key.get("pageSession") is not None and split_key.get("appFamily") is None:
        errors.append("splitKey.pageSession requires appFamily")

    image = sample.get("image")
    if not isinstance(image, dict):
        errors.append("image must be an object")
        image = {}
    image_path = image.get("path")
    width = image.get("width")
    height = image.get("height")
    image_sha = image.get("sha256")
    if not isinstance(image_path, str) or not image_path.startswith("images/"):
        errors.append("image.path must be a relative images/... path")
    elif Path(image_path).is_absolute() or ".." in Path(image_path).parts:
        errors.append("image.path must not escape the dataset root")
    if not _is_int(width) or width <= 0:
        errors.append("image.width must be a positive integer")
        width = 0
    if not _is_int(height) or height <= 0:
        errors.append("image.height must be a positive integer")
        height = 0
    if not isinstance(image_sha, str) or len(image_sha) != 64:
        errors.append("image.sha256 must be a 64-character digest")
    elif sample_id != image_sha:
        errors.append("sampleId must equal image.sha256")
    perceptual_hash = image.get("perceptualHash")
    if not isinstance(perceptual_hash, str) or len(perceptual_hash) != 16:
        errors.append("image.perceptualHash must be a 16-character digest")
    if image.get("coordinateSpace") != COORDINATE_SYSTEM:
        errors.append(f"image.coordinateSpace must equal {COORDINATE_SYSTEM}")
    if image.get("rectEncoding") != RECT_ENCODING:
        errors.append(f"image.rectEncoding must equal {RECT_ENCODING}")
    mime_type = image.get("mimeType")
    image_format = IMAGE_FORMATS.get(mime_type) if isinstance(mime_type, str) else None
    if image_format is None:
        errors.append("image.mimeType must be image/png or image/jpeg")
    elif (
        isinstance(image_path, str)
        and image_path != f"images/{sample_id}{image_format[0]}"
    ):
        errors.append(
            "image.path must be content-addressed with the canonical MIME extension"
        )
    if dataset_root is not None and isinstance(image_path, str):
        resolved_root = dataset_root.resolve()
        resolved_image = (dataset_root / image_path).resolve()
        try:
            resolved_image.relative_to(resolved_root)
        except ValueError:
            errors.append("image.path resolves outside the dataset root")
        else:
            if not resolved_image.is_file():
                errors.append(f"image file does not exist: {image_path}")
            else:
                try:
                    with Image.open(resolved_image) as image_file:
                        actual_width, actual_height = image_file.size
                        actual_format = image_file.format
                    if (actual_width, actual_height) != (width, height):
                        errors.append(
                            f"image dimensions differ from metadata: {actual_width}x{actual_height} != {width}x{height}"
                        )
                    actual_sha = sha256_file(resolved_image)
                    if actual_sha != image_sha:
                        errors.append(
                            f"image SHA-256 differs from metadata: {image_path}"
                        )
                    actual_perceptual_hash = perceptual_hash_file(resolved_image)
                    if actual_perceptual_hash != perceptual_hash:
                        errors.append(
                            f"image perceptual hash differs from metadata: {image_path}"
                        )
                    if image_format is not None and actual_format != image_format[1]:
                        errors.append(
                            f"image format differs from MIME metadata: {actual_format} != {image_format[1]}"
                        )
                except OSError as error:
                    errors.append(f"image cannot be decoded: {image_path}: {error}")

    sources = sample.get("sources")
    if not isinstance(sources, list) or not sources:
        errors.append("sources must be a non-empty array")
        sources = []
    source_ids: set[str] = set()
    for index, source in enumerate(sources):
        prefix = f"sources[{index}]"
        if not isinstance(source, dict):
            errors.append(f"{prefix} must be an object")
            continue
        source_kind = source.get("kind")
        if source_kind not in SOURCE_KINDS:
            errors.append(
                f"{prefix}.kind must be one of {', '.join(SOURCE_KINDS)}"
            )
        diagnostic_id = source.get("diagnosticId")
        if not isinstance(diagnostic_id, str) or not diagnostic_id.strip():
            errors.append(f"{prefix}.diagnosticId must be a non-empty string")
        elif diagnostic_id in source_ids:
            errors.append(f"{prefix}.diagnosticId duplicates {diagnostic_id}")
        else:
            source_ids.add(diagnostic_id)
        errors.extend(
            _optional_string_errors(source.get("createdAt"), f"{prefix}.createdAt")
        )
        status = source.get("status")
        if not isinstance(status, str) or not status.strip():
            errors.append(f"{prefix}.status must be a non-empty string")
        expected_source_coordinates = (
            "original-image-pixels"
            if source_kind == "external_screenshot"
            else "virtual-screen-pixels"
        )
        if source.get("selectionCoordinateSpace") != expected_source_coordinates:
            errors.append(
                f"{prefix}.selectionCoordinateSpace must be {expected_source_coordinates}"
            )
        errors.extend(
            _screen_selection_errors(
                source.get("selection"), f"{prefix}.selection", width, height
            )
        )
        for name in ("requestedOcrPackId", "resolvedOcrPackId", "ocrPackVersionKey"):
            errors.extend(_optional_string_errors(source.get(name), f"{prefix}.{name}"))
        if source_kind == "external_screenshot":
            for name in ("originUrl", "descriptionUrl", "licenseUrl"):
                errors.extend(
                    _required_https_url_errors(source.get(name), f"{prefix}.{name}")
                )
            for name in (
                "sourceTitle",
                "author",
                "licenseName",
                "retrievedAt",
                "category",
                "query",
            ):
                errors.extend(
                    _required_string_errors(source.get(name), f"{prefix}.{name}")
                )
            original_sha = source.get("originalSha256")
            if original_sha is not None and (
                not isinstance(original_sha, str)
                or len(original_sha) != 64
                or any(character not in "0123456789abcdef" for character in original_sha)
            ):
                errors.append(
                    f"{prefix}.originalSha256 must be a lowercase SHA-256 digest or null"
                )

    proposals = sample.get("ocrProposals")
    if not isinstance(proposals, list):
        errors.append("ocrProposals must be an array")
        proposals = []
    proposal_ids: set[str] = set()
    proposal_by_id: dict[str, dict[str, Any]] = {}
    observation_keys: set[tuple[str, int]] = set()
    for index, proposal in enumerate(proposals):
        prefix = f"ocrProposals[{index}]"
        if not isinstance(proposal, dict):
            errors.append(f"{prefix} must be an object")
            continue
        proposal_id = proposal.get("id")
        if not isinstance(proposal_id, str) or not proposal_id.strip():
            errors.append(f"{prefix}.id must be a non-empty string")
        elif proposal_id in proposal_ids:
            errors.append(f"{prefix}.id duplicates {proposal_id}")
        else:
            proposal_ids.add(proposal_id)
            proposal_by_id[proposal_id] = proposal
        errors.extend(
            _box_errors(proposal.get("textBox"), f"{prefix}.textBox", width, height)
        )
        observations = proposal.get("observations")
        if not isinstance(observations, list) or not observations:
            errors.append(f"{prefix}.observations must be a non-empty array")
            observations = []
        elif len(observations) != 1:
            errors.append(
                f"{prefix}.observations must contain exactly one raw OCR observation"
            )
        observation_by_key: dict[tuple[str, int], dict[str, Any]] = {}
        for observation_index, observation in enumerate(observations):
            observation_prefix = f"{prefix}.observations[{observation_index}]"
            if not isinstance(observation, dict):
                errors.append(f"{observation_prefix} must be an object")
                continue
            diagnostic_id = observation.get("diagnosticId")
            source_line_index = observation.get("sourceLineIndex")
            if not isinstance(diagnostic_id, str) or not diagnostic_id.strip():
                errors.append(
                    f"{observation_prefix}.diagnosticId must be a non-empty string"
                )
            elif diagnostic_id not in source_ids:
                errors.append(
                    f"{observation_prefix} references unknown source {diagnostic_id}"
                )
            if not _is_int(source_line_index) or source_line_index < 0:
                errors.append(
                    f"{observation_prefix}.sourceLineIndex must be a non-negative integer"
                )
            if isinstance(diagnostic_id, str) and _is_int(source_line_index):
                observation_key = (diagnostic_id, source_line_index)
                if observation_key in observation_keys:
                    errors.append(
                        f"OCR observation is duplicated: {diagnostic_id}/{source_line_index}"
                    )
                else:
                    observation_keys.add(observation_key)
                    observation_by_key[observation_key] = observation
            errors.extend(
                _box_errors(
                    observation.get("textBox"),
                    f"{observation_prefix}.textBox",
                    width,
                    height,
                )
            )
            errors.extend(
                _box_errors(
                    observation.get("detectedBox"),
                    f"{observation_prefix}.detectedBox",
                    width,
                    height,
                    nullable=True,
                )
            )
            if not isinstance(observation.get("recognizedText"), str):
                errors.append(f"{observation_prefix}.recognizedText must be a string")
            score = observation.get("recognitionScore")
            if not _is_number(score) or not 0.0 <= score <= 1.0:
                errors.append(
                    f"{observation_prefix}.recognitionScore must be between 0 and 1"
                )
            decision = observation.get("pipelineDecision")
            if not isinstance(decision, dict):
                errors.append(
                    f"{observation_prefix}.pipelineDecision must be an object"
                )
            else:
                for name in ("acceptedForTranslation", "rejectedFragmentByOcrFilter"):
                    if decision.get(name) is not None and not isinstance(
                        decision.get(name), bool
                    ):
                        errors.append(
                            f"{observation_prefix}.pipelineDecision.{name} must be boolean or null"
                        )
                for name in ("rejectionReason", "preservationReason"):
                    errors.extend(
                        _optional_string_errors(
                            decision.get(name),
                            f"{observation_prefix}.pipelineDecision.{name}",
                        )
                    )
            if "leadingIconSeparated" in observation and not isinstance(
                observation["leadingIconSeparated"], bool
            ):
                errors.append(
                    f"{observation_prefix}.leadingIconSeparated must be boolean"
                )
            errors.extend(
                _optional_string_errors(
                    observation.get("refinementReason"),
                    f"{observation_prefix}.refinementReason",
                )
            )
            for name in ("inkHeight", "backgroundLuma", "foregroundLuma", "contrast"):
                if name in observation and not _is_number(observation[name]):
                    errors.append(f"{observation_prefix}.{name} must be numeric")
        anchor = proposal.get("anchorObservation")
        if not isinstance(anchor, dict):
            errors.append(f"{prefix}.anchorObservation must be an object")
        else:
            anchor_diagnostic = anchor.get("diagnosticId")
            anchor_index = anchor.get("sourceLineIndex")
            if not isinstance(anchor_diagnostic, str) or not _is_int(anchor_index):
                anchor_observation = None
                errors.append(
                    f"{prefix}.anchorObservation must reference one of its observations"
                )
            else:
                anchor_observation = observation_by_key.get(
                    (anchor_diagnostic, anchor_index)
                )
                if anchor_observation is None:
                    errors.append(
                        f"{prefix}.anchorObservation must reference one of its observations"
                    )
            if anchor_observation is not None and (
                not _box_errors(proposal.get("textBox"), prefix, width, height)
                and not _box_errors(
                    anchor_observation.get("textBox"), prefix, width, height
                )
                and proposal["textBox"] != anchor_observation["textBox"]
            ):
                errors.append(
                    f"{prefix}.textBox must equal its anchor observation textBox"
                )

    containers = sample.get("layoutContainers")
    if not isinstance(containers, list):
        errors.append("layoutContainers must be an array")
        containers = []
    container_by_id: dict[str, dict[str, Any]] = {}
    for index, container in enumerate(containers):
        prefix = f"layoutContainers[{index}]"
        if not isinstance(container, dict):
            errors.append(f"{prefix} must be an object")
            continue
        container_id = container.get("id")
        if not isinstance(container_id, str) or not container_id.strip():
            errors.append(f"{prefix}.id must be a non-empty string")
        elif container_id in container_by_id:
            errors.append(f"{prefix}.id duplicates {container_id}")
        else:
            container_by_id[container_id] = container
        if container.get("role") not in CONTAINER_ROLES:
            errors.append(f"{prefix}.role is invalid")
        errors.extend(_box_errors(container.get("box"), f"{prefix}.box", width, height))
        parent_id = container.get("parentId")
        if parent_id is not None and not isinstance(parent_id, str):
            errors.append(f"{prefix}.parentId must be a string or null")
    for container_id, container in container_by_id.items():
        parent_id = container.get("parentId")
        if isinstance(parent_id, str):
            parent = container_by_id.get(parent_id)
            if parent is None:
                errors.append(
                    f"container {container_id} references unknown parent {parent_id}"
                )
            elif (
                not _box_errors(parent.get("box"), "parent", width, height)
                and not _box_errors(container.get("box"), "container", width, height)
                and not _contains(parent["box"], container["box"])
            ):
                errors.append(
                    f"container {parent_id} must contain child {container_id}"
                )
    errors.extend(_cycle_errors(container_by_id, "parentId", "container"))

    annotations = sample.get("annotations")
    if not isinstance(annotations, list):
        errors.append("annotations must be an array")
        annotations = []
    annotation_by_id: dict[str, dict[str, Any]] = {}
    referenced_proposals: set[str] = set()
    for index, annotation in enumerate(annotations):
        prefix = f"annotations[{index}]"
        if not isinstance(annotation, dict):
            errors.append(f"{prefix} must be an object")
            continue
        annotation_id = annotation.get("id")
        if not isinstance(annotation_id, str) or not annotation_id.strip():
            errors.append(f"{prefix}.id must be a non-empty string")
        elif annotation_id in annotation_by_id:
            errors.append(f"{prefix}.id duplicates {annotation_id}")
        else:
            annotation_by_id[annotation_id] = annotation
        textness = annotation.get("textness")
        role = annotation.get("role")
        policy = annotation.get("translationPolicy")
        status = annotation.get("labelStatus")
        relation = annotation.get("relation")
        patch_mode = annotation.get("patchMode")
        if textness not in TEXTNESS_VALUES:
            errors.append(f"{prefix}.textness is invalid")
        if policy not in TRANSLATION_POLICIES:
            errors.append(f"{prefix}.translationPolicy is invalid")
        if status not in LABEL_STATUSES:
            errors.append(f"{prefix}.labelStatus is invalid")
        if relation not in RELATIONS:
            errors.append(f"{prefix}.relation is invalid")
        if patch_mode not in PATCH_MODES:
            errors.append(f"{prefix}.patchMode is invalid")
        proposal_links = annotation.get("proposalIds")
        if not isinstance(proposal_links, list) or any(
            not isinstance(value, str) for value in proposal_links
        ):
            errors.append(f"{prefix}.proposalIds must be a string array")
            proposal_links = []
        elif len(set(proposal_links)) != len(proposal_links):
            errors.append(f"{prefix}.proposalIds must not contain duplicates")
        for proposal_id in proposal_links:
            if proposal_id not in proposal_ids:
                errors.append(f"{prefix} references unknown proposal {proposal_id}")
            if proposal_id in referenced_proposals:
                errors.append(
                    f"proposal {proposal_id} is linked by multiple annotations"
                )
            referenced_proposals.add(proposal_id)
        annotation_box_errors = _box_errors(
            annotation.get("textBox"), f"{prefix}.textBox", width, height
        )
        errors.extend(annotation_box_errors)
        if not annotation_box_errors:
            for proposal_id in proposal_links:
                proposal = proposal_by_id.get(proposal_id)
                if proposal is None or _box_errors(
                    proposal.get("textBox"), prefix, width, height
                ):
                    continue
                if not _meaningfully_overlaps(
                    annotation["textBox"], proposal["textBox"]
                ):
                    errors.append(
                        f"{prefix}.textBox must meaningfully overlap linked proposal {proposal_id}"
                    )
        attributes = annotation.get("attributes")
        if not isinstance(attributes, dict):
            errors.append(f"{prefix}.attributes must be an object")
            attributes = {}
        for name in ("illegible", "truncated"):
            if not isinstance(attributes.get(name), bool):
                errors.append(f"{prefix}.attributes.{name} must be boolean")
        group_id = annotation.get("groupId")
        errors.extend(_optional_string_errors(group_id, f"{prefix}.groupId"))
        reading_order = annotation.get("readingOrder")
        if reading_order is not None and (
            not _is_int(reading_order) or reading_order < 0
        ):
            errors.append(
                f"{prefix}.readingOrder must be a non-negative integer or null"
            )
        parent_annotation_id = annotation.get("parentAnnotationId")
        if parent_annotation_id is not None and not isinstance(
            parent_annotation_id, str
        ):
            errors.append(f"{prefix}.parentAnnotationId must be a string or null")
        container_id = annotation.get("containerId")
        if container_id is not None:
            if not isinstance(container_id, str):
                errors.append(f"{prefix}.containerId must be a string or null")
            elif container_id not in container_by_id:
                errors.append(f"{prefix} references unknown container {container_id}")

        transcription = annotation.get("transcription")
        mask_box = annotation.get("maskBox")
        layout_box = annotation.get("layoutBox")
        if status == "verified":
            if not proposal_links and relation != "none":
                errors.append(
                    f"{prefix} manual regions without OCR proposals require relation none"
                )
            if proposal_links and relation == "single" and len(proposal_links) != 1:
                errors.append(
                    f"{prefix}.single relation requires exactly one OCR proposal"
                )
        if status in ("unreviewed", "ignored"):
            if textness != "unknown" or role is not None or policy != "unknown":
                errors.append(
                    f"{prefix} unfinished labels require unknown textness, null role, and unknown policy"
                )
            if relation != "unknown" or patch_mode != "unknown":
                errors.append(
                    f"{prefix} unfinished labels require unknown relation and patch mode"
                )
            if (
                transcription is not None
                or mask_box is not None
                or layout_box is not None
            ):
                errors.append(
                    f"{prefix} unfinished labels cannot carry translation targets"
                )
        elif textness == "unknown":
            errors.append(f"{prefix} verified labels cannot have unknown textness")
        elif textness == "text":
            if role not in TEXT_ROLES:
                errors.append(f"{prefix}.role must be a text role")
            if policy not in ("translate", "preserve", "review"):
                errors.append(f"{prefix}.translationPolicy is invalid for text")
            if relation not in ("single", "split_required", "merge_required", "none"):
                errors.append(f"{prefix}.relation is invalid for text")
            if patch_mode not in ("rect_safe", "mask_required", "none"):
                errors.append(f"{prefix}.patchMode is invalid for text")
            if not attributes.get("illegible") and (
                not isinstance(transcription, str) or not transcription.strip()
            ):
                errors.append(f"{prefix}.transcription is required for legible text")
            if patch_mode == "rect_safe":
                if relation != "single":
                    errors.append(f"{prefix}.rect_safe requires a single relation")
                errors.extend(_box_errors(mask_box, f"{prefix}.maskBox", width, height))
                errors.extend(
                    _box_errors(layout_box, f"{prefix}.layoutBox", width, height)
                )
                boxes_are_valid = all(
                    isinstance(box, dict)
                    and not _box_errors(box, prefix, width, height)
                    for box in (annotation.get("textBox"), mask_box, layout_box)
                )
                if boxes_are_valid and not _contains(mask_box, annotation["textBox"]):
                    errors.append(f"{prefix}.maskBox must contain textBox")
                if boxes_are_valid and not _intersects(
                    layout_box, annotation["textBox"]
                ):
                    errors.append(f"{prefix}.layoutBox must intersect textBox")
                if boxes_are_valid and not _contains(layout_box, mask_box):
                    errors.append(f"{prefix}.layoutBox must contain maskBox")
                if boxes_are_valid:
                    for proposal_id in proposal_links:
                        proposal = proposal_by_id.get(proposal_id)
                        if proposal is None or _box_errors(
                            proposal.get("textBox"), prefix, width, height
                        ):
                            continue
                        model_context = context_crop_bounds(proposal["textBox"])
                        if not box_inside_bounds(mask_box, model_context):
                            errors.append(
                                f"{prefix}.maskBox exceeds proposal {proposal_id} model context"
                            )
                        if not box_inside_bounds(layout_box, model_context):
                            errors.append(
                                f"{prefix}.layoutBox exceeds proposal {proposal_id} model context"
                            )
            elif mask_box is not None or layout_box is not None:
                errors.append(
                    f"{prefix} non-rect patch modes cannot carry rectangle targets"
                )
        elif textness == "non_text":
            if role not in NON_TEXT_ROLES:
                errors.append(f"{prefix}.role must be a non-text role")
            if policy != "preserve" or relation != "none" or patch_mode != "none":
                errors.append(f"{prefix}.translationPolicy is invalid for non-text")
            if (
                transcription is not None
                or mask_box is not None
                or layout_box is not None
            ):
                errors.append(
                    f"{prefix} non-text labels cannot carry translation targets"
                )
        elif textness == "mixed":
            if (
                role != "mixed_content"
                or policy not in ("preserve", "review")
                or relation != "split_required"
                or patch_mode not in ("mask_required", "none")
            ):
                errors.append(
                    f"{prefix} mixed labels require mixed_content and preserve/review"
                )
            if (
                transcription is not None
                or mask_box is not None
                or layout_box is not None
            ):
                errors.append(
                    f"{prefix} mixed labels use child annotations for text targets"
                )
        if isinstance(container_id, str) and container_id in container_by_id:
            container_box = container_by_id[container_id].get("box")
            if _box_errors(container_box, "container", width, height):
                continue
            for name in ("textBox", "maskBox", "layoutBox"):
                box = annotation.get(name)
                if isinstance(box, dict) and not _box_errors(
                    box, prefix, width, height
                ):
                    if not _contains(container_box, box):
                        errors.append(
                            f"container {container_id} must contain {prefix}.{name}"
                        )

    for annotation_id, annotation in annotation_by_id.items():
        parent_id = annotation.get("parentAnnotationId")
        if parent_id is None:
            continue
        if not isinstance(parent_id, str):
            continue
        parent = annotation_by_id.get(parent_id)
        if parent is None:
            errors.append(
                f"annotation {annotation_id} references unknown parent {parent_id}"
            )
        elif parent.get("textness") != "mixed":
            errors.append(f"annotation parent {parent_id} must have mixed textness")
        elif (
            not _box_errors(parent.get("textBox"), "parent", width, height)
            and not _box_errors(annotation.get("textBox"), "child", width, height)
            and not _contains(parent["textBox"], annotation["textBox"])
        ):
            errors.append(
                f"mixed annotation {parent_id} must contain child {annotation_id}"
            )
    errors.extend(_cycle_errors(annotation_by_id, "parentAnnotationId", "annotation"))

    for annotation_id, annotation in annotation_by_id.items():
        if (
            annotation.get("labelStatus") != "verified"
            or annotation.get("relation") != "merge_required"
        ):
            continue
        raw_proposal_ids = annotation.get("proposalIds")
        linked_proposals = (
            {value for value in raw_proposal_ids if isinstance(value, str)}
            if isinstance(raw_proposal_ids, list)
            else set()
        )
        group_id = annotation.get("groupId")
        if isinstance(group_id, str) and group_id.strip():
            for peer in annotations:
                if (
                    isinstance(peer, dict)
                    and peer.get("labelStatus") == "verified"
                    and peer.get("relation") == "merge_required"
                    and peer.get("groupId") == group_id
                    and isinstance(peer.get("proposalIds"), list)
                ):
                    linked_proposals.update(
                        value for value in peer["proposalIds"] if isinstance(value, str)
                    )
        if len(linked_proposals) < 2:
            errors.append(
                f"verified merge annotation {annotation_id} requires two linked proposals or a shared merge group"
            )

    for annotation_id, annotation in annotation_by_id.items():
        if (
            annotation.get("textness") != "mixed"
            or annotation.get("labelStatus") != "verified"
        ):
            continue
        child_types = {
            child_textness
            for child in annotations
            if isinstance(child, dict)
            and child.get("parentAnnotationId") == annotation_id
            for child_textness in (child.get("textness"),)
            if isinstance(child_textness, str)
        }
        if not {"text", "non_text"}.issubset(child_types):
            errors.append(
                f"verified mixed annotation {annotation_id} requires text and non-text children"
            )

    unlinked_proposals = sorted(proposal_ids - referenced_proposals)
    if unlinked_proposals:
        errors.append(
            f"OCR proposals without annotations: {', '.join(unlinked_proposals)}"
        )

    if errors:
        raise DatasetValidationError(errors)


def summarize_sample(sample: dict[str, Any], split_seed: str) -> dict[str, Any]:
    annotations = sample.get("annotations", [])
    verified = [item for item in annotations if item.get("labelStatus") == "verified"]
    ignored = [item for item in annotations if item.get("labelStatus") == "ignored"]
    latest_source = sample.get("sources", [{}])[-1]
    return {
        "sampleId": sample["sampleId"],
        "image": sample["image"],
        "revision": sample["revision"],
        "splitGroup": sample["splitGroup"],
        "split": deterministic_split(sample["splitGroup"], split_seed),
        "sourceLabel": latest_source.get("diagnosticId", sample["sampleId"][:12]),
        "createdAt": sample.get("createdAt"),
        "updatedAt": sample.get("updatedAt"),
        "counts": {
            "total": len(annotations),
            "verified": len(verified),
            "ignored": len(ignored),
            "resolved": len(verified) + len(ignored),
            "text": sum(item.get("textness") == "text" for item in verified),
            "nonText": sum(item.get("textness") == "non_text" for item in verified),
            "mixed": sum(item.get("textness") == "mixed" for item in verified),
            "unreviewed": sum(
                item.get("labelStatus") == "unreviewed" for item in annotations
            ),
        },
    }
