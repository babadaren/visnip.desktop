from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

from . import MODEL_CONTRACT_VERSION
from .model_contract_constants import (
    INPUT_CHANNELS,
    INPUT_HEIGHT,
    INPUT_WIDTH,
    OUTPUT_WIDTH,
)
from .release_gate import validate_statistical_evidence
from .schema import (
    DatasetValidationError,
    atomic_write_json,
    read_json,
    sha256_file,
    utc_timestamp,
)
from .validate_dataset import validate_dataset_manifest


EXPECTED_CHANNELS = [
    "local_rgb_r",
    "local_rgb_g",
    "local_rgb_b",
    "context_rgb_r",
    "context_rgb_g",
    "context_rgb_b",
    "context_candidate_mask",
]
EXPECTED_SLICES = {
    "textness": (0, 3, "softmax", ["text_only", "mixed", "non_text"]),
    "role": (
        3,
        6,
        "softmax",
        ["heading", "paragraph", "ui_label", "caption_metadata", "data_code", "other"],
    ),
    "relation": (
        9,
        4,
        "softmax",
        ["single", "split_required", "merge_required", "none"],
    ),
    "patchMode": (13, 3, "softmax", ["rect_safe", "mask_required", "none"]),
    "eraseBox": (16, 4, "identity", None),
    "layoutBox": (20, 4, "identity", None),
}
DEPLOYMENT_BLOCK_REASON = (
    "The Visnip runtime does not yet contain a repository-owned end-to-end qualification harness; "
    "candidate evidence cannot authorize deployment"
)


class QualificationError(ValueError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise QualificationError(message)


def _require_sha256(value: Any, name: str) -> str:
    _require(
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value),
        f"{name} must be a lowercase SHA-256 digest",
    )
    return value


def _require_plain_filename(value: Any, name: str) -> str:
    _require(
        isinstance(value, str) and value and Path(value).name == value,
        f"{name} must be a file name",
    )
    return value


def _validate_contract(
    contract: dict[str, Any], run_directory: Path, model_sha: str
) -> dict[str, Any]:
    template = read_json(Path(__file__).with_name("model_contract.template.json"))
    _require(
        contract.get("contractVersion") == MODEL_CONTRACT_VERSION,
        "Unsupported model contract version",
    )
    _require(
        contract.get("modelFamily") == "visnip-ui-region", "Unexpected model family"
    )
    _require(
        contract.get("runtime") == template.get("runtime"),
        "Contract runtime differs from the template",
    )
    _require(
        contract.get("training") == template.get("training"),
        "Contract training differs from the template",
    )
    _require(
        contract.get("runtimePolicy") == template.get("runtimePolicy"),
        "Contract runtime policy differs from template",
    )
    _require(
        contract.get("artifactStatus") == "research_candidate",
        "Contract is not a research candidate",
    )
    _require(
        contract.get("deploymentEligible") is False,
        "Candidate contract must remain non-deployable",
    )
    _require(
        contract.get("modelFile") == "candidate_model.onnx",
        "Contract modelFile is invalid",
    )
    _require(
        contract.get("modelSha256") == model_sha,
        "Candidate model hash differs from its contract",
    )

    runtime = contract.get("runtime")
    _require(isinstance(runtime, dict), "Contract runtime must be an object")
    _require(
        runtime.get("engine") == "onnxruntime",
        "Contract runtime engine must be onnxruntime",
    )
    _require(
        runtime.get("opset") == 17 and contract.get("onnxOpset") == 17,
        "Contract ONNX opset must be 17",
    )

    model_input = contract.get("input")
    _require(isinstance(model_input, dict), "Contract input must be an object")
    _require(model_input.get("name") == "regions", "Contract input name is invalid")
    _require(model_input.get("dtype") == "float32", "Contract input dtype is invalid")
    _require(
        model_input.get("shape") == ["N", INPUT_CHANNELS, INPUT_HEIGHT, INPUT_WIDTH],
        "Contract input shape is invalid",
    )
    _require(
        model_input.get("channels") == EXPECTED_CHANNELS,
        "Contract input channels are invalid",
    )
    _require(
        isinstance(model_input.get("goldenTensor"), dict),
        "Contract goldenTensor must be an object",
    )
    normalized_input = dict(model_input)
    normalized_input["goldenTensor"] = dict(normalized_input["goldenTensor"])
    normalized_input["goldenTensor"]["sha256"] = None
    _require(
        normalized_input == template.get("input"),
        "Contract preprocessing differs from the template",
    )

    model_output = contract.get("output")
    _require(isinstance(model_output, dict), "Contract output must be an object")
    _require(
        model_output == template.get("output"),
        "Contract output definition differs from the template",
    )
    _require(
        model_output.get("name") == "predictions", "Contract output name is invalid"
    )
    _require(model_output.get("dtype") == "float32", "Contract output dtype is invalid")
    _require(
        model_output.get("shape") == ["N", OUTPUT_WIDTH],
        "Contract output shape is invalid",
    )
    slices = model_output.get("slices")
    _require(
        isinstance(slices, dict) and set(slices) == set(EXPECTED_SLICES),
        "Contract output slices are invalid",
    )
    for name, (offset, length, postprocess, labels) in EXPECTED_SLICES.items():
        value = slices.get(name)
        _require(
            isinstance(value, dict), f"Contract output slice {name} must be an object"
        )
        _require(
            value.get("offset") == offset and value.get("length") == length,
            f"Contract output slice {name} bounds are invalid",
        )
        _require(
            value.get("postprocess") == postprocess,
            f"Contract output slice {name} postprocess is invalid",
        )
        if labels is not None:
            _require(
                value.get("labels") == labels,
                f"Contract output slice {name} labels are invalid",
            )
        else:
            _require(
                value.get("encoding") == "normalized-context-cxcywh",
                f"Contract output slice {name} encoding is invalid",
            )
            _require(
                value.get("modelOutputRange") == [0.0, 1.0],
                f"Contract output slice {name} range is invalid",
            )

    thresholds = contract.get("acceptanceThresholds")
    _require(
        isinstance(thresholds, dict), "Contract acceptanceThresholds must be an object"
    )
    _require(
        set(thresholds) == {"textness", "relation", "patchMode"},
        "Contract threshold names are invalid",
    )
    for name, threshold in thresholds.items():
        _require(
            isinstance(threshold, (int, float))
            and not isinstance(threshold, bool)
            and math.isfinite(threshold)
            and 0.0 < threshold < 1.0,
            f"Contract threshold {name} is invalid",
        )

    golden = model_input["goldenTensor"]
    golden_name = _require_plain_filename(
        golden.get("path"), "Contract goldenTensor.path"
    )
    golden_sha = _require_sha256(golden.get("sha256"), "Contract goldenTensor.sha256")
    golden_path = run_directory / golden_name
    _require(
        golden_path.is_file(), f"Golden preprocessing tensor is missing: {golden_path}"
    )
    _require(
        sha256_file(golden_path) == golden_sha,
        "Golden preprocessing tensor hash differs from contract",
    )
    canonical_golden_path = Path(__file__).with_name("preprocess_golden.json")
    _require(
        sha256_file(golden_path) == sha256_file(canonical_golden_path),
        "Golden preprocessing tensor differs from the repository canonical file",
    )

    training_run = contract.get("trainingRun")
    _require(isinstance(training_run, dict), "Contract trainingRun must be an object")
    dataset_evidence = training_run.get("datasetManifest")
    _require(
        isinstance(dataset_evidence, dict), "Contract datasetManifest must be an object"
    )
    manifest_name = _require_plain_filename(
        dataset_evidence.get("path"), "Contract datasetManifest.path"
    )
    manifest_sha = _require_sha256(
        dataset_evidence.get("sha256"), "Contract datasetManifest.sha256"
    )
    manifest_path = run_directory / manifest_name
    _require(manifest_path.is_file(), f"Dataset manifest is missing: {manifest_path}")
    _require(
        sha256_file(manifest_path) == manifest_sha,
        "Dataset manifest hash differs from contract",
    )
    manifest = read_json(manifest_path)
    try:
        validate_dataset_manifest(manifest)
    except DatasetValidationError as error:
        raise QualificationError(f"Dataset manifest is invalid:\n{error}") from error
    test_split = dataset_evidence.get("testSplit")
    _require(
        isinstance(test_split, dict), "Contract test split evidence must be an object"
    )
    _require(
        test_split == manifest["splits"]["test"],
        "Test split evidence differs from manifest entries",
    )
    return manifest


def validate_onnx_io(
    model_input: Any, model_output: Any, contract: dict[str, Any]
) -> None:
    _require(
        model_input.name == contract["input"]["name"],
        "ONNX input name differs from contract",
    )
    _require(
        model_input.type == "tensor(float)", "ONNX input dtype differs from contract"
    )
    _require(
        model_input.shape[1:] == [INPUT_CHANNELS, INPUT_HEIGHT, INPUT_WIDTH],
        "ONNX input shape differs from contract",
    )
    _require(
        model_output.name == contract["output"]["name"],
        "ONNX output name differs from contract",
    )
    _require(
        model_output.type == "tensor(float)", "ONNX output dtype differs from contract"
    )
    _require(
        model_output.shape[1:] == [OUTPUT_WIDTH],
        "ONNX output shape differs from contract",
    )
    input_batch = model_input.shape[0]
    output_batch = model_output.shape[0]
    _require(
        isinstance(input_batch, str) and input_batch and input_batch == output_batch,
        "ONNX input and output must share a symbolic dynamic batch dimension",
    )


def validate_onnx_contract(model_path: Path, contract: dict[str, Any]) -> None:
    try:
        import numpy as np
        import onnx
        import onnxruntime
    except ImportError as error:
        raise QualificationError(
            f"ONNX qualification dependencies are unavailable: {error}"
        ) from error

    try:
        model = onnx.load(model_path)
        onnx.checker.check_model(model)
        default_opsets = [
            value.version
            for value in model.opset_import
            if value.domain in ("", "ai.onnx")
        ]
        _require(
            default_opsets == [17], f"ONNX model opset is invalid: {default_opsets}"
        )
        session = onnxruntime.InferenceSession(
            str(model_path), providers=["CPUExecutionProvider"]
        )
        _require(
            len(session.get_inputs()) == 1 and len(session.get_outputs()) == 1,
            "ONNX model must have one input and output",
        )
        model_input = session.get_inputs()[0]
        model_output = session.get_outputs()[0]
        validate_onnx_io(model_input, model_output, contract)
        values = np.zeros(
            (2, INPUT_CHANNELS, INPUT_HEIGHT, INPUT_WIDTH), dtype=np.float32
        )
        values[1, :6] = 0.5
        values[1, 6] = 1.0
        outputs = session.run([model_output.name], {model_input.name: values})[0]
        _require(
            outputs.shape == (2, OUTPUT_WIDTH),
            "ONNX runtime batch-2 output shape is invalid",
        )
        _require(
            bool(np.isfinite(outputs).all()),
            "ONNX runtime output contains non-finite values",
        )
    except QualificationError:
        raise
    except Exception as error:
        raise QualificationError(f"ONNX model validation failed: {error}") from error


def validate_candidate_metadata(run_directory: Path) -> dict[str, Any]:
    run_directory = run_directory.resolve()
    contract_path = run_directory / "model_contract.json"
    metrics_path = run_directory / "metrics.json"
    model_path = run_directory / "candidate_model.onnx"
    for path in (contract_path, metrics_path, model_path):
        _require(path.is_file(), f"Required candidate input does not exist: {path}")

    contract = read_json(contract_path)
    metrics = read_json(metrics_path)
    _require(isinstance(contract, dict), "Model contract must be an object")
    _require(isinstance(metrics, dict), "Metrics must be an object")
    model_sha = sha256_file(model_path)
    contract_sha = sha256_file(contract_path)
    metrics_sha = sha256_file(metrics_path)
    manifest = _validate_contract(contract, run_directory, model_sha)

    dataset_evidence = contract["trainingRun"]["datasetManifest"]
    test_split_sha = dataset_evidence["testSplit"]["sha256"]
    _require(
        metrics.get("artifactStatus") == "research_candidate",
        "Metrics are not for a research candidate",
    )
    _require(
        metrics.get("deploymentEligible") is False,
        "Candidate metrics must remain non-deployable",
    )
    _require(
        metrics.get("modelSha256") == model_sha, "Metrics are for a different model"
    )
    _require(
        metrics.get("contractSha256") == contract_sha,
        "Metrics are for a different contract",
    )
    _require(
        metrics.get("datasetManifestSha256") == dataset_evidence["sha256"],
        "Metrics use a different dataset",
    )
    _require(
        metrics.get("testSplitManifestSha256") == test_split_sha,
        "Metrics use a different test split",
    )
    _require(
        metrics.get("test") == contract.get("testEvaluation"),
        "Metrics and contract test evaluations differ",
    )
    metric_errors = validate_statistical_evidence(
        metrics.get("test"),
        contract.get("acceptanceThresholds"),
        available_split_groups=manifest["splits"]["test"]["splitGroupCount"],
        expected_unit_count=manifest["splits"]["test"]["trainingUnitCount"],
    )
    _require(
        not metric_errors,
        "ONNX test metric evidence is invalid:\n" + "\n".join(metric_errors),
    )
    _require(
        metrics["test"]["statisticalGateEligible"] is True,
        "ONNX test metrics did not pass the gate",
    )

    return {
        "runDirectory": run_directory,
        "contract": contract,
        "contractPath": contract_path,
        "contractSha256": contract_sha,
        "metricsPath": metrics_path,
        "metricsSha256": metrics_sha,
        "modelPath": model_path,
        "modelSha256": model_sha,
        "manifest": manifest,
        "datasetEvidence": dataset_evidence,
    }


def qualify(run_directory: Path) -> dict[str, Any]:
    run_directory = run_directory.resolve()
    audit_path = run_directory / "candidate_audit.json"
    audit_path.unlink(missing_ok=True)
    evidence = validate_candidate_metadata(run_directory)
    contract = evidence["contract"]
    model_path = evidence["modelPath"]
    validate_onnx_contract(model_path, contract)
    dataset_evidence = evidence["datasetEvidence"]
    manifest = evidence["manifest"]

    audit = {
        "auditVersion": 1,
        "status": "candidate_gate_passed",
        "auditedAt": utc_timestamp(),
        "deploymentEligible": False,
        "deploymentBlockedReason": DEPLOYMENT_BLOCK_REASON,
        "model": {"path": model_path.name, "sha256": evidence["modelSha256"]},
        "contract": {
            "path": evidence["contractPath"].name,
            "sha256": evidence["contractSha256"],
        },
        "metrics": {
            "path": evidence["metricsPath"].name,
            "sha256": evidence["metricsSha256"],
        },
        "datasetManifest": {
            "path": dataset_evidence["path"],
            "sha256": dataset_evidence["sha256"],
            "testSplitSha256": manifest["splits"]["test"]["sha256"],
        },
    }
    atomic_write_json(audit_path, audit)
    return audit


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Audit a Visnip UI region candidate without authorizing deployment"
    )
    parser.add_argument("--run", type=Path, required=True)
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)
    try:
        audit = qualify(args.run)
    except (
        QualificationError,
        DatasetValidationError,
        OSError,
        json.JSONDecodeError,
    ) as error:
        print(str(error), file=sys.stderr)
        return 1
    print(json.dumps(audit, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
