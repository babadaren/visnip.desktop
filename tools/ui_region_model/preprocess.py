from __future__ import annotations

import numpy as np
from PIL import Image

from .geometry import context_crop_bounds, letterbox_geometry, local_crop_bounds
from .model_contract_constants import INPUT_HEIGHT, INPUT_WIDTH


def crop_with_fill(
    image: Image.Image,
    bounds: tuple[int, int, int, int],
    fill: int | tuple[int, int, int],
) -> Image.Image:
    left, top, right, bottom = bounds
    canvas = Image.new(image.mode, (right - left, bottom - top), color=fill)
    source_left = max(0, left)
    source_top = max(0, top)
    source_right = min(image.width, right)
    source_bottom = min(image.height, bottom)
    if source_right > source_left and source_bottom > source_top:
        crop = image.crop((source_left, source_top, source_right, source_bottom))
        canvas.paste(crop, (source_left - left, source_top - top))
    return canvas


def letterbox(
    image: Image.Image,
    size: tuple[int, int],
    fill: int | tuple[int, int, int],
    resample: int,
) -> Image.Image:
    target_width, target_height = size
    resized_width, resized_height, offset_x, offset_y = letterbox_geometry(
        image.width, image.height, target_width, target_height
    )
    resized = image.resize((resized_width, resized_height), resample=resample)
    result = Image.new(image.mode, size, color=fill)
    result.paste(resized, (offset_x, offset_y))
    return result


def box_mask(image_size: tuple[int, int], box: dict[str, int]) -> Image.Image:
    pixels = np.zeros((image_size[1], image_size[0]), dtype=np.uint8)
    x0 = max(0, box["x"])
    y0 = max(0, box["y"])
    x1 = min(image_size[0], box["x"] + box["width"])
    y1 = min(image_size[1], box["y"] + box["height"])
    pixels[y0:y1, x0:x1] = 255
    return Image.fromarray(pixels)


def prepare_region_input(
    image: Image.Image, proposal_box: dict[str, int]
) -> np.ndarray:
    rgb = image.convert("RGB")
    local_bounds = local_crop_bounds(proposal_box)
    context_bounds = context_crop_bounds(proposal_box)
    local = letterbox(
        crop_with_fill(rgb, local_bounds, (127, 127, 127)),
        (INPUT_WIDTH, INPUT_HEIGHT),
        (127, 127, 127),
        Image.Resampling.BILINEAR,
    )
    context = letterbox(
        crop_with_fill(rgb, context_bounds, (127, 127, 127)),
        (INPUT_WIDTH, INPUT_HEIGHT),
        (127, 127, 127),
        Image.Resampling.BILINEAR,
    )
    full_mask = box_mask(rgb.size, proposal_box)
    mask = letterbox(
        crop_with_fill(full_mask, context_bounds, 0),
        (INPUT_WIDTH, INPUT_HEIGHT),
        0,
        Image.Resampling.NEAREST,
    )
    local_array = np.asarray(local, dtype=np.float32).transpose(2, 0, 1) / 127.5 - 1.0
    context_array = (
        np.asarray(context, dtype=np.float32).transpose(2, 0, 1) / 127.5 - 1.0
    )
    mask_array = np.asarray(mask, dtype=np.float32)[None, :, :] / 255.0
    return np.ascontiguousarray(
        np.concatenate((local_array, context_array, mask_array), axis=0),
        dtype=np.float32,
    )
