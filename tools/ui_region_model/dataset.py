from __future__ import annotations

import io
import random
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import torch
from PIL import Image, ImageEnhance
from torch.utils.data import Dataset

from .geometry import context_crop_bounds, normalized_cxcywh
from .model import (
    PATCH_MODE_CLASSES,
    RELATION_CLASSES,
    ROLE_CLASSES,
    TEXTNESS_CLASSES,
)
from .preprocess import prepare_region_input
from .validate_dataset import load_manifest_sample


ROLE_TO_COARSE = {
    "title": "heading",
    "heading": "heading",
    "body": "paragraph",
    "menu_item": "ui_label",
    "label": "ui_label",
    "value": "ui_label",
    "button": "ui_label",
    "link": "ui_label",
    "tab": "ui_label",
    "badge": "ui_label",
    "caption": "caption_metadata",
    "metadata": "caption_metadata",
    "date_number": "caption_metadata",
    "code": "data_code",
    "identifier": "data_code",
    "other_text": "other",
}


@dataclass(frozen=True)
class RegionRecord:
    image_bytes: bytes
    sample_id: str
    split_group: str
    proposal: dict[str, Any]
    annotation: dict[str, Any]


def build_records(
    dataset_root: Path, split: str, dataset_manifest: dict[str, Any]
) -> list[RegionRecord]:
    records: list[RegionRecord] = []
    entries = [
        entry for entry in dataset_manifest["samples"] if entry["split"] == split
    ]
    for entry in entries:
        sample, image_bytes = load_manifest_sample(dataset_root, entry)
        proposals = {proposal["id"]: proposal for proposal in sample["ocrProposals"]}
        for annotation in sample["annotations"]:
            if annotation["labelStatus"] != "verified":
                continue
            for proposal_id in annotation["proposalIds"]:
                records.append(
                    RegionRecord(
                        image_bytes,
                        sample["sampleId"],
                        sample["splitGroup"],
                        proposals[proposal_id],
                        annotation,
                    )
                )
    return records


class UiRegionDataset(Dataset):
    def __init__(
        self,
        dataset_root: Path,
        split: str,
        dataset_manifest: dict[str, Any],
        augment: bool = False,
    ):
        self.records = build_records(dataset_root, split, dataset_manifest)
        self.augment = augment
        groups = sorted({record.split_group for record in self.records})
        self.group_indices = {group: index for index, group in enumerate(groups)}

    def __len__(self) -> int:
        return len(self.records)

    def __getitem__(self, index: int) -> tuple[torch.Tensor, dict[str, torch.Tensor]]:
        record = self.records[index]
        annotation = record.annotation
        with Image.open(io.BytesIO(record.image_bytes)) as source:
            image = source.convert("RGB")
        if self.augment:
            image = ImageEnhance.Brightness(image).enhance(random.uniform(0.82, 1.18))
            image = ImageEnhance.Contrast(image).enhance(random.uniform(0.82, 1.18))

        proposal_box = record.proposal["textBox"]
        context_bounds = context_crop_bounds(proposal_box)
        inputs = torch.from_numpy(prepare_region_input(image, proposal_box))

        textness_name = {
            "text": "text_only",
            "mixed": "mixed",
            "non_text": "non_text",
        }[annotation["textness"]]
        coarse_role = ROLE_TO_COARSE.get(annotation["role"])
        geometry_valid = annotation["patchMode"] == "rect_safe"
        erase_box = (
            normalized_cxcywh(annotation["maskBox"], context_bounds)
            if geometry_valid
            else [0.0] * 4
        )
        layout_box = (
            normalized_cxcywh(annotation["layoutBox"], context_bounds)
            if geometry_valid
            else [0.0] * 4
        )
        targets = {
            "textness": torch.tensor(
                TEXTNESS_CLASSES.index(textness_name), dtype=torch.long
            ),
            "role": torch.tensor(
                ROLE_CLASSES.index(coarse_role) if coarse_role else -1, dtype=torch.long
            ),
            "relation": torch.tensor(
                RELATION_CLASSES.index(annotation["relation"]), dtype=torch.long
            ),
            "patchMode": torch.tensor(
                PATCH_MODE_CLASSES.index(annotation["patchMode"]), dtype=torch.long
            ),
            "eraseBox": torch.tensor(erase_box, dtype=torch.float32),
            "layoutBox": torch.tensor(layout_box, dtype=torch.float32),
            "geometryValid": torch.tensor(geometry_valid, dtype=torch.bool),
            "cluster": torch.tensor(
                self.group_indices[record.split_group], dtype=torch.long
            ),
            "translationPolicy": torch.tensor(
                ("translate", "preserve", "review").index(
                    annotation["translationPolicy"]
                ),
                dtype=torch.long,
            ),
        }
        return inputs, targets
