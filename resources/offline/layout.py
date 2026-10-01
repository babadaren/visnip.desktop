"""Deterministic composition core; no model imports or network access.

Context units and destructive pixel edits are separate. A failed layout never
erases its source text. Font selection here is an estimate, not font recognition.
"""
from __future__ import annotations

import re
from collections import Counter
from dataclasses import dataclass, field

from PIL import Image, ImageChops, ImageDraw, ImageFont, ImageStat


@dataclass
class Unit:
    identifier: str
    text: str
    boxes: list[tuple[int, int, int, int]]
    confidence: float = 1.0
    source_lines: list[str] = field(default_factory=list)
    role: str = "text"
    alignment: list[tuple[str, float, float]] = field(default_factory=list)

    def __post_init__(self):
        if not self.source_lines:
            self.source_lines = [self.text]

    @property
    def box(self):
        return (min(b[0] for b in self.boxes), min(b[1] for b in self.boxes),
                max(b[2] for b in self.boxes), max(b[3] for b in self.boxes))


def group_paragraphs(lines: list[Unit]) -> list[Unit]:
    """Conservative wrapped-prose grouping. Short UI labels retain their identity."""
    from .ui_structure import metadata_line
    groups: list[Unit] = []
    for line in sorted(lines, key=lambda item: (item.box[1], item.box[0])):
        left, top, right, bottom = line.box
        merged = False
        for group in reversed(groups):
            previous = group.boxes[-1]
            height = previous[3] - previous[1]
            prose = len(group.text.split()) >= 5 or len(re.findall(r"[\u4e00-\u9fff]", group.text)) >= 16
            continuation = not metadata_line(line.text) and (
                len(re.findall(r'[A-Za-z]+', line.text)) >= 3
                or len(re.findall(r'[\u3400-\u9fff]', line.text)) >= 6
                or bool(re.fullmatch(r'[a-z]+[.!?]', line.text.strip())))
            if (group.role == 'text' and line.role == 'text' and prose and continuation and 0 <= top - previous[3] <= height * 0.6
                    and abs(left - previous[0]) <= height * 0.5
                    and abs((bottom - top) - height) <= height * 0.25
                    and right <= previous[2] + height
                    and not re.search(r"[。！？.!?:：]$", group.text.strip())):
                separator = "" if re.search(r"[\u4e00-\u9fff]$", group.text) else " "
                group.text += separator + line.text
                group.boxes.extend(line.boxes)
                group.source_lines.extend(line.source_lines)
                group.confidence = min(group.confidence, line.confidence)
                merged = True
                break
        if not merged:
            groups.append(Unit(line.identifier, line.text, list(line.boxes), line.confidence, list(line.source_lines), line.role))
    return groups


def protected_tokens(text: str) -> list[str]:
    """One shared definition for validation and content-free diagnostics."""
    return re.findall(r"\d+(?:[.,]\d+)*|\{[A-Za-z_][A-Za-z_0-9]*\}|%[sd]", text)


def canonical_token(token: str) -> str:
    """Numbers compare by value; separator and full-width digits are formatting.

    A model that writes 1200 for a source 1,200 has not changed the number, and
    rejecting it would drop a correct translation. Everything else is compared
    literally, so real value changes still fail closed.
    """
    if not token[:1].isdigit():
        return token
    value = ''.join(chr(ord(char)-0xFEE0) if '０' <= char <= '９' else char for char in token)
    return re.sub(r"(?<=\d),(?=\d{3}(?!\d))", '', value)


def protected_token_counts(text: str) -> Counter:
    """The single comparison used by the translator and the compositor."""
    return Counter(canonical_token(token) for token in protected_tokens(text))


def validate_translations(units: list[Unit], result: dict) -> dict[str, str]:
    expected = {unit.identifier for unit in units}
    if set(result) != expected:
        raise ValueError("translation_id_mismatch")
    for unit in units:
        text = result[unit.identifier]
        if not isinstance(text, str) or not text.strip() or len(text) > 8000:
            raise ValueError("invalid_translation")
        # Numbers and software placeholders must not be silently invented/lost.
        if protected_token_counts(unit.text) != protected_token_counts(text):
            raise ValueError("protected_token_mismatch")
    return result


def wrap_text(text: str, font, width: int) -> list[str] | None:
    # First target languages are English and simplified Chinese; RTL needs a
    # separately validated shaping backend before being advertised.
    tokens = re.findall(r"[A-Za-z0-9_./:@%+-]+\s*|[^\n]", text.replace("\n", " "))
    lines = []
    current = ""
    for token in tokens:
        if font.getlength(token.strip()) > width:
            return None  # Do not split/truncate an identifier to force it to fit.
        proposed = current + token
        if current and font.getlength(proposed.rstrip()) > width:
            lines.append(current.rstrip())
            current = token.lstrip()
        else:
            current = proposed
    if current.strip():
        lines.append(current.rstrip())
    return lines


def estimate_font(source_text: str, box, stroke_crop: Image.Image, fonts: list[str]):
    bounds = stroke_crop.getbbox()
    if not bounds:
        return None
    ink_width, ink_height = bounds[2] - bounds[0], bounds[3] - bounds[1]
    line_text = source_text.splitlines()[0]
    best = None
    # Compare rendering of the recognized SOURCE text, not target-script em ratios.
    for filename in fonts:
        for size in range(max(8, int(ink_height * .8)), min(96, int(ink_height * 1.8) + 5)):
            font = ImageFont.truetype(filename, size)
            x0, y0, x1, y1 = font.getbbox(line_text)
            score = abs((y1 - y0) - ink_height) / max(ink_height, 1)
            score += abs((x1 - x0) - ink_width) / max(ink_width, 1)
            if best is None or score < best[0]:
                best = (score, filename, size)
    return best


def compose(source: Image.Image, units: list[Unit], translations: dict[str, str],
            stroke_mask: Image.Image, repair, fonts: list[str], protected_mask: Image.Image,
            *, source_fonts=None, repair_region=None, readable=False):
    from .appearance import appearance, estimate_source, font_at, font_size_for_height, target_family, recover_source_marks
    from pathlib import Path
    validate_translations(units, translations)
    source = source.convert("RGBA")
    stroke_mask = stroke_mask.convert("L").point(lambda value: 255 if value else 0)
    protected_mask = protected_mask.convert("L")
    if source.size != stroke_mask.size or source.size != protected_mask.size:
        raise ValueError("mask_size_mismatch")
    result = source.copy()
    edit_union = Image.new("L", source.size)
    reports = []
    for unit in units:
        box = unit.box
        translated = translations[unit.identifier].strip()
        report = {"id": unit.identifier, "source": unit.text, "translation": translated,
                  "box": list(box), "status": "preserved", "reason": "",
                  "layout_policy": "readable" if readable else "strict"}
        reports.append(report)
        if translated == unit.text.strip():
            report["reason"] = "equivalent"
            continue
        left, top, right, bottom = box
        if left < 0 or top < 0 or right > source.width or bottom > source.height or left >= right or top >= bottom:
            raise ValueError("invalid_unit_box")
        if protected_mask.crop(box).getbbox():
            report["reason"] = "protected_region"
            continue
        crop = source.crop(box)
        stroke = stroke_mask.crop(box)
        if not stroke.getbbox():
            report["reason"] = "no_confident_text_mask"
            continue
        for text, source_box in zip(unit.source_lines, unit.boxes):
            relative = (source_box[0]-left, source_box[1]-top, source_box[2]-left, source_box[3]-top)
            refined = recover_source_marks(crop.crop(relative), stroke.crop(relative), text, source_fonts or fonts)
            if readable:
                from .appearance import complete_readable_strokes
                refined = complete_readable_strokes(crop.crop(relative), refined)
            stroke.paste(refined, relative[:2])
        style = appearance(crop, stroke)
        if style is None:
            report['reason'] = 'foreground_unavailable'
            continue
        stroke = style['erase']
        sample_box = unit.boxes[0]
        relative_sample = (sample_box[0]-left, sample_box[1]-top, sample_box[2]-left, sample_box[3]-top)
        estimate = estimate_source(unit.source_lines[0], style['font_ink'].crop(relative_sample), source_fonts or fonts)
        if estimate is None:
            report["reason"] = "font_unavailable"
            continue
        score, source_font, source_size, source_ink_height = estimate
        filename = target_family(source_font, fonts)
        # Use font reference metrics, not this sentence's accidental descenders.
        # Otherwise equally sized 'Settings' and 'Free' would get different sizes.
        source_reference = '国' if re.search(r'[\u4e00-\u9fff]', unit.source_lines[0]) else 'H'
        target_reference = '国' if re.search(r'[\u4e00-\u9fff]', translated) else 'H'
        reference_box = font_at(source_font, source_size).getbbox(source_reference)
        visual_height = reference_box[3] - reference_box[1]
        # Nominal point/pixel size is the visual hierarchy. CJK glyphs should not
        # be shrunk to a Latin capital's short ink-height before layout is tried.
        target_size = source_size
        fitted = None
        for size in range(target_size, max(7, int(target_size * .8)) - 1, -1):
            font = font_at(filename, size)
            lines = wrap_text(translated, font, crop.width)
            if not lines:
                continue
            ascent, descent = font.getmetrics()
            line_height = ascent + descent
            glyph_heights = [font.getbbox(line)[3] - font.getbbox(line)[1] for line in lines]
            needed = sum(glyph_heights) + max(0, len(lines) - 1) * max(2, round(size * .2))
            if needed <= crop.height:
                fitted = font, lines, needed, glyph_heights
                break
        if fitted is None:
            report["reason"] = "layout_unfit"
            continue
        font, lines, needed, glyph_heights = fitted
        glyph_mask = Image.new("L", crop.size)
        draw = ImageDraw.Draw(glyph_mask)
        y = max(0, min(style['ink'].getbbox()[1], crop.height - needed))
        for line, height in zip(lines, glyph_heights):
            bounds = font.getbbox(line)
            draw.text((-bounds[0], y - bounds[1]), line, font=font, fill=255)
            y += height + max(2, round(font.size * .2))

        # The appearance pass separates antialias fringes from independent artwork.
        obstacles = style['obstacles']
        collision = ImageChops.multiply(obstacles, glyph_mask.point(lambda v: 255 if v else 0))
        report['obstacle_pixels'] = sum(obstacles.histogram()[1:])
        report['collision_pixels'] = sum(collision.histogram()[1:])
        if collision.getbbox():
            report["reason"] = "artwork_collision"
            continue
        previous = edit_union.crop(box)
        edit = ImageChops.lighter(stroke, glyph_mask)
        if ImageChops.multiply(previous, edit).getbbox():
            report["reason"] = "neighbor_collision"
            continue
        foreground = style['foreground']
        repaired = (repair_region(source, box, stroke) if repair_region else repair(crop, stroke)).convert('RGBA')
        if repaired.size != crop.size:
            raise ValueError('repair_size_mismatch')
        # Accept only repaired pixels under the stroke mask, regardless of what
        # an inpainting model generated outside that mask.
        patched = Image.composite(repaired, crop, stroke)
        ink = Image.new("RGBA", crop.size, foreground + (255,))
        patched = Image.composite(ink, patched, glyph_mask)
        result.paste(patched, (left, top))
        edit_union.paste(ImageChops.lighter(previous, edit), (left, top))
        report.update(status="applied", reason="", font_pixels=font.size,
                      source_font_pixels=source_size, source_ink_height=source_ink_height,
                      reference_ink_height=visual_height,
                      target_ink_height=max(glyph_heights), text_color=list(foreground),
                      font_family=Path(filename).name, source_font_family=Path(source_font).name,
                      style_fit_score=round(score, 3), font_estimated=True, font_source="source_ink_fit")
    # Defensive invariant independent of learned-model quality.
    outside = ImageChops.invert(edit_union.point(lambda value: 255 if value else 0))
    difference = ImageChops.difference(source, result)
    if any(ImageChops.multiply(channel, outside).getbbox() for channel in difference.split()):
        raise ValueError("outside_edit_region_changed")
    return result, reports, edit_union
