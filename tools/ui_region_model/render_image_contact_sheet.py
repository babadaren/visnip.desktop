from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


FONT_PATH = Path(r"C:\Windows\Fonts\arial.ttf")
CELL_WIDTH = 480
CELL_HEIGHT = 330
LABEL_HEIGHT = 42
GRID_COLUMNS = 2


def _font() -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    if FONT_PATH.is_file():
        return ImageFont.truetype(str(FONT_PATH), 17)
    return ImageFont.load_default()


def render(input_root: Path, output_path: Path) -> None:
    paths = sorted(
        path
        for path in input_root.iterdir()
        if path.is_file() and path.suffix.casefold() in (".png", ".jpg", ".jpeg")
    )
    if not paths:
        raise ValueError(f"No images found in {input_root}")
    rows = math.ceil(len(paths) / GRID_COLUMNS)
    sheet = Image.new(
        "RGB", (GRID_COLUMNS * CELL_WIDTH, rows * CELL_HEIGHT), "#f4f5f7"
    )
    draw = ImageDraw.Draw(sheet)
    label_font = _font()
    for index, path in enumerate(paths):
        column = index % GRID_COLUMNS
        row = index // GRID_COLUMNS
        x = column * CELL_WIDTH
        y = row * CELL_HEIGHT
        with Image.open(path) as source:
            image = source.convert("RGB")
        image.thumbnail((CELL_WIDTH - 20, CELL_HEIGHT - LABEL_HEIGHT - 20))
        paste_x = x + (CELL_WIDTH - image.width) // 2
        paste_y = y + LABEL_HEIGHT + (CELL_HEIGHT - LABEL_HEIGHT - image.height) // 2
        sheet.paste(image, (paste_x, paste_y))
        label = f"{index + 1:02d}  {path.stem[:48]}"
        draw.text((x + 10, y + 10), label, fill="#182230", font=label_font)
        draw.rectangle(
            (x, y, x + CELL_WIDTH - 1, y + CELL_HEIGHT - 1), outline="#aab2bf"
        )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(output_path, "JPEG", quality=92, optimize=True)
    print(f"Rendered {len(paths)} images to {output_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Render a screenshot contact sheet")
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    render(arguments.input.resolve(), arguments.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
