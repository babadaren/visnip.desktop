from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

from PIL import Image, ImageDraw, ImageFont

from .schema import read_json


FONT_PATH = Path(r"C:\Windows\Fonts\arial.ttf")


def _font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    if FONT_PATH.is_file():
        return ImageFont.truetype(str(FONT_PATH), size)
    return ImageFont.load_default()


def _context_crop(image: Image.Image, box: dict[str, int]) -> Image.Image:
    horizontal = max(18, box["height"] * 2)
    vertical = max(12, box["height"])
    return image.crop(
        (
            max(0, box["x"] - horizontal),
            max(0, box["y"] - vertical),
            min(image.width, box["x"] + box["width"] + horizontal),
            min(image.height, box["y"] + box["height"] + vertical),
        )
    )


def _is_external(sample: dict[str, Any]) -> bool:
    return any(
        isinstance(source, dict) and source.get("kind") == "external_screenshot"
        for source in sample.get("sources", [])
    )


def _render_sample(
    ordinal: int, sample: dict[str, Any], dataset_root: Path, output_root: Path
) -> dict[str, Any]:
    image_path = dataset_root / sample["image"]["path"]
    with Image.open(image_path) as source:
        image = source.convert("RGB")
    overlay = image.copy()
    draw = ImageDraw.Draw(overlay)
    label_font = _font(14)
    proposals = sample["ocrProposals"]
    for index, proposal in enumerate(proposals):
        box = proposal["textBox"]
        left, top = box["x"], box["y"]
        right = left + box["width"]
        bottom = top + box["height"]
        color = "#e02f44"
        draw.rectangle((left, top, right, bottom), outline=color, width=2)
        tag = str(index)
        bounds = draw.textbbox((0, 0), tag, font=label_font)
        tag_width = bounds[2] - bounds[0] + 8
        tag_height = bounds[3] - bounds[1] + 6
        tag_top = max(0, top - tag_height)
        draw.rectangle((left, tag_top, left + tag_width, tag_top + tag_height), fill=color)
        draw.text((left + 4, tag_top + 2), tag, fill="white", font=label_font)
    overlay_name = f"external-{ordinal:02d}-overlay.jpg"
    overlay.save(output_root / overlay_name, "JPEG", quality=92, optimize=True)

    columns = 3
    cell_width = 400
    cell_height = 145
    rows = max(1, math.ceil(len(proposals) / columns))
    sheet = Image.new("RGB", (columns * cell_width, rows * cell_height), "white")
    sheet_draw = ImageDraw.Draw(sheet)
    title_font = _font(14)
    small_font = _font(12)
    for index, proposal in enumerate(proposals):
        column = index % columns
        row = index // columns
        x = column * cell_width
        y = row * cell_height
        observation = proposal["observations"][0]
        text = observation["recognizedText"].replace("\n", " ")
        if len(text) > 48:
            text = text[:45] + "..."
        score = observation["recognitionScore"]
        sheet_draw.text(
            (x + 6, y + 5),
            f"#{index:03d} [{score:.2f}] {text}",
            fill="#111827",
            font=title_font,
        )
        context = _context_crop(image, proposal["textBox"])
        context.thumbnail((cell_width - 12, cell_height - 50))
        sheet.paste(context, (x + (cell_width - context.width) // 2, y + 28))
        box = proposal["textBox"]
        sheet_draw.text(
            (x + 6, y + cell_height - 18),
            f"{box['x']},{box['y']} {box['width']}x{box['height']}",
            fill="#526078",
            font=small_font,
        )
        sheet_draw.rectangle(
            (x, y, x + cell_width - 1, y + cell_height - 1), outline="#c9ced8"
        )
    crops_name = f"external-{ordinal:02d}-proposals.jpg"
    sheet.save(output_root / crops_name, "JPEG", quality=92, optimize=True)
    source = next(
        source
        for source in sample["sources"]
        if source.get("kind") == "external_screenshot"
    )
    return {
        "ordinal": ordinal,
        "sampleId": sample["sampleId"],
        "samplePath": f"samples/{sample['sampleId']}.json",
        "sourceTitle": source["sourceTitle"],
        "category": source["category"],
        "proposalCount": len(proposals),
        "overlay": overlay_name,
        "proposalSheet": crops_name,
    }


def render(dataset_root: Path, output_root: Path) -> None:
    output_root.mkdir(parents=True, exist_ok=True)
    samples: list[dict[str, Any]] = []
    for path in sorted((dataset_root / "samples").glob("*.json")):
        sample = read_json(path)
        if _is_external(sample):
            samples.append(sample)
    if not samples:
        raise ValueError("Dataset has no external screenshot samples")
    index = [
        _render_sample(ordinal, sample, dataset_root, output_root)
        for ordinal, sample in enumerate(samples, 1)
    ]
    (output_root / "index.json").write_text(
        json.dumps(index, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Rendered {len(index)} external samples into {output_root}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Render external OCR proposal audit sheets")
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    render(arguments.dataset.resolve(), arguments.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
