from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .geometry import context_crop_bounds
from .schema import atomic_write_json, read_json, utc_timestamp, validate_sample


@dataclass(frozen=True)
class MixedRegion:
    transcription: str
    role: str = "label"
    policy: str = "translate"
    icon_position: str = "prefix"


@dataclass(frozen=True)
class ReviewRules:
    non_text: frozenset[int] = frozenset()
    mixed: dict[int, MixedRegion] | None = None
    split: frozenset[int] = frozenset()
    merge_groups: tuple[tuple[str, tuple[int, ...]], ...] = ()
    roles: dict[str, frozenset[int]] | None = None
    preserve: frozenset[int] = frozenset()
    transcriptions: dict[int, str] | None = None
    symbol_text: frozenset[int] = frozenset()


def _indices(*values: int) -> frozenset[int]:
    return frozenset(values)


ROOT_REVIEW_RULES: dict[int, ReviewRules] = {
    2: ReviewRules(
        non_text=_indices(4, 7, 9, 11),
        mixed={
            5: MixedRegion("Test AI 2"),
            6: MixedRegion("Test AI 1"),
            8: MixedRegion("Test AI 3"),
            12: MixedRegion("Test AI 5"),
            13: MixedRegion("Test AI 6"),
        },
        roles={
            "label": _indices(0, 2, 3, 10, 14),
            "date_number": _indices(1),
            "code": _indices(15, 16, 17, 18),
        },
        preserve=_indices(1, 15, 16, 17, 18),
        transcriptions={10: "Test AI 4"},
    ),
    6: ReviewRules(
        non_text=_indices(12, 13, 21, 22, 23, 24, 25, 26, 27, 29, 30, 55),
        roles={
            "title": _indices(0),
            "tab": _indices(1, 2, 3, 4, 5, 6, 7),
            "button": _indices(8, 9, 14, 15, 16, 17, 18, 19, 20, 28),
            "value": _indices(10, 11),
            "body": frozenset(range(31, 49)),
            "metadata": _indices(49, 50, 51, 52, 53, 54),
            "date_number": _indices(56),
        },
        preserve=_indices(10, 11, 49, 50, 51, 52, 53, 54, 56),
        transcriptions={
            28: "Znajdz",
            49: "Strona 1 z 2",
            50: "899 slow",
            51: "4 883",
            52: "znaki",
            54: "slaski",
        },
    ),
    9: ReviewRules(
        non_text=_indices(3, 22, 25, 37, 47, 67),
        mixed={
            9: MixedRegion("Next"),
            14: MixedRegion("Text Selection"),
            19: MixedRegion("Yehiel Sheeeke16"),
            31: MixedRegion("Editing Richard"),
            32: MixedRegion("List of Socialist... / Fascism in Greece", role="tab"),
            36: MixedRegion("manjarodistrowatch", role="value", policy="preserve"),
            51: MixedRegion("Recent Buddies"),
        },
        split=_indices(16, 28, 40, 41),
        roles={
            "heading": _indices(76),
            "button": _indices(7, 8, 10, 11, 12, 13, 60, 78),
            "body": _indices(
                40,
                41,
                43,
                45,
                50,
                52,
                54,
                58,
                62,
                64,
                65,
                69,
                70,
                71,
                72,
                73,
                75,
                77,
                79,
                80,
                81,
                82,
                83,
                84,
                85,
                86,
                87,
                88,
                89,
            ),
            "date_number": _indices(15, 18, 20, 56, 57, 59, 68, 74),
        },
        preserve=_indices(0, 1, 2, 4, 5, 6, 15, 16, 18, 20, 28, 56, 57, 59, 68, 74),
    ),
    10: ReviewRules(
        mixed={
            8: MixedRegion("Sign in", role="button"),
            20: MixedRegion("28/30", role="metadata", policy="preserve", icon_position="suffix"),
            24: MixedRegion("5/23", role="metadata", policy="preserve", icon_position="suffix"),
        },
        merge_groups=(("copyright", (34, 35)),),
        roles={
            "title": _indices(0),
            "tab": _indices(1, 2, 3, 4, 5, 6, 7),
            "heading": _indices(10, 25, 26),
            "badge": _indices(11),
            "body": _indices(12),
            "label": _indices(13, 14, 21, 22, 27, 28),
            "button": _indices(15, 16, 17, 19, 29, 30, 31, 32),
            "date_number": _indices(9, 18, 23),
            "link": _indices(33, 36),
            "metadata": _indices(34, 35),
        },
        preserve=_indices(0, 9, 14, 18, 20, 23, 24, 25, 26, 27, 28, 34, 35),
        transcriptions={
            0: "OPENFRONT",
            9: "v0.31.12",
            33: "Terms of Service",
            34: "OpenFront(TM)",
            35: "and Contributors",
            36: "Privacy Policy",
        },
    ),
    11: ReviewRules(
        non_text=_indices(11, 14, 17, 26, 42),
        mixed={
            0: MixedRegion("LibreOffice documento test.odt", role="title"),
            10: MixedRegion("Inserisci voce di indice", role="button"),
            12: MixedRegion("Nota di chiusura", role="button"),
            13: MixedRegion("Segnalibro", role="button"),
            15: MixedRegion("Database bibliografico", role="button"),
            16: MixedRegion("Riferimenti", role="button", icon_position="suffix"),
            18: MixedRegion("Indice generale", role="button"),
            19: MixedRegion("Nota a pie di pagina", role="button"),
            23: MixedRegion("Riferimento incrociato", role="button"),
            25: MixedRegion("Comando di campo", role="button"),
            27: MixedRegion("Citazione", role="button"),
            28: MixedRegion("Sorgenti dati", role="button"),
            29: MixedRegion("Aggiorna tutto", role="button"),
            38: MixedRegion("Pagina 1 di 1", role="metadata"),
        },
        split=_indices(37),
        merge_groups=(
            ("footnote-settings", (20, 21, 22)),
            ("word-count", (39, 40)),
        ),
        roles={
            "tab": frozenset(range(1, 10)),
            "button": _indices(24),
            "title": _indices(30),
            "heading": _indices(31, 33, 35, 36),
            "body": _indices(32, 34, 37),
            "metadata": _indices(39, 40, 41),
            "date_number": _indices(43),
        },
        preserve=_indices(39, 40, 41, 43),
        transcriptions={
            20: "Impostazioni",
            21: "note a pie di pagina",
            22: "/di chiusura",
            24: "Didascalia",
            34: "Lorem ipsum...",
            36: "Sezione 1",
            37: "Sezione 1 / Sezione 2",
            39: "19 parole, 98 carat",
            40: "teri",
            41: "Stile di pagina",
        },
    ),
    14: ReviewRules(
        non_text=_indices(9, 29),
        mixed={
            20: MixedRegion("Netwerk", role="menu_item"),
            23: MixedRegion("Bureaublad", role="menu_item"),
            26: MixedRegion("Prullenbak", role="menu_item"),
            37: MixedRegion("Software Beheerder", role="menu_item"),
            53: MixedRegion("Systeemgereedschap", role="menu_item"),
            56: MixedRegion("Scherm vergrendelen", role="menu_item"),
        },
        split=_indices(5),
        roles={
            "title": _indices(2, 65),
            "tab": _indices(3, 4, 5),
            "button": _indices(6, 7, 8),
            "heading": _indices(11, 12),
            "menu_item": frozenset(range(13, 61)),
            "metadata": _indices(61, 62, 63, 64, 66),
        },
        preserve=_indices(2, 10, 40, 41, 42, 43, 61, 62, 63, 64, 65, 66),
        transcriptions={
            5: "Beeld / Zoeken / Extra / Documenten / Hulp",
            15: "Favorieten",
            19: "RAD Tool for Python and wxWindow",
            28: "Hulpprogramma's",
            30: "Eclipse",
            31: "Internet",
            32: "Eclipse Integrated Development Environment",
            33: "Evir",
            39: "Ontwikkeling",
            47: "Scientific Computing using GNU Octave",
            50: "KLinkStatus",
            54: "Dynamische dialoog-editor",
            55: "dialoog-editor",
            61: "Filter:",
        },
    ),
    15: ReviewRules(
        non_text=_indices(4, 8, 10, 25, 28),
        mixed={
            0: MixedRegion("Portail docu", role="title"),
            2: MixedRegion("OpenProject", role="title", policy="preserve"),
            11: MixedRegion("Lots de Travaux", role="menu_item"),
            13: MixedRegion("Membres", role="heading"),
            14: MixedRegion("Boards NEW", role="menu_item"),
            18: MixedRegion("Calendrier", role="menu_item"),
            19: MixedRegion("Membres", role="menu_item"),
            20: MixedRegion("Editer", role="button"),
            21: MixedRegion("Membre", role="button"),
            22: MixedRegion("Afficher tous les membres", role="button"),
            23: MixedRegion("Documents", role="menu_item"),
            26: MixedRegion("Reunions", role="menu_item"),
            29: MixedRegion("Parametres du projet", role="menu_item"),
            31: MixedRegion("Derniere actualite", role="heading"),
            32: MixedRegion("Suivi des lots de travaux", role="heading"),
        },
        merge_groups=(("project-title", (15, 16)),),
        roles={
            "title": _indices(1, 7),
            "menu_item": _indices(6, 9, 12, 24, 27),
            "heading": _indices(12, 17, 30),
            "body": frozenset(range(33, 40)),
            "badge": _indices(5),
        },
        preserve=_indices(2, 5, 17, 33, 34, 35, 36, 37, 38, 39),
        transcriptions={
            1: "mentaire",
            3: "Recherche",
            15: "Projet de refonte des pages",
            16: "web de la bibliotheque en portail documentaire",
            17: "Manager : Benoit SOUBEYRAN",
            24: "ments",
            27: "ions",
        },
    ),
    16: ReviewRules(
        non_text=_indices(8, 13, 17, 24, 25),
        roles={
            "tab": _indices(0, 1),
            "date_number": _indices(2),
            "heading": _indices(4),
            "menu_item": _indices(3, 5, 7, 10, 12, 15, 16, 20, 21, 22, 23),
            "label": _indices(6, 9, 11, 14, 18, 19),
        },
        preserve=_indices(2),
        transcriptions={
            2: "M08 2 2:29 PM",
            9: "Default web browser, mail client, file browser",
            14: "Configure applications which run on login",
        },
    ),
    17: ReviewRules(
        non_text=_indices(0, 6, 10),
        mixed={
            3: MixedRegion("Domyslny styl akapitu / Liberation Serif", role="value"),
            4: MixedRegion("12 pkt", role="value"),
            12: MixedRegion("0 slow / 0 znakow", role="metadata", policy="preserve"),
        },
        split=_indices(2, 5, 7, 8, 9),
        roles={
            "title": _indices(1),
            "menu_item": _indices(2),
            "date_number": _indices(5, 7, 8, 9, 11, 15),
            "metadata": _indices(13, 14),
        },
        preserve=_indices(1, 5, 7, 8, 9, 11, 12, 13, 14, 15),
        transcriptions={
            1: "Bez tytulu 1 - LibreOffice Writer",
            2: "Plik / Edycja / Widok / Wstaw / Format / Style / Tabela / Formularz / Narzedzia / Okno / Pomoc",
            5: "1",
            7: "2 / 3",
            8: "16 / 17 / 18 / 19",
            9: "10 / 11 / 12 / 13 / 14 / 15 / 16 / 17 / 18 / 19",
            11: "Strona 1 z 1",
            13: "Domyslny styl strony",
        },
    ),
    19: ReviewRules(
        non_text=_indices(42),
        split=_indices(7, 13, 16, 28, 36),
        roles={
            "title": _indices(0),
            "identifier": _indices(1, 5, 53),
            "label": _indices(2, 3, 4, 6, 8, 10, 12, 14, 15, 17, 19, 20, 21, 23, 24, 26, 30, 31, 33, 37, 39, 40, 43, 45, 46, 47, 48, 50, 52, 54, 55, 57, 58),
            "date_number": _indices(7, 9, 11, 13, 16, 18, 22, 25, 27, 28, 29, 32, 34, 35, 36, 38, 41, 44, 49, 51, 56, 59),
            "code": _indices(63, 66, 68, 70, 72),
            "other_text": _indices(60, 61, 62, 64, 65, 67, 69, 71, 73),
        },
        preserve=frozenset(range(0, 74)),
        transcriptions={
            4: "Protection Table",
            13: "Physical I 100 / S 100 / C 100 / P 100 / W 100",
            14: "true neutral follower of Tabernacle",
            16: "Elemental F 100 / C 100 / E 100 / P 100 / A 100",
            28: "Magical M 100 / Mi 100 / B 100 / P 100 / F 100",
            36: "Spherical N 100 / Ch 100 / D 100 / Sp 100 / Co 100",
            48: "using spacecos",
            50: "Food",
            57: "SHIFT for inventory",
            58: "Limit",
        },
    ),
    20: ReviewRules(
        mixed={
            24: MixedRegion("Full Paging ON", role="menu_item", policy="preserve"),
            33: MixedRegion("Horizontal Paging Only", role="menu_item", policy="preserve"),
        },
        split=_indices(29, 32, 35, 37, 40, 42, 45, 46, 49, 51, 53, 57, 59, 62, 64, 65, 66, 68, 69, 76, 77, 80, 81, 85, 86, 87, 88, 94, 95, 104, 110, 111, 112, 113, 116),
        roles={
            "title": _indices(0, 18, 41, 131, 133),
            "menu_item": frozenset(range(3, 20)) | _indices(24, 28, 33, 34, 47, 54),
            "code": frozenset(range(1, 3)) | frozenset(range(20, 41)) | frozenset(range(42, 71)) | frozenset(range(75, 78)) | frozenset(range(80, 89)) | frozenset(range(94, 99)) | frozenset(range(101, 106)) | frozenset(range(110, 117)),
            "button": frozenset(range(71, 75)) | frozenset(range(78, 94)) | frozenset(range(99, 101)) | frozenset(range(106, 110)) | frozenset(range(117, 131)),
            "date_number": _indices(132),
        },
        preserve=frozenset(range(0, 134)),
        symbol_text=_indices(48, 121, 126, 129, 130),
        transcriptions={
            1: "$ uname -a",
            2: "OpenBSD vbox-bsd.my.domain 6.1 GENERIC#19 amd64",
            4: "XTerm",
            6: "Fvwm Modules",
            8: "Fvwm Simple Config",
            12: "Recapture Screen",
            17: "Colormap Follows Mouse",
            19: "Colormap Follows Focus",
            20: "load averages:",
            24: "Full Paging ON",
            28: "All Paging OFF",
            29: "CPU states: 86.8% user, 0.0% nice, 13.2% system",
            32: "Memory: Real 80M / 36M active / total Free 861M / Cache 176M / Swap 182M",
            33: "Horizontal Paging Only",
            42: "73994 / user",
            45: "25M / run",
            46: "4:50 / 82.52% / glxgears",
            48: "-",
            49: "87174 / _x11",
            51: "28M / sleep",
            53: "1:42 / 16.46% / Xorg",
            57: "25156 / _ntp",
            59: "-20 / 1156K / 2440K / sleep",
            62: "0.00% / ntpd",
            64: "1 / root",
            65: "0% / init",
            66: "5146 / user",
            68: "10% / fvwm",
            69: "80502 / root",
            76: "67554 / user",
            77: "10% / xterm",
            80: "57174 / _smtpd",
            81: "0.0% / smtpd",
            85: "8233 / user",
            86: "10% / xterm",
            87: "78411 / user",
            88: "10% / xcalc",
            94: "96602 / user",
            95: "10% / xclock",
            104: "26176 / smtpd",
            110: "69472 / user",
            111: "10% / top",
            112: "25888 / _smtpd",
            113: "10% / smtpd",
            116: "0.0% / smtpd",
            121: "-",
            126: "+",
            129: "+/-",
            130: "=",
        },
    ),
}


def _expand_box(
    box: dict[str, int], image_width: int, image_height: int, padding: int
) -> dict[str, int]:
    left_bound, top_bound, right_bound, bottom_bound = context_crop_bounds(box)
    left = max(0, left_bound, box["x"] - padding)
    top = max(0, top_bound, box["y"] - padding)
    right = min(image_width, right_bound, box["x"] + box["width"] + padding)
    bottom = min(image_height, bottom_bound, box["y"] + box["height"] + padding)
    return {"x": left, "y": top, "width": right - left, "height": bottom - top}


def _set_text(
    annotation: dict[str, Any],
    text: str,
    *,
    role: str,
    policy: str,
    relation: str,
    image_width: int,
    image_height: int,
    textured: bool,
) -> None:
    annotation.update(
        {
            "attributes": {"illegible": False, "truncated": False},
            "groupId": None,
            "labelStatus": "verified",
            "layoutBox": None,
            "maskBox": None,
            "parentAnnotationId": None,
            "patchMode": "none",
            "relation": relation,
            "role": role,
            "textness": "text",
            "transcription": text,
            "translationPolicy": policy,
        }
    )
    if relation != "single":
        return
    if textured:
        annotation["patchMode"] = "mask_required"
        return
    box = annotation["textBox"]
    mask_padding = max(1, box["height"] // 12)
    layout_padding = max(mask_padding, box["height"] // 3)
    annotation["maskBox"] = _expand_box(
        box, image_width, image_height, mask_padding
    )
    annotation["layoutBox"] = _expand_box(
        box, image_width, image_height, layout_padding
    )
    annotation["patchMode"] = "rect_safe"


def _set_non_text(annotation: dict[str, Any], role: str = "icon") -> None:
    annotation.update(
        {
            "attributes": {"illegible": False, "truncated": False},
            "groupId": None,
            "labelStatus": "verified",
            "layoutBox": None,
            "maskBox": None,
            "parentAnnotationId": None,
            "patchMode": "none",
            "relation": "none",
            "role": role,
            "textness": "non_text",
            "transcription": None,
            "translationPolicy": "preserve",
        }
    )


def _child(
    parent: dict[str, Any],
    suffix: str,
    box: dict[str, int],
    *,
    textness: str,
    role: str,
    transcription: str | None,
    policy: str,
) -> dict[str, Any]:
    return {
        "attributes": {"illegible": False, "truncated": False},
        "containerId": None,
        "groupId": None,
        "id": f"{parent['id']}-child-{suffix}",
        "labelStatus": "verified",
        "layoutBox": None,
        "maskBox": None,
        "parentAnnotationId": parent["id"],
        "patchMode": "none",
        "proposalIds": [],
        "readingOrder": None,
        "relation": "none",
        "role": role,
        "textBox": box,
        "textness": textness,
        "transcription": transcription,
        "translationPolicy": policy,
    }


def _set_mixed(
    annotation: dict[str, Any], specification: MixedRegion
) -> list[dict[str, Any]]:
    annotation.update(
        {
            "attributes": {"illegible": False, "truncated": False},
            "groupId": None,
            "labelStatus": "verified",
            "layoutBox": None,
            "maskBox": None,
            "parentAnnotationId": None,
            "patchMode": "none",
            "relation": "split_required",
            "role": "mixed_content",
            "textness": "mixed",
            "transcription": None,
            "translationPolicy": "review",
        }
    )
    box = annotation["textBox"]
    icon_width = min(max(4, round(box["height"] * 0.9)), max(1, box["width"] // 3))
    text_width = max(1, box["width"] - icon_width)
    if specification.icon_position == "suffix":
        text_box = {**box, "width": text_width}
        icon_box = {
            "x": box["x"] + text_width,
            "y": box["y"],
            "width": icon_width,
            "height": box["height"],
        }
    else:
        icon_box = {**box, "width": icon_width}
        text_box = {
            "x": box["x"] + icon_width,
            "y": box["y"],
            "width": text_width,
            "height": box["height"],
        }
    return [
        _child(
            annotation,
            "text",
            text_box,
            textness="text",
            role=specification.role,
            transcription=specification.transcription,
            policy=specification.policy,
        ),
        _child(
            annotation,
            "icon",
            icon_box,
            textness="non_text",
            role="icon",
            transcription=None,
            policy="preserve",
        ),
    ]


def _role_for_index(rules: ReviewRules, index: int, fallback: str) -> str:
    for role, indices in (rules.roles or {}).items():
        if index in indices:
            return role
    return fallback


def review_sample(
    dataset_root: Path, sample_path: Path, ordinal: int, rules: ReviewRules
) -> dict[str, int]:
    sample = read_json(sample_path)
    proposal_annotations = [
        annotation for annotation in sample["annotations"] if annotation["proposalIds"]
    ]
    if len(proposal_annotations) != len(sample["ocrProposals"]):
        raise ValueError(
            f"Sample {ordinal} no longer has one annotation per OCR proposal"
        )
    sample["annotations"] = proposal_annotations
    image_width = sample["image"]["width"]
    image_height = sample["image"]["height"]
    category = next(
        source["category"]
        for source in sample["sources"]
        if source["kind"] == "external_screenshot"
    )
    textured = category == "open_source_game"
    children: list[dict[str, Any]] = []
    for index, (proposal, annotation) in enumerate(
        zip(sample["ocrProposals"], proposal_annotations, strict=True)
    ):
        recognized = " ".join(
            str(proposal["observations"][0]["recognizedText"]).split()
        )
        transcription = (rules.transcriptions or {}).get(index, recognized)
        if index in rules.non_text:
            _set_non_text(annotation)
            continue
        if rules.mixed and index in rules.mixed:
            children.extend(_set_mixed(annotation, rules.mixed[index]))
            continue
        role = _role_for_index(rules, index, annotation["role"] or "other_text")
        policy = "preserve" if index in rules.preserve else annotation["translationPolicy"]
        if index in rules.symbol_text:
            role = _role_for_index(rules, index, "button")
            policy = "preserve"
        relation = "split_required" if index in rules.split else "single"
        _set_text(
            annotation,
            transcription,
            role=role,
            policy=policy,
            relation=relation,
            image_width=image_width,
            image_height=image_height,
            textured=textured,
        )

    for group_name, indices in rules.merge_groups:
        group_id = f"external-{ordinal:02d}-{group_name}"
        for index in indices:
            annotation = proposal_annotations[index]
            if annotation["textness"] != "text":
                raise ValueError(
                    f"Sample {ordinal} merge group {group_name} contains non-text index {index}"
                )
            annotation.update(
                {
                    "groupId": group_id,
                    "relation": "merge_required",
                    "patchMode": "none",
                    "maskBox": None,
                    "layoutBox": None,
                }
            )

    sample["annotations"].extend(children)
    sample["coverage"] = {
        "textComplete": True,
        "anchorsComplete": True,
        "protectedRegionsComplete": True,
    }
    sample["revision"] += 1
    sample["updatedAt"] = utc_timestamp()
    validate_sample(sample, dataset_root)
    atomic_write_json(sample_path, sample)
    return {
        "proposals": len(sample["ocrProposals"]),
        "children": len(children),
        "nonText": sum(
            annotation["textness"] == "non_text"
            for annotation in proposal_annotations
        ),
        "mixed": sum(
            annotation["textness"] == "mixed"
            for annotation in proposal_annotations
        ),
        "split": sum(
            annotation["relation"] == "split_required"
            for annotation in proposal_annotations
        ),
        "merge": sum(
            annotation["relation"] == "merge_required"
            for annotation in proposal_annotations
        ),
    }


def review(dataset_root: Path, audit_index: Path, ordinals: set[int]) -> dict[str, Any]:
    raw_entries = json.loads(audit_index.read_text(encoding="utf-8"))
    if not isinstance(raw_entries, list) or not all(
        isinstance(entry, dict) for entry in raw_entries
    ):
        raise ValueError("Audit index must be an array of objects")
    entries = {
        int(entry["ordinal"]): entry for entry in raw_entries
    }
    unknown = sorted(ordinals - ROOT_REVIEW_RULES.keys())
    if unknown:
        raise ValueError(f"No root review rules for ordinals: {unknown}")
    details: dict[str, Any] = {}
    for ordinal in sorted(ordinals):
        entry = entries[ordinal]
        sample_path = dataset_root / entry["samplePath"]
        details[str(ordinal)] = review_sample(
            dataset_root, sample_path, ordinal, ROOT_REVIEW_RULES[ordinal]
        )
    result = {"reviewedSamples": len(details), "details": details}
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Apply the audited external screenshot review decisions."
    )
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--audit-index", type=Path, required=True)
    parser.add_argument(
        "--ordinals",
        nargs="*",
        type=int,
        default=sorted(ROOT_REVIEW_RULES),
    )
    arguments = parser.parse_args()
    review(
        arguments.dataset.resolve(),
        arguments.audit_index.resolve(),
        set(arguments.ordinals),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
