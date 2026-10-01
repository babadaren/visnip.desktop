from __future__ import annotations

import math
from typing import Any


MIN_CLASS_UNIT_SUPPORT = 25
MIN_CLASS_GROUP_SUPPORT = 10
MIN_SAFETY_GROUP_SUPPORT = 598
MIN_GEOMETRY_GROUP_SUPPORT = 200
ROLE_MACRO_F1_MINIMUM = 0.90
CLASS_COUNTS = {"textness": 3, "role": 6, "relation": 4, "patchMode": 3}
SAFETY_CATEGORIES = ("nonText", "mixed", "unsafeText")
GEOMETRY_LIMITS = {
    "eraseCoverageP01": (">=", 0.99),
    "eraseCoverageMinimum": (">=", 0.90),
    "eraseSpillP99": ("<=", 0.001),
    "eraseSpillMaximum": ("<=", 0.01),
    "layoutCoverageP01": (">=", 0.99),
    "layoutCoverageMinimum": (">=", 0.90),
    "layoutSpillP99": ("<=", 0.001),
    "layoutSpillMaximum": ("<=", 0.01),
    "eraseOutsideLayoutP99": ("<=", 0.001),
    "eraseOutsideLayoutMaximum": ("<=", 0.01),
}


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_probability(value: Any) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
        and 0.0 <= value <= 1.0
    )


def wilson_upper(
    errors: int, total: int, z: float = 1.6448536269514722
) -> float | None:
    if total == 0:
        return None
    proportion = errors / total
    denominator = 1 + z * z / total
    center = proportion + z * z / (2 * total)
    radius = z * math.sqrt(
        proportion * (1 - proportion) / total + z * z / (4 * total * total)
    )
    return (center + radius) / denominator


def wilson_lower(
    successes: int, total: int, z: float = 1.6448536269514722
) -> float | None:
    if total == 0:
        return None
    proportion = successes / total
    denominator = 1 + z * z / total
    center = proportion + z * z / (2 * total)
    radius = z * math.sqrt(
        proportion * (1 - proportion) / total + z * z / (4 * total * total)
    )
    return (center - radius) / denominator


def _matches(first: Any, second: Any, tolerance: float = 1e-9) -> bool:
    if first is None or second is None:
        return first is second
    return (
        _is_probability(first)
        and _is_probability(second)
        and math.isclose(first, second, abs_tol=tolerance)
    )


def _at_least(value: Any, threshold: float) -> bool:
    return _is_probability(value) and value >= threshold


def _at_most(value: Any, threshold: float) -> bool:
    return _is_probability(value) and value <= threshold


def statistical_gate_eligible(evaluation: dict[str, Any]) -> bool:
    classification = evaluation["classification"]
    safety = evaluation["safetyGate"]
    geometry = evaluation["geometry"]
    cluster_slices = safety["clusterSlices"]
    cluster_gates_pass = all(
        cluster_slices[name]["clusterCount"] >= MIN_SAFETY_GROUP_SUPPORT
        and cluster_slices[name]["falseAcceptClusters"] == 0
        and _at_most(cluster_slices[name]["falseAcceptClusterRate95Upper"], 0.005)
        for name in SAFETY_CATEGORIES
    )
    geometry_pass = geometry["splitGroupCount"] >= MIN_GEOMETRY_GROUP_SUPPORT and all(
        _at_least(geometry[name], threshold)
        if operator == ">="
        else _at_most(geometry[name], threshold)
        for name, (operator, threshold) in GEOMETRY_LIMITS.items()
    )
    return bool(
        cluster_gates_pass
        and safety["safeTextCount"] >= MIN_SAFETY_GROUP_SUPPORT
        and safety["safeTextSplitGroupCount"] >= MIN_SAFETY_GROUP_SUPPORT
        and safety["safeTextMissedSplitGroups"] == 0
        and _at_least(safety["safeTextSplitGroupRecall95Lower"], 0.995)
        and _at_least(safety["safeTextRecall"], 0.99)
        and all(classification[name]["evaluable"] for name in CLASS_COUNTS)
        and _at_least(classification["role"]["macroF1"], ROLE_MACRO_F1_MINIMUM)
        and geometry_pass
    )


def validate_statistical_evidence(
    evaluation: Any,
    acceptance_thresholds: Any,
    required_engine: str = "onnxruntime",
    available_split_groups: int | None = None,
    expected_unit_count: int | None = None,
) -> list[str]:
    errors: list[str] = []
    if not isinstance(evaluation, dict):
        return ["test evaluation must be an object"]
    if evaluation.get("gateEvidenceVersion") != 1:
        errors.append("test gateEvidenceVersion must equal 1")
    if evaluation.get("inferenceEngine") != required_engine:
        errors.append(f"test inferenceEngine must equal {required_engine}")
    unit_count = evaluation.get("unitCount")
    if not _is_int(unit_count) or unit_count <= 0:
        errors.append("test unitCount must be a positive integer")
    if expected_unit_count is not None:
        if not _is_int(expected_unit_count) or expected_unit_count <= 0:
            errors.append("expected test unit count must be a positive integer")
        elif unit_count != expected_unit_count:
            errors.append("test unitCount differs from the dataset manifest")
    if available_split_groups is not None and (
        not _is_int(available_split_groups) or available_split_groups <= 0
    ):
        errors.append("available test split-group count must be a positive integer")

    classification = evaluation.get("classification")
    if not isinstance(classification, dict):
        errors.append("test classification must be an object")
        classification = {}
    for head, class_count in CLASS_COUNTS.items():
        value = classification.get(head)
        prefix = f"test classification.{head}"
        if not isinstance(value, dict):
            errors.append(f"{prefix} must be an object")
            continue
        support = value.get("support")
        group_support = value.get("splitGroupSupport")
        per_class_f1 = value.get("perClassF1")
        confusion_matrix = value.get("confusionMatrix")
        if (
            not isinstance(support, list)
            or len(support) != class_count
            or any(not _is_int(item) or item < 0 for item in support)
        ):
            errors.append(f"{prefix}.support is invalid")
            continue
        if (
            not isinstance(group_support, list)
            or len(group_support) != class_count
            or any(not _is_int(item) or item < 0 for item in group_support)
        ):
            errors.append(f"{prefix}.splitGroupSupport is invalid")
            continue
        if any(groups > units for groups, units in zip(group_support, support)):
            errors.append(f"{prefix}.splitGroupSupport exceeds unit support")
        if available_split_groups is not None and any(
            groups > available_split_groups for groups in group_support
        ):
            errors.append(f"{prefix}.splitGroupSupport exceeds the test split")
        if (
            not isinstance(per_class_f1, list)
            or len(per_class_f1) != class_count
            or any(
                item is not None and not _is_probability(item) for item in per_class_f1
            )
        ):
            errors.append(f"{prefix}.perClassF1 is invalid")
            continue
        matrix_valid = (
            isinstance(confusion_matrix, list)
            and len(confusion_matrix) == class_count
            and all(
                isinstance(row, list)
                and len(row) == class_count
                and all(_is_int(item) and item >= 0 for item in row)
                for row in confusion_matrix
            )
        )
        if not matrix_valid:
            errors.append(f"{prefix}.confusionMatrix is invalid")
            continue
        matrix_support = [sum(row) for row in confusion_matrix]
        if matrix_support != support:
            errors.append(f"{prefix}.support differs from confusion matrix")
        expected_f1: list[float | None] = []
        for class_index in range(class_count):
            true_positive = confusion_matrix[class_index][class_index]
            false_positive = sum(
                confusion_matrix[row][class_index]
                for row in range(class_count)
                if row != class_index
            )
            false_negative = sum(
                confusion_matrix[class_index][column]
                for column in range(class_count)
                if column != class_index
            )
            denominator = 2 * true_positive + false_positive + false_negative
            expected_f1.append(2 * true_positive / denominator if denominator else None)
        if any(
            not _matches(reported, expected)
            for reported, expected in zip(per_class_f1, expected_f1)
        ):
            errors.append(f"{prefix}.perClassF1 differs from confusion matrix")
        if value.get("minimumUnitSupport") != MIN_CLASS_UNIT_SUPPORT:
            errors.append(f"{prefix}.minimumUnitSupport is invalid")
        if value.get("minimumSplitGroupSupport") != MIN_CLASS_GROUP_SUPPORT:
            errors.append(f"{prefix}.minimumSplitGroupSupport is invalid")
        evaluable = all(item >= MIN_CLASS_UNIT_SUPPORT for item in support) and all(
            item >= MIN_CLASS_GROUP_SUPPORT for item in group_support
        )
        if value.get("evaluable") is not evaluable:
            errors.append(f"{prefix}.evaluable differs from its support")
        if evaluable and any(item is None for item in per_class_f1):
            errors.append(f"{prefix}.perClassF1 cannot contain null when evaluable")
            continue
        macro_f1 = sum(expected_f1) / class_count if evaluable else None
        if not _matches(value.get("macroF1"), macro_f1):
            errors.append(f"{prefix}.macroF1 differs from per-class evidence")
    if _is_int(unit_count):
        for head in ("textness", "relation", "patchMode"):
            value = classification.get(head)
            support = value.get("support") if isinstance(value, dict) else None
            if (
                isinstance(support, list)
                and all(_is_int(item) for item in support)
                and sum(support) != unit_count
            ):
                errors.append(f"test unitCount differs from {head} support")
        role = classification.get("role")
        role_support = role.get("support") if isinstance(role, dict) else None
        textness = classification.get("textness")
        textness_support = (
            textness.get("support") if isinstance(textness, dict) else None
        )
        if (
            isinstance(role_support, list)
            and all(_is_int(item) for item in role_support)
            and isinstance(textness_support, list)
            and len(textness_support) == CLASS_COUNTS["textness"]
            and all(_is_int(item) for item in textness_support)
            and sum(role_support) != textness_support[0]
        ):
            errors.append("test role support differs from text-only support")

    safety = evaluation.get("safetyGate")
    if not isinstance(safety, dict):
        errors.append("test safetyGate must be an object")
        safety = {}
    thresholds = safety.get("thresholds")
    if not isinstance(acceptance_thresholds, dict) or not isinstance(thresholds, dict):
        errors.append("test safety thresholds must be objects")
    else:
        for name in ("textness", "relation", "patchMode"):
            if not _matches(thresholds.get(name), acceptance_thresholds.get(name)):
                errors.append(
                    f"test safety threshold {name} differs from the model contract"
                )
    cluster_slices = safety.get("clusterSlices")
    if not isinstance(cluster_slices, dict):
        errors.append("test safetyGate.clusterSlices must be an object")
        cluster_slices = {}
    for category in SAFETY_CATEGORIES:
        unit_total = safety.get(category)
        unit_errors = safety.get(f"{category}FalseAccepts")
        if (
            not _is_int(unit_total)
            or unit_total < 0
            or not _is_int(unit_errors)
            or not 0 <= unit_errors <= unit_total
        ):
            errors.append(f"test safetyGate {category} unit counts are invalid")
        value = cluster_slices.get(category)
        if not isinstance(value, dict):
            errors.append(f"test safetyGate.clusterSlices.{category} must be an object")
            continue
        total = value.get("clusterCount")
        false_accepts = value.get("falseAcceptClusters")
        if (
            not _is_int(total)
            or total < 0
            or not _is_int(false_accepts)
            or not 0 <= false_accepts <= total
        ):
            errors.append(
                f"test safetyGate.clusterSlices.{category} counts are invalid"
            )
            continue
        if _is_int(unit_total) and _is_int(unit_errors):
            if total > unit_total:
                errors.append(
                    f"test safetyGate.clusterSlices.{category} has more groups than units"
                )
            if false_accepts > unit_errors or (
                (false_accepts == 0) != (unit_errors == 0)
            ):
                errors.append(
                    f"test safetyGate {category} unit and group false accepts disagree"
                )
            if (total == 0) != (unit_total == 0):
                errors.append(
                    f"test safetyGate {category} unit and group support disagree"
                )
        if available_split_groups is not None and total > available_split_groups:
            errors.append(
                f"test safetyGate.clusterSlices.{category} exceeds the test split"
            )
        expected_rate = false_accepts / total if total else None
        expected_upper = wilson_upper(false_accepts, total)
        if not _matches(value.get("falseAcceptClusterRate"), expected_rate):
            errors.append(
                f"test safetyGate.clusterSlices.{category} rate is inconsistent"
            )
        if not _matches(value.get("falseAcceptClusterRate95Upper"), expected_upper):
            errors.append(
                f"test safetyGate.clusterSlices.{category} confidence bound is inconsistent"
            )

    safe_count = safety.get("safeTextCount")
    accepted_safe = safety.get("acceptedSafeText")
    safe_groups = safety.get("safeTextSplitGroupCount")
    missed_groups = safety.get("safeTextMissedSplitGroups")
    if (
        not _is_int(safe_count)
        or safe_count < 0
        or not _is_int(accepted_safe)
        or not 0 <= accepted_safe <= safe_count
    ):
        errors.append("test safe-text unit counts are invalid")
    else:
        expected_recall = accepted_safe / safe_count if safe_count else None
        if not _matches(safety.get("safeTextRecall"), expected_recall):
            errors.append("test safeTextRecall is inconsistent")
    if (
        not _is_int(safe_groups)
        or safe_groups < 0
        or not _is_int(missed_groups)
        or not 0 <= missed_groups <= safe_groups
    ):
        errors.append("test safe-text split-group counts are invalid")
    else:
        expected_lower = wilson_lower(safe_groups - missed_groups, safe_groups)
        if not _matches(safety.get("safeTextSplitGroupRecall95Lower"), expected_lower):
            errors.append("test safe-text confidence bound is inconsistent")
    if (
        _is_int(safe_count)
        and _is_int(accepted_safe)
        and 0 <= accepted_safe <= safe_count
        and _is_int(safe_groups)
        and _is_int(missed_groups)
        and 0 <= missed_groups <= safe_groups
    ):
        missed_units = safe_count - accepted_safe
        if safe_groups > safe_count:
            errors.append("test safe-text split-group support exceeds unit support")
        if missed_groups > missed_units or (
            (missed_groups == 0) != (missed_units == 0)
        ):
            errors.append("test safe-text unit and split-group misses disagree")
        if (safe_groups == 0) != (safe_count == 0):
            errors.append("test safe-text unit and split-group support disagree")
        if available_split_groups is not None and safe_groups > available_split_groups:
            errors.append("test safe-text split-group support exceeds the test split")
    if all(
        _is_int(safety.get(name))
        for name in ("nonText", "mixed", "unsafeText", "safeTextCount")
    ) and _is_int(unit_count):
        partition_count = (
            sum(safety[name] for name in ("nonText", "mixed", "unsafeText"))
            + safety["safeTextCount"]
        )
        if partition_count != unit_count:
            errors.append("test safety slices do not partition unitCount")
    accepted_by_policy = safety.get("visuallyAcceptedByTranslationPolicy")
    if (
        not isinstance(accepted_by_policy, dict)
        or set(accepted_by_policy) != {"translate", "preserve", "review"}
        or any(not _is_int(value) or value < 0 for value in accepted_by_policy.values())
    ):
        errors.append("test visually accepted translation-policy counts are invalid")
    elif _is_int(accepted_safe) and all(
        _is_int(safety.get(f"{name}FalseAccepts")) for name in SAFETY_CATEGORIES
    ):
        expected_accepted = accepted_safe + sum(
            safety[f"{name}FalseAccepts"] for name in SAFETY_CATEGORIES
        )
        if sum(accepted_by_policy.values()) != expected_accepted:
            errors.append(
                "test visually accepted translation-policy counts disagree with safety slices"
            )

    geometry = evaluation.get("geometry")
    if not isinstance(geometry, dict):
        errors.append("test geometry must be an object")
        geometry = {}
    for name in ("unitCount", "splitGroupCount"):
        if not _is_int(geometry.get(name)) or geometry[name] < 0:
            errors.append(f"test geometry.{name} must be a non-negative integer")
    if _is_int(geometry.get("unitCount")) and _is_int(geometry.get("splitGroupCount")):
        geometry_units = geometry["unitCount"]
        geometry_groups = geometry["splitGroupCount"]
        if geometry_groups > geometry_units:
            errors.append("test geometry split-group support exceeds unit support")
        if (geometry_groups == 0) != (geometry_units == 0):
            errors.append("test geometry unit and split-group support disagree")
        if (
            available_split_groups is not None
            and geometry_groups > available_split_groups
        ):
            errors.append("test geometry split-group support exceeds the test split")
    has_geometry = _is_int(geometry.get("unitCount")) and geometry["unitCount"] > 0
    for name in ("eraseMeanIoU", "layoutMeanIoU", *GEOMETRY_LIMITS):
        if (has_geometry and not _is_probability(geometry.get(name))) or (
            not has_geometry and geometry.get(name) is not None
        ):
            errors.append(f"test geometry.{name} must be a probability")
    for prefix in ("erase", "layout"):
        minimum = geometry.get(f"{prefix}CoverageMinimum")
        percentile = geometry.get(f"{prefix}CoverageP01")
        if (
            _is_probability(minimum)
            and _is_probability(percentile)
            and minimum > percentile
        ):
            errors.append(f"test geometry.{prefix} coverage minimum exceeds its P01")
        percentile = geometry.get(f"{prefix}SpillP99")
        maximum = geometry.get(f"{prefix}SpillMaximum")
        if (
            _is_probability(percentile)
            and _is_probability(maximum)
            and percentile > maximum
        ):
            errors.append(f"test geometry.{prefix} spill P99 exceeds its maximum")
    outside_p99 = geometry.get("eraseOutsideLayoutP99")
    outside_maximum = geometry.get("eraseOutsideLayoutMaximum")
    if (
        _is_probability(outside_p99)
        and _is_probability(outside_maximum)
        and outside_p99 > outside_maximum
    ):
        errors.append("test geometry erase-outside-layout P99 exceeds its maximum")

    def support_vector(head: str, field: str) -> list[int] | None:
        value = classification.get(head)
        raw = value.get(field) if isinstance(value, dict) else None
        if (
            not isinstance(raw, list)
            or len(raw) != CLASS_COUNTS[head]
            or any(not _is_int(item) or item < 0 for item in raw)
        ):
            return None
        return raw

    textness_support = support_vector("textness", "support")
    textness_groups = support_vector("textness", "splitGroupSupport")
    relation_support = support_vector("relation", "support")
    relation_groups = support_vector("relation", "splitGroupSupport")
    role_groups = support_vector("role", "splitGroupSupport")
    patch_support = support_vector("patchMode", "support")
    patch_groups = support_vector("patchMode", "splitGroupSupport")
    if textness_support is not None:
        for category, class_index in (("mixed", 1), ("nonText", 2)):
            if (
                _is_int(safety.get(category))
                and safety[category] != textness_support[class_index]
            ):
                errors.append(
                    f"test safetyGate {category} differs from textness support"
                )
        if _is_int(safety.get("safeTextCount")) and _is_int(safety.get("unsafeText")):
            if safety["safeTextCount"] + safety["unsafeText"] != textness_support[0]:
                errors.append(
                    "test safe and unsafe text counts differ from text-only support"
                )
    if textness_groups is not None:
        for category, class_index in (("mixed", 1), ("nonText", 2)):
            value = cluster_slices.get(category)
            if (
                isinstance(value, dict)
                and _is_int(value.get("clusterCount"))
                and value["clusterCount"] != textness_groups[class_index]
            ):
                errors.append(
                    f"test safetyGate {category} groups differ from textness support"
                )
        unsafe = cluster_slices.get("unsafeText")
        if (
            isinstance(unsafe, dict)
            and _is_int(unsafe.get("clusterCount"))
            and _is_int(safe_groups)
        ):
            minimum_text_groups = max(unsafe["clusterCount"], safe_groups)
            maximum_text_groups = unsafe["clusterCount"] + safe_groups
            if not minimum_text_groups <= textness_groups[0] <= maximum_text_groups:
                errors.append(
                    "test safe and unsafe text groups disagree with text-only support"
                )
        if role_groups is not None and not (
            max(role_groups) <= textness_groups[0] <= sum(role_groups)
        ):
            errors.append("test role groups cannot cover all text-only groups")
    if patch_support is not None and _is_int(safe_count):
        if patch_support[0] != safe_count:
            errors.append("test rect-safe support differs from safe-text support")
        if (
            _is_int(geometry.get("unitCount"))
            and patch_support[0] != geometry["unitCount"]
        ):
            errors.append("test geometry unit support differs from rect-safe support")
    if patch_groups is not None and _is_int(safe_groups):
        if patch_groups[0] != safe_groups:
            errors.append("test rect-safe group support differs from safe-text support")
        if (
            _is_int(geometry.get("splitGroupCount"))
            and patch_groups[0] != geometry["splitGroupCount"]
        ):
            errors.append("test geometry group support differs from rect-safe support")
    if relation_support is not None and _is_int(safe_count):
        if relation_support[0] < safe_count:
            errors.append(
                "test single-relation support is smaller than safe-text support"
            )
    if relation_groups is not None and _is_int(safe_groups):
        if relation_groups[0] < safe_groups:
            errors.append(
                "test single-relation group support is smaller than safe-text support"
            )
    if textness_support is not None and relation_support is not None:
        if relation_support[1] < textness_support[1]:
            errors.append("test split-required support is smaller than mixed support")
        if relation_support[3] < textness_support[2]:
            errors.append("test no-relation support is smaller than non-text support")
    if textness_groups is not None and relation_groups is not None:
        if relation_groups[1] < textness_groups[1]:
            errors.append(
                "test split-required group support is smaller than mixed support"
            )
        if relation_groups[3] < textness_groups[2]:
            errors.append(
                "test no-relation group support is smaller than non-text support"
            )
    if textness_support is not None and patch_support is not None:
        if patch_support[2] < textness_support[2]:
            errors.append("test no-patch support is smaller than non-text support")
    if textness_groups is not None and patch_groups is not None:
        if patch_groups[2] < textness_groups[2]:
            errors.append(
                "test no-patch group support is smaller than non-text support"
            )

    if not errors:
        eligible = statistical_gate_eligible(evaluation)
        if evaluation.get("statisticalGateEligible") is not eligible:
            errors.append(
                "test statisticalGateEligible differs from recomputed evidence"
            )
    elif not isinstance(evaluation.get("statisticalGateEligible"), bool):
        errors.append("test statisticalGateEligible must be boolean")
    return errors
