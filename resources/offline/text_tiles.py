"""Native-resolution text-only inference windows. Never shrink a screenshot to hide detail."""
from __future__ import annotations


def _positions(start: int, end: int, window: int, overlap: int) -> list[int]:
    if end - start <= window:
        return [start]
    last = end - window
    result = list(range(start, last + 1, window - overlap))
    if result[-1] != last:
        result.append(last)
    return result


def text_tiles(size: tuple[int, int], units, window: int = 512, margin: int = 24):
    """Cover OCR text, with contextual margins, without repeated edge/empty windows.

    Unlike iterating range(0, image.width, stride) and clamping every origin,
    this produces each edge tile once. Pixels and output coordinates are unchanged.
    """
    width, height = size
    boxes = [b for unit in units for b in unit.boxes
             if 0 <= b[0] < b[2] <= width and 0 <= b[1] < b[3] <= height]
    if not boxes:
        return []
    left = max(0, min(b[0] for b in boxes) - margin)
    top = max(0, min(b[1] for b in boxes) - margin)
    right = min(width, max(b[2] for b in boxes) + margin)
    bottom = min(height, max(b[3] for b in boxes) + margin)
    result = []
    for y in _positions(top, bottom, window, 64):
        for x in _positions(left, right, window, 64):
            box = (x, y, min(x + window, right), min(y + window, bottom))
            if any(box[0] < b[2] and b[0] < box[2] and box[1] < b[3] and b[1] < box[3] for b in boxes):
                result.append(box)
    return result
