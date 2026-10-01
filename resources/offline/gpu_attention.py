"""Bound attention workspace without changing image size, keys or learned weights.

Only query rows are chunked: every query still attends to ALL keys, with the
same decomposed relative position term. This is inference-only, not windowed or
approximate attention. Upstream files and checkpoints are left untouched.
"""
from __future__ import annotations
from types import MethodType


def chunked_attention(self, x):
    import torch
    from hi_sam.modeling.image_encoder import get_rel_pos
    if self.training or torch.is_grad_enabled():
        return self._vislate_original_forward(x)
    batch, height, width, channels = x.shape
    qkv = self.qkv(x).reshape(batch,height*width,3,self.num_heads,-1).permute(2,0,3,1,4)
    query,key,value = qkv.reshape(3,batch*self.num_heads,height*width,-1).unbind(0)
    heads = batch*self.num_heads
    relative_h = get_rel_pos(height,height,self.rel_pos_h) if self.use_rel_pos else None
    relative_w = get_rel_pos(width,width,self.rel_pos_w) if self.use_rel_pos else None
    output = torch.empty_like(query)
    for first in range(0,height,self._vislate_query_rows):
        last=min(height,first+self._vislate_query_rows)
        q = query[:,first*width:last*width]
        attention = (q*self.scale) @ key.transpose(-2,-1)
        if self.use_rel_pos:
            grid=q.reshape(heads,last-first,width,-1)
            rh=torch.einsum('bhwc,hkc->bhwk',grid,relative_h[first:last])
            rw=torch.einsum('bhwc,wkc->bhwk',grid,relative_w)
            attention=(attention.view(heads,last-first,width,height,width)
                       +rh[:,:,:,:,None]+rw[:,:,:,None,:]).reshape(heads,(last-first)*width,height*width)
        attention=attention.softmax(dim=-1)
        output[:,first*width:last*width]=attention @ value
    output=output.view(batch,self.num_heads,height,width,-1).permute(0,2,3,1,4).reshape(batch,height,width,-1)
    return self.proj(output)


def bound_attention_workspace(model, query_rows: int):
    if query_rows not in (2,4,8,16):
        raise ValueError('unsupported_attention_rows')
    changed=0
    for block in model.image_encoder.blocks:
        if block.window_size != 0: continue
        attention=block.attn
        if not hasattr(attention,'_vislate_original_forward'):
            attention._vislate_original_forward=attention.forward
        attention._vislate_query_rows=query_rows
        attention.forward=MethodType(chunked_attention,attention)
        changed+=1
    return changed
