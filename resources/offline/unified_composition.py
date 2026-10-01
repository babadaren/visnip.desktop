"""Plan all image edits before drawing; preserve valid text on layout failure.

No screenshot text or pixels are persisted by this compositor. Valid translations
that cannot fit safely are returned for a persistent client-side text panel.
"""
from __future__ import annotations
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from time import perf_counter
from statistics import median
import re
from PIL import Image, ImageChops, ImageDraw, ImageFont, ImageStat
from .appearance import estimate_source, target_family
from .layout import protected_token_counts
from .unified_elements import TextElement, allocate_layout, intersection, flat_background, _contrast

# Degradation policy, in the order the user sees it: keep the source size, then
# shrink, then tighten leading, then shrink again to clear artwork, then preserve.
SHRINK_FLOOR_RATIO = .70
TIGHT_LEADING_RATIO = .08
NORMAL_LEADING_RATIO = .2
READABILITY_FLOOR = {'cjk': 10, 'latin': 9}
EXHAUSTIVE_POSITION_SLACK = 40
NEAREST_POSITION_COUNT = 12


def validate_values(units, translations):
    if set(translations) != {u.identifier for u in units}:
        raise ValueError('translation_id_mismatch')
    for unit in units:
        value = translations[unit.identifier]
        if not isinstance(value, str) or not value.strip() or len(value) > 8000:
            raise ValueError('invalid_translation')
        if protected_token_counts(unit.text) != protected_token_counts(value):
            raise ValueError('protected_token_mismatch')


@lru_cache(maxsize=128)
def _font(path, size):
    return ImageFont.truetype(path, size)


def _wrap(text, font, width):
    # Identifiers are never truncated merely to force an apparently successful fit.
    tokens = re.findall(r'[A-Za-z0-9_./:@%+\-]+|\n|[^\n]', text)
    lines, line = [], ''
    for token in tokens:
        if token == '\n':
            lines.append(line.rstrip())
            line = ''
            continue
        if font.getlength(token.strip()) > width:
            return None
        if line and font.getlength((line+token).rstrip()) > width:
            lines.append(line.rstrip())
            line = token.lstrip()
        else:
            line += token
    if line.strip():
        lines.append(line.rstrip())
    return lines or None


def _style(crop, mask):
    if not mask.getbbox():
        return None
    rgb = crop.convert('RGB')
    background = flat_background(rgb)
    if background is None:
        ring = Image.new('L', crop.size)
        ImageDraw.Draw(ring).rectangle((0, 0, crop.width-1, crop.height-1), outline=255)
        background = tuple(round(v) for v in ImageStat.Stat(rgb, ring).median)
    distance = _contrast(rgb, background)
    # Use luminance to avoid selecting only the red/cyan ClearType fringe of
    # otherwise neutral text. Geometry still uses full-channel contrast.
    colour_distance = ImageChops.difference(rgb, Image.new('RGB', crop.size, background)).convert('L')
    hist = colour_distance.histogram(mask=mask)
    total, accum, limit = sum(hist), 0, 0
    for value, count in enumerate(hist):
        accum += count
        if accum >= total*.98:
            limit = value
            break
    if limit < 5:
        return None
    core = ImageChops.multiply(mask, colour_distance.point(lambda v: 255 if v >= max(4, limit) else 0))
    if not core.getbbox():
        return None
    foreground = tuple(round(v) for v in ImageStat.Stat(rgb, core).median)
    visible = distance.point(lambda v: 255 if v > max(4, limit*.20) else 0)
    # Source ink on the same contrast scale as the style pass. Used only to
    # identify the source font, never to widen the erasure mask.
    front = Image.new('RGB', (1, 1), foreground).convert('L').getpixel((0, 0))
    back = Image.new('RGB', (1, 1), background).convert('L').getpixel((0, 0))
    span = max(1, abs(front-back))
    coverage = rgb.convert('L').point(lambda v: min(255, round(255*abs(v-back)/span)))
    return foreground, visible, ImageChops.multiply(coverage, mask)


def _reference_glyph(text):
    """A glyph that stays the same size inside one script, unlike a whole word."""
    match = re.search(r'[\u3400-\u9fff]', text)
    return match.group(0) if match else 'H'


def _readability_floor(text):
    return READABILITY_FLOOR['cjk' if re.search(r'[\u3400-\u9fff]', text) else 'latin']


@lru_cache(maxsize=4096)
def _reference_ink(filename, size, reference):
    bounds = _font(filename, size).getbbox(reference)
    return max(1, bounds[3]-bounds[1])


@lru_cache(maxsize=1024)
def _equivalent_size(base_font, base_size, target_font, reference):
    """Size in target_font whose reference glyph matches base_font at base_size.

    Copying a pixel size between a Latin and a CJK family changes how large the
    text looks; matching the reference glyph keeps the visual hierarchy.
    """
    if base_font == target_font:
        return base_size
    observed = _reference_ink(base_font, base_size, reference)
    lo, hi = 6, 192
    while lo < hi:
        middle = (lo+hi)//2
        if _reference_ink(target_font, middle, reference) < observed:
            lo = middle+1
        else:
            hi = middle
    return lo


def _font_candidates(source_font, font_files):
    """Target family first, remaining candidates in configured order."""
    preferred = target_family(source_font, font_files)
    order = [preferred]+[name for name in font_files if name != preferred]
    return order[:4]


def _fit_target(target, candidates, base_font, base_size, width, height, reference):
    """Fit the translation, degrading size then leading before giving up."""
    floor_limit = _readability_floor(target)
    for tight in (False, True):
        for filename in candidates:
            start = _equivalent_size(base_font, base_size, filename, reference)
            floor = max(floor_limit, round(start*SHRINK_FLOOR_RATIO))
            for size in ([floor] if tight else range(start, floor-1, -1)):
                try:
                    font = _font(filename, size)
                except OSError:
                    break
                lines = _wrap(target, font, width)
                if not lines:
                    continue
                bounds = [font.getbbox(line) for line in lines]
                heights = [max(1, bb[3]-bb[1]) for bb in bounds]
                gap = max(1, round(size*(TIGHT_LEADING_RATIO if tight else NORMAL_LEADING_RATIO)))
                needed = sum(heights)+(len(lines)-1)*gap
                if needed <= height:
                    if tight:
                        mode = 'tight_leading'
                    else:
                        mode = 'source_size' if size == start else 'shrunk'
                    return font, lines, bounds, heights, gap, needed, mode, start
    return None


def _glyph_for(font, lines, bounds, heights, gap, size, top):
    glyph = Image.new('L', size)
    painter = ImageDraw.Draw(glyph)
    y = top
    for line, box, height in zip(lines, bounds, heights):
        painter.text((-box[0], y-box[1]), line, font=font, fill=255)
        y += height+gap
    return glyph


def _placed_without_collision(font, lines, bounds, heights, gap, size, needed, obstacles, preferred):
    """Nearest vertical position that leaves independent artwork untouched."""
    slack = size[1]-needed
    if slack <= 0:
        return None
    positions = sorted(range(slack+1), key=lambda top: abs(top-preferred))
    # Small slack is cheap to search exhaustively; large slack only needs the
    # positions closest to the source baseline.
    limit = len(positions) if slack <= EXHAUSTIVE_POSITION_SLACK else NEAREST_POSITION_COUNT
    for top in positions[:limit]:
        glyph = _glyph_for(font, lines, bounds, heights, gap, size, top)
        blocked = ImageChops.multiply(obstacles, glyph.point(lambda v: 255 if v else 0))
        if not blocked.getbbox():
            return glyph, blocked
    return None


def _identify_source(unit, box, ink, fonts):
    """Source family from its own glyphs; the target family then matches its style."""
    line = (unit.source_lines or [unit.text])[0]
    if not line.strip():
        return None
    x0, y0, x1, y1 = unit.boxes[0]
    left, top = box[0], box[1]
    region = (x0-left, y0-top, x1-left, y1-top)
    if region[0] < 0 or region[1] < 0 or region[2] > ink.width or region[3] > ink.height:
        return None
    observed = ink.crop(region)
    if not observed.getbbox():
        return None
    return estimate_source(line, observed, fonts)


@dataclass
class EditPlan:
    unit: TextElement
    report: dict
    erase: Image.Image
    glyph: Image.Image
    foreground: tuple[int, int, int]
    edit: Image.Image
    rejected: bool = False


def _panel(report, reason):
    report.update(status='preserved', reason=reason, display_mode='panel', translation_status='translated')


def _source_sizes(units, translations, mask, font_file):
    """Font size of each changed region, from its own glyphs, then unified.

    Ink height alone depends on ascenders/descenders ("Projects" vs
    "Discussions"). Rendering the same source line finds the size that
    produces the observed ink, so labels of one style get one size. Sizes
    within 15% of each other are snapped to their median.
    """
    from .appearance import font_size_for_height
    sizes = {}
    for unit in units:
        if translations[unit.identifier].strip() == unit.text.strip():
            continue
        lines = unit.source_lines if len(unit.source_lines) == len(unit.boxes) else [unit.text]*len(unit.boxes)
        measured = []
        for text, line_box in zip(lines, unit.boxes):
            bounds = mask.crop(line_box).getbbox()
            if bounds and text.strip():
                measured.append(font_size_for_height(font_file, text.strip(), bounds[3]-bounds[1]))
        if measured:
            sizes[unit.identifier] = median(measured)
    values = list(sizes.values())
    return {identifier: round(median([w for w in values if abs(w-v) <= v*.15]))
            for identifier, v in sizes.items()}


def compose(source, units, translations, stroke_mask, repair, fonts, protected_mask,
            *, source_fonts=None, repair_region=None, readable=True, prepare_repair=None, metrics=None):
    """Same-size bitmap plus complete region reports and an actual edit union.

    All proposals are based on the immutable original. Conflict resolution is
    symmetric, so reversing detector order cannot change whose text disappears.
    """
    del source_fonts, readable
    units = [TextElement.from_unit(u) for u in units]
    validate_values(units, translations)
    source = source.convert('RGBA')
    mask = stroke_mask.convert('L').point(lambda v: 255 if v else 0)
    protected = protected_mask.convert('L').point(lambda v: 255 if v else 0)
    if source.size != mask.size or source.size != protected.size:
        raise ValueError('mask_size_mismatch')
    timings = metrics if metrics is not None else {}
    units = allocate_layout(source, units)
    reports, plans = [], []
    regular = [str(p) for p in fonts if not any(s in Path(p).name.lower() for s in ('bold', 'bd.', 'bd.tt', 'semibold'))]
    font_files = regular or list(fonts)
    source_sizes = _source_sizes(units, translations, mask, font_files[0]) if font_files else {}
    for unit in units:
        target = translations[unit.identifier].strip()
        box = unit.layout_box or unit.box
        l, t, r, b = box
        report = dict(id=unit.identifier, source=unit.text, translation=target,
                      source_boxes=[list(x) for x in unit.boxes], box=list(box),
                      status='preserved', reason='', display_mode='unchanged',
                      translation_status='preserved', layout_policy='unified-v1')
        reports.append(report)
        if target == unit.text.strip():
            report['reason'] = 'equivalent'
            continue
        report['translation_status'] = 'translated'
        crop = source.crop(box)
        allowed = Image.new('L', crop.size)
        draw = ImageDraw.Draw(allowed)
        for x0, y0, x1, y1 in unit.boxes:
            draw.rectangle((x0-l, y0-t, x1-l-1, y1-t-1), fill=255)
        erase = ImageChops.multiply(mask.crop(box), allowed)
        if ImageChops.multiply(erase, protected.crop(box)).getbbox():
            _panel(report, 'protected_region')
            continue
        if not erase.getbbox():
            _panel(report, 'no_confident_text_mask')
            continue
        style = _style(crop, erase)
        if style is None:
            _panel(report, 'foreground_unavailable')
            continue
        if not font_files:
            _panel(report, 'font_unavailable')
            continue
        foreground, visible, ink = style
        if unit.identifier in source_sizes:
            estimate, size_source = max(8, min(160, source_sizes[unit.identifier])), 'source_glyph_fit'
        else:
            heights = []
            for x0, y0, x1, y1 in unit.boxes:
                bounds = erase.crop((x0-l, y0-t, x1-l, y1-t)).getbbox()
                if bounds:
                    heights.append(bounds[3]-bounds[1])
            estimate = max(8, min(160, round((median(heights) if heights else crop.height)*1.15)))
            size_source = 'readable_ink_height'
        identified = _identify_source(unit, box, ink, font_files)
        source_font = identified[1] if identified else font_files[0]
        fitted = _fit_target(target, _font_candidates(source_font, font_files), font_files[0],
                             estimate, crop.width, crop.height, _reference_glyph(target))
        if not fitted:
            _panel(report, 'layout_unfit')
            continue
        font, lines, bounds, heights, gap, needed, fit_mode, size_start = fitted
        obstacles = ImageChops.subtract(visible, erase)
        preferred_y = max(0, min(erase.getbbox()[1], crop.height-needed))
        glyph = _glyph_for(font, lines, bounds, heights, gap, crop.size, preferred_y)
        collision = ImageChops.multiply(obstacles, glyph.point(lambda v: 255 if v else 0))
        if collision.getbbox():
            # Search vertical slack before rejecting a label. Decorations remain
            # untouched; this is not permission to erase an underline or icon.
            placed = _placed_without_collision(font, lines, bounds, heights, gap, crop.size,
                                               needed, obstacles, preferred_y)
            if placed:
                glyph, collision = placed
        if collision.getbbox():
            # A slightly smaller label is a better outcome than dropping the
            # region; shrink stepwise and only then preserve the source text.
            floor = max(_readability_floor(target), round(size_start*.6))
            for size in range(font.size-1, floor-1, -1):
                smaller = _font(font.path, size)
                lines2 = _wrap(target, smaller, crop.width)
                if not lines2:
                    break
                boxes2 = [smaller.getbbox(line) for line in lines2]
                heights2 = [max(1, bb[3]-bb[1]) for bb in boxes2]
                gap2 = max(1, round(size*.1))
                needed2 = sum(heights2)+(len(lines2)-1)*gap2
                if needed2 > crop.height:
                    continue
                placed = _placed_without_collision(smaller, lines2, boxes2, heights2, gap2, crop.size,
                                                   needed2, obstacles, preferred_y)
                if placed:
                    font, lines, bounds, heights, gap, needed = smaller, lines2, boxes2, heights2, gap2, needed2
                    glyph, collision = placed
                    fit_mode = 'collision_shrunk'
                    break
        report.update(obstacle_pixels=sum(obstacles.histogram()[1:]), collision_pixels=sum(collision.histogram()[1:]))
        if collision.getbbox():
            _panel(report, 'artwork_collision')
            continue
        edit = ImageChops.lighter(erase, glyph).point(lambda v: 255 if v else 0)
        if ImageChops.multiply(edit, protected.crop(box)).getbbox():
            _panel(report, 'protected_region')
            continue
        intrudes = False
        for other in units:
            if other.identifier == unit.identifier:
                continue
            for other_box in other.boxes:
                overlap = intersection(box, other_box)
                if overlap and edit.crop((overlap[0]-l, overlap[1]-t, overlap[2]-l, overlap[3]-t)).getbbox():
                    intrudes = True
                    break
            if intrudes:
                break
        if intrudes:
            _panel(report, 'neighbor_collision')
            continue
        report.update(font_pixels=font.size, font_family=Path(font.path).name, source_font_pixels=estimate,
                      source_font_family=Path(source_font).name,
                      source_ink_height=(identified[3] if identified else None),
                      target_ink_height=max(heights), fit_mode=fit_mode,
                      shrink_ratio=round(font.size/max(1, size_start), 3),
                      style_fit_score=(round(identified[0], 3) if identified else None),
                      text_color=list(foreground), font_estimated=True, font_source=size_source)
        plans.append(EditPlan(unit, report, erase, glyph, foreground, edit))
    # Detect proposal conflicts without modifying any pixels or depending on order.
    for i, a in enumerate(plans):
        for b in plans[i+1:]:
            x = intersection(tuple(a.report['box']), tuple(b.report['box']))
            if not x:
                continue
            ab, bb = a.report['box'], b.report['box']
            am = a.edit.crop((x[0]-ab[0], x[1]-ab[1], x[2]-ab[0], x[3]-ab[1]))
            bm = b.edit.crop((x[0]-bb[0], x[1]-bb[1], x[2]-bb[0], x[3]-bb[1]))
            if ImageChops.multiply(am, bm).getbbox():
                a.rejected = b.rejected = True
                _panel(a.report, 'neighbor_collision')
                _panel(b.report, 'neighbor_collision')
    accepted = [p for p in plans if not p.rejected]
    erasure = Image.new('L', source.size)
    for plan in accepted:
        l, t, r, b = plan.report['box']
        erasure.paste(ImageChops.lighter(erasure.crop((l, t, r, b)), plan.erase), (l, t))
    start = perf_counter()
    if accepted and prepare_repair:
        prepare_repair(source, erasure)
    timings['repair'] = round((perf_counter()-start)*1000)
    result = source.copy()
    edits = Image.new('L', source.size)
    for plan in accepted:
        box = tuple(plan.report['box'])
        l, t, r, b = box
        original = source.crop(box)
        fixed = (repair_region(source, box, plan.erase) if repair_region else repair(original, plan.erase)).convert('RGBA')
        if fixed.size != original.size:
            raise ValueError('repair_size_mismatch')
        patch = Image.composite(fixed, original, plan.erase)
        patch = Image.composite(Image.new('RGBA', original.size, plan.foreground+(255,)), patch, plan.glyph)
        # Overlapping blank drawing rectangles must not reset earlier edits.
        result.paste(Image.composite(patch, result.crop(box), plan.edit), (l, t))
        edits.paste(ImageChops.lighter(edits.crop(box), plan.edit), (l, t))
        plan.report.update(status='applied', reason='', display_mode='inline', translation_status='translated')
    outside = ImageChops.invert(edits)
    difference = ImageChops.difference(source, result)
    if any(ImageChops.multiply(c, outside).getbbox() for c in difference.split()):
        raise ValueError('outside_edit_region_changed')
    if ImageChops.multiply(edits, protected).getbbox():
        raise ValueError('outside_edit_region_changed')
    timings['inline_regions'] = sum(r['display_mode'] == 'inline' for r in reports)
    timings['panel_regions'] = sum(r['display_mode'] == 'panel' for r in reports)
    return result, reports, edits
