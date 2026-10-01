from __future__ import annotations

import json
import hashlib
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from types import SimpleNamespace

from PIL import Image

from tools.ui_region_model.export_diagnostics import export_diagnostics
from tools.ui_region_model.import_external_screenshots import import_selection
from tools.ui_region_model.qualify import (
    QualificationError,
    qualify,
    validate_candidate_metadata,
    validate_onnx_io,
)
from tools.ui_region_model.release_gate import (
    statistical_gate_eligible,
    validate_statistical_evidence,
    wilson_lower,
    wilson_upper,
)
from tools.ui_region_model.schema import (
    DatasetValidationError,
    atomic_write_json,
    read_json,
    sha256_file,
    validate_sample,
)
from tools.ui_region_model.server import make_server
from tools.ui_region_model.preprocess import prepare_region_input
from tools.ui_region_model.validate_dataset import (
    build_dataset_manifest,
    canonical_sha256,
    load_manifest_sample,
    validate_dataset,
    validate_dataset_manifest,
)


def passing_test_evaluation() -> dict:
    thresholds = {"textness": 0.95, "relation": 0.9, "patchMode": 0.9}

    def classification(support: list[int]) -> dict:
        return {
            "macroF1": 1.0,
            "perClassF1": [1.0] * len(support),
            "confusionMatrix": [
                [count if row == column else 0 for column in range(len(support))]
                for row, count in enumerate(support)
            ],
            "support": support,
            "splitGroupSupport": [10] * len(support),
            "minimumUnitSupport": 25,
            "minimumSplitGroupSupport": 10,
            "evaluable": True,
        }

    zero_error_slice = {
        "clusterCount": 598,
        "falseAcceptClusters": 0,
        "falseAcceptClusterRate": 0.0,
        "falseAcceptClusterRate95Upper": wilson_upper(0, 598),
    }
    evaluation = {
        "gateEvidenceVersion": 1,
        "inferenceEngine": "onnxruntime",
        "unitCount": 2392,
        "loss": {"total": 0.0},
        "classification": {
            "textness": classification([1196, 598, 598]),
            "role": classification([196, 200, 200, 200, 200, 200]),
            "relation": classification([598, 598, 598, 598]),
            "patchMode": classification([598, 598, 1196]),
        },
        "geometry": {
            "unitCount": 598,
            "splitGroupCount": 598,
            "eraseMeanIoU": 1.0,
            "layoutMeanIoU": 1.0,
            "eraseCoverageP01": 1.0,
            "eraseCoverageMinimum": 1.0,
            "eraseSpillP99": 0.0,
            "eraseSpillMaximum": 0.0,
            "layoutCoverageP01": 1.0,
            "layoutCoverageMinimum": 1.0,
            "layoutSpillP99": 0.0,
            "layoutSpillMaximum": 0.0,
            "eraseOutsideLayoutP99": 0.0,
            "eraseOutsideLayoutMaximum": 0.0,
        },
        "safetyGate": {
            "thresholds": thresholds,
            "nonText": 598,
            "nonTextFalseAccepts": 0,
            "mixed": 598,
            "mixedFalseAccepts": 0,
            "unsafeText": 598,
            "unsafeTextFalseAccepts": 0,
            "clusterSlices": {
                "nonText": dict(zero_error_slice),
                "mixed": dict(zero_error_slice),
                "unsafeText": dict(zero_error_slice),
            },
            "safeTextCount": 598,
            "acceptedSafeText": 598,
            "safeTextRecall": 1.0,
            "safeTextSplitGroupCount": 598,
            "safeTextMissedSplitGroups": 0,
            "safeTextSplitGroupRecall95Lower": wilson_lower(598, 598),
            "visuallyAcceptedByTranslationPolicy": {
                "translate": 598,
                "preserve": 0,
                "review": 0,
            },
        },
    }
    evaluation["classification"]["textness"]["splitGroupSupport"] = [598, 598, 598]
    evaluation["classification"]["role"]["splitGroupSupport"] = [
        98,
        100,
        100,
        100,
        100,
        100,
    ]
    evaluation["classification"]["relation"]["splitGroupSupport"] = [
        598,
        598,
        10,
        598,
    ]
    evaluation["classification"]["patchMode"]["splitGroupSupport"] = [598, 10, 598]
    evaluation["statisticalGateEligible"] = statistical_gate_eligible(evaluation)
    evaluation["deploymentEligible"] = False
    return evaluation


class PipelineTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.diagnostics = self.root / "diagnostics"
        self.dataset = self.root / "dataset"
        self.diagnostics.mkdir()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_diagnostic(
        self, name: str, color: tuple[int, int, int], detected_box: bool = True
    ) -> None:
        directory = self.diagnostics / name
        directory.mkdir()
        image = Image.new("RGB", (160, 100), color=color)
        image.save(directory / "original.png")
        line = {
            "index": 0,
            "box": {"x": 20, "y": 18, "width": 48, "height": 16},
            "text": "Public",
            "score": 0.97,
        }
        if detected_box:
            line["detectedBox"] = dict(line["box"])
            line["acceptedForTranslation"] = True
        analysis = {
            "createdAt": "2026-07-28T12:00:00.000Z",
            "originalFile": "original.png",
            "status": "succeeded",
            "selection": {"x": 0, "y": 0, "width": 160, "height": 100},
            "ocrLines": [line],
        }
        (directory / "analysis.json").write_text(json.dumps(analysis), encoding="utf-8")

    def exported_sample(self) -> dict:
        sample_path = next((self.dataset / "samples").glob("*.json"))
        return read_json(sample_path)

    def test_export_keeps_each_ocr_observation_as_an_independent_proposal(self) -> None:
        self.write_diagnostic(
            "20260728_120000_overlay1_job1", (245, 245, 245), detected_box=False
        )
        self.write_diagnostic(
            "20260728_120001_overlay1_job1", (245, 245, 245), detected_box=True
        )
        summary = export_diagnostics(self.diagnostics, self.dataset)
        self.assertEqual(summary["samples"], 1)
        sample = self.exported_sample()
        self.assertEqual(len(sample["ocrProposals"]), 2)
        self.assertEqual(len(sample["annotations"]), 2)
        self.assertTrue(
            all(
                len(proposal["observations"]) == 1
                for proposal in sample["ocrProposals"]
            )
        )
        for annotation, proposal in zip(sample["annotations"], sample["ocrProposals"]):
            self.assertEqual(annotation["textness"], "unknown")
            self.assertEqual(annotation["proposalIds"], [proposal["id"]])
        validate_sample(sample, self.dataset)

    def test_repeated_export_is_strictly_idempotent(self) -> None:
        self.write_diagnostic("a", (245, 245, 245))
        first = export_diagnostics(self.diagnostics, self.dataset)
        metadata_before = (self.dataset / "dataset.json").read_bytes()
        sample_before = next((self.dataset / "samples").glob("*.json")).read_bytes()
        second = export_diagnostics(self.diagnostics, self.dataset)
        self.assertEqual(first["addedCandidates"], 1)
        self.assertEqual(second["addedCandidates"], 0)
        self.assertEqual(second["updatedSamples"], 0)
        self.assertEqual((self.dataset / "dataset.json").read_bytes(), metadata_before)
        self.assertEqual(
            next((self.dataset / "samples").glob("*.json")).read_bytes(), sample_before
        )

    def test_manifest_loader_rejects_sample_mutation(self) -> None:
        self.write_diagnostic("a", (245, 245, 245))
        export_diagnostics(self.diagnostics, self.dataset)
        manifest = build_dataset_manifest(self.dataset)
        entry = manifest["samples"][0]

        sample, image_bytes = load_manifest_sample(self.dataset, entry)
        self.assertEqual(sample["sampleId"], entry["sampleId"])
        self.assertTrue(image_bytes)

        sample_path = self.dataset / "samples" / f"{entry['sampleId']}.json"
        sample["extension"] = {"changedAfterSnapshot": True}
        atomic_write_json(sample_path, sample)
        with self.assertRaises(DatasetValidationError) as caught:
            load_manifest_sample(self.dataset, entry)
        self.assertIn("changed after manifest creation", str(caught.exception))

    def test_manifest_loader_rejects_image_mutation(self) -> None:
        self.write_diagnostic("a", (245, 245, 245))
        export_diagnostics(self.diagnostics, self.dataset)
        manifest = build_dataset_manifest(self.dataset)
        entry = manifest["samples"][0]
        sample = read_json(self.dataset / "samples" / f"{entry['sampleId']}.json")
        image_path = self.dataset / sample["image"]["path"]

        Image.new("RGB", (160, 100), color=(0, 0, 0)).save(image_path)
        with self.assertRaises(DatasetValidationError):
            load_manifest_sample(self.dataset, entry)

    def test_legacy_split_group_migration_preserves_manual_value(self) -> None:
        self.write_diagnostic("a", (245, 245, 245))
        export_diagnostics(self.diagnostics, self.dataset)
        sample_path = next((self.dataset / "samples").glob("*.json"))
        sample = read_json(sample_path)
        sample.pop("splitGroupMethod")
        sample["splitGroup"] = " Curated-App-Session "
        sample["splitKey"]["appFamily"] = " Chrome "
        sample_path.write_text(json.dumps(sample), encoding="utf-8")
        export_diagnostics(self.diagnostics, self.dataset)
        migrated = read_json(sample_path)
        self.assertEqual(migrated["splitGroup"], "curated-app-session")
        self.assertEqual(migrated["splitGroupMethod"], "manual")
        self.assertEqual(migrated["splitKey"]["appFamily"], "chrome")

    def test_similar_images_share_a_split_group(self) -> None:
        self.write_diagnostic("a", (240, 240, 240))
        self.write_diagnostic("b", (241, 241, 241))
        export_diagnostics(self.diagnostics, self.dataset)
        samples = [
            read_json(path)
            for path in sorted((self.dataset / "samples").glob("*.json"))
        ]
        self.assertEqual(len(samples), 2)
        self.assertEqual(samples[0]["splitGroup"], samples[1]["splitGroup"])

    def test_text_annotation_requires_safe_geometry(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        sample = self.exported_sample()
        annotation = sample["annotations"][0]
        annotation.update(
            {
                "textness": "text",
                "role": "button",
                "transcription": "Public",
                "translationPolicy": "translate",
                "labelStatus": "verified",
                "relation": "single",
                "patchMode": "rect_safe",
                "maskBox": dict(annotation["textBox"]),
                "layoutBox": {"x": 16, "y": 14, "width": 70, "height": 24},
            }
        )
        validate_sample(sample, self.dataset)
        annotation["maskBox"] = {"x": 21, "y": 18, "width": 47, "height": 16}
        with self.assertRaises(DatasetValidationError):
            validate_sample(sample, self.dataset)

    def test_model_context_limit_is_a_schema_rule(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        sample = self.exported_sample()
        annotation = sample["annotations"][0]
        annotation.update(
            {
                "textness": "text",
                "role": "button",
                "transcription": "Public",
                "translationPolicy": "translate",
                "labelStatus": "verified",
                "relation": "single",
                "patchMode": "rect_safe",
                "maskBox": dict(annotation["textBox"]),
                "layoutBox": {"x": 0, "y": 0, "width": 160, "height": 100},
            }
        )
        with self.assertRaises(DatasetValidationError):
            validate_sample(sample, self.dataset)

    def test_validator_treats_ignored_as_resolved(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        sample_path = next((self.dataset / "samples").glob("*.json"))
        sample = read_json(sample_path)
        annotation = sample["annotations"][0]
        annotation["labelStatus"] = "ignored"
        sample["coverage"]["textComplete"] = True
        sample["coverage"]["anchorsComplete"] = True
        sample["coverage"]["protectedRegionsComplete"] = True
        sample["splitKey"]["independenceReviewed"] = True
        sample["splitGroupMethod"] = "manual"

        atomic_write_json(sample_path, sample)
        summary = validate_dataset(self.dataset, require_reviewed=True)
        self.assertEqual(summary["unreviewed"], 0)

    def test_strict_validation_requires_all_coverage_and_split_isolation(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        sample_path = next((self.dataset / "samples").glob("*.json"))
        sample = read_json(sample_path)
        sample["annotations"][0]["labelStatus"] = "ignored"
        sample["coverage"].update(
            {
                "textComplete": False,
                "anchorsComplete": True,
                "protectedRegionsComplete": True,
            }
        )
        sample["splitKey"]["independenceReviewed"] = False
        atomic_write_json(sample_path, sample)
        with self.assertRaises(DatasetValidationError) as caught:
            validate_dataset(self.dataset, require_reviewed=True)
        self.assertIn("text coverage is incomplete", str(caught.exception))
        self.assertIn("split isolation was not reviewed", str(caught.exception))

    def test_annotation_must_remain_anchored_to_its_ocr_proposal(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        sample = self.exported_sample()
        sample["annotations"][0]["textBox"] = {
            "x": 100,
            "y": 70,
            "width": 20,
            "height": 10,
        }
        with self.assertRaises(DatasetValidationError) as caught:
            validate_sample(sample, self.dataset)
        self.assertIn(
            "must meaningfully overlap linked proposal", str(caught.exception)
        )

        sample = self.exported_sample()
        sample["annotations"][0]["textBox"] = {
            "x": 67,
            "y": 18,
            "width": 50,
            "height": 16,
        }
        with self.assertRaises(DatasetValidationError) as caught:
            validate_sample(sample, self.dataset)
        self.assertIn(
            "must meaningfully overlap linked proposal", str(caught.exception)
        )

        sample = self.exported_sample()
        sample["annotations"][0]["textBox"] = {
            "x": 44,
            "y": 18,
            "width": 48,
            "height": 16,
        }
        validate_sample(sample, self.dataset)

    def test_nested_sources_and_observations_are_validated(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        sample = self.exported_sample()
        sample["sources"] = ["not-an-object"]
        with self.assertRaises(DatasetValidationError):
            validate_sample(sample, self.dataset)

        baseline = self.exported_sample()
        corruptions = {
            "mime": lambda value: value["image"].__setitem__("mimeType", []),
            "anchor": lambda value: value["ocrProposals"][0][
                "anchorObservation"
            ].__setitem__("sourceLineIndex", []),
            "container": lambda value: value["annotations"][0].__setitem__(
                "containerId", []
            ),
            "parent": lambda value: value["annotations"][0].__setitem__(
                "parentAnnotationId", []
            ),
            "textness": lambda value: value["annotations"][0].__setitem__(
                "textness", []
            ),
            "nan-dpi": lambda value: value["splitKey"].__setitem__(
                "dpiScale", float("nan")
            ),
            "nan-observation": lambda value: value["ocrProposals"][0]["observations"][
                0
            ].__setitem__("contrast", float("inf")),
            "unknown-nan": lambda value: value.__setitem__(
                "extension", {"score": float("nan")}
            ),
            "proposal-ids": lambda value: value["annotations"][0].__setitem__(
                "proposalIds", [{}]
            ),
        }
        for name, corrupt in corruptions.items():
            with self.subTest(name=name):
                malformed = json.loads(json.dumps(baseline))
                corrupt(malformed)
                with self.assertRaises(DatasetValidationError):
                    validate_sample(malformed, self.dataset)

    def test_external_screenshot_source_requires_traceable_provenance(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        sample = self.exported_sample()
        source = sample["sources"][0]
        source.update(
            {
                "kind": "external_screenshot",
                "selectionCoordinateSpace": "original-image-pixels",
                "originUrl": "https://upload.wikimedia.org/example.png",
                "descriptionUrl": "https://commons.wikimedia.org/wiki/File:Example.png",
                "sourceTitle": "File:Example.png",
                "author": "Example Author",
                "licenseName": "CC BY-SA 4.0",
                "licenseUrl": "https://creativecommons.org/licenses/by-sa/4.0/",
                "retrievedAt": "2026-07-29T00:00:00Z",
                "category": "application",
                "query": "software application screenshot",
                "originalSha256": "a" * 64,
            }
        )
        validate_sample(sample, self.dataset)

        without_original_hash = json.loads(json.dumps(sample))
        without_original_hash["sources"][0].pop("originalSha256")
        validate_sample(without_original_hash, self.dataset)

        for name in ("originUrl", "licenseName", "originalSha256"):
            with self.subTest(name=name):
                malformed = json.loads(json.dumps(sample))
                malformed["sources"][0][name] = ""
                with self.assertRaises(DatasetValidationError):
                    validate_sample(malformed, self.dataset)

    def test_external_import_preserves_provenance_and_raw_ocr_proposals(self) -> None:
        source_root = self.root / "external"
        images_root = source_root / "downloaded"
        images_root.mkdir(parents=True)
        image_path = images_root / "example.png"
        Image.new("RGB", (160, 100), color=(245, 245, 245)).save(image_path)
        image_sha = sha256_file(image_path)
        selection_path = source_root / "selection.json"
        selection_path.write_text(
            json.dumps(
                {
                    "selected": [
                        {
                            "normalizedPath": image_path.name,
                            "normalizedSha256": image_sha,
                            "normalizedWidth": 160,
                            "normalizedHeight": 100,
                            "retrievedAt": "2026-07-29T00:00:00Z",
                            "category": "application_ui",
                            "title": "File:Example application.png",
                            "originUrl": "https://upload.wikimedia.org/example.png",
                            "descriptionUrl": "https://commons.wikimedia.org/wiki/File:Example.png",
                            "author": "Example Author",
                            "licenseName": "CC0",
                            "licenseUrl": "https://creativecommons.org/publicdomain/zero/1.0/",
                            "query": "test pool",
                            "originalSha256": "a" * 64,
                        }
                    ]
                }
            ),
            encoding="utf-8",
        )
        ocr_path = source_root / "ocr.json"
        ocr_path.write_text(
            json.dumps(
                {
                    "formatVersion": 1,
                    "results": [
                        {
                            "path": str(image_path),
                            "packId": "general-v5",
                            "width": 160,
                            "height": 100,
                            "error": "",
                            "lines": [
                                {
                                    "index": 0,
                                    "textBox": {
                                        "x": 20,
                                        "y": 18,
                                        "width": 48,
                                        "height": 16,
                                    },
                                    "detectedBox": {
                                        "x": 20,
                                        "y": 18,
                                        "width": 48,
                                        "height": 16,
                                    },
                                    "recognizedText": "Public",
                                    "recognitionScore": 0.97,
                                    "leadingIconSeparated": False,
                                    "refinementReason": "",
                                }
                            ],
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        summary = import_selection(
            selection_path, images_root, ocr_path, self.dataset
        )
        self.assertEqual(summary["imported"], 1)
        self.assertEqual(summary["proposals"], 1)
        sample = self.exported_sample()
        self.assertEqual(sample["sources"][0]["kind"], "external_screenshot")
        self.assertEqual(sample["annotations"][0]["labelStatus"], "unreviewed")
        self.assertIsNone(
            sample["ocrProposals"][0]["observations"][0]["pipelineDecision"][
                "acceptedForTranslation"
            ]
        )
        validate_sample(sample, self.dataset)

    def test_invalid_nested_boxes_report_errors_instead_of_crashing(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        sample = self.exported_sample()
        annotation = sample["annotations"][0]
        annotation.update(
            {
                "textness": "text",
                "role": "button",
                "transcription": "Public",
                "translationPolicy": "translate",
                "labelStatus": "verified",
                "relation": "single",
                "patchMode": "rect_safe",
                "maskBox": dict(annotation["textBox"]),
                "layoutBox": dict(annotation["textBox"]),
            }
        )
        sample["ocrProposals"][0].pop("textBox")
        with self.assertRaises(DatasetValidationError):
            validate_sample(sample, self.dataset)

        sample = self.exported_sample()
        sample["layoutContainers"].append(
            {"id": "broken", "role": "button", "parentId": None}
        )
        sample["annotations"][0]["containerId"] = "broken"
        with self.assertRaises(DatasetValidationError):
            validate_sample(sample, self.dataset)

    def test_incremental_export_upserts_source_and_observation_state(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        analysis_path = self.diagnostics / "a" / "analysis.json"
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
        analysis["status"] = "partial"
        analysis["ocrLines"][0]["acceptedForTranslation"] = False
        analysis_path.write_text(json.dumps(analysis), encoding="utf-8")
        summary = export_diagnostics(self.diagnostics, self.dataset)
        self.assertEqual(summary["updatedSamples"], 1)
        self.assertEqual(summary["addedCandidates"], 0)
        sample = self.exported_sample()
        proposal = sample["ocrProposals"][0]
        self.assertNotIn("recognizedText", proposal)
        self.assertEqual(sample["sources"][0]["status"], "partial")
        self.assertFalse(
            proposal["observations"][0]["pipelineDecision"]["acceptedForTranslation"]
        )

    def test_incremental_export_removes_disappeared_unreviewed_lines(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        analysis_path = self.diagnostics / "a" / "analysis.json"
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
        second = dict(analysis["ocrLines"][0])
        second.update(
            {"index": 1, "box": {"x": 80, "y": 18, "width": 48, "height": 16}}
        )
        second["detectedBox"] = dict(second["box"])
        analysis["ocrLines"].append(second)
        analysis_path.write_text(json.dumps(analysis), encoding="utf-8")
        export_diagnostics(self.diagnostics, self.dataset)
        analysis["ocrLines"].pop()
        analysis_path.write_text(json.dumps(analysis), encoding="utf-8")
        summary = export_diagnostics(self.diagnostics, self.dataset)
        self.assertEqual(summary["removedCandidates"], 1)
        self.assertEqual(len(self.exported_sample()["ocrProposals"]), 1)

    def test_incremental_export_rejects_source_image_mutation(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        Image.new("RGB", (160, 100), color=(20, 30, 40)).save(
            self.diagnostics / "a" / "original.png"
        )
        with self.assertRaises(DatasetValidationError):
            export_diagnostics(self.diagnostics, self.dataset)

    def test_jpeg_diagnostic_keeps_a_truthful_extension_and_mime_type(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        directory = self.diagnostics / "a"
        with Image.open(directory / "original.png") as image:
            image.save(directory / "original.jpg", format="JPEG")
        (directory / "original.png").unlink()
        analysis_path = directory / "analysis.json"
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
        analysis["originalFile"] = "original.jpg"
        analysis_path.write_text(json.dumps(analysis), encoding="utf-8")
        export_diagnostics(self.diagnostics, self.dataset)
        sample = self.exported_sample()
        self.assertTrue(sample["image"]["path"].endswith(".jpg"))
        self.assertEqual(sample["image"]["mimeType"], "image/jpeg")
        validate_sample(sample, self.dataset)

    def test_coordinate_and_content_addressing_contract_is_enforced(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        sample = self.exported_sample()
        sample["image"]["coordinateSpace"] = "screen-pixels"
        with self.assertRaises(DatasetValidationError):
            validate_sample(sample, self.dataset)
        sample = self.exported_sample()
        sample["splitKey"]["appFamily"] = " Chrome "
        with self.assertRaises(DatasetValidationError):
            validate_sample(sample, self.dataset)
        sample = self.exported_sample()
        sample["splitKey"]["appFamily"] = "chr\u043eme"
        with self.assertRaises(DatasetValidationError):
            validate_sample(sample, self.dataset)

    def test_proposal_cannot_combine_multiple_raw_ocr_observations(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        sample = self.exported_sample()
        duplicate = dict(sample["ocrProposals"][0]["observations"][0])
        duplicate["sourceLineIndex"] = 1
        sample["ocrProposals"][0]["observations"].append(duplicate)
        with self.assertRaises(DatasetValidationError) as caught:
            validate_sample(sample, self.dataset)
        self.assertIn("exactly one raw OCR observation", str(caught.exception))

    def test_overlapping_lines_in_one_diagnostic_are_not_dropped(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        analysis_path = self.diagnostics / "a" / "analysis.json"
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
        second = dict(analysis["ocrLines"][0])
        second["index"] = 1
        second["text"] = "Publ1c"
        second["score"] = 0.76
        analysis["ocrLines"].append(second)
        analysis_path.write_text(json.dumps(analysis), encoding="utf-8")
        export_diagnostics(self.diagnostics, self.dataset)
        sample = self.exported_sample()
        self.assertEqual(len(sample["ocrProposals"]), 2)
        self.assertEqual(len(sample["annotations"]), 2)

    def test_merge_group_requires_two_distinct_ocr_proposals(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        analysis_path = self.diagnostics / "a" / "analysis.json"
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
        second = dict(analysis["ocrLines"][0])
        second.update(
            {
                "index": 1,
                "box": {"x": 72, "y": 18, "width": 48, "height": 16},
                "detectedBox": {"x": 72, "y": 18, "width": 48, "height": 16},
                "text": "repository",
            }
        )
        analysis["ocrLines"].append(second)
        analysis_path.write_text(json.dumps(analysis), encoding="utf-8")
        export_diagnostics(self.diagnostics, self.dataset)
        sample = self.exported_sample()
        for annotation, transcription in zip(
            sample["annotations"], ("Public", "repository")
        ):
            annotation.update(
                {
                    "textness": "text",
                    "role": "body",
                    "transcription": transcription,
                    "translationPolicy": "translate",
                    "labelStatus": "verified",
                    "relation": "merge_required",
                    "patchMode": "none",
                    "groupId": "merge-1",
                }
            )
        validate_sample(sample, self.dataset)
        sample["annotations"][1]["groupId"] = "merge-2"
        with self.assertRaises(DatasetValidationError):
            validate_sample(sample, self.dataset)

    def test_validator_checks_the_image_bytes(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        sample = self.exported_sample()
        image_path = self.dataset / sample["image"]["path"]
        Image.new("RGB", (160, 100), color=(0, 0, 0)).save(image_path)
        with self.assertRaises(DatasetValidationError):
            validate_sample(sample, self.dataset)

    def test_model_contract_packs_one_contiguous_output(self) -> None:
        contract_path = Path(__file__).parents[1] / "model_contract.template.json"
        contract = json.loads(contract_path.read_text(encoding="utf-8"))
        self.assertEqual(contract["contractVersion"], 2)
        self.assertEqual(contract["input"]["shape"], ["N", 7, 96, 256])
        self.assertEqual(contract["output"]["shape"], ["N", 24])
        occupied: list[int] = []
        for value in contract["output"]["slices"].values():
            occupied.extend(range(value["offset"], value["offset"] + value["length"]))
        self.assertEqual(sorted(occupied), list(range(24)))
        self.assertEqual(
            contract["output"]["slices"]["textness"]["postprocess"], "softmax"
        )
        self.assertEqual(
            contract["output"]["slices"]["eraseBox"]["postprocess"], "identity"
        )
        self.assertEqual(
            contract["output"]["slices"]["eraseBox"]["modelOutputRange"], [0.0, 1.0]
        )

    def test_preprocessing_matches_the_versioned_golden_tensor(self) -> None:
        golden_path = Path(__file__).parents[1] / "preprocess_golden.json"
        golden = json.loads(golden_path.read_text(encoding="utf-8"))
        width = golden["image"]["width"]
        height = golden["image"]["height"]
        image = Image.new("RGB", (width, height))
        image.putdata(
            [
                ((x * 17 + y * 3) % 256, (x * 5 + y * 29) % 256, (x * 11 + y * 7) % 256)
                for y in range(height)
                for x in range(width)
            ]
        )
        tensor = prepare_region_input(image, golden["proposalBox"]).astype(
            "<f4", copy=False
        )
        self.assertEqual(list(tensor.shape), golden["tensor"]["shape"])
        self.assertEqual(
            hashlib.sha256(tensor.tobytes(order="C")).hexdigest(),
            golden["tensor"]["sha256"],
        )

    def test_unlinked_proposal_is_rejected(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        sample = self.exported_sample()
        sample["annotations"].clear()
        with self.assertRaises(DatasetValidationError):
            validate_sample(sample, self.dataset)

    def test_candidate_metadata_is_hash_bound_and_fail_closed(self) -> None:
        run = self.root / "run"
        run.mkdir()
        model_path = run / "candidate_model.onnx"
        model_path.write_bytes(b"candidate model bytes")
        golden_path = run / "preprocess_golden.json"
        golden_path.write_bytes(
            (Path(__file__).parents[1] / "preprocess_golden.json").read_bytes()
        )
        samples = sorted(
            [
                {
                    "sampleId": f"{index:064x}",
                    "splitGroup": f"group-{index:03d}",
                    "split": "test",
                    "sampleJsonSha256": f"{index:064x}",
                    "imageSha256": f"{index + 1000:064x}",
                    "trainingUnitCount": 4,
                }
                for index in range(598)
            ],
            key=lambda value: value["sampleId"],
        )
        test_split = {
            "sha256": canonical_sha256(samples),
            "sampleCount": 598,
            "splitGroupCount": 598,
            "trainingUnitCount": 2392,
        }
        empty_split = {
            "sha256": canonical_sha256([]),
            "sampleCount": 0,
            "splitGroupCount": 0,
            "trainingUnitCount": 0,
        }
        dataset_manifest_path = run / "dataset_manifest.json"
        atomic_write_json(
            dataset_manifest_path,
            {
                "manifestVersion": 1,
                "datasetId": "qualification-test",
                "metadataSha256": "b" * 64,
                "samples": samples,
                "splits": {
                    "train": empty_split,
                    "validation": empty_split,
                    "test": test_split,
                },
            },
        )
        contract_path = Path(__file__).parents[1] / "model_contract.template.json"
        contract = json.loads(contract_path.read_text(encoding="utf-8"))
        test_metrics = passing_test_evaluation()
        contract["input"]["goldenTensor"]["sha256"] = sha256_file(golden_path)
        contract.update(
            {
                "artifactStatus": "research_candidate",
                "deploymentEligible": False,
                "modelFile": model_path.name,
                "modelSha256": sha256_file(model_path),
                "onnxOpset": 17,
                "acceptanceThresholds": {
                    "textness": 0.95,
                    "relation": 0.9,
                    "patchMode": 0.9,
                },
                "testEvaluation": test_metrics,
                "trainingRun": {
                    "datasetManifest": {
                        "path": dataset_manifest_path.name,
                        "sha256": sha256_file(dataset_manifest_path),
                        "testSplit": test_split,
                    }
                },
            }
        )
        model_contract_path = run / "model_contract.json"
        atomic_write_json(model_contract_path, contract)
        metrics = {
            "artifactStatus": "research_candidate",
            "deploymentEligible": False,
            "modelSha256": sha256_file(model_path),
            "contractSha256": sha256_file(model_contract_path),
            "datasetManifestSha256": sha256_file(dataset_manifest_path),
            "testSplitManifestSha256": test_split["sha256"],
            "test": test_metrics,
        }
        metrics_path = run / "metrics.json"
        atomic_write_json(metrics_path, metrics)
        evidence = validate_candidate_metadata(run)
        self.assertEqual(evidence["modelSha256"], sha256_file(model_path))
        self.assertFalse((run / "candidate_audit.json").exists())
        self.assertFalse((run / "deployment_manifest.json").exists())
        atomic_write_json(run / "candidate_audit.json", {"status": "stale"})
        metrics["modelSha256"] = "0" * 64
        atomic_write_json(metrics_path, metrics)
        with self.assertRaises(QualificationError):
            qualify(run)
        self.assertFalse((run / "candidate_audit.json").exists())

    def test_statistical_gate_and_manifest_aggregates_are_recomputed(self) -> None:
        thresholds = {"textness": 0.95, "relation": 0.9, "patchMode": 0.9}
        self.assertTrue(
            validate_statistical_evidence({"statisticalGateEligible": True}, thresholds)
        )

        evaluation = passing_test_evaluation()
        self.assertEqual(
            validate_statistical_evidence(
                evaluation, thresholds, available_split_groups=598
            ),
            [],
        )
        evaluation["safetyGate"]["clusterSlices"]["nonText"]["falseAcceptClusters"] = 1
        self.assertTrue(validate_statistical_evidence(evaluation, thresholds))

        evaluation = passing_test_evaluation()
        evaluation["safetyGate"]["nonTextFalseAccepts"] = 1
        errors = validate_statistical_evidence(evaluation, thresholds)
        self.assertIn("unit and group false accepts disagree", "\n".join(errors))

        evaluation = passing_test_evaluation()
        evaluation["safetyGate"]["acceptedSafeText"] = 597
        evaluation["safetyGate"]["safeTextRecall"] = 597 / 598
        evaluation["safetyGate"]["visuallyAcceptedByTranslationPolicy"]["translate"] = (
            597
        )
        errors = validate_statistical_evidence(evaluation, thresholds)
        self.assertIn("unit and split-group misses disagree", "\n".join(errors))

        evaluation = passing_test_evaluation()
        self.assertTrue(
            validate_statistical_evidence(
                evaluation, thresholds, available_split_groups=20
            )
        )

        evaluation = passing_test_evaluation()
        errors = validate_statistical_evidence(
            evaluation, thresholds, expected_unit_count=2391
        )
        self.assertIn("unitCount differs from the dataset manifest", "\n".join(errors))

        evaluation = passing_test_evaluation()
        evaluation["classification"]["role"]["support"] = [25] * 6
        evaluation["classification"]["role"]["confusionMatrix"] = [
            [25 if row == column else 0 for column in range(6)] for row in range(6)
        ]
        errors = validate_statistical_evidence(
            evaluation, thresholds, available_split_groups=598
        )
        self.assertIn("role support differs from text-only support", "\n".join(errors))

        evaluation = passing_test_evaluation()
        evaluation["classification"]["role"]["confusionMatrix"][0][0] -= 1
        evaluation["classification"]["role"]["confusionMatrix"][0][1] += 1
        errors = validate_statistical_evidence(
            evaluation, thresholds, available_split_groups=598
        )
        self.assertIn("perClassF1 differs from confusion matrix", "\n".join(errors))

        evaluation = passing_test_evaluation()
        evaluation["geometry"]["unitCount"] = 200
        evaluation["geometry"]["splitGroupCount"] = 200
        errors = validate_statistical_evidence(
            evaluation, thresholds, available_split_groups=598
        )
        self.assertIn(
            "geometry unit support differs from rect-safe support", "\n".join(errors)
        )

        evaluation = passing_test_evaluation()
        evaluation["safetyGate"]["nonText"] = 597
        evaluation["safetyGate"]["unsafeText"] = 599
        errors = validate_statistical_evidence(
            evaluation, thresholds, available_split_groups=598
        )
        self.assertIn("nonText differs from textness support", "\n".join(errors))

        evaluation = passing_test_evaluation()
        evaluation["classification"]["role"]["splitGroupSupport"] = [10] * 6
        errors = validate_statistical_evidence(
            evaluation, thresholds, available_split_groups=598
        )
        self.assertIn(
            "role groups cannot cover all text-only groups", "\n".join(errors)
        )

        evaluation = passing_test_evaluation()
        evaluation["classification"]["relation"]["splitGroupSupport"][1] = 10
        errors = validate_statistical_evidence(
            evaluation, thresholds, available_split_groups=598
        )
        self.assertIn(
            "split-required group support is smaller than mixed support",
            "\n".join(errors),
        )

        evaluation = passing_test_evaluation()
        evaluation["geometry"]["eraseCoverageMinimum"] = 1.0
        evaluation["geometry"]["eraseCoverageP01"] = 0.99
        errors = validate_statistical_evidence(
            evaluation, thresholds, available_split_groups=598
        )
        self.assertIn("coverage minimum exceeds its P01", "\n".join(errors))

        entries = [
            {
                "sampleId": "d" * 64,
                "splitGroup": "group-a",
                "split": "test",
                "sampleJsonSha256": "a" * 64,
                "imageSha256": "b" * 64,
                "trainingUnitCount": 1,
            }
        ]
        empty = {
            "sha256": canonical_sha256([]),
            "sampleCount": 0,
            "splitGroupCount": 0,
            "trainingUnitCount": 0,
        }
        manifest = {
            "manifestVersion": 1,
            "datasetId": "test",
            "metadataSha256": "c" * 64,
            "samples": entries,
            "splits": {
                "train": empty,
                "validation": empty,
                "test": {
                    "sha256": canonical_sha256(entries),
                    "sampleCount": 2,
                    "splitGroupCount": 1,
                    "trainingUnitCount": 1,
                },
            },
        }
        with self.assertRaises(DatasetValidationError):
            validate_dataset_manifest(manifest)

    def test_onnx_contract_requires_a_shared_dynamic_batch(self) -> None:
        contract = read_json(Path(__file__).parents[1] / "model_contract.template.json")
        model_input = SimpleNamespace(
            name="regions",
            type="tensor(float)",
            shape=["batch", 7, 96, 256],
        )
        model_output = SimpleNamespace(
            name="predictions",
            type="tensor(float)",
            shape=["batch", 24],
        )
        validate_onnx_io(model_input, model_output, contract)
        model_input.shape[0] = 1
        with self.assertRaises(QualificationError):
            validate_onnx_io(model_input, model_output, contract)

    def test_server_rejects_stale_revision(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        server = make_server("127.0.0.1", 0, self.dataset)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            with urllib.request.urlopen(f"{base}/api/samples") as response:
                sample_id = json.load(response)["samples"][0]["sampleId"]
            with urllib.request.urlopen(f"{base}/api/samples/{sample_id}") as response:
                sample = json.load(response)
            body = json.dumps(sample).encode("utf-8")
            request = urllib.request.Request(
                f"{base}/api/samples/{sample_id}",
                data=body,
                method="PUT",
                headers={"Content-Type": "application/json"},
            )
            with urllib.request.urlopen(request) as response:
                saved = json.load(response)
            self.assertEqual(saved["revision"], sample["revision"] + 1)
            stale_request = urllib.request.Request(
                f"{base}/api/samples/{sample_id}",
                data=body,
                method="PUT",
                headers={"Content-Type": "application/json"},
            )
            with self.assertRaises(urllib.error.HTTPError) as caught:
                urllib.request.urlopen(stale_request)
            self.assertEqual(caught.exception.code, 409)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)

    def test_server_rejects_non_standard_json_and_surrogates(self) -> None:
        self.write_diagnostic("a", (250, 250, 250))
        export_diagnostics(self.diagnostics, self.dataset)
        server = make_server("127.0.0.1", 0, self.dataset)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            with urllib.request.urlopen(f"{base}/api/samples") as response:
                sample_id = json.load(response)["samples"][0]["sampleId"]
            with urllib.request.urlopen(f"{base}/api/samples/{sample_id}") as response:
                sample = json.load(response)
            sample["extension"] = {"score": float("nan")}
            invalid_number = urllib.request.Request(
                f"{base}/api/samples/{sample_id}",
                data=json.dumps(sample).encode("utf-8"),
                method="PUT",
                headers={"Content-Type": "application/json"},
            )
            with self.assertRaises(urllib.error.HTTPError) as caught:
                urllib.request.urlopen(invalid_number)
            self.assertEqual(caught.exception.code, 400)
            sample.pop("extension")
            sample["splitGroup"] = "\ud800"
            invalid_unicode = urllib.request.Request(
                f"{base}/api/samples/{sample_id}",
                data=json.dumps(sample).encode("utf-8"),
                method="PUT",
                headers={"Content-Type": "application/json"},
            )
            with self.assertRaises(urllib.error.HTTPError) as caught:
                urllib.request.urlopen(invalid_unicode)
            self.assertEqual(caught.exception.code, 400)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)


if __name__ == "__main__":
    unittest.main()
