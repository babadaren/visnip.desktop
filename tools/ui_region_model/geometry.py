from __future__ import annotations

from typing import Mapping


Box = Mapping[str, int]


def local_crop_bounds(box: Box) -> tuple[int, int, int, int]:
    padding = max(2, (box["height"] + 2) // 4)
    return (
        box["x"] - padding,
        box["y"] - padding,
        box["x"] + box["width"] + padding,
        box["y"] + box["height"] + padding,
    )


def context_crop_bounds(box: Box) -> tuple[int, int, int, int]:
    crop_width = max(box["width"] * 2, box["height"] * 12)
    crop_height = box["height"] * 5
    left = box["x"] - (crop_width - box["width"]) // 2
    top = box["y"] - (crop_height - box["height"]) // 2
    return left, top, left + crop_width, top + crop_height


def box_inside_bounds(box: Box, bounds: tuple[int, int, int, int]) -> bool:
    left, top, right, bottom = bounds
    return (
        box["x"] >= left
        and box["y"] >= top
        and box["x"] + box["width"] <= right
        and box["y"] + box["height"] <= bottom
    )


def normalized_cxcywh(box: Box, bounds: tuple[int, int, int, int]) -> list[float]:
    if not box_inside_bounds(box, bounds):
        raise ValueError(f"box {dict(box)} exceeds model context {bounds}")
    left, top, right, bottom = bounds
    width = right - left
    height = bottom - top
    x0 = (box["x"] - left) / width
    y0 = (box["y"] - top) / height
    x1 = (box["x"] + box["width"] - left) / width
    y1 = (box["y"] + box["height"] - top) / height
    return [(x0 + x1) / 2, (y0 + y1) / 2, x1 - x0, y1 - y0]


def letterbox_geometry(
    source_width: int, source_height: int, target_width: int, target_height: int
) -> tuple[int, int, int, int]:
    if min(source_width, source_height, target_width, target_height) <= 0:
        raise ValueError("letterbox dimensions must be positive")
    if target_width * source_height <= target_height * source_width:
        resized_width = target_width
        resized_height = max(
            1, (2 * source_height * target_width + source_width) // (2 * source_width)
        )
    else:
        resized_height = target_height
        resized_width = max(
            1, (2 * source_width * target_height + source_height) // (2 * source_height)
        )
    offset_x = (target_width - resized_width) // 2
    offset_y = (target_height - resized_height) // 2
    return resized_width, resized_height, offset_x, offset_y
