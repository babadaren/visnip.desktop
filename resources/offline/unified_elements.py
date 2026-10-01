"""Shared screenshot geometry: source ink, context and drawing space are distinct.

No website names, fixture sizes or translations are used here. CTC anchors are
recognition hints, not permissions to erase pixels. All inference is local.
"""
from __future__ import annotations
from dataclasses import dataclass, field, replace
from difflib import SequenceMatcher
from collections import Counter
from statistics import median
from typing import Callable, Iterable, Sequence
import re
from PIL import Image, ImageChops, ImageDraw, ImageStat

Box = tuple[int, int, int, int]
Anchor = tuple[str, float, float]
AlignedReader = Callable[[Image.Image], tuple[str, float, list[Anchor]]]


@dataclass
class TextElement:
    identifier: str
    text: str
    boxes: list[Box]
    confidence: float = 1.0
    source_lines: list[str] = field(default_factory=list)
    role: str = 'text'
    alignment: list[Anchor] = field(default_factory=list)
    layout_box: Box | None = None
    context_group: str = ''

    def __post_init__(self):
        if not self.source_lines:
            self.source_lines = [self.text]
        if not self.boxes:
            raise ValueError('invalid_unit_box')

    @property
    def box(self) -> Box:
        return union_box(self.boxes)

    @classmethod
    def from_unit(cls, unit):
        return cls(unit.identifier, unit.text, list(unit.boxes), float(unit.confidence),
                   list(unit.source_lines), unit.role, list(getattr(unit, 'alignment', [])),
                   getattr(unit, 'layout_box', None), getattr(unit, 'context_group', ''))


def intersection(a: Box, b: Box) -> Box | None:
    l, t, r, d = max(a[0], b[0]), max(a[1], b[1]), min(a[2], b[2]), min(a[3], b[3])
    return (l, t, r, d) if l < r and t < d else None


def union_box(boxes: Iterable[Box]) -> Box:
    boxes = list(boxes)
    return (min(b[0] for b in boxes), min(b[1] for b in boxes),
            max(b[2] for b in boxes), max(b[3] for b in boxes))


def area(box: Box) -> int:
    return max(0, box[2]-box[0]) * max(0, box[3]-box[1])


def same_row(a: Box, b: Box) -> bool:
    h = min(a[3]-a[1], b[3]-b[1])
    return min(a[3], b[3])-max(a[1], b[1]) >= h*.65


def text_key(value: str) -> str:
    return re.sub(r'\s+', '', value).casefold()


def _contrast(image, background):
    channels = ImageChops.difference(image.convert('RGB'), Image.new('RGB', image.size, background)).split()
    return ImageChops.lighter(ImageChops.lighter(channels[0], channels[1]), channels[2])


def flat_background(image):
    if min(image.size) < 2:
        return None
    ring = Image.new('L', image.size)
    ImageDraw.Draw(ring).rectangle((0, 0, image.width-1, image.height-1), outline=255)
    background = tuple(round(v) for v in ImageStat.Stat(image.convert('RGB'), ring).median)
    hist = _contrast(image, background).histogram(mask=ring)
    return background if sum(hist[:9]) >= sum(hist)*.90 else None


def column_runs(image):
    background = flat_background(image)
    if background is None:
        return None
    ink = _contrast(image, background).point(lambda v: 255 if v > 12 else 0)
    pixels = ink.load()
    start, runs = None, []
    for x in range(ink.width+1):
        present = x < ink.width and any(pixels[x, y] for y in range(ink.height))
        if present and start is None:
            start = x
        if not present and start is not None:
            runs.append((start, x))
            start = None
    return runs, ink


def _valid_elements(image, elements):
    identifiers = set()
    if len(elements) > 256:
        raise ValueError('ocr_empty_or_too_many_units')
    for element in elements:
        if element.identifier in identifiers:
            raise ValueError('translation_id_mismatch')
        identifiers.add(element.identifier)
        for l, t, r, b in element.boxes:
            if not (0 <= l < r <= image.width and 0 <= t < b <= image.height):
                raise ValueError('invalid_unit_box')


def normalize_elements(image, units, reader: AlignedReader | None = None):
    """Resolve overlapping readings before translation, independent of order.

    Disjoint identical labels and separate rows are never deduplicated. An
    ambiguous overlap remains explicit so a compositor can expose panel text.
    """
    elements = [TextElement.from_unit(u) for u in units]
    _valid_elements(image, elements)
    elements.sort(key=lambda u: (u.box[1], u.box[0], u.identifier))
    groups = []
    stats = dict(raw_regions=len(elements), overlap_groups=0, rereads=0,
                 duplicates_removed=0, gutters_split=0, unresolved_overlaps=0)
    for element in elements:
        connected = [g for g in groups if any(same_row(element.box, v.box)
                     and intersection(element.box, v.box) for v in g)]
        merged = [element]
        for group in connected:
            merged.extend(group)
            groups.remove(group)
        groups.append(merged)
    normalized = []
    for group in groups:
        if len(group) == 1:
            normalized.extend(group)
            continue
        stats['overlap_groups'] += 1
        group.sort(key=lambda u: (u.box[0], u.box[1], u.identifier))
        bounds = union_box(u.box for u in group)
        if reader and len(group) <= 12 and bounds[3]-bounds[1] <= 96 and bounds[2]-bounds[0] <= 4096:
            text, confidence, anchors = reader(image.crop(bounds))
            stats['rereads'] += 1
            key = text_key(text)
            matches = [SequenceMatcher(None, text_key(u.text), key, autojunk=False)
                       .find_longest_match().size for u in group]
            covered = all(m >= max(2, len(text_key(u.text))*.70) for m, u in zip(matches, group))
            pattern = r'\d+(?:[.,]\d+)*|\{\w+\}|%[sd]'
            protected = Counter()
            for u in group:
                protected |= Counter(re.findall(pattern, u.text))
            tokens = Counter(re.findall(pattern, text))
            if text.strip() and confidence >= .85 and covered and not (protected-tokens):
                normalized.append(TextElement(group[0].identifier, text.strip(), [bounds], confidence,
                    alignment=[(ch, bounds[0]+x0, bounds[0]+x1) for ch, x0, x1 in anchors]))
                stats['duplicates_removed'] += len(group)-1
                continue
        kept = []
        for element in sorted(group, key=lambda u: (-u.confidence, u.identifier)):
            duplicate = any(text_key(element.text) == text_key(other.text)
                and (overlap := intersection(element.box, other.box))
                and area(overlap) >= .85*min(area(element.box), area(other.box)) for other in kept)
            if duplicate:
                stats['duplicates_removed'] += 1
            else:
                kept.append(element)
        stats['unresolved_overlaps'] += int(len(kept) > 1)
        normalized.extend(kept)
    output = []
    for element in sorted(normalized, key=lambda u: (u.box[1], u.box[0], u.identifier)):
        row_peers = sum(same_row(element.box, other.box) and other.identifier != element.identifier
                        and (other.role == 'ui-label' or len(other.text.split()) <= 3)
                        for other in normalized)
        parts = split_visual_gutters(image, element, allow_single=row_peers >= 3)
        stats['gutters_split'] += len(parts)-1
        output.extend(parts)
    _valid_elements(image, output)
    stats['element_regions'] = len(output)
    return output, stats


def split_visual_gutters(image, unit, *, allow_single=False):
    """Repeated large pixel gaps and decoded word boundaries define UI elements.

    Ordinary spaces do not cause word-by-word translation. Gap measurements are
    relative to visible glyph height rather than a particular screenshot size.
    """
    from .translation_policy import preserve_token
    if len(unit.boxes) != 1 or not unit.alignment or unit.role != 'text' or preserve_token(unit.text):
        return [unit]
    info = column_runs(image.crop(unit.box))
    if info is None:
        return [unit]
    runs, ink = info
    bounds = ink.getbbox()
    if len(runs) < 4 or not bounds:
        return [unit]
    # Copyright symbols, descenders and antialiasing can be taller than most
    # letters. The typical connected glyph height is a more stable scale.
    from .appearance import _components
    full_height = bounds[3]-bounds[1]
    component_heights = [max(y for _,y in c)-min(y for _,y in c)+1 for c in _components(ink)]
    plausible = [h for h in component_heights if h >= max(4, full_height*.45)]
    h = median(plausible) if plausible else full_height
    gaps = [(runs[i-1][1], runs[i][0]) for i in range(1, len(runs))]
    normal = [b-a for a, b in gaps if 0 < b-a < h*.8]
    threshold = max(h*.9, median(normal)*2.8 if normal else h*.9)
    strong = [(a, b) for a, b in gaps if b-a >= threshold]
    minimum_cuts = 1 if allow_single else 2
    if len(strong) < minimum_cuts:
        return [unit]
    anchors = unit.alignment
    if text_key(''.join(c for c, _, _ in anchors)) != text_key(unit.text):
        return [unit]
    l, t, r, btm = unit.box
    cuts = []
    for gap_start, gap_end in strong:
        center = l+(gap_start+gap_end)/2
        index = next((i for i, (_, a, b) in enumerate(anchors) if (a+b)/2 >= center), len(anchors))
        if not 0 < index < len(anchors):
            continue
        candidates = [i for i in range(max(1, index-2), min(len(anchors), index+3))
                      if anchors[i-1][0].isspace() or anchors[i][0].isspace()]
        if not candidates:
            continue
        index = min(candidates, key=lambda i: abs((anchors[i][1]+anchors[i][2])/2-center))
        if cuts and index <= cuts[-1][0]:
            continue
        cuts.append((index, gap_start, gap_end))
    if len(cuts) < minimum_cuts:
        return [unit]
    segments, i, x0 = [], 0, 0
    for j, a, b in cuts:
        segments.append((i, j, x0, min(r-l, a+1)))
        i, x0 = j, max(0, b-1)
    segments.append((i, len(anchors), x0, r-l))
    output = []
    for n, (i, j, x0, x1) in enumerate(segments):
        value = ''.join(c for c, _, _ in anchors[i:j]).strip()
        if not value or x0 >= x1:
            return [unit]
        role = 'ui-label'
        if re.fullmatch(r'[©®]\s*\d{4}(?:\s+\S+){1,8}', value) or not re.search(r'\w', value):
            role = 'identity'
        output.append(TextElement(f'{unit.identifier}s{n}', value, [(l+x0, t, l+x1, btm)],
                      unit.confidence, role=role, alignment=anchors[i:j], context_group=unit.identifier))
    if text_key(''.join(u.text for u in output)) != text_key(unit.text):
        return [unit]
    return output


def adopt_refinement(elements, refined):
    """Keep validated icon helpers; their extra blank area is draw space only."""
    originals = {u.identifier: u for u in elements}
    output = []
    for unit in refined:
        new = TextElement.from_unit(unit)
        old = originals.get(unit.identifier)
        if old and len(new.boxes) == len(old.boxes) == 1:
            l, t, r, b = new.box
            source_right = min(r, old.box[2])
            if source_right > l:
                new.layout_box = new.box if r > source_right else new.layout_box
                new.boxes = [(l, t, source_right, b)]
            if new.text == old.text:
                new.alignment = list(getattr(old, 'alignment', []))
                new.context_group = getattr(old, 'context_group', '')
        output.append(new)
    return output


def group_elements(elements):
    """Join wrapped prose, not label rows or metadata; keep source line boxes."""
    from .ui_structure import metadata_line
    groups = []
    for line in sorted(elements, key=lambda u: (u.box[1], u.box[0], u.identifier)):
        merged = False
        for group in reversed(groups):
            a, b = group.boxes[-1], line.box
            h = a[3]-a[1]
            prose = len(group.text.split()) >= 5 or len(re.findall(r'[\u3400-\u9fff]', group.text)) >= 16
            continuation = len(line.text.split()) >= 3 or len(re.findall(r'[\u3400-\u9fff]', line.text)) >= 6
            if (group.role == line.role == 'text' and prose and continuation and not metadata_line(line.text)
                    and 0 <= b[1]-a[3] <= h*.6 and abs(b[0]-a[0]) <= h*.5
                    and abs((b[3]-b[1])-h) <= h*.25 and b[2] <= a[2]+h
                    and not re.search(r'[。！？.!?:：]$', group.text.strip())):
                group.text += ('' if re.search(r'[\u3400-\u9fff]$', group.text) else ' ')+line.text
                group.boxes.extend(line.boxes)
                group.source_lines.extend(line.source_lines)
                group.alignment.extend(line.alignment)
                group.confidence = min(group.confidence, line.confidence)
                group.layout_box = None
                merged = True
                break
        if not merged:
            groups.append(replace(line, boxes=list(line.boxes), source_lines=list(line.source_lines),
                                  alignment=list(line.alignment)))
    return groups


def allocate_layout(image, units):
    """Reserve ALL original text before sharing neighbouring blank drawing space."""
    _valid_elements(image, units)
    result = []
    for unit in units:
        l, t, r, b = unit.box
        if unit.role == 'identity':
            result.append(replace(unit, layout_box=unit.box))
            continue
        end = image.width
        for other in units:
            if other.identifier == unit.identifier:
                continue
            x0, y0, x1, y1 = other.box
            if x0 >= r and max(t, y0) < min(b, y1):
                end = min(end, (r+x0)//2)
        # Old helpers only offered a small amount of extra space. Inspect the
        # actual blank row instead; frame edges and neighbouring text still stop it.
        background = flat_background(image.crop(unit.box))
        if end <= r or background is None:
            result.append(replace(unit, layout_box=unit.box))
            continue
        strip = _contrast(image.crop((r, t, end, b)), background)
        blank = 0
        for x in range(strip.width):
            if any(strip.getpixel((x, y)) > 8 for y in range(strip.height)):
                break
            blank += 1
        result.append(replace(unit, layout_box=(l, t, r+max(0, blank-1), b)))
    return sorted(result, key=lambda u: (u.box[1], u.box[0], u.identifier))


def decode_ctc_anchors(ids, scores, alphabet, *, original_width, resized_width, padded_width):
    """Local-x CTC time anchors, not exact glyph boxes; padding mapped explicitly."""
    from math import isfinite
    if len(ids) != len(scores) or not len(ids) or not (0 < original_width and 0 < resized_width <= padded_width):
        raise ValueError('invalid_ocr_alignment')
    step = padded_width/len(ids)*original_width/resized_width
    decoded = []
    i = 0
    while i < len(ids):
        index = int(ids[i])
        j = i+1
        while j < len(ids) and int(ids[j]) == index:
            j += 1
        if not 0 <= index < len(alphabet):
            raise ValueError('ocr_dictionary_model_mismatch')
        if index:
            score = float(scores[i])
            if not isfinite(score):
                raise ValueError('invalid_ocr_alignment')
            start, end = min(float(original_width), i*step), min(float(original_width), j*step)
            decoded.append((alphabet[index], start, end, score))
        i = j
    while decoded and decoded[0][0].isspace():
        decoded.pop(0)
    while decoded and decoded[-1][0].isspace():
        decoded.pop()
    text = ''.join(item[0] for item in decoded)
    confidence = sum(item[3] for item in decoded)/len(decoded) if decoded else 0.
    return text, confidence, [(ch, start, end) for ch, start, end, _ in decoded]
