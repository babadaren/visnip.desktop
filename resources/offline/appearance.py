"""Estimate visible text appearance, independently of the erasure-mask margin.

No font files are shipped. Font matching is an estimate against available system
fonts, not a claim to recover the exact original font from any image.
"""
from __future__ import annotations
from functools import lru_cache
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont, ImageStat


def max_channel(image):
    red, green, blue = image.convert('RGB').split()
    return ImageChops.lighter(ImageChops.lighter(red, green), blue)


def quantile(histogram, fraction):
    limit = sum(histogram) * fraction
    count = 0
    for index, value in enumerate(histogram):
        count += value
        if count >= limit:
            return index
    return 255


def appearance(crop, predicted_stroke):
    """Separate foreground pixels, visible ink, and the larger cleanup mask."""
    rgb = crop.convert('RGB')
    stroke = predicted_stroke.convert('L').point(lambda v: 255 if v else 0)
    border = Image.new('L', crop.size)
    ImageDraw.Draw(border).rectangle((0, 0, crop.width-1, crop.height-1), outline=255)
    border = ImageChops.subtract(border, stroke)
    if not border.getbbox():
        border = Image.new('L', crop.size, 255)
    background = tuple(round(v) for v in ImageStat.Stat(rgb, border).median)
    distance = max_channel(ImageChops.difference(rgb, Image.new('RGB', crop.size, background)))
    histogram = distance.histogram(mask=stroke)
    peak = quantile(histogram, .97)
    if peak < 5:
        return None
    # Select high-coverage source pixels, not a median over anti-aliased edges
    # mixed with the background. Works for light, dark and coloured UI text.
    # Max-channel distance over-selects coloured ClearType fringes of black
    # text. Luminance contrast selects the high-coverage interior instead.
    colour_distance = ImageChops.difference(rgb, Image.new('RGB', crop.size, background)).convert('L')
    colour_histogram = colour_distance.histogram(mask=stroke)
    core_limit = max(4, quantile(colour_histogram, .99))
    color_mask = ImageChops.multiply(stroke, colour_distance.point(lambda v: 255 if v >= core_limit else 0))
    if not color_mask.getbbox():
        return None
    foreground = tuple(round(v) for v in ImageStat.Stat(rgb, color_mask).median)
    foreground_distance = max_channel(ImageChops.difference(rgb, Image.new('RGB', crop.size, foreground)))
    visible = distance.point(lambda v: 255 if v >= max(4, round(peak * .22)) else 0)
    # Only repair connected, colour-consistent antialias fringes (at most 2 px).
    # Distant artwork does not become erasable merely because its colour matches.
    fringe = ImageChops.multiply(stroke.filter(ImageFilter.MaxFilter(5)), visible)
    fringe = ImageChops.multiply(fringe, foreground_distance.point(lambda v: 255 if v <= peak * .8 else 0))
    erase = ImageChops.lighter(stroke, fringe)
    ink = ImageChops.multiply(erase, visible)
    obstacles = ImageChops.subtract(visible, erase)
    foreground_luma=Image.new('RGB',(1,1),foreground).convert('L').getpixel((0,0))
    background_luma=Image.new('RGB',(1,1),background).convert('L').getpixel((0,0))
    contrast=max(1,abs(foreground_luma-background_luma))
    coverage=rgb.convert('L').point(lambda v:min(255,round(255*abs(v-background_luma)/contrast)))
    font_ink=ImageChops.multiply(coverage,erase)
    return {'background': background, 'foreground': foreground, 'ink': ink, 'font_ink':font_ink,
            'erase': erase, 'obstacles': obstacles, 'contrast': peak}


@lru_cache(maxsize=384)
def font_at(filename, size):
    return ImageFont.truetype(filename, size)


def font_size_for_height(filename, text, height):
    lo, hi = 6, min(192, max(16, height * 3))
    while lo < hi:
        middle = (lo + hi) // 2
        box = font_at(filename, middle).getbbox(text)
        if box[3] - box[1] < height:
            lo = middle + 1
        else:
            hi = middle
    return lo


_source_fit_cache = {}


def estimate_source(text, ink, fonts):
    import hashlib
    identity=hashlib.sha256(text.encode('utf-8')+b'\0'+str(tuple(fonts)).encode('utf-8')+str(ink.size).encode('ascii')+ink.tobytes()).digest()
    if identity in _source_fit_cache:return _source_fit_cache[identity]
    result=_estimate_source_uncached(text,ink,fonts)
    if len(_source_fit_cache)>=128:_source_fit_cache.pop(next(iter(_source_fit_cache)))
    _source_fit_cache[identity]=result
    return result


def _estimate_source_uncached(text, ink, fonts):
    bounds = ink.getbbox()
    if not bounds:
        return None
    observed = ink.crop(bounds)
    width, height = observed.size
    candidates = []
    for filename in fonts:
        centre = font_size_for_height(filename, text, height)
        for size in range(max(6, centre - 2), centre + 3):
            font = font_at(filename, size)
            b = font.getbbox(text)
            fw, fh = b[2]-b[0], b[3]-b[1]
            if not fw or not fh:
                continue
            dimensions = abs(fw-width)/max(width, 1) + 2*abs(fh-height)/max(height,1)
            candidates.append((dimensions, filename, size, b))
    best = None
    soft=bool(sum(observed.histogram()[1:255]))
    for dimensions, filename, size, b in sorted(candidates)[:16]:
        rendered = Image.new('L', (b[2]-b[0], b[3]-b[1]))
        ImageDraw.Draw(rendered).text((-b[0], -b[1]), text, font=font_at(filename,size), fill=255)
        rendered = rendered.resize(observed.size)
        if not soft:rendered=rendered.point(lambda v:255 if v>50 else 0)
        union = ImageChops.lighter(rendered, observed)
        mismatch = ImageChops.difference(rendered, observed)
        if soft:
            actual_mass=sum(i*n for i,n in enumerate(observed.histogram()))
            expected_mass=sum(i*n for i,n in enumerate(rendered.histogram()))
            shape=sum(i*n for i,n in enumerate(mismatch.histogram()))/max(1,sum(i*n for i,n in enumerate(union.histogram())))
            density=abs(actual_mass-expected_mass)/max(1,actual_mass)
            score=dimensions+.45*shape+1.2*density
        else:
            shape = sum(mismatch.histogram()[1:]) / max(1, sum(union.histogram()[1:]))
            score = dimensions + .3 * shape
        if best is None or score < best[0]:
            best = (score, filename, size, height)
    return best


def target_family(source, fonts):
    if source in fonts:
        return source
    name = Path(source).name.lower()
    serif = any(s in name for s in ('times', 'simsun', 'serif'))
    bold = any(s in name for s in ('bold', 'bd.', 'bd.tt', 'arialbd', 'segoeuib'))
    preferences = ('simsun', 'simhei', 'msyh') if serif else (('msyhbd', 'msyh', 'simhei') if bold else ('msyh.', 'msyh', 'simhei'))
    for marker in preferences:
        for filename in fonts:
            if marker in Path(filename).name.lower():
                return filename
    linux_family = 'notoserifcjk' if serif else 'notosanscjk'
    linux_weight = 'bold' if bold else 'regular'
    for filename in fonts:
        name = Path(filename).name.lower()
        if linux_family in name and linux_weight in name:
            return filename
    return fonts[0]


def _components(mask):
    """8-connected components of a small, binary text crop (no model dependency)."""
    width, height = mask.size
    remaining = bytearray(mask.tobytes())
    groups = []
    for first in range(len(remaining)):
        if not remaining[first]:
            continue
        remaining[first] = 0
        pending, pixels = [first], []
        while pending:
            position = pending.pop()
            x, y = position % width, position // width
            pixels.append((x, y))
            for ny in range(max(0, y-1), min(height, y+2)):
                for nx in range(max(0, x-1), min(width, x+2)):
                    neighbor = ny*width + nx
                    if remaining[neighbor]:
                        remaining[neighbor] = 0
                        pending.append(neighbor)
        groups.append(pixels)
    return groups


def complete_readable_strokes(crop, stroke):
    """Complete visible glyph edges on flat backgrounds without matching fonts.

    A substantial component must be anchored in the text prediction. Independent
    graphics remain obstacles. Only small, nearby detached glyph marks receive
    the relaxed rule; the compositor still enforces explicit protected regions.
    """
    rgb = crop.convert('RGB')
    predicted = stroke.convert('L').point(lambda v: 255 if v else 0)
    if not predicted.getbbox() or min(crop.size) < 3:
        return predicted
    border = Image.new('L', crop.size)
    ImageDraw.Draw(border).rectangle((0, 0, crop.width-1, crop.height-1), outline=255)
    border = ImageChops.subtract(border, predicted)
    if not border.getbbox():
        return predicted
    background = tuple(round(v) for v in ImageStat.Stat(rgb, border).median)
    delta = max_channel(ImageChops.difference(rgb, Image.new('RGB', crop.size, background)))
    hist = delta.histogram(mask=border)
    if sum(hist[:9]) < .94 * sum(hist):
        return predicted  # Gradients and texture still require learned repair.
    visible = delta.point(lambda v: 255 if v > 4 else 0)
    completed = ImageChops.multiply(predicted, visible)
    pixels = completed.load()
    independent = []
    for component in _components(visible):
        overlap = sum(bool(predicted.getpixel(p)) for p in component)
        xs, ys = zip(*component)
        width, height = max(xs)-min(xs)+1, max(ys)-min(ys)+1
        rule = width > crop.height * 3 and height <= 2
        if overlap >= max(2, .35 * len(component)) and not rule:
            for p in component:
                pixels[p] = 255
        else:
            independent.append(component)
    # Detached i/j dots may sit four pixels above a 13px screenshot font.
    # Font-template equality is not needed for this small, local completion.
    glyph_bounds = completed.getbbox()
    glyph_height = glyph_bounds[3]-glyph_bounds[1] if glyph_bounds else crop.height
    reach = max(4, round(glyph_height*.30))
    mark_width = max(3, round(glyph_height*.23))
    mark_area = max(9, mark_width*mark_width)
    nearby = completed.filter(ImageFilter.MaxFilter(2*reach+1))
    for component in independent:
        xs, ys = zip(*component)
        if (len(component) <= mark_area and max(xs)-min(xs) < mark_width and max(ys)-min(ys) < mark_width
                and any(nearby.getpixel(p) for p in component)):
            for p in component:
                pixels[p] = 255
    return ImageChops.lighter(predicted, completed)


def recover_source_marks(crop, stroke, source_text, fonts):
    """Recover detached glyph marks only when a fitted source glyph supports them.

    Hi-SAM can omit the dot of i/j in Windows text. A blanket mask dilation or
    ignoring small obstacles would also erase icons. Instead require an isolated,
    small component at the position of a detached mark in the recognized source,
    near existing text, with the same foreground colour. No protected-area rule
    or final artwork-collision check is weakened here.
    """
    style = appearance(crop, stroke)
    if style is None or not style['obstacles'].getbbox():
        return stroke
    estimate = estimate_source(source_text, style['ink'], fonts)
    if estimate is None or estimate[0] > .5:
        return stroke
    _, filename, size, _ = estimate
    font = font_at(filename, size)
    box = font.getbbox(source_text)
    bounds = style['ink'].getbbox()
    if not bounds or box[2] <= box[0] or box[3] <= box[1]:
        return stroke
    rendered = Image.new('L', (box[2]-box[0], box[3]-box[1]))
    ImageDraw.Draw(rendered).text((-box[0], -box[1]), source_text, font=font, fill=255)
    rendered = rendered.resize((bounds[2]-bounds[0], bounds[3]-bounds[1]))
    expected = Image.new('L', crop.size)
    expected.paste(rendered.point(lambda v: 255 if v >= 50 else 0), bounds[:2])
    tiny = Image.new('L', crop.size)
    tiny_pixels = tiny.load()
    mark_limit = max(2, round(size*.24))
    for component in _components(expected):
        xs, ys = zip(*component)
        if (len(component) <= max(9, size)
                and max(xs)-min(xs)+1 <= mark_limit
                and max(ys)-min(ys)+1 <= mark_limit):
            for x, y in component:
                tiny_pixels[x, y] = 255
    if not tiny.getbbox():
        return stroke
    tolerance = tiny.filter(ImageFilter.MaxFilter(5))
    nearby = stroke.convert('L').filter(ImageFilter.MaxFilter(2*max(3, round(size*.25))+1))
    rgb = crop.convert('RGB')
    recovered = Image.new('L', crop.size)
    recovered_pixels = recovered.load()
    # A dot may be partially present in the predicted mask. Classify its missing
    # fragment against a complete rendered dot, rather than discarding the whole
    # mark merely because one of its pixels was already detected.
    for component in _components(style['obstacles']):
        if len(component) > max(9, size):
            continue
        xs, ys = zip(*component)
        if max(xs)-min(xs)+1 > mark_limit or max(ys)-min(ys)+1 > mark_limit:
            continue
        if any(stroke.getpixel((x,y)) for x,y in component):
            continue
        if sum(bool(tolerance.getpixel((x,y))) for x,y in component) < .75*len(component):
            continue
        if not any(nearby.getpixel((x,y)) for x,y in component):
            continue
        # The missing part can consist entirely of antialiased pixels. Their
        # colour is a mixture of foreground/background, not the solid ink colour.
        # Test that colour line instead of rejecting a valid grey edge of black
        # text (or accepting a differently coloured icon).
        direction = tuple(f-b for f,b in zip(style['foreground'], style['background']))
        magnitude = sum(v*v for v in direction)
        # ClearType distributes coverage between RGB subpixels: even a black
        # dot has red/cyan edge pixels. Coverage is conserved over the complete
        # small mark, so compare its integrated colour, not each edge pixel.
        pixels = [rgb.getpixel((x,y)) for x,y in component]
        mean = tuple(sum(p[channel] for p in pixels)/len(pixels) for channel in range(3))
        delta = tuple(v-b for v,b in zip(mean,style['background']))
        alpha = sum(v*d for v,d in zip(delta,direction)) / max(1,magnitude)
        residual = max(abs(v-alpha*d) for v,d in zip(delta,direction))
        if not (.12 <= alpha <= 1.05) or residual > max(8,style['contrast']*.05):
            continue
        for x,y in component:
            recovered_pixels[x,y] = 255
    if not recovered.getbbox():
        return stroke
    # Include only the mark's own visible edge, not its entire bounding box.
    edge = max_channel(ImageChops.difference(rgb, Image.new('RGB', crop.size, style['background'])))
    recovered = ImageChops.multiply(recovered.filter(ImageFilter.MaxFilter(3)),
                                   edge.point(lambda v: 255 if v >= 4 else 0))
    return ImageChops.lighter(stroke.convert('L'), recovered)
