"""Choose a confidently uniform UI background path before invoking heavy models.

One quality contract, no user-facing basic mode. Only confidently uniform OCR
regions qualify; uncertain regions still use Hi-SAM. Editing remains bounded by
compose's stroke/target masks and protected-region checks.
"""
from __future__ import annotations
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageStat
from .appearance import appearance, estimate_source
from .layout import Unit


def uniform_colour(image: Image.Image, exclude: Image.Image | None = None):
    rgb=image.convert('RGB'); ring=Image.new('L',image.size)
    ImageDraw.Draw(ring).rectangle((0,0,image.width-1,image.height-1),outline=255,width=2)
    if exclude is not None: ring=ImageChops.subtract(ring,exclude)
    if not ring.getbbox():return None
    stat=ImageStat.Stat(rgb,ring)
    if any(high-low>4 for low,high in stat.extrema):return None
    return tuple(round(v) for v in stat.median)


def simple_text_mask(source: Image.Image, unit: Unit, fonts: list[str], diagnostics=None):
    """Return local masks only when pixels, source glyph metrics and OCR agree."""
    from .appearance import max_channel
    import cv2
    import numpy as np
    def reject(reason):
        if diagnostics is not None:diagnostics.append({'id':unit.identifier,'reason':reason})
        return None
    if unit.confidence<.85 or len(unit.boxes)!=len(unit.source_lines):return reject('ocr_confidence')
    regions=[]
    for text,box in zip(unit.source_lines,unit.boxes):
        crop=source.crop(box).convert('RGB');background=uniform_colour(crop)
        if background is None:
            l,t,r,b=box
            context=source.crop((max(0,l-3),max(0,t-3),min(source.width,r+3),min(source.height,b+3)))
            background=uniform_colour(context)
        if background is None or crop.height>180 or crop.width<5:return reject('background')
        distance=max_channel(ImageChops.difference(crop,Image.new('RGB',crop.size,background)))
        visible=distance.point(lambda p:255 if p>5 else 0)
        count=sum(visible.histogram()[1:]);fraction=count/max(1,crop.width*crop.height)
        if not .012<=fraction<=.55:return reject('ink_coverage')
        data=np.asarray(visible)
        n,labels,stats,_=cv2.connectedComponentsWithStats(data,8)
        if n<2:return reject('no_components')
        # Reject long rules, filled icons, and large merged artwork.
        for x,y,w,h,area in stats[1:]:
            if (w>crop.width*.6 and h<3) or area>crop.width*crop.height*.22:return reject('artwork')
            # ClearType often connects multiple letters/words. Width alone is
            # not artwork evidence; the source-glyph and colour checks below
            # decide. Solid blocks and independent rules are still rejected.
            if h>crop.height*.97 and w>crop.height:return reject('component_shape')
        style=appearance(crop,visible)
        if style is None:return reject('appearance')
        fit=estimate_source(text,style['font_ink'],fonts)
        if fit is None or fit[0]>.52:return reject('source_fit')
        # A flat background alone is insufficient: match the OCR source glyphs
        # to the foreground so an adjacent monochrome icon is not treated as ink.
        from .appearance import font_at
        observed=style['ink'].getbbox();f=font_at(fit[1],fit[2]);glyph_box=f.getbbox(text)
        expected=Image.new('L',(glyph_box[2]-glyph_box[0],glyph_box[3]-glyph_box[1]))
        ImageDraw.Draw(expected).text((-glyph_box[0],-glyph_box[1]),text,font=f,fill=255)
        expected=expected.resize((observed[2]-observed[0],observed[3]-observed[1])).point(lambda v:255 if v>40 else 0)
        actual=style['font_ink'].crop(observed).point(lambda v:255 if v>110 else 0)
        union=ImageChops.lighter(expected,actual);mismatch=ImageChops.difference(expected,actual)
        if sum(mismatch.histogram()[1:])/max(1,sum(union.histogram()[1:]))>.58:return reject('glyph_shape')
        # Most visible pixels must lie on a single foreground/background colour
        # family. Small subpixel fringes are allowed, colourful artwork is not.
        rgb=np.asarray(crop).astype('float32');bg=np.asarray(background);fg=np.asarray(style['foreground'])
        vector=fg-bg;den=float(vector@vector)
        if den<100:return reject('contrast')
        alpha=np.clip(((rgb-bg)@vector)/den,0,1)
        core=(data>0)&(alpha>.45)
        if core.sum()<3:return reject('colour')
        # ClearType intentionally colours individual subpixels. Integrate each
        # connected glyph, rather than rejecting valid black text as colourful.
        # A separate colourful icon remains a separate component and is rejected.
        for label in range(1,n):
            positions=labels==label
            if int(positions.sum())<6:continue
            mean=rgb[positions].mean(axis=0)
            coverage=float(np.clip(((mean-bg)@vector)/den,0,1))
            error=float(np.max(np.abs(mean-(bg+coverage*vector))))
            if error>max(16,float(np.max(np.abs(vector)))*.15):return reject('colour')
        # No rectangular background cover. Include actual anti-alias pixels only.
        erased=distance.point(lambda p:255 if p>2 else 0)
        regions.append((box,erased))
    return regions


class AdaptiveVision:
    def __init__(self, config, assets, threads):
        self.config=config;self.assets=assets;self.threads=threads
        self.hisam=None;self.lama=None;self.last_stats={}

    def segment(self,image,units):
        output=Image.new('L',image.size);uncertain=[];flat=0;decisions=[]
        for unit in units:
            masks=simple_text_mask(image,unit,self.config['fonts'],decisions)
            if masks is None:uncertain.append(unit);continue
            for box,mask in masks:
                output.paste(ImageChops.lighter(output.crop(box),mask),box[:2])
            flat+=1
        model_ms=0
        if uncertain:
            import time
            begin=time.perf_counter()
            if self.hisam is None:
                from .adapters import HiSamAdapter
                self.hisam=HiSamAdapter(self.config['hisam_source_directory'],self.assets['hisam'],self.config.get('device','cpu'),self.threads,
                                       precision=self.config.get('vision_precision','fp32'),
                                       attention_rows=int(self.config.get('vision_attention_rows',0)))
            predicted=self.hisam.segment(image,uncertain)
            for unit in uncertain:
                for box in unit.boxes:output.paste(ImageChops.lighter(output.crop(box),predicted.crop(box)),box[:2])
            model_ms=round((time.perf_counter()-begin)*1000)
        self.last_stats={'simple_regions':flat,'heavy_regions':len(uncertain),'heavy_ms':model_ms,
                         'vision_device':str(self.hisam.device) if uncertain else 'not_used',
                         'vision_precision':self.config.get('vision_precision','fp32'),
                         'tiles':getattr(self.hisam,'last_stats',{}).get('tiles',0) if uncertain else 0,'fallback_reasons':decisions}
        return output

    def repair(self,image,mask):
        background=uniform_colour(image,mask)
        if background is not None:
            # Require all remaining pixels, not just the border, to be uniform.
            outside=ImageChops.invert(mask.point(lambda v:255 if v else 0))
            stat=ImageStat.Stat(image.convert('RGB'),outside)
            if outside.getbbox() and all(high-low<=4 for low,high in stat.extrema):
                return Image.new('RGB',image.size,background)
        if self.lama is None:
            from .adapters import LamaJitAdapter
            self.lama=LamaJitAdapter(self.assets['lama_jit'],self.config.get('device','cpu'))
        return self.lama.repair(image,mask)

    def repair_region(self,source,box,stroke):
        crop=source.crop(box)
        background=uniform_colour(crop,stroke)
        outer=ImageChops.invert(stroke.point(lambda v:255 if v else 0))
        if background is not None:
            outside=ImageChops.invert(stroke.point(lambda v:255 if v else 0))
            if outside.getbbox() and all(b-a<=4 for a,b in ImageStat.Stat(crop.convert('RGB'),outside).extrema):
                return Image.new('RGB',crop.size,background)
            # Tiny near-white antialias remnants next to erased text are not a
            # textured background. Never apply this exception to distant artwork.
            from .appearance import max_channel
            delta=max_channel(ImageChops.difference(crop.convert('RGB'),Image.new('RGB',crop.size,background)))
            remaining=ImageChops.multiply(delta,outside)
            residue=remaining.point(lambda v:255 if v>4 else 0)
            distant=ImageChops.subtract(residue,stroke.filter(ImageFilter.MaxFilter(5)))
            if remaining.getextrema()[1]<=10 and not distant.getbbox():
                return Image.new('RGB',crop.size,background)
        if self.lama is None:
            from .adapters import LamaJitAdapter
            self.lama=LamaJitAdapter(self.assets['lama_jit'],self.config.get('device','cpu'))
        self.last_stats.setdefault('repair_fallbacks',[]).append({'box':list(box),'border_uniform':background is not None,
            'outside_range':list(ImageStat.Stat(crop.convert('RGB'),outer).extrema) if outer.getbbox() else []})
        self.last_stats['lama_repairs']=self.last_stats.get('lama_repairs',0)+1
        return self.lama.repair_region(source,box,stroke)
