/*
 * Copyright 2008 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 * Author: Alan Hourihane <alanh@tungstengraphics.com>
 * Author: Jakob Bornecrantz <wallbraker@gmail.com>
 *
 */

#include "xorg-server.h"
#include "xf86.h"
#include "xf86_OSproc.h"

#include "xorg_tracker.h"
#include "xorg_exa.h"

#include "dri2.h"

#include "pipe/p_state.h"
#include "pipe/p_inlines.h"

#include "util/u_rect.h"

/* Make all the #if cases in the code esier to read */
/* XXX can it be set to 1? */
#ifndef DRI2INFOREC_VERSION
#define DRI2INFOREC_VERSION 0
#endif

typedef struct {
    PixmapPtr pPixmap;
    struct pipe_texture *tex;
    struct pipe_fence_handle *fence;
} *BufferPrivatePtr;

static Bool
dri2_do_create_buffer(DrawablePtr pDraw, DRI2BufferPtr buffer, unsigned int format)
{
    struct pipe_texture *tex = NULL;
    ScreenPtr pScreen = pDraw->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    modesettingPtr ms = modesettingPTR(pScrn);
    struct exa_pixmap_priv *exa_priv;
    BufferPrivatePtr private = buffer->driverPrivate;
    PixmapPtr pPixmap;
    unsigned stride, handle;

    if (pDraw->type == DRAWABLE_PIXMAP)
	pPixmap = (PixmapPtr) pDraw;
    else
	pPixmap = (*pScreen->GetWindowPixmap)((WindowPtr) pDraw);
    exa_priv = exaGetPixmapDriverPrivate(pPixmap);

    switch (buffer->attachment) {
    default:
	if (buffer->attachment != DRI2BufferFakeFrontLeft ||
	    pDraw->type != DRAWABLE_PIXMAP) {
	    private->pPixmap = (*pScreen->CreatePixmap)(pScreen, pDraw->width,
							pDraw->height,
							pDraw->depth,
							0);
	}
	break;
    case DRI2BufferFrontLeft:
	break;
    case DRI2BufferStencil:
#if DRI2INFOREC_VERSION >= 3
    case DRI2BufferDepthStencil:
#else
    /* Works on old X servers because sanity checking is for the weak */
    case 9:
#endif
	if (exa_priv->depth_stencil_tex &&
	    !pf_is_depth_stencil(exa_priv->depth_stencil_tex->format))
	    exa_priv->depth_stencil_tex = NULL;
        /* Fall through */
    case DRI2BufferDepth:
	if (exa_priv->depth_stencil_tex)
	    pipe_texture_reference(&tex, exa_priv->depth_stencil_tex);
        else {
	    struct pipe_texture template;
	    memset(&template, 0, sizeof(template));
	    template.target = PIPE_TEXTURE_2D;
	    if (buffer->attachment == DRI2BufferDepth)
		template.format = ms->ds_depth_bits_last ?
		    PIPE_FORMAT_X8Z24_UNORM : PIPE_FORMAT_Z24X8_UNORM;
	    else
		template.format = ms->ds_depth_bits_last ?
		    PIPE_FORMAT_S8Z24_UNORM : PIPE_FORMAT_Z24S8_UNORM;
	    pf_get_block(template.format, &template.block);
	    template.width[0] = pDraw->width;
	    template.height[0] = pDraw->height;
	    template.depth[0] = 1;
	    template.last_level = 0;
	    template.tex_usage = PIPE_TEXTURE_USAGE_DEPTH_STENCIL |
		PIPE_TEXTURE_USAGE_DISPLAY_TARGET;
	    tex = ms->screen->texture_create(ms->screen, &template);
	    pipe_texture_reference(&exa_priv->depth_stencil_tex, tex);
	}
	break;
    }

    if (!private->pPixmap) {
	private->pPixmap = pPixmap;
	pPixmap->refcnt++;
    }

    if (!tex) {
	/* First call to make sure we have a pixmap private */
	exaMoveInPixmap(private->pPixmap);
	xorg_exa_set_shared_usage(private->pPixmap);
	pScreen->ModifyPixmapHeader(private->pPixmap, 0, 0, 0, 0, 0, NULL);
	/* Second call to make sure texture has valid contents */
	exaMoveInPixmap(private->pPixmap);
	tex = xorg_exa_get_texture(private->pPixmap);
    }

    if (!tex)
	FatalError("NO TEXTURE IN DRI2\n");

    ms->api->shared_handle_from_texture(ms->api, ms->screen, tex, &stride, &handle);

    buffer->name = handle;
    buffer->pitch = stride;
    buffer->cpp = 4;
    buffer->driverPrivate = private;
    buffer->flags = 0; /* not tiled */
#if DRI2INFOREC_VERSION == 2
    ((DRI2Buffer2Ptr)buffer)->format = 0;
#elif DRI2INFOREC_VERSION >= 3
    buffer->format = 0;
#endif
    private->tex = tex;

    return TRUE;
}

static void
dri2_do_destroy_buffer(DrawablePtr pDraw, DRI2BufferPtr buffer)
{
    ScreenPtr pScreen = pDraw->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    modesettingPtr ms = modesettingPTR(pScrn);
    BufferPrivatePtr private = buffer->driverPrivate;
    struct exa_pixmap_priv *exa_priv = exaGetPixmapDriverPrivate(private->pPixmap);

    pipe_texture_reference(&private->tex, NULL);
    ms->screen->fence_reference(ms->screen, &private->fence, NULL);
    pipe_texture_reference(&exa_priv->depth_stencil_tex, NULL);
    (*pScreen->DestroyPixmap)(private->pPixmap);
}

#if DRI2INFOREC_VERSION >= 2

static DRI2Buffer2Ptr
dri2_create_buffer(DrawablePtr pDraw, unsigned int attachment, unsigned int format)
{
    DRI2Buffer2Ptr buffer;
    BufferPrivatePtr private;

    buffer = xcalloc(1, sizeof *buffer);
    if (!buffer)
	return NULL;

    private = xcalloc(1, sizeof *private);
    if (!private) {
	goto fail;
    }

    buffer->attachment = attachment;
    buffer->driverPrivate = private;

    /* So far it is safe to downcast a DRI2Buffer2Ptr to DRI2BufferPtr */
    if (dri2_do_create_buffer(pDraw, (DRI2BufferPtr)buffer, format))
	return buffer;

    xfree(private);
fail:
    xfree(buffer);
    return NULL;
}

static void
dri2_destroy_buffer(DrawablePtr pDraw, DRI2Buffer2Ptr buffer)
{
    /* So far it is safe to downcast a DRI2Buffer2Ptr to DRI2BufferPtr */
    dri2_do_destroy_buffer(pDraw, (DRI2BufferPtr)buffer);

    xfree(buffer->driverPrivate);
    xfree(buffer);
}

#else /* DRI2INFOREC_VERSION < 2 */

static DRI2BufferPtr
dri2_create_buffers(DrawablePtr pDraw, unsigned int *attachments, int count)
{
    BufferPrivatePtr privates;
    DRI2BufferPtr buffers;
    int i;

    buffers = xcalloc(count, sizeof *buffers);
    if (!buffers)
	goto fail_buffers;

    privates = xcalloc(count, sizeof *privates);
    if (!privates)
	goto fail_privates;

    for (i = 0; i < count; i++) {
	buffers[i].attachment = attachments[i];
	buffers[i].driverPrivate = &privates[i];

	if (!dri2_do_create_buffer(pDraw, &buffers[i], 0))
	    goto fail;
    }

    return buffers;

fail:
    xfree(privates);
fail_privates:
    xfree(buffers);
fail_buffers:
    return NULL;
}

static void
dri2_destroy_buffers(DrawablePtr pDraw, DRI2BufferPtr buffers, int count)
{
    int i;

    for (i = 0; i < count; i++) {
	dri2_do_destroy_buffer(pDraw, &buffers[i]);
    }

    if (buffers) {
	xfree(buffers[0].driverPrivate);
	xfree(buffers);
    }
}

#endif /* DRI2INFOREC_VERSION >= 2 */

static void
dri2_copy_region(DrawablePtr pDraw, RegionPtr pRegion,
                 DRI2BufferPtr pDestBuffer, DRI2BufferPtr pSrcBuffer)
{
    ScreenPtr pScreen = pDraw->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    modesettingPtr ms = modesettingPTR(pScrn);
    BufferPrivatePtr dst_priv = pDestBuffer->driverPrivate;
    BufferPrivatePtr src_priv = pSrcBuffer->driverPrivate;
    DrawablePtr src_draw;
    DrawablePtr dst_draw;
    GCPtr gc;
    RegionPtr copy_clip;
    Bool save_accel;

    /*
     * In driCreateBuffers we dewrap windows into the
     * backing pixmaps in order to get to the texture.
     * We need to use the real drawable in CopyArea
     * so that cliprects and offsets are correct.
     */
    src_draw = (pSrcBuffer->attachment == DRI2BufferFrontLeft) ? pDraw :
       &src_priv->pPixmap->drawable;
    dst_draw = (pDestBuffer->attachment == DRI2BufferFrontLeft) ? pDraw :
       &dst_priv->pPixmap->drawable;

    /*
     * The clients implements glXWaitX with a copy front to fake and then
     * waiting on the server to signal its completion of it. While
     * glXWaitGL is a client side flush and a copy from fake to front.
     * This is how it is done in the DRI2 protocol, how ever depending
     * which type of drawables the server does things a bit differently
     * then what the protocol says as the fake and front are the same.
     *
     * for pixmaps glXWaitX is a server flush.
     * for pixmaps glXWaitGL is a client flush.
     * for windows glXWaitX is a copy from front to fake then a server flush.
     * for windows glXWaitGL is a client flush then a copy from fake to front.
     *
     * XXX in the windows case this code always flushes but that isn't a
     * must in the glXWaitGL case but we don't know if this is a glXWaitGL
     * or a glFlush/glFinish call.
     */
    if (dst_priv->pPixmap == src_priv->pPixmap) {
	/* pixmap glXWaitX */
	if (pSrcBuffer->attachment == DRI2BufferFrontLeft &&
	    pDestBuffer->attachment == DRI2BufferFakeFrontLeft) {
	    ms->ctx->flush(ms->ctx, PIPE_FLUSH_SWAPBUFFERS, NULL);
	    return;
	}
	/* pixmap glXWaitGL */
	if (pDestBuffer->attachment == DRI2BufferFrontLeft &&
	    pSrcBuffer->attachment == DRI2BufferFakeFrontLeft) {
	    return;
	} else {
	    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		"copying between the same pixmap\n");
	}
    }

    gc = GetScratchGC(pDraw->depth, pScreen);
    copy_clip = REGION_CREATE(pScreen, NULL, 0);
    REGION_COPY(pScreen, copy_clip, pRegion);
    (*gc->funcs->ChangeClip) (gc, CT_REGION, copy_clip, 0);
    ValidateGC(dst_draw, gc);

    /* If this is a full buffer swap, throttle on the previous one */
    if (dst_priv->fence && REGION_NUM_RECTS(pRegion) == 1) {
	BoxPtr extents = REGION_EXTENTS(pScreen, pRegion);

	if (extents->x1 == 0 && extents->y1 == 0 &&
	    extents->x2 == pDraw->width && extents->y2 == pDraw->height) {
	    ms->screen->fence_finish(ms->screen, dst_priv->fence, 0);
	    ms->screen->fence_reference(ms->screen, &dst_priv->fence, NULL);
	}
    }

    /* Try to make sure the blit will be accelerated */
    save_accel = ms->exa->accel;
    ms->exa->accel = TRUE;

    /* In case it won't be though, make sure the GPU copy contents of the
     * source pixmap will be used for the software fallback - presumably the
     * client modified them before calling in here.
     */
    exaMoveInPixmap(src_priv->pPixmap);
    DamageRegionAppend(src_draw, pRegion);
    DamageRegionProcessPending(src_draw);

    (*gc->ops->CopyArea)(src_draw, dst_draw, gc,
			 0, 0, pDraw->width, pDraw->height, 0, 0);
    ms->exa->accel = save_accel;

    FreeScratchGC(gc);

    ms->ctx->flush(ms->ctx, PIPE_FLUSH_SWAPBUFFERS,
		   pDestBuffer->attachment == DRI2BufferFrontLeft ?
		   &dst_priv->fence : NULL);
}

Bool
xorg_dri2_init(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    modesettingPtr ms = modesettingPTR(pScrn);
    DRI2InfoRec dri2info;

#if DRI2INFOREC_VERSION >= 2
    dri2info.version = DRI2INFOREC_VERSION;
#else
    dri2info.version = 1;
#endif
    dri2info.fd = ms->fd;

    dri2info.driverName = pScrn->driverName;
    dri2info.deviceName = "/dev/dri/card0"; /* FIXME */

#if DRI2INFOREC_VERSION >= 2
    dri2info.CreateBuffer = dri2_create_buffer;
    dri2info.DestroyBuffer = dri2_destroy_buffer;
#else
    dri2info.CreateBuffers = dri2_create_buffers;
    dri2info.DestroyBuffers = dri2_destroy_buffers;
#endif
    dri2info.CopyRegion = dri2_copy_region;
    dri2info.Wait = NULL;

    ms->d_depth_bits_last =
	 ms->screen->is_format_supported(ms->screen, PIPE_FORMAT_X8Z24_UNORM,
					 PIPE_TEXTURE_2D,
					 PIPE_TEXTURE_USAGE_DEPTH_STENCIL, 0);
    ms->ds_depth_bits_last =
	 ms->screen->is_format_supported(ms->screen, PIPE_FORMAT_S8Z24_UNORM,
					 PIPE_TEXTURE_2D,
					 PIPE_TEXTURE_USAGE_DEPTH_STENCIL, 0);

    return DRI2ScreenInit(pScreen, &dri2info);
}

void
xorg_dri2_close(ScreenPtr pScreen)
{
    DRI2CloseScreen(pScreen);
}

/* vim: set sw=4 ts=8 sts=4: */