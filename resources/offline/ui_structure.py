"""Conservative UI structure refinement before prose grouping and pixel editing.

Only detach a graphic when its separate image column and an OCR prefix agree.
Do not merge footer badges into prose or modify already-target-language text.
"""
from __future__ import annotations
import re
from PIL import Image, ImageChops, ImageStat
from .layout import Unit

DECORATIONS = '\u2606\u2605\u2022\u25cf\u25cb\u25a0\u25a1'


def column_runs(image):
    from .appearance import max_channel
    rgb=image.convert('RGB')
    border=Image.new('L',rgb.size)
    from PIL import ImageDraw
    ImageDraw.Draw(border).rectangle((0,0,rgb.width-1,rgb.height-1),outline=255)
    background=tuple(round(v) for v in ImageStat.Stat(rgb,border).median)
    active=max_channel(ImageChops.difference(rgb,Image.new('RGB',rgb.size,background))).point(lambda p:255 if p>12 else 0)
    a=active.load();runs=[];start=None
    for x in range(rgb.width+1):
        present=x<rgb.width and any(a[x,y] for y in range(rgb.height))
        if present and start is None:start=x
        if not present and start is not None:runs.append((start,x));start=None
    return runs,active


def _label_key(text):
    return re.sub(r'\s+', ' ', text.strip()).casefold()


def _crop_inline_label(image, unit, recognize_line):
    """Exclude a separate leading graphic only after re-reading the label.

    Detection boxes often contain an icon even when recognition omits it. A
    bounded recognizer check locates the actual text without a font-template
    match. Plain leading words, numbers, and arbitrary Chinese text are not
    discarded to make a crop fit. No additional model is loaded here.
    """
    from .translation_policy import preserve_token
    if (recognize_line is None or len(unit.boxes) != 1 or unit.confidence < .85
            or not 2 <= len(unit.text.strip()) <= 64 or preserve_token(unit.text)):
        return unit
    left, top, right, bottom = unit.box
    if not (8 <= bottom-top <= 48 and 15 <= right-left <= 480):
        return unit
    crop = image.crop(unit.box)
    runs, ink = column_runs(crop)
    if len(runs) < 2:
        return unit
    gaps = [(runs[i-1][1], runs[i][0]) for i in range(1, len(runs))
            if runs[i][0]-runs[i-1][1] >= max(5, round(crop.height*.24))]
    # Rightmost matching suffix excludes both the icon and any neighbouring
    # label fragment captured by an over-wide detector box (e.g. Settings).
    for end, begin in list(reversed(gaps))[:3]:
        if crop.width-begin < 10 or not ink.crop((0, 0, end, crop.height)).getbbox():
            continue
        x = max(0, begin-1)
        text, confidence = recognize_line(crop.crop((x, 0, crop.width, crop.height)))
        if confidence < .88 or len(text.strip()) < 2:
            continue
        original_key, new_key = _label_key(unit.text), _label_key(text)
        equivalent = original_key == new_key
        # An angle-bracket code icon can be recognised as punctuation. A table
        # icon can be recognised as one Chinese glyph. Require a separate
        # compact graphic with a rectangular outline for the latter case.
        suffix = original_key.endswith(new_key)
        prefix = original_key[:-len(new_key)].strip() if suffix else ''
        punctuation_icon = bool(prefix and re.fullmatch(r'[<>\u2606\u2605\u25a1\u25a0\u25cb\u25cf]+', prefix))
        square_icon = False
        if len(prefix) == 1 and re.fullmatch(r'[\u3400-\u9fff]', prefix):
            bounds = ink.crop((0, 0, end, crop.height)).getbbox()
            if bounds:
                shape = ink.crop(bounds)
                rows = [sum(bool(shape.getpixel((xx, yy))) for xx in range(shape.width)) for yy in range(shape.height)]
                cols = [sum(bool(shape.getpixel((xx, yy))) for yy in range(shape.height)) for xx in range(shape.width)]
                square_icon = (8 <= shape.width <= crop.height*1.2 and 8 <= shape.height
                    and max(rows[:3]) >= shape.width*.8 and max(rows[-3:]) >= shape.width*.8
                    and max(cols[:3]) >= shape.height*.8 and max(cols[-3:]) >= shape.height*.8)
        if equivalent or (suffix and (punctuation_icon or square_icon)):
            return Unit(unit.identifier, text.strip(), [(left+x, top, right, bottom)],
                        min(unit.confidence, confidence), role='ui-label')
    return unit


def refine_ui_regions(image, units, *, recognize_line=None):
    output=[]
    for unit in units:
        unit = _crop_inline_label(image, unit, recognize_line)
        if len(unit.boxes)!=1:
            output.append(unit);continue
        left,top,right,bottom=unit.box
        text=unit.text
        crop=image.crop(unit.box)
        runs,active=column_runs(crop)
        explicit=re.match('^['+re.escape(DECORATIONS)+']+\\s*',text)
        # A/I are normal standalone words, never infer that they are graphics.
        stray=re.match(r'^(?![AaIi]\s)[A-Za-z]\s+(?=[A-Za-z])',text)
        # An ordinary initial letter must not be removed based on OCR alone.
        if len(runs)>1 and (explicit or stray):
            first=runs[0]
            boundary=next((i for i in range(1,len(runs)) if runs[i][0]-runs[i-1][1]>=max(5,round(crop.height*.25))),None)
            if boundary is not None:
                end=runs[boundary-1][1];begin=runs[boundary][0]
                graphic=active.crop((first[0],0,end,crop.height)).getbbox()
                body=active.crop((begin,0,crop.width,crop.height)).getbbox()
                graphic_h=(graphic[3]-graphic[1]) if graphic else 0
                body_h=(body[3]-body[1]) if body else 0
                separate=(end-first[0]<=crop.height*1.15 and graphic_h>=crop.height*.45)
                axis_icon = False
                if graphic:
                    shape = active.crop((first[0], 0, end, crop.height)).crop(graphic)
                    # A chart's L-shaped axes are not a leading letter. Font
                    # descenders can make its height nearly equal to the text.
                    rows = [sum(bool(shape.getpixel((x,y))) for x in range(shape.width)) for y in range(shape.height)]
                    columns = [sum(bool(shape.getpixel((x,y))) for y in range(shape.height)) for x in range(shape.width)]
                    axis_icon = (shape.width >= 8 and shape.height >= 8
                                 and max(rows[-2:]) >= shape.width * .8
                                 and max(columns[:2]) >= shape.height * .8)
                nonletter=explicit or axis_icon or (graphic and body and graphic_h>=body_h+2 and graphic[1]<=body[1]-1)
                if separate and nonletter:
                    left+=begin-1
                    text=text[(explicit or stray).end():].strip()
        # A typographic middle dot with clearly separated text on both sides is
        # an inline delimiter, not part of the text to repaint. Preserve it.
        parts=re.split(r'\s+[\u00b7|]\s+',text)
        if len(parts)==2:
            local=image.crop((left,top,right,bottom));groups,ink=column_runs(local)
            # Ordinary word spaces are also gaps. Locate the isolated middle
            # dot itself rather than requiring exactly two gaps in the line.
            delimiters=[]
            for index in range(1,len(groups)-1):
                start,end=groups[index]
                bounds=ink.crop((start,0,end,local.height)).getbbox()
                if (bounds and end-start<=4 and bounds[3]-bounds[1]<=4
                        and start-groups[index-1][1]>=5 and groups[index+1][0]-end>=5):
                    delimiters.append(index)
            if len(delimiters)==1:
                index=delimiters[0];first_end=groups[index-1][1];last_start=groups[index+1][0]
                output.extend([Unit(unit.identifier+'a',parts[0],[(left,top,left+first_end+1,bottom)],unit.confidence),
                               Unit(unit.identifier+'b',parts[1],[(left+last_start-1,top,right,bottom)],unit.confidence)])
                continue
        output.append(Unit(unit.identifier,text,[(left,top,right,bottom)],unit.confidence, role=unit.role))
    return _refine_dropdown_groups(image, units, output, recognize_line)


def _person_outline(mask):
    """A head/shoulders icon has an open bottom, unlike a printed digit 8."""
    bounds = mask.getbbox()
    if not bounds:
        return False
    shape = mask.crop(bounds)
    w, h = shape.size
    if not (8 <= w <= 24 and 9 <= h <= 28 and .65 <= w/h <= 1.5):
        return False
    lower = range(max(0, h-3), h)
    center = range(w//3, max(w//3+1, 2*w//3))
    empty_center = sum(not shape.getpixel((x,y)) for y in lower for x in center)
    edge_rows = 0
    for y in lower:
        xs = [x for x in range(w) if shape.getpixel((x,y))]
        edge_rows += bool(xs and max(xs)-min(xs) >= w*.65)
    return empty_center >= .85*len(lower)*len(center) and edge_rows >= 2


def _refine_dropdown_groups(image, raw, refined, recognize_line):
    """Use independently verified rows to locate a dropdown's icon gutter.

    This is deliberately a second pass: no single unknown glyph is deleted on
    its own. At least four already verified, vertically separated menu labels
    must agree on the text column. A re-read must retain the full label suffix.
    """
    if recognize_line is None:
        return refined
    from statistics import median
    originals = {u.identifier:u for u in raw}
    anchors = [u for u in refined if u.role == 'ui-label' and u.identifier in originals
               and u.box[0]-originals[u.identifier].box[0] >= 4]
    groups = []
    for anchor in anchors:
        group = next((g for g in groups if abs(median(u.box[0] for u in g)-anchor.box[0]) <= 3), None)
        if group is None:
            groups.append([anchor])
        else:
            group.append(anchor)
    result = {u.identifier:u for u in refined}
    for group in groups[:8]:
        if len(group) < 4:
            continue
        ordered = sorted(group, key=lambda u:u.box[1])
        height = median(u.box[3]-u.box[1] for u in group)
        if sum(b.box[1]-a.box[1] >= height*.8 for a,b in zip(ordered,ordered[1:])) < 3:
            continue
        column = round(median(u.box[0] for u in group))
        y0, y1 = ordered[0].box[1]-height*3, ordered[-1].box[3]+height*3
        for unit in refined:
            if (unit.role != 'text' or len(unit.boxes) != 1 or unit.confidence < .85
                    or not 2 <= len(unit.text.strip()) <= 64):
                continue
            left, top, right, bottom = unit.box
            if not (y0 <= top <= y1 and 8 <= bottom-top <= 48
                    and column-height*1.6 <= left < column-4 and right > column+8):
                continue
            crop = image.crop(unit.box)
            runs, ink = column_runs(crop)
            candidates = [(runs[i-1][1],runs[i][0]) for i in range(1,len(runs))
                          if runs[i][0]-runs[i-1][1] >= 4
                          and abs(left+runs[i][0]-1-column) <= 3]
            if len(candidates) != 1:
                continue
            end, begin = candidates[0]
            graphic = ink.crop((0,0,end,crop.height))
            bounds = graphic.getbbox()
            if not bounds or not (6 <= bounds[2]-bounds[0] <= height*1.25
                                   and 7 <= bounds[3]-bounds[1] <= height*1.4):
                continue
            text, confidence = recognize_line(crop.crop((begin-1,0,crop.width,crop.height)))
            old, new = _label_key(unit.text), _label_key(text)
            if confidence < .88 or len(new) < 2 or not old.endswith(new):
                continue
            prefix = old[:-len(new)].strip()
            # Never strip ordinary words or quantities. The digit-like profile
            # icon additionally requires its distinct open-bottom silhouette.
            symbolic = bool(prefix and len(prefix) <= 3
                            and not re.search(r'[A-Za-z0-9]',prefix))
            digit_icon = prefix == '8' and _person_outline(graphic)
            if not prefix or symbolic or digit_icon:
                result[unit.identifier] = Unit(unit.identifier,text.strip(),
                    [(left+begin-1,top,right,bottom)],min(unit.confidence,confidence),role='ui-label')
        values = list(result.values())
        _mark_avatar_identity(image, values, column, ordered[0].box[1])
        for unit in values:
            if (unit.role == 'text' and unit.box[0] > column+60
                    and any(u.role == 'ui-label' and abs((u.box[1]+u.box[3])-(unit.box[1]+unit.box[3])) <= 6
                            for u in values)):
                result[unit.identifier] = _inset_badge_label(image, unit, recognize_line)
        # A dropdown row owns blank space to its right. Do not force a four-
        # character translation into a five-letter English detector box.
        # Stop at the first non-background pixel or neighbouring text rectangle.
        for unit in list(result.values()):
            if unit.role == 'ui-label' and abs(unit.box[0]-column) <= 3 and y0 <= unit.box[1] <= y1:
                result[unit.identifier] = _extend_blank_row(image, unit, list(result.values()))
    return [result[u.identifier] for u in refined]


def _extend_blank_row(image, unit, units):
    from .appearance import max_channel
    from PIL import ImageDraw
    left,top,right,bottom = unit.box
    end = min(image.width, right+3*(bottom-top))
    for other in units:
        x0,y0,x1,y1 = other.box
        if other.identifier != unit.identifier and x0 >= right and max(top,y0) < min(bottom,y1):
            end = min(end, x0-2)
    if end <= right:
        return unit
    crop = image.crop(unit.box).convert('RGB')
    border = Image.new('L',crop.size)
    ImageDraw.Draw(border).rectangle((0,0,crop.width-1,crop.height-1),outline=255)
    background = tuple(round(x) for x in ImageStat.Stat(crop,border).median)
    strip = image.crop((right,top,end,bottom)).convert('RGB')
    distance = max_channel(ImageChops.difference(strip,Image.new('RGB',strip.size,background)))
    free = 0
    for x in range(strip.width):
        if any(distance.getpixel((x,y)) > 8 for y in range(strip.height)):
            break
        free += 1
    if free < 3:
        return unit
    return Unit(unit.identifier,unit.text,[(left,top,right+free-1,bottom)],
                unit.confidence,role=unit.role)


def _mark_avatar_identity(image, units, column, first_menu_top):
    """Preserve a two-line identity beside an avatar, not arbitrary menu copy."""
    candidates = [u for u in units if u.role == 'text' and len(u.boxes) == 1
                  and column+8 <= u.box[0] <= column+60 and u.box[3] < first_menu_top
                  and 2 <= len(u.text) <= 64 and u.confidence >= .9]
    if len(candidates) != 2:
        return
    a,b = sorted(candidates,key=lambda u:u.box[1])
    h = max(a.box[3]-a.box[1], b.box[3]-b.box[1])
    if abs(a.box[0]-b.box[0]) > 4 or not -2 <= b.box[1]-a.box[3] <= h*.6:
        return
    x0 = max(0, min(a.box[0],b.box[0])-round(h*2.5))
    x1 = min(a.box[0],b.box[0])-3
    _, ink = column_runs(image.crop((x0,a.box[1],x1,b.box[3])))
    bounds = ink.getbbox()
    if bounds and bounds[2]-bounds[0] >= h*1.1 and bounds[3]-bounds[1] >= h*1.2:
        a.role = b.role = 'identity'


def _inset_badge_label(image, unit, recognize_line):
    """Exclude an enclosing badge rim, only after verifying its complete text."""
    from .appearance import _components
    if len(unit.boxes) != 1 or not 2 <= len(unit.text.strip()) <= 20:
        return unit
    left,top,right,bottom = unit.box
    if not (10 <= bottom-top <= 36 and (bottom-top)*1.2 <= right-left <= (bottom-top)*8):
        return unit
    box = (max(0,left-4),max(0,top-4),min(image.width,right+4),min(image.height,bottom+4))
    crop = image.crop(box)
    _,ink = column_runs(crop)
    components = _components(ink)
    def bounds(points):
        xs,ys = zip(*points)
        return min(xs),min(ys),max(xs)+1,max(ys)+1
    for rim in sorted(components,key=len,reverse=True)[:2]:
        x0,y0,x1,y1 = bounds(rim)
        if x1-x0 < (right-left)*.85 or y1-y0 < (bottom-top)*.85:
            continue
        others = [p for c in components if c is not rim for p in c
                  if x0+2 <= p[0] < x1-2 and y0+2 <= p[1] < y1-2]
        if not others:
            continue
        tx0,ty0,tx1,ty1 = bounds(others)
        if tx1-tx0 < 8 or ty1-ty0 < 5:
            continue
        # Horizontal space at glyph height, including clearance to rounded ends.
        content = (tx0-1, max(y0+2,ty0-2), tx1+1, min(y1-2,ty1+2))
        if any(content[0] <= x < content[2] and content[1] <= y < content[3] for x,y in rim):
            continue
        text, confidence = recognize_line(crop.crop(content))
        if confidence >= .9 and _label_key(text) == _label_key(unit.text):
            return Unit(unit.identifier,text.strip(),
                [(box[0]+content[0],box[1]+content[1],box[0]+content[2],box[1]+content[3])],
                min(unit.confidence,confidence),role='ui-label')
    return unit


def metadata_line(text):
    from .translation_policy import LANGUAGE_NAMES
    cleaned=re.sub('['+re.escape(DECORATIONS)+r'\s]+',' ',text).strip()
    if re.fullmatch(r'[\d.,+%kKmMbB:/()-]+',cleaned):return True
    return any(re.fullmatch(re.escape(name)+r'(?:\s+[\d.,+%kKmMbB]+)*',cleaned) for name in LANGUAGE_NAMES)


def already_target(text,target):
    if target!='zh-Hans':return False
    cjk=len(re.findall(r'[\u3400-\u9fff]',text));latin=len(re.findall(r'[A-Za-z]',text))
    # Keep Chinese UI copy (with product names) verbatim. Mixed English prose is
    # not exempted merely because it contains one Chinese term.
    return cjk>=3 and cjk>=latin
