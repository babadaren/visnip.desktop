from __future__ import annotations

import argparse
import inspect
import json
import math
import random
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import numpy as np
import onnx
import onnxruntime
import torch
import torch.nn.functional as functional
from torch import nn
from torch.utils.data import DataLoader

from . import MODEL_CONTRACT_VERSION
from .dataset import ROLE_TO_COARSE, UiRegionDataset
from .model import (
    ERASE_BOX_SLICE,
    LAYOUT_BOX_SLICE,
    PATCH_MODE_CLASSES,
    PATCH_MODE_SLICE,
    RELATION_CLASSES,
    RELATION_SLICE,
    ROLE_CLASSES,
    ROLE_SLICE,
    TEXTNESS_CLASSES,
    TEXTNESS_SLICE,
    UiRegionNet,
    parameter_count,
)
from .model_contract_constants import (
    INPUT_CHANNELS,
    INPUT_HEIGHT,
    INPUT_WIDTH,
    OUTPUT_WIDTH,
)
from .release_gate import (
    MIN_CLASS_GROUP_SUPPORT,
    MIN_CLASS_UNIT_SUPPORT,
    statistical_gate_eligible,
    validate_statistical_evidence,
    wilson_lower,
    wilson_upper,
)
from .schema import atomic_write_json, dataset_lock, sha256_file
from .validate_dataset import build_dataset_manifest, validate_dataset


def seed_everything(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)
        torch.backends.cudnn.benchmark = False
        torch.backends.cudnn.deterministic = True
    torch.use_deterministic_algorithms(True)


def focal_cross_entropy(
    logits: torch.Tensor, targets: torch.Tensor, gamma: float = 2.0
) -> torch.Tensor:
    cross_entropy = functional.cross_entropy(logits, targets, reduction="none")
    probability = torch.exp(-cross_entropy)
    return ((1.0 - probability) ** gamma * cross_entropy).mean()


def cxcywh_to_xyxy(boxes: torch.Tensor) -> torch.Tensor:
    center_x, center_y, width, height = boxes.unbind(dim=-1)
    return torch.stack(
        (
            center_x - width / 2,
            center_y - height / 2,
            center_x + width / 2,
            center_y + height / 2,
        ),
        dim=-1,
    )


def box_iou(predicted: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
    predicted_xyxy = cxcywh_to_xyxy(predicted)
    target_xyxy = cxcywh_to_xyxy(target)
    intersection_min = torch.maximum(predicted_xyxy[:, :2], target_xyxy[:, :2])
    intersection_max = torch.minimum(predicted_xyxy[:, 2:], target_xyxy[:, 2:])
    intersection = (intersection_max - intersection_min).clamp(min=0).prod(dim=1)
    predicted_area = (
        (predicted_xyxy[:, 2:] - predicted_xyxy[:, :2]).clamp(min=0).prod(dim=1)
    )
    target_area = (target_xyxy[:, 2:] - target_xyxy[:, :2]).clamp(min=0).prod(dim=1)
    union = predicted_area + target_area - intersection
    return intersection / union.clamp(min=1e-7)


def generalized_iou_loss(predicted: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
    predicted_xyxy = cxcywh_to_xyxy(predicted)
    target_xyxy = cxcywh_to_xyxy(target)
    intersection_min = torch.maximum(predicted_xyxy[:, :2], target_xyxy[:, :2])
    intersection_max = torch.minimum(predicted_xyxy[:, 2:], target_xyxy[:, 2:])
    intersection = (intersection_max - intersection_min).clamp(min=0).prod(dim=1)
    predicted_area = (
        (predicted_xyxy[:, 2:] - predicted_xyxy[:, :2]).clamp(min=0).prod(dim=1)
    )
    target_area = (target_xyxy[:, 2:] - target_xyxy[:, :2]).clamp(min=0).prod(dim=1)
    union = predicted_area + target_area - intersection
    enclosing_min = torch.minimum(predicted_xyxy[:, :2], target_xyxy[:, :2])
    enclosing_max = torch.maximum(predicted_xyxy[:, 2:], target_xyxy[:, 2:])
    enclosing_area = (enclosing_max - enclosing_min).clamp(min=0).prod(dim=1)
    generalized_iou = intersection / union.clamp(min=1e-7) - (
        enclosing_area - union
    ) / enclosing_area.clamp(min=1e-7)
    return (1.0 - generalized_iou).mean()


def compute_loss(
    predictions: torch.Tensor, targets: dict[str, torch.Tensor]
) -> tuple[torch.Tensor, dict[str, float]]:
    textness_loss = focal_cross_entropy(
        predictions[:, TEXTNESS_SLICE], targets["textness"]
    )
    relation_loss = focal_cross_entropy(
        predictions[:, RELATION_SLICE], targets["relation"]
    )
    patch_loss = focal_cross_entropy(
        predictions[:, PATCH_MODE_SLICE], targets["patchMode"]
    )

    role_mask = targets["role"] >= 0
    if role_mask.any():
        role_loss = focal_cross_entropy(
            predictions[role_mask, ROLE_SLICE], targets["role"][role_mask]
        )
    else:
        role_loss = predictions.sum() * 0.0

    geometry_mask = targets["geometryValid"]
    if geometry_mask.any():
        predicted_erase = predictions[geometry_mask, ERASE_BOX_SLICE]
        predicted_layout = predictions[geometry_mask, LAYOUT_BOX_SLICE]
        target_erase = targets["eraseBox"][geometry_mask]
        target_layout = targets["layoutBox"][geometry_mask]
        regression_loss = functional.smooth_l1_loss(
            predicted_erase, target_erase
        ) + functional.smooth_l1_loss(predicted_layout, target_layout)
        geometry_loss = (
            regression_loss
            + generalized_iou_loss(predicted_erase, target_erase)
            + generalized_iou_loss(predicted_layout, target_layout)
        )
    else:
        geometry_loss = predictions.sum() * 0.0

    total = (
        textness_loss
        + 0.7 * role_loss
        + 0.8 * relation_loss
        + 0.8 * patch_loss
        + 1.5 * geometry_loss
    )
    components = {
        "total": float(total.detach()),
        "textness": float(textness_loss.detach()),
        "role": float(role_loss.detach()),
        "relation": float(relation_loss.detach()),
        "patchMode": float(patch_loss.detach()),
        "geometry": float(geometry_loss.detach()),
    }
    return total, components


def move_targets(
    targets: dict[str, torch.Tensor], device: torch.device
) -> dict[str, torch.Tensor]:
    return {
        name: value.to(device, non_blocking=True) for name, value in targets.items()
    }


def run_training_epoch(
    model: nn.Module,
    loader: DataLoader,
    optimizer: torch.optim.Optimizer,
    device: torch.device,
) -> dict[str, float]:
    model.train()
    totals: dict[str, float] = {}
    batches = 0
    for inputs, targets in loader:
        inputs = inputs.to(device, non_blocking=True)
        targets = move_targets(targets, device)
        optimizer.zero_grad(set_to_none=True)
        predictions = model(inputs)
        loss, components = compute_loss(predictions, targets)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), max_norm=5.0)
        optimizer.step()
        for name, value in components.items():
            totals[name] = totals.get(name, 0.0) + value
        batches += 1
    return {name: value / max(1, batches) for name, value in totals.items()}


def _classification_metrics(
    targets: list[int], predictions: list[int], clusters: list[int], class_count: int
) -> dict[str, Any]:
    confusion_matrix = [[0] * class_count for _ in range(class_count)]
    for target, prediction in zip(targets, predictions):
        confusion_matrix[target][prediction] += 1
    scores: list[float | None] = []
    support: list[int] = []
    group_support: list[int] = []
    for class_index in range(class_count):
        class_support = sum(target == class_index for target in targets)
        support.append(class_support)
        group_support.append(
            len(
                {
                    cluster
                    for target, cluster in zip(targets, clusters)
                    if target == class_index
                }
            )
        )
        true_positive = sum(
            t == class_index and p == class_index for t, p in zip(targets, predictions)
        )
        false_positive = sum(
            t != class_index and p == class_index for t, p in zip(targets, predictions)
        )
        false_negative = sum(
            t == class_index and p != class_index for t, p in zip(targets, predictions)
        )
        denominator = 2 * true_positive + false_positive + false_negative
        scores.append(2 * true_positive / denominator if denominator else None)
    evaluable = all(value >= MIN_CLASS_UNIT_SUPPORT for value in support) and all(
        value >= MIN_CLASS_GROUP_SUPPORT for value in group_support
    )
    return {
        "macroF1": sum(value for value in scores if value is not None) / class_count
        if evaluable
        else None,
        "perClassF1": scores,
        "confusionMatrix": confusion_matrix,
        "support": support,
        "splitGroupSupport": group_support,
        "minimumUnitSupport": MIN_CLASS_UNIT_SUPPORT,
        "minimumSplitGroupSupport": MIN_CLASS_GROUP_SUPPORT,
        "evaluable": evaluable,
    }


def _percentile(values: list[float], quantile: float) -> float | None:
    if not values:
        return None
    return float(
        np.quantile(np.asarray(values, dtype=np.float64), quantile, method="linear")
    )


def _geometry_safety(
    predicted: torch.Tensor, target: torch.Tensor
) -> tuple[torch.Tensor, torch.Tensor]:
    predicted_xyxy = cxcywh_to_xyxy(predicted)
    target_xyxy = cxcywh_to_xyxy(target)
    intersection_min = torch.maximum(predicted_xyxy[:, :2], target_xyxy[:, :2])
    intersection_max = torch.minimum(predicted_xyxy[:, 2:], target_xyxy[:, 2:])
    intersection = (intersection_max - intersection_min).clamp(min=0).prod(dim=1)
    predicted_area = (
        (predicted_xyxy[:, 2:] - predicted_xyxy[:, :2]).clamp(min=0).prod(dim=1)
    )
    target_area = (target_xyxy[:, 2:] - target_xyxy[:, :2]).clamp(min=0).prod(dim=1)
    coverage = intersection / target_area.clamp(min=1e-7)
    spill = (predicted_area - intersection).clamp(min=0) / predicted_area.clamp(
        min=1e-7
    )
    return coverage, spill


@torch.no_grad()
def evaluate(
    model: nn.Module,
    loader: DataLoader,
    device: torch.device,
    textness_threshold: float,
    relation_threshold: float,
    patch_threshold: float,
    inference_engine: str = "pytorch",
) -> dict[str, Any]:
    model.eval()
    loss_totals: dict[str, float] = {}
    batches = 0
    truth: dict[str, list[int]] = {
        "textness": [],
        "role": [],
        "relation": [],
        "patchMode": [],
    }
    predicted: dict[str, list[int]] = {
        "textness": [],
        "role": [],
        "relation": [],
        "patchMode": [],
    }
    truth_clusters: dict[str, list[int]] = {
        "textness": [],
        "role": [],
        "relation": [],
        "patchMode": [],
    }
    erase_ious: list[float] = []
    layout_ious: list[float] = []
    geometry_clusters: dict[int, dict[str, list[float]]] = {}
    slice_counts = {
        "nonText": 0,
        "nonTextFalseAccepts": 0,
        "mixed": 0,
        "mixedFalseAccepts": 0,
        "unsafeText": 0,
        "unsafeTextFalseAccepts": 0,
    }
    cluster_safety: dict[int, dict[str, dict[str, bool]]] = {}
    safe_text_count = 0
    accepted_safe_text = 0
    accepted_by_policy = {"translate": 0, "preserve": 0, "review": 0}

    for inputs, targets in loader:
        inputs = inputs.to(device, non_blocking=True)
        targets = move_targets(targets, device)
        outputs = model(inputs)
        _, components = compute_loss(outputs, targets)
        for name, value in components.items():
            loss_totals[name] = loss_totals.get(name, 0.0) + value
        batches += 1

        slices = {
            "textness": TEXTNESS_SLICE,
            "role": ROLE_SLICE,
            "relation": RELATION_SLICE,
            "patchMode": PATCH_MODE_SLICE,
        }
        for name, value_slice in slices.items():
            target_values = targets[name]
            prediction_values = outputs[:, value_slice].argmax(dim=1)
            mask = target_values >= 0
            truth[name].extend(target_values[mask].cpu().tolist())
            predicted[name].extend(prediction_values[mask].cpu().tolist())
            truth_clusters[name].extend(targets["cluster"][mask].cpu().tolist())

        geometry_mask = targets["geometryValid"]
        if geometry_mask.any():
            predicted_erase = outputs[geometry_mask, ERASE_BOX_SLICE]
            predicted_layout = outputs[geometry_mask, LAYOUT_BOX_SLICE]
            target_erase = targets["eraseBox"][geometry_mask]
            target_layout = targets["layoutBox"][geometry_mask]
            erase_ious.extend(box_iou(predicted_erase, target_erase).cpu().tolist())
            layout_ious.extend(box_iou(predicted_layout, target_layout).cpu().tolist())
            erase_coverage, erase_spill = _geometry_safety(
                predicted_erase, target_erase
            )
            layout_coverage, layout_spill = _geometry_safety(
                predicted_layout, target_layout
            )
            _, erase_outside_layout = _geometry_safety(
                predicted_erase, predicted_layout
            )
            cluster_ids = targets["cluster"][geometry_mask].cpu().tolist()
            values = zip(
                cluster_ids,
                erase_coverage.cpu().tolist(),
                erase_spill.cpu().tolist(),
                layout_coverage.cpu().tolist(),
                layout_spill.cpu().tolist(),
                erase_outside_layout.cpu().tolist(),
            )
            for (
                cluster_id,
                erase_cover,
                erase_overflow,
                layout_cover,
                layout_overflow,
                outside_layout,
            ) in values:
                cluster = geometry_clusters.setdefault(
                    cluster_id,
                    {
                        "eraseCoverage": [],
                        "eraseSpill": [],
                        "layoutCoverage": [],
                        "layoutSpill": [],
                        "eraseOutsideLayout": [],
                    },
                )
                cluster["eraseCoverage"].append(erase_cover)
                cluster["eraseSpill"].append(erase_overflow)
                cluster["layoutCoverage"].append(layout_cover)
                cluster["layoutSpill"].append(layout_overflow)
                cluster["eraseOutsideLayout"].append(outside_layout)

        textness_probability = outputs[:, TEXTNESS_SLICE].softmax(dim=1)
        relation_probability = outputs[:, RELATION_SLICE].softmax(dim=1)
        patch_probability = outputs[:, PATCH_MODE_SLICE].softmax(dim=1)
        accepted = (
            (textness_probability.argmax(dim=1) == 0)
            & (relation_probability.argmax(dim=1) == 0)
            & (patch_probability.argmax(dim=1) == 0)
            & (textness_probability[:, 0] >= textness_threshold)
            & (relation_probability[:, 0] >= relation_threshold)
            & (patch_probability[:, 0] >= patch_threshold)
        )
        safe_text = (
            (targets["textness"] == 0)
            & (targets["relation"] == 0)
            & (targets["patchMode"] == 0)
        )
        non_text = targets["textness"] == 2
        mixed = targets["textness"] == 1
        unsafe_text = (targets["textness"] == 0) & ~safe_text
        for name, mask in (
            ("nonText", non_text),
            ("mixed", mixed),
            ("unsafeText", unsafe_text),
        ):
            slice_counts[name] += int(mask.sum())
            slice_counts[f"{name}FalseAccepts"] += int((accepted & mask).sum())
        accepted_safe_text += int((accepted & safe_text).sum())
        safe_text_count += int(safe_text.sum())
        for policy_index, policy_name in enumerate(("translate", "preserve", "review")):
            accepted_by_policy[policy_name] += int(
                (accepted & (targets["translationPolicy"] == policy_index)).sum()
            )

        cluster_ids = targets["cluster"].cpu().tolist()
        for category, mask in (
            ("nonText", non_text),
            ("mixed", mixed),
            ("unsafeText", unsafe_text),
        ):
            for cluster_id, is_member, is_false_accept in zip(
                cluster_ids,
                mask.cpu().tolist(),
                (accepted & mask).cpu().tolist(),
            ):
                cluster = cluster_safety.setdefault(cluster_id, {})
                category_state = cluster.setdefault(
                    category, {"present": False, "falseAccept": False}
                )
                category_state["present"] = category_state["present"] or bool(is_member)
                category_state["falseAccept"] = category_state["falseAccept"] or bool(
                    is_false_accept
                )
        for cluster_id, is_member, is_missed in zip(
            cluster_ids,
            safe_text.cpu().tolist(),
            (safe_text & ~accepted).cpu().tolist(),
        ):
            cluster = cluster_safety.setdefault(cluster_id, {})
            category_state = cluster.setdefault(
                "safeText", {"present": False, "missed": False}
            )
            category_state["present"] = category_state["present"] or bool(is_member)
            category_state["missed"] = category_state["missed"] or bool(is_missed)

    classification = {
        "textness": _classification_metrics(
            truth["textness"],
            predicted["textness"],
            truth_clusters["textness"],
            len(TEXTNESS_CLASSES),
        ),
        "role": _classification_metrics(
            truth["role"], predicted["role"], truth_clusters["role"], len(ROLE_CLASSES)
        ),
        "relation": _classification_metrics(
            truth["relation"],
            predicted["relation"],
            truth_clusters["relation"],
            len(RELATION_CLASSES),
        ),
        "patchMode": _classification_metrics(
            truth["patchMode"],
            predicted["patchMode"],
            truth_clusters["patchMode"],
            len(PATCH_MODE_CLASSES),
        ),
    }
    cluster_slices: dict[str, dict[str, float | int | None]] = {}
    for category in ("nonText", "mixed", "unsafeText"):
        category_clusters = [
            value[category]
            for value in cluster_safety.values()
            if value.get(category, {}).get("present")
        ]
        false_accept_clusters = sum(value["falseAccept"] for value in category_clusters)
        cluster_slices[category] = {
            "clusterCount": len(category_clusters),
            "falseAcceptClusters": false_accept_clusters,
            "falseAcceptClusterRate": false_accept_clusters / len(category_clusters)
            if category_clusters
            else None,
            "falseAcceptClusterRate95Upper": wilson_upper(
                false_accept_clusters, len(category_clusters)
            ),
        }
    group_erase_coverage = [
        min(value["eraseCoverage"]) for value in geometry_clusters.values()
    ]
    group_erase_spill = [
        max(value["eraseSpill"]) for value in geometry_clusters.values()
    ]
    group_layout_coverage = [
        min(value["layoutCoverage"]) for value in geometry_clusters.values()
    ]
    group_layout_spill = [
        max(value["layoutSpill"]) for value in geometry_clusters.values()
    ]
    group_erase_outside_layout = [
        max(value["eraseOutsideLayout"]) for value in geometry_clusters.values()
    ]
    geometry = {
        "unitCount": len(erase_ious),
        "splitGroupCount": len(geometry_clusters),
        "eraseMeanIoU": sum(erase_ious) / len(erase_ious) if erase_ious else None,
        "layoutMeanIoU": sum(layout_ious) / len(layout_ious) if layout_ious else None,
        "eraseCoverageP01": _percentile(group_erase_coverage, 0.01),
        "eraseCoverageMinimum": min(group_erase_coverage)
        if group_erase_coverage
        else None,
        "eraseSpillP99": _percentile(group_erase_spill, 0.99),
        "eraseSpillMaximum": max(group_erase_spill) if group_erase_spill else None,
        "layoutCoverageP01": _percentile(group_layout_coverage, 0.01),
        "layoutCoverageMinimum": min(group_layout_coverage)
        if group_layout_coverage
        else None,
        "layoutSpillP99": _percentile(group_layout_spill, 0.99),
        "layoutSpillMaximum": max(group_layout_spill) if group_layout_spill else None,
        "eraseOutsideLayoutP99": _percentile(group_erase_outside_layout, 0.99),
        "eraseOutsideLayoutMaximum": max(group_erase_outside_layout)
        if group_erase_outside_layout
        else None,
    }
    safe_text_clusters = [
        value["safeText"]
        for value in cluster_safety.values()
        if value.get("safeText", {}).get("present")
    ]
    safe_text_missed_clusters = sum(value["missed"] for value in safe_text_clusters)
    safety_gate = {
        "thresholds": {
            "textness": textness_threshold,
            "relation": relation_threshold,
            "patchMode": patch_threshold,
        },
        **slice_counts,
        "clusterSlices": cluster_slices,
        "safeTextCount": safe_text_count,
        "acceptedSafeText": accepted_safe_text,
        "safeTextRecall": accepted_safe_text / safe_text_count
        if safe_text_count
        else None,
        "safeTextSplitGroupCount": len(safe_text_clusters),
        "safeTextMissedSplitGroups": safe_text_missed_clusters,
        "safeTextSplitGroupRecall95Lower": wilson_lower(
            len(safe_text_clusters) - safe_text_missed_clusters, len(safe_text_clusters)
        ),
        "visuallyAcceptedByTranslationPolicy": accepted_by_policy,
    }
    evaluation = {
        "gateEvidenceVersion": 1,
        "inferenceEngine": inference_engine,
        "unitCount": len(truth["textness"]),
        "loss": {name: value / max(1, batches) for name, value in loss_totals.items()},
        "classification": classification,
        "geometry": geometry,
        "safetyGate": safety_gate,
    }
    evaluation["statisticalGateEligible"] = statistical_gate_eligible(evaluation)
    return evaluation


class OnnxRuntimeAdapter:
    def __init__(self, session: onnxruntime.InferenceSession):
        self.session = session

    def eval(self) -> OnnxRuntimeAdapter:
        return self

    def __call__(self, inputs: torch.Tensor) -> torch.Tensor:
        values = np.ascontiguousarray(inputs.detach().cpu().numpy(), dtype=np.float32)
        outputs = self.session.run(["predictions"], {"regions": values})[0]
        return torch.from_numpy(outputs).to(inputs.device)


def export_onnx(
    model: nn.Module,
    path: Path,
    consistency_inputs: torch.Tensor,
) -> onnxruntime.InferenceSession:
    model.eval()
    example = torch.zeros(
        (1, INPUT_CHANNELS, INPUT_HEIGHT, INPUT_WIDTH), dtype=torch.float32
    )
    export_options: dict[str, Any] = {
        "input_names": ["regions"],
        "output_names": ["predictions"],
        "dynamic_axes": {"regions": {0: "batch"}, "predictions": {0: "batch"}},
        "opset_version": 17,
        "do_constant_folding": True,
    }
    if "dynamo" in inspect.signature(torch.onnx.export).parameters:
        export_options["dynamo"] = False
    torch.onnx.export(model.cpu(), example, path, **export_options)
    exported = onnx.load(path)
    onnx.checker.check_model(exported)
    session = onnxruntime.InferenceSession(
        str(path), providers=["CPUExecutionProvider"]
    )
    session_input = session.get_inputs()[0]
    session_output = session.get_outputs()[0]
    if (
        len(session_input.shape) != 4
        or len(session_output.shape) != 2
        or not isinstance(session_input.shape[0], str)
        or session_input.shape[0] != session_output.shape[0]
    ):
        raise RuntimeError(
            f"ONNX batch dimension must be dynamic and shared: {session_input.shape} -> {session_output.shape}"
        )
    real_inputs = consistency_inputs.detach().cpu().to(dtype=torch.float32)
    if real_inputs.shape[0] < 2:
        real_inputs = torch.cat((real_inputs, real_inputs), dim=0)
    checks = [
        torch.zeros(
            (2, INPUT_CHANNELS, INPUT_HEIGHT, INPUT_WIDTH), dtype=torch.float32
        ),
        real_inputs,
    ]
    model = model.cpu()
    for check_inputs in checks:
        runtime_outputs = session.run(
            ["predictions"],
            {"regions": np.ascontiguousarray(check_inputs.numpy(), dtype=np.float32)},
        )[0]
        with torch.no_grad():
            torch_outputs = model(check_inputs).numpy()
        if runtime_outputs.shape != (check_inputs.shape[0], OUTPUT_WIDTH):
            raise RuntimeError(f"ONNX output shape mismatch: {runtime_outputs.shape}")
        if not np.isfinite(runtime_outputs).all():
            raise RuntimeError("ONNX output contains non-finite values")
        np.testing.assert_allclose(runtime_outputs, torch_outputs, rtol=1e-4, atol=1e-5)
    return session


def ensure_output_directory(path: Path) -> None:
    if path.exists() and any(path.iterdir()):
        raise ValueError(f"Output directory is not empty: {path}")
    path.mkdir(parents=True, exist_ok=True)


def dataset_class_support(dataset: UiRegionDataset) -> dict[str, list[int]]:
    support = {
        "textness": [0] * len(TEXTNESS_CLASSES),
        "role": [0] * len(ROLE_CLASSES),
        "relation": [0] * len(RELATION_CLASSES),
        "patchMode": [0] * len(PATCH_MODE_CLASSES),
    }
    for record in dataset.records:
        annotation = record.annotation
        textness = {"text": "text_only", "mixed": "mixed", "non_text": "non_text"}[
            annotation["textness"]
        ]
        support["textness"][TEXTNESS_CLASSES.index(textness)] += 1
        support["relation"][RELATION_CLASSES.index(annotation["relation"])] += 1
        support["patchMode"][PATCH_MODE_CLASSES.index(annotation["patchMode"])] += 1
        coarse_role = ROLE_TO_COARSE.get(annotation["role"])
        if coarse_role:
            support["role"][ROLE_CLASSES.index(coarse_role)] += 1
    return support


def parse_args(argv: list[str]) -> argparse.Namespace:
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    parser = argparse.ArgumentParser(
        description="Train Visnip UiRegionNet from random initialization"
    )
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument(
        "--output", type=Path, default=Path("tools/ui_region_model/runs") / timestamp
    )
    parser.add_argument("--epochs", type=int, default=80)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--learning-rate", type=float, default=3e-4)
    parser.add_argument("--weight-decay", type=float, default=1e-4)
    parser.add_argument("--textness-threshold", type=float, default=0.95)
    parser.add_argument("--relation-threshold", type=float, default=0.90)
    parser.add_argument("--patch-threshold", type=float, default=0.90)
    parser.add_argument("--seed", type=int, default=20260728)
    parser.add_argument("--workers", type=int, default=0)
    parser.add_argument(
        "--device", default="cuda" if torch.cuda.is_available() else "cpu"
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.epochs <= 0 or args.batch_size <= 0:
        print("epochs and batch-size must be positive", file=sys.stderr)
        return 2
    for name in ("textness_threshold", "relation_threshold", "patch_threshold"):
        if not 0.0 < getattr(args, name) < 1.0:
            print(f"{name.replace('_', '-')} must be between 0 and 1", file=sys.stderr)
            return 2
    try:
        with dataset_lock(args.dataset):
            validation_summary = validate_dataset(args.dataset, require_reviewed=True)
            dataset_manifest = build_dataset_manifest(args.dataset)
            datasets = {
                split: UiRegionDataset(
                    args.dataset,
                    split,
                    dataset_manifest,
                    augment=(split == "train"),
                )
                for split in ("train", "validation", "test")
            }
        ensure_output_directory(args.output)
        dataset_manifest_path = args.output / "dataset_manifest.json"
        atomic_write_json(dataset_manifest_path, dataset_manifest)
        dataset_manifest_sha = sha256_file(dataset_manifest_path)
        seed_everything(args.seed)
        empty_splits = [name for name, dataset in datasets.items() if not len(dataset)]
        if empty_splits:
            raise ValueError(f"Dataset splits are empty: {', '.join(empty_splits)}")
        class_support = {
            name: dataset_class_support(dataset) for name, dataset in datasets.items()
        }
        class_names = {
            "textness": TEXTNESS_CLASSES,
            "role": ROLE_CLASSES,
            "relation": RELATION_CLASSES,
            "patchMode": PATCH_MODE_CLASSES,
        }
        for head, counts in class_support["train"].items():
            missing = [
                class_names[head][index]
                for index, count in enumerate(counts)
                if count == 0
            ]
            if missing:
                raise ValueError(
                    f"Training split is missing {head} classes: {', '.join(missing)}"
                )

        device = torch.device(args.device)
        loaders = {
            name: DataLoader(
                dataset,
                batch_size=args.batch_size,
                shuffle=name == "train",
                num_workers=args.workers,
                pin_memory=device.type == "cuda",
            )
            for name, dataset in datasets.items()
        }
        model = UiRegionNet().to(device)
        optimizer = torch.optim.AdamW(
            model.parameters(), lr=args.learning_rate, weight_decay=args.weight_decay
        )
        scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(
            optimizer, T_max=args.epochs
        )
        best_loss = math.inf
        history: list[dict[str, Any]] = []
        checkpoint_path = args.output / "best.pt"
        for epoch in range(1, args.epochs + 1):
            training_metrics = run_training_epoch(
                model, loaders["train"], optimizer, device
            )
            validation_metrics = evaluate(
                model,
                loaders["validation"],
                device,
                args.textness_threshold,
                args.relation_threshold,
                args.patch_threshold,
            )
            scheduler.step()
            epoch_record = {
                "epoch": epoch,
                "learningRate": optimizer.param_groups[0]["lr"],
                "train": training_metrics,
                "validation": validation_metrics,
            }
            history.append(epoch_record)
            print(json.dumps(epoch_record, ensure_ascii=False), flush=True)
            validation_loss = validation_metrics["loss"]["total"]
            if validation_loss < best_loss:
                best_loss = validation_loss
                torch.save(
                    {
                        "model": model.state_dict(),
                        "epoch": epoch,
                        "validationLoss": best_loss,
                    },
                    checkpoint_path,
                )

        checkpoint = torch.load(checkpoint_path, map_location=device, weights_only=True)
        model.load_state_dict(checkpoint["model"])
        validation_metrics = evaluate(
            model,
            loaders["validation"],
            device,
            args.textness_threshold,
            args.relation_threshold,
            args.patch_threshold,
        )
        onnx_path = args.output / "candidate_model.onnx"
        consistency_inputs, _ = next(iter(loaders["test"]))
        onnx_session = export_onnx(model, onnx_path, consistency_inputs[:4])
        test_metrics = evaluate(
            OnnxRuntimeAdapter(onnx_session),
            loaders["test"],
            torch.device("cpu"),
            args.textness_threshold,
            args.relation_threshold,
            args.patch_threshold,
            inference_engine="onnxruntime",
        )
        metric_errors = validate_statistical_evidence(
            test_metrics,
            {
                "textness": args.textness_threshold,
                "relation": args.relation_threshold,
                "patchMode": args.patch_threshold,
            },
            available_split_groups=dataset_manifest["splits"]["test"][
                "splitGroupCount"
            ],
            expected_unit_count=dataset_manifest["splits"]["test"]["trainingUnitCount"],
        )
        if metric_errors:
            raise RuntimeError(
                "Invalid ONNX test evidence:\n" + "\n".join(metric_errors)
            )
        test_metrics["deploymentEligible"] = False
        model_sha = sha256_file(onnx_path)
        golden_source = Path(__file__).with_name("preprocess_golden.json")
        golden_path = args.output / golden_source.name
        shutil.copy2(golden_source, golden_path)
        golden_sha = sha256_file(golden_path)
        contract = json.loads(
            Path(__file__)
            .with_name("model_contract.template.json")
            .read_text(encoding="utf-8")
        )
        contract["input"]["goldenTensor"]["sha256"] = golden_sha
        contract.update(
            {
                "contractVersion": MODEL_CONTRACT_VERSION,
                "modelSha256": model_sha,
                "modelFile": onnx_path.name,
                "parameterCount": parameter_count(model),
                "onnxOpset": 17,
                "artifactStatus": "research_candidate",
                "deploymentEligible": False,
                "qualification": "qualify.py may audit this candidate but cannot authorize deployment",
                "acceptanceThresholds": {
                    "textness": args.textness_threshold,
                    "relation": args.relation_threshold,
                    "patchMode": args.patch_threshold,
                },
                "testEvaluation": test_metrics,
                "trainingRun": {
                    "seed": args.seed,
                    "epochs": args.epochs,
                    "bestEpoch": checkpoint["epoch"],
                    "python": sys.version,
                    "torch": torch.__version__,
                    "numpy": np.__version__,
                    "onnx": onnx.__version__,
                    "onnxruntime": onnxruntime.__version__,
                    "device": str(device),
                    "datasetSummary": validation_summary,
                    "datasetManifest": {
                        "path": dataset_manifest_path.name,
                        "sha256": dataset_manifest_sha,
                        "testSplit": dataset_manifest["splits"]["test"],
                    },
                    "classSupport": class_support,
                },
            }
        )
        contract_path = args.output / "model_contract.json"
        atomic_write_json(contract_path, contract)
        contract_sha = sha256_file(contract_path)
        atomic_write_json(
            args.output / "metrics.json",
            {
                "artifactStatus": "research_candidate",
                "modelSha256": model_sha,
                "contractSha256": contract_sha,
                "datasetManifestSha256": dataset_manifest_sha,
                "testSplitManifestSha256": dataset_manifest["splits"]["test"]["sha256"],
                "history": history,
                "validation": validation_metrics,
                "test": test_metrics,
                "deploymentEligible": False,
            },
        )
        print(
            json.dumps(
                {"output": str(args.output), "test": test_metrics},
                ensure_ascii=False,
                indent=2,
            )
        )
        return 0
    except (ValueError, OSError, RuntimeError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
