from __future__ import annotations

import argparse
import json
import re
import sys
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlparse

from .export_diagnostics import default_dataset_root
from .schema import (
    CONTAINER_ROLES,
    LABEL_STATUSES,
    NON_TEXT_ROLES,
    PATCH_MODES,
    RELATIONS,
    TEXT_ROLES,
    TEXTNESS_VALUES,
    TRANSLATION_POLICIES,
    DatasetValidationError,
    atomic_write_json,
    dataset_lock,
    iter_sample_paths,
    read_json,
    sample_path,
    strict_json_loads,
    summarize_sample,
    utc_timestamp,
    validate_dataset_metadata,
    validate_sample,
)


SAMPLE_ID_PATTERN = re.compile(r"^[A-Za-z0-9_-]{1,128}$")
MAX_REQUEST_BYTES = 8 * 1024 * 1024


class AnnotationServer(ThreadingHTTPServer):
    def __init__(self, address: tuple[str, int], dataset_root: Path):
        self.dataset_root = dataset_root.resolve()
        self.static_root = Path(__file__).with_name("web").resolve()
        self.metadata = read_json(self.dataset_root / "dataset.json")
        validate_dataset_metadata(self.metadata)
        self.write_lock = threading.RLock()
        super().__init__(address, AnnotationRequestHandler)


class AnnotationRequestHandler(BaseHTTPRequestHandler):
    server: AnnotationServer

    def log_message(self, format_string: str, *args: object) -> None:
        sys.stdout.write(f"{self.address_string()} - {format_string % args}\n")

    def _json(self, status: HTTPStatus, value: Any) -> None:
        payload = json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.end_headers()
        self.wfile.write(payload)

    def _error(
        self, status: HTTPStatus, message: str, details: list[str] | None = None
    ) -> None:
        value: dict[str, Any] = {"error": message}
        if details:
            value["details"] = details
        self._json(status, value)

    def _serve_file(self, root: Path, relative_path: str) -> None:
        candidate = (root / unquote(relative_path)).resolve()
        try:
            candidate.relative_to(root)
        except ValueError:
            self._error(HTTPStatus.NOT_FOUND, "File not found")
            return
        if not candidate.is_file():
            self._error(HTTPStatus.NOT_FOUND, "File not found")
            return
        suffix_types = {
            ".html": "text/html; charset=utf-8",
            ".css": "text/css; charset=utf-8",
            ".js": "text/javascript; charset=utf-8",
            ".png": "image/png",
            ".jpg": "image/jpeg",
            ".jpeg": "image/jpeg",
        }
        content_type = suffix_types.get(
            candidate.suffix.lower(), "application/octet-stream"
        )
        payload = candidate.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header(
            "Cache-Control",
            "no-store" if candidate.suffix != ".png" else "private, max-age=3600",
        )
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; object-src 'none'; base-uri 'none'",
        )
        self.end_headers()
        self.wfile.write(payload)

    def _sample_id(self, path: str) -> str | None:
        prefix = "/api/samples/"
        if not path.startswith(prefix):
            return None
        sample_id = unquote(path[len(prefix) :])
        return sample_id if SAMPLE_ID_PATTERN.fullmatch(sample_id) else None

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        try:
            if path == "/api/config":
                self._json(
                    HTTPStatus.OK,
                    {
                        "datasetId": self.server.metadata["datasetId"],
                        "datasetRoot": str(self.server.dataset_root),
                        "splitSeed": self.server.metadata["splitSeed"],
                        "labels": {
                            "textness": TEXTNESS_VALUES,
                            "textRoles": TEXT_ROLES,
                            "nonTextRoles": NON_TEXT_ROLES,
                            "containerRoles": CONTAINER_ROLES,
                            "translationPolicies": TRANSLATION_POLICIES,
                            "labelStatuses": LABEL_STATUSES,
                            "relations": RELATIONS,
                            "patchModes": PATCH_MODES,
                        },
                    },
                )
                return
            if path == "/api/samples":
                samples = []
                for json_path in iter_sample_paths(self.server.dataset_root):
                    sample = read_json(json_path)
                    validate_sample(sample, self.server.dataset_root)
                    samples.append(
                        summarize_sample(sample, self.server.metadata["splitSeed"])
                    )
                samples.sort(
                    key=lambda item: (item["sourceLabel"], item["sampleId"]),
                    reverse=True,
                )
                self._json(HTTPStatus.OK, {"samples": samples})
                return
            sample_id = self._sample_id(path)
            if sample_id is not None:
                json_path = sample_path(self.server.dataset_root, sample_id)
                if not json_path.is_file():
                    self._error(HTTPStatus.NOT_FOUND, "Sample not found")
                    return
                sample = read_json(json_path)
                validate_sample(sample, self.server.dataset_root)
                self._json(HTTPStatus.OK, sample)
                return
            if path.startswith("/dataset/"):
                self._serve_file(self.server.dataset_root, path[len("/dataset/") :])
                return
            relative = "index.html" if path == "/" else path.lstrip("/")
            self._serve_file(self.server.static_root, relative)
        except DatasetValidationError as error:
            self._error(
                HTTPStatus.INTERNAL_SERVER_ERROR,
                "Dataset validation failed",
                list(error.errors),
            )
        except (OSError, json.JSONDecodeError) as error:
            self._error(HTTPStatus.INTERNAL_SERVER_ERROR, str(error))

    def do_PUT(self) -> None:
        path = urlparse(self.path).path
        sample_id = self._sample_id(path)
        if sample_id is None:
            self._error(HTTPStatus.NOT_FOUND, "Endpoint not found")
            return
        try:
            content_length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._error(HTTPStatus.BAD_REQUEST, "Invalid Content-Length")
            return
        if content_length <= 0 or content_length > MAX_REQUEST_BYTES:
            self._error(HTTPStatus.BAD_REQUEST, "Request body size is invalid")
            return
        try:
            incoming = strict_json_loads(self.rfile.read(content_length))
        except (
            UnicodeDecodeError,
            json.JSONDecodeError,
            DatasetValidationError,
        ) as error:
            self._error(HTTPStatus.BAD_REQUEST, f"Invalid JSON: {error}")
            return
        if not isinstance(incoming, dict):
            self._error(HTTPStatus.BAD_REQUEST, "JSON root must be an object")
            return

        json_path = sample_path(self.server.dataset_root, sample_id)
        with self.server.write_lock, dataset_lock(self.server.dataset_root):
            try:
                if not json_path.is_file():
                    self._error(HTTPStatus.NOT_FOUND, "Sample not found")
                    return
                current = read_json(json_path)
                if incoming.get("sampleId") != sample_id:
                    self._error(
                        HTTPStatus.BAD_REQUEST, "sampleId does not match the URL"
                    )
                    return
                if incoming.get("revision") != current.get("revision"):
                    self._error(
                        HTTPStatus.CONFLICT,
                        "Sample was changed by another session",
                        [
                            f"expected revision {current.get('revision')}, received {incoming.get('revision')}"
                        ],
                    )
                    return
                for field in (
                    "schemaVersion",
                    "sampleId",
                    "createdAt",
                    "image",
                    "sources",
                    "ocrProposals",
                ):
                    if incoming.get(field) != current.get(field):
                        self._error(
                            HTTPStatus.BAD_REQUEST, f"Immutable field changed: {field}"
                        )
                        return
                incoming["revision"] = current["revision"] + 1
                incoming["updatedAt"] = utc_timestamp()
                validate_sample(incoming, self.server.dataset_root)
                atomic_write_json(json_path, incoming)
                self._json(HTTPStatus.OK, incoming)
            except DatasetValidationError as error:
                self._error(
                    HTTPStatus.BAD_REQUEST,
                    "Dataset validation failed",
                    list(error.errors),
                )
            except (OSError, json.JSONDecodeError) as error:
                self._error(HTTPStatus.INTERNAL_SERVER_ERROR, str(error))


def make_server(host: str, port: int, dataset_root: Path) -> AnnotationServer:
    if host not in ("127.0.0.1", "localhost", "::1"):
        raise ValueError("The annotation server only binds to the local machine")
    if not dataset_root.is_dir():
        raise ValueError(f"Dataset directory does not exist: {dataset_root}")
    return AnnotationServer((host, port), dataset_root)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run the local Visnip UI region annotation tool"
    )
    parser.add_argument("--dataset", type=Path, default=default_dataset_root())
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)
    try:
        server = make_server(args.host, args.port, args.dataset)
    except (ValueError, OSError, DatasetValidationError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    print(
        f"Visnip UI region annotation: http://{args.host}:{server.server_port}",
        flush=True,
    )
    print(f"Dataset: {server.dataset_root}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
