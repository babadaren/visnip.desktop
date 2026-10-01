from __future__ import annotations

import argparse
import copy
import json
from collections import Counter
from pathlib import Path
from statistics import median
from typing import Any

from .geometry import context_crop_bounds
from .schema import atomic_write_json, read_json, utc_timestamp, validate_sample


GAME_CATEGORY = "open_source_game"
CODE_MARKERS = ("://", "\\", "/", "::", "{}", "[]", "()", ".exe", ".dll")


def _external_source(sample: dict[str, Any]) -> dict[str, Any] | None:
    return next(
        (
            source
            for source in sample.get("sources", [])
            if isinstance(source, dict) and source.get("kind") == "external_screenshot"
        ),
        None,
    )


def _recognized_text(proposal: dict[str, Any]) -> str:
    observation = proposal["observations"][0]
    return " ".join(str(observation["recognizedText"]).split())


def _expand(
    box: dict[str, int], width: int, height: int, pad_x: int, pad_y: int
) -> dict[str, int]:
    context_left, context_top, context_right, context_bottom = context_crop_bounds(box)
    left = max(0, context_left, box["x"] - pad_x)
    top = max(0, context_top, box["y"] - pad_y)
    right = min(width, context_right, box["x"] + box["width"] + pad_x)
    bottom = min(height, context_bottom, box["y"] + box["height"] + pad_y)
    return {
        "x": left,
        "y": top,
        "width": max(1, right - left),
        "height": max(1, bottom - top),
    }


def _role(
    text: str,
    box: dict[str, int],
    image_width: int,
    image_height: int,
    median_height: float,
    category: str,
) -> tuple[str, str]:
    letters = sum(character.isalpha() for character in text)
    digits = sum(character.isdigit() for character in text)
    if digits and not letters:
        return "date_number", "preserve"
    lower = text.casefold()
    if any(marker in lower for marker in CODE_MARKERS):
        return "code", "preserve"
    if box["height"] >= max(22, median_height * 1.4) and len(text) <= 60:
        return ("title" if box["y"] < image_height * 0.22 else "heading"), "translate"
    if len(text) >= 45 or box["width"] >= image_width * 0.48:
        return "body", "translate"
    if category == GAME_CATEGORY:
        return "other_text", "translate"
    if box["x"] < image_width * 0.25 and box["width"] < image_width * 0.36:
        return "menu_item", "translate"
    return "label", "translate"


def _bootstrap_annotation(
    annotation: dict[str, Any],
    proposal: dict[str, Any],
    image_width: int,
    image_height: int,
    median_height: float,
    category: str,
    reading_order: int,
) -> None:
    box = copy.deepcopy(proposal["textBox"])
    text = _recognized_text(proposal)
    alphanumeric = sum(character.isalnum() for character in text)
    common = {
        "attributes": {"illegible": False, "truncated": False},
        "containerId": None,
        "groupId": None,
        "labelStatus": "verified",
        "layoutBox": None,
        "maskBox": None,
        "parentAnnotationId": None,
        "readingOrder": reading_order,
        "textBox": box,
    }
    if not text or alphanumeric == 0:
        annotation.update(
            {
                **common,
                "patchMode": "none",
                "relation": "none",
                "role": "icon",
                "textness": "non_text",
                "transcription": None,
                "translationPolicy": "preserve",
            }
        )
        return

    role, policy = _role(
        text,
        box,
        image_width,
        image_height,
        median_height,
        category,
    )
    patch_mode = "mask_required" if category == GAME_CATEGORY else "rect_safe"
    annotation.update(
        {
            **common,
            "patchMode": patch_mode,
            "relation": "single",
            "role": role,
            "textness": "text",
            "transcription": text,
            "translationPolicy": policy,
        }
    )
    if patch_mode == "rect_safe":
        mask_padding = max(1, box["height"] // 12)
        layout_padding = max(mask_padding, box["height"] // 3)
        annotation["maskBox"] = _expand(
            box, image_width, image_height, mask_padding, mask_padding
        )
        annotation["layoutBox"] = _expand(
            box,
            image_width,
            image_height,
            layout_padding,
            max(mask_padding, layout_padding // 2),
        )


def bootstrap(dataset_root: Path) -> dict[str, Any]:
    samples = 0
    annotations = 0
    counts: Counter[str] = Counter()
    for sample_path in sorted((dataset_root / "samples").glob("*.json")):
        sample = read_json(sample_path)
        source = _external_source(sample)
        if source is None:
            continue
        proposals = sample["ocrProposals"]
        by_id = {proposal["id"]: proposal for proposal in proposals}
        heights = [proposal["textBox"]["height"] for proposal in proposals]
        median_height = median(heights) if heights else 1.0
        reading_order = 0
        for annotation in sample["annotations"]:
            if not annotation["proposalIds"]:
                continue
            if len(annotation["proposalIds"]) != 1:
                raise ValueError(
                    f"Bootstrap only supports one proposal per annotation: {annotation['id']}"
                )
            proposal = by_id[annotation["proposalIds"][0]]
            _bootstrap_annotation(
                annotation,
                proposal,
                sample["image"]["width"],
                sample["image"]["height"],
                median_height,
                str(source["category"]),
                reading_order,
            )
            reading_order += 1
            annotations += 1
            counts[f"textness:{annotation['textness']}"] += 1
            counts[f"role:{annotation['role']}"] += 1
            counts[f"patchMode:{annotation['patchMode']}"] += 1
        sample["revision"] += 1
        sample["updatedAt"] = utc_timestamp()
        validate_sample(sample, dataset_root)
        atomic_write_json(sample_path, sample)
        samples += 1
    summary = {
        "samples": samples,
        "annotations": annotations,
        "counts": dict(sorted(counts.items())),
        "coverageMarkedComplete": False,
    }
    print(json.dumps(summary, ensure_ascii=False))
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Create conservative draft labels for human review of external samples."
    )
    parser.add_argument("--dataset", type=Path, required=True)
    arguments = parser.parse_args()
    bootstrap(arguments.dataset.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
