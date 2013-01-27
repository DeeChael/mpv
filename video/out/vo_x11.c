/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "config.h"
#include "vo.h"
#include "aspect.h"
#include "video/csputils.h"
#include "video/mp_image.h"
#include "video/vfcap.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <errno.h>

#include "x11_common.h"

#ifdef HAVE_SHM
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#endif

#include "sub/sub.h"
#include "sub/draw_bmp.h"

#include "video/sws_utils.h"
#define MODE_RGB  0x1
#define MODE_BGR  0x2

#include "core/mp_msg.h"
#include "osdep/timer.h"

extern int sws_flags;

struct priv {
    struct vo *vo;

    struct mp_image *original_image;

    /* local data */
    unsigned char *ImageData[2];
    //! original unaligned pointer for free
    unsigned char *ImageDataOrig[2];

    /* X11 related variables */
    XImage *myximage[2];
    int depth, bpp;
    XWindowAttributes attribs;

    int int_pause;

    int Flip_Flag;
    int zoomFlag;

    uint32_t image_width;
    uint32_t image_height;
    uint32_t in_format;
    uint32_t out_format;
    int out_offset;
    int srcW;
    int srcH;

    int old_vo_dwidth;
    int old_vo_dheight;

    struct SwsContext *swsContext;
    int dst_width;

    XVisualInfo vinfo;
    int ximage_depth;

    int firstTime;

    int current_buf;
    int visible_buf;
    int num_buffers;
    int total_buffers;

    int Shmem_Flag;
#ifdef HAVE_SHM
    int Shm_Warned_Slow;

    XShmSegmentInfo Shminfo[2];
    int gXErrorFlag;
#endif
};

static void flip_page(struct vo *vo);

static void check_events(struct vo *vo)
{
    struct priv *p = vo->priv;

    int ret = vo_x11_check_events(vo);

    if (ret & VO_EVENT_RESIZE)
        vo_x11_clearwindow(vo, vo->x11->window);
    else if (ret & VO_EVENT_EXPOSE)
        vo_x11_clearwindow_part(vo, vo->x11->window,
                                    p->myximage[p->current_buf]->width,
                                    p->myximage[p->current_buf]->height);
    if (ret & VO_EVENT_EXPOSE && p->int_pause)
        flip_page(vo);
}

/* Scan the available visuals on this Display/Screen.  Try to find
 * the 'best' available TrueColor visual that has a decent color
 * depth (at least 15bit).  If there are multiple visuals with depth
 * >= 15bit, we prefer visuals with a smaller color depth. */
static int find_depth_from_visuals(Display * dpy, int screen,
                                   Visual ** visual_return)
{
    XVisualInfo visual_tmpl;
    XVisualInfo *visuals;
    int nvisuals, i;
    int bestvisual = -1;
    int bestvisual_depth = -1;

    visual_tmpl.screen = screen;
    visual_tmpl.class = TrueColor;
    visuals = XGetVisualInfo(dpy,
                             VisualScreenMask | VisualClassMask,
                             &visual_tmpl, &nvisuals);
    if (visuals != NULL)
    {
        for (i = 0; i < nvisuals; i++)
        {
            mp_msg(MSGT_VO, MSGL_V,
                   "vo: X11 truecolor visual %#lx, depth %d, R:%lX G:%lX B:%lX\n",
                   visuals[i].visualid, visuals[i].depth,
                   visuals[i].red_mask, visuals[i].green_mask,
                   visuals[i].blue_mask);
            /*
             * Save the visual index and its depth, if this is the first
             * truecolor visul, or a visual that is 'preferred' over the
             * previous 'best' visual.
             */
            if (bestvisual_depth == -1
                || (visuals[i].depth >= 15
                    && (visuals[i].depth < bestvisual_depth
                        || bestvisual_depth < 15)))
            {
                bestvisual = i;
                bestvisual_depth = visuals[i].depth;
            }
        }

        if (bestvisual != -1 && visual_return != NULL)
            *visual_return = visuals[bestvisual].visual;

        XFree(visuals);
    }
    return bestvisual_depth;
}

static void getMyXImage(struct priv *p, int foo)
{
    struct vo *vo = p->vo;
#ifdef HAVE_SHM
    if (vo->x11->display_is_local && XShmQueryExtension(vo->x11->display)) {
        p->Shmem_Flag = 1;
        vo->x11->ShmCompletionEvent = XShmGetEventBase(vo->x11->display)
                                    + ShmCompletion;
    } else {
        p->Shmem_Flag = 0;
        mp_msg(MSGT_VO, MSGL_WARN,
               "Shared memory not supported\nReverting to normal Xlib\n");
    }

    if (p->Shmem_Flag) {
        p->myximage[foo] =
            XShmCreateImage(vo->x11->display, p->vinfo.visual, p->depth,
                            ZPixmap, NULL, &p->Shminfo[foo], p->image_width,
                            p->image_height);
        if (p->myximage[foo] == NULL) {
            mp_msg(MSGT_VO, MSGL_WARN,
                   "Shared memory error,disabling ( Ximage error )\n");
            goto shmemerror;
        }
        p->Shminfo[foo].shmid = shmget(IPC_PRIVATE,
                                       p->myximage[foo]->bytes_per_line *
                                       p->myximage[foo]->height,
                                       IPC_CREAT | 0777);
        if (p->Shminfo[foo].shmid < 0) {
            XDestroyImage(p->myximage[foo]);
            mp_msg(MSGT_VO, MSGL_V, "%s\n", strerror(errno));
            //perror( strerror( errno ) );
            mp_msg(MSGT_VO, MSGL_WARN,
                   "Shared memory error,disabling ( seg id error )\n");
            goto shmemerror;
        }
        p->Shminfo[foo].shmaddr = (char *) shmat(p->Shminfo[foo].shmid, 0, 0);

        if (p->Shminfo[foo].shmaddr == ((char *) -1)) {
            XDestroyImage(p->myximage[foo]);
            mp_msg(MSGT_VO, MSGL_WARN,
                   "Shared memory error,disabling ( address error )\n");
            goto shmemerror;
        }
        p->myximage[foo]->data = p->Shminfo[foo].shmaddr;
        p->ImageData[foo] = (unsigned char *) p->myximage[foo]->data;
        p->Shminfo[foo].readOnly = False;
        XShmAttach(vo->x11->display, &p->Shminfo[foo]);

        XSync(vo->x11->display, False);

        if (p->gXErrorFlag) {
            XDestroyImage(p->myximage[foo]);
            shmdt(p->Shminfo[foo].shmaddr);
            mp_msg(MSGT_VO, MSGL_WARN, "Shared memory error,disabling.\n");
            p->gXErrorFlag = 0;
            goto shmemerror;
        } else
            shmctl(p->Shminfo[foo].shmid, IPC_RMID, 0);

        if (!p->firstTime) {
            mp_msg(MSGT_VO, MSGL_V, "Sharing memory.\n");
            p->firstTime = 1;
        }
    } else {
shmemerror:
        p->Shmem_Flag = 0;
#endif
    p->myximage[foo] =
        XCreateImage(vo->x11->display, p->vinfo.visual, p->depth, ZPixmap,
                     0, NULL, p->image_width, p->image_height, 8, 0);
    p->ImageDataOrig[foo] =
        malloc(p->myximage[foo]->bytes_per_line * p->image_height + 32);
    p->myximage[foo]->data = p->ImageDataOrig[foo] + 16
                           - ((long)p->ImageDataOrig & 15);
    memset(p->myximage[foo]->data, 0, p->myximage[foo]->bytes_per_line
                                      * p->image_height);
    p->ImageData[foo] = p->myximage[foo]->data;
#ifdef HAVE_SHM
}
#endif
}

static void freeMyXImage(struct priv *p, int foo)
{
    struct vo *vo = p->vo;
#ifdef HAVE_SHM
    if (p->Shmem_Flag) {
        XShmDetach(vo->x11->display, &p->Shminfo[foo]);
        XDestroyImage(p->myximage[foo]);
        shmdt(p->Shminfo[foo].shmaddr);
    } else
#endif
    {
        if (p->myximage[foo]) {
            p->myximage[foo]->data = p->ImageDataOrig[foo];
            XDestroyImage(p->myximage[foo]);
            p->ImageDataOrig[foo] = NULL;
        }
    }
    p->myximage[foo] = NULL;
    p->ImageData[foo] = NULL;
}

#if BYTE_ORDER == BIG_ENDIAN
#define BO_NATIVE    MSBFirst
#define BO_NONNATIVE LSBFirst
#else
#define BO_NATIVE    LSBFirst
#define BO_NONNATIVE MSBFirst
#endif
const struct fmt2Xfmtentry_s {
    uint32_t mpfmt;
    int byte_order;
    unsigned red_mask;
    unsigned green_mask;
    unsigned blue_mask;
} fmt2Xfmt[] = {
    {IMGFMT_RGB8,  BO_NATIVE,    0x00000007, 0x00000038, 0x000000C0},
    {IMGFMT_RGB8,  BO_NONNATIVE, 0x00000007, 0x00000038, 0x000000C0},
    {IMGFMT_BGR8,  BO_NATIVE,    0x000000E0, 0x0000001C, 0x00000003},
    {IMGFMT_BGR8,  BO_NONNATIVE, 0x000000E0, 0x0000001C, 0x00000003},
    {IMGFMT_RGB15, BO_NATIVE,    0x0000001F, 0x000003E0, 0x00007C00},
    {IMGFMT_BGR15, BO_NATIVE,    0x00007C00, 0x000003E0, 0x0000001F},
    {IMGFMT_RGB16, BO_NATIVE,    0x0000001F, 0x000007E0, 0x0000F800},
    {IMGFMT_BGR16, BO_NATIVE,    0x0000F800, 0x000007E0, 0x0000001F},
    {IMGFMT_RGB24, MSBFirst,     0x00FF0000, 0x0000FF00, 0x000000FF},
    {IMGFMT_RGB24, LSBFirst,     0x000000FF, 0x0000FF00, 0x00FF0000},
    {IMGFMT_BGR24, MSBFirst,     0x000000FF, 0x0000FF00, 0x00FF0000},
    {IMGFMT_BGR24, LSBFirst,     0x00FF0000, 0x0000FF00, 0x000000FF},
    {IMGFMT_RGB32, BO_NATIVE,    0x000000FF, 0x0000FF00, 0x00FF0000},
    {IMGFMT_RGB32, BO_NONNATIVE, 0xFF000000, 0x00FF0000, 0x0000FF00},
    {IMGFMT_BGR32, BO_NATIVE,    0x00FF0000, 0x0000FF00, 0x000000FF},
    {IMGFMT_BGR32, BO_NONNATIVE, 0x0000FF00, 0x00FF0000, 0xFF000000},
    {IMGFMT_ARGB,  MSBFirst,     0x00FF0000, 0x0000FF00, 0x000000FF},
    {IMGFMT_ARGB,  LSBFirst,     0x0000FF00, 0x00FF0000, 0xFF000000},
    {IMGFMT_ABGR,  MSBFirst,     0x000000FF, 0x0000FF00, 0x00FF0000},
    {IMGFMT_ABGR,  LSBFirst,     0xFF000000, 0x00FF0000, 0x0000FF00},
    {IMGFMT_RGBA,  MSBFirst,     0xFF000000, 0x00FF0000, 0x0000FF00},
    {IMGFMT_RGBA,  LSBFirst,     0x000000FF, 0x0000FF00, 0x00FF0000},
    {IMGFMT_BGRA,  MSBFirst,     0x0000FF00, 0x00FF0000, 0xFF000000},
    {IMGFMT_BGRA,  LSBFirst,     0x00FF0000, 0x0000FF00, 0x000000FF},
    {0}
};

static int config(struct vo *vo, uint32_t width, uint32_t height,
                  uint32_t d_width, uint32_t d_height, uint32_t flags,
                  uint32_t format)
{
    struct priv *p = vo->priv;

    Colormap theCmap;
    const struct fmt2Xfmtentry_s *fmte = fmt2Xfmt;

    mp_image_unrefp(&p->original_image);

    p->Flip_Flag = flags & VOFLAG_FLIPPING;
    p->zoomFlag = 1;

    p->old_vo_dwidth = -1;
    p->old_vo_dheight = -1;

    p->in_format = format;
    p->srcW = width;
    p->srcH = height;

    XGetWindowAttributes(vo->x11->display, vo->x11->rootwin, &p->attribs);
    p->depth = p->attribs.depth;

    if (p->depth != 15 && p->depth != 16 && p->depth != 24 && p->depth != 32) {
        Visual *visual;

        p->depth = find_depth_from_visuals(vo->x11->display, vo->x11->screen,
                                           &visual);
    }
    if (!XMatchVisualInfo(vo->x11->display, vo->x11->screen, p->depth,
                          DirectColor, &p->vinfo)
         || (WinID > 0
             && p->vinfo.visualid != XVisualIDFromVisual(p->attribs.visual)))
    {
        XMatchVisualInfo(vo->x11->display, vo->x11->screen, p->depth, TrueColor,
                         &p->vinfo);
    }

    /* set image size (which is indeed neither the input nor output size),
       if zoom is on it will be changed during draw_image anyway so we don't
       duplicate the aspect code here
     */
    p->image_width = (width + 7) & (~7);
    p->image_height = height;

    {
        theCmap = vo_x11_create_colormap(vo, &p->vinfo);

        vo_x11_create_vo_window(vo, &p->vinfo, vo->dx, vo->dy, vo->dwidth,
                                vo->dheight, flags, theCmap, "x11");
    }

    if (WinID > 0) {
        unsigned depth, dummy_uint;
        int dummy_int;
        Window dummy_win;
        XGetGeometry(vo->x11->display, vo->x11->window, &dummy_win, &dummy_int,
                     &dummy_int, &dummy_uint, &dummy_uint, &dummy_uint, &depth);
        p->depth = depth;
    }

    int i;
    for (i = 0; i < p->total_buffers; i++)
        freeMyXImage(p, i);
    sws_freeContext(p->swsContext);
    p->num_buffers = 2;
    p->total_buffers = p->num_buffers;
    for (i = 0; i < p->total_buffers; i++)
        getMyXImage(p, i);

    while (fmte->mpfmt) {
        int depth = IMGFMT_RGB_DEPTH(fmte->mpfmt);
        /* bits_per_pixel in X seems to be set to 16 for 15 bit formats
           => force depth to 16 so that only the color masks are used for the format check */
        if (depth == 15)
            depth = 16;

        if (depth == p->myximage[0]->bits_per_pixel &&
            fmte->byte_order == p->myximage[0]->byte_order &&
            fmte->red_mask == p->myximage[0]->red_mask &&
            fmte->green_mask == p->myximage[0]->green_mask &&
            fmte->blue_mask == p->myximage[0]->blue_mask)
            break;
        fmte++;
    }
    if (!fmte->mpfmt) {
        mp_msg(
            MSGT_VO, MSGL_ERR,
            "X server image format not supported, please contact the developers\n");
        return -1;
    }
    p->out_format = fmte->mpfmt;
    p->bpp = p->myximage[0]->bits_per_pixel;
    p->out_offset = 0;
    // We can easily "emulate" non-native RGB32 and BGR32
    if (p->out_format == (IMGFMT_BGR32 | 128)
        || p->out_format == (IMGFMT_RGB32 | 128))
    {
        p->out_format &= ~128;
#if BYTE_ORDER == BIG_ENDIAN
        p->out_offset = 1;
#else
        p->out_offset = -1;
#endif
    }

    /* always allocate swsContext as size could change between frames */
    p->swsContext = sws_getContextFromCmdLine(width, height, p->in_format,
                                              width, height, p->out_format);
    if (!p->swsContext)
        return -1;

    p->dst_width = width;

    return 0;
}

static void Display_Image(struct priv *p, XImage *myximage, uint8_t *ImageData)
{
    struct vo *vo = p->vo;

    XImage *x_image = p->myximage[p->current_buf];

    int x = (vo->dwidth - p->dst_width) / 2;
    int y = (vo->dheight - x_image->height) / 2;

    // do not draw if the image needs rescaling
    if ((p->old_vo_dwidth != vo->dwidth ||
         p->old_vo_dheight != vo->dheight) && p->zoomFlag)
        return;

    if (WinID == 0) {
        x = vo->dx;
        y = vo->dy;
    }
    x_image->data += p->out_offset;
#ifdef HAVE_SHM
    if (p->Shmem_Flag) {
        XShmPutImage(vo->x11->display, vo->x11->window, vo->x11->vo_gc,
                     x_image, 0, 0, x, y, p->dst_width, x_image->height,
                     True);
        vo->x11->ShmCompletionWaitCount++;
    } else
#endif
    {
        XPutImage(vo->x11->display, vo->x11->window, vo->x11->vo_gc,
                  x_image, 0, 0, x, y, p->dst_width, x_image->height);
    }
    x_image->data -= p->out_offset;
}

static struct mp_image get_x_buffer(struct priv *p, int buf_index)
{
    struct mp_image img = {0};
    mp_image_set_size(&img, p->image_width, p->image_height);
    mp_image_setfmt(&img, p->out_format);

    img.planes[0] = p->ImageData[buf_index];
    img.stride[0] = p->image_width * ((p->bpp + 7) / 8);

    return img;
}

static void draw_osd(struct vo *vo, struct osd_state *osd)
{
    struct priv *p = vo->priv;

    struct mp_image img = get_x_buffer(p, p->current_buf);

    struct mp_osd_res res = {
        .w = img.w,
        .h = img.h,
        .display_par = vo->monitor_par,
        .video_par = vo->aspdat.par,
    };

    osd_draw_on_image(osd, res, osd->vo_pts, 0, &img);
}

static mp_image_t *get_screenshot(struct vo *vo)
{
    struct priv *p = vo->priv;

    if (!p->original_image)
        return NULL;

    struct mp_image *res = mp_image_new_ref(p->original_image);
    mp_image_set_display_size(res, vo->aspdat.prew, vo->aspdat.preh);
    return res;
}

static void wait_for_completion(struct vo *vo, int max_outstanding)
{
#ifdef HAVE_SHM
    struct priv *ctx = vo->priv;
    struct vo_x11_state *x11 = vo->x11;
    if (ctx->Shmem_Flag) {
        while (x11->ShmCompletionWaitCount > max_outstanding) {
            if (!ctx->Shm_Warned_Slow) {
                mp_msg(MSGT_VO, MSGL_WARN, "[VO_X11] X11 can't keep up! Waiting"
                                           " for XShm completion events...\n");
                ctx->Shm_Warned_Slow = 1;
            }
            usec_sleep(1000);
            check_events(vo);
        }
    }
#endif
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    Display_Image(p, p->myximage[p->current_buf],
                     p->ImageData[p->current_buf]);
    p->visible_buf = p->current_buf;
    p->current_buf = (p->current_buf + 1) % p->num_buffers;

    if (!p->Shmem_Flag)
        XSync(vo->x11->display, False);
}

static void draw_image(struct vo *vo, mp_image_t *mpi)
{
    struct priv *p = vo->priv;
    uint8_t *dst[MP_MAX_PLANES] = {NULL};
    int dstStride[MP_MAX_PLANES] = {0};

    wait_for_completion(vo, p->num_buffers - 1);

    if ((p->old_vo_dwidth != vo->dwidth || p->old_vo_dheight != vo->dheight)
        /*&& y==0 */ && p->zoomFlag)
    {
        int newW = vo->dwidth;
        int newH = vo->dheight;
        struct SwsContext *oldContext = p->swsContext;

        p->old_vo_dwidth = vo->dwidth;
        p->old_vo_dheight = vo->dheight;

        if (vo_fs)
            aspect(vo, &newW, &newH, A_ZOOM);
        if (sws_flags == 0)
            newW &= (~31);      // not needed but, if the user wants the FAST_BILINEAR SCALER, then its needed

        p->swsContext
            = sws_getContextFromCmdLine(p->srcW, p->srcH, p->in_format, newW,
                                        newH, p->out_format);
        if (p->swsContext) {
            p->image_width = (newW + 7) & (~7);
            p->image_height = newH;

            int i;
            for (i = 0; i < p->total_buffers; i++)
                freeMyXImage(p, i);
            sws_freeContext(oldContext);
            for (i = 0; i < p->total_buffers; i++)
                getMyXImage(p, i);
        } else
            p->swsContext = oldContext;
        p->dst_width = newW;
    }

    dstStride[0] = p->image_width * ((p->bpp + 7) / 8);
    dst[0] = p->ImageData[p->current_buf];
    if (p->Flip_Flag) {
        dst[0] += dstStride[0] * (p->image_height - 1);
        dstStride[0] = -dstStride[0];
    }
    sws_scale(p->swsContext, (const uint8_t **)mpi->planes, mpi->stride,
              0, mpi->h, dst, dstStride);

    mp_image_setrefp(&p->original_image, mpi);
}

static int redraw_frame(struct vo *vo)
{
    struct priv *p = vo->priv;

    if (!p->original_image)
        return false;

    draw_image(vo, p->original_image);
    return true;
}

static int query_format(struct vo *vo, uint32_t format)
{
    struct priv *p = vo->priv;
    mp_msg(MSGT_VO, MSGL_DBG2,
           "vo_x11: query_format was called: %x (%s)\n", format,
           vo_format_name(format));
    if (IMGFMT_IS_RGB(format)) {
        for (int n = 0; fmt2Xfmt[n].mpfmt; n++) {
            if (fmt2Xfmt[n].mpfmt == format) {
                if (IMGFMT_RGB_DEPTH(format) == p->ximage_depth) {
                    return VFCAP_CSP_SUPPORTED | VFCAP_CSP_SUPPORTED_BY_HW |
                           VFCAP_OSD | VFCAP_FLIP;
                } else {
                    return VFCAP_CSP_SUPPORTED | VFCAP_OSD | VFCAP_FLIP;
                }
            }
        }
    }

    switch (format) {
    case IMGFMT_420P:
        return VFCAP_CSP_SUPPORTED | VFCAP_OSD;
    }
    return 0;
}

static void find_x11_depth(struct vo *vo)
{
    struct vo_x11_state *x11 = vo->x11;
    struct priv *p = vo->priv;
    XImage *mXImage = NULL;
    int depth, bpp, ximage_depth;
    unsigned int mask;
    XWindowAttributes attribs;

    // get color depth (from root window, or the best visual):
    XGetWindowAttributes(x11->display, x11->rootwin, &attribs);
    depth = attribs.depth;

    if (depth != 15 && depth != 16 && depth != 24 && depth != 32)
    {
        Visual *visual;

        depth = find_depth_from_visuals(x11->display, x11->screen, &visual);
        if (depth != -1)
            mXImage = XCreateImage(x11->display, visual, depth, ZPixmap,
                                   0, NULL, 1, 1, 8, 1);
    } else
        mXImage =
            XGetImage(x11->display, x11->rootwin, 0, 0, 1, 1, AllPlanes, ZPixmap);

    ximage_depth = depth;   // display depth on screen

    // get bits/pixel from XImage structure:
    if (mXImage == NULL)
    {
        mask = 0;
    } else
    {
        /* for the depth==24 case, the XImage structures might use
         * 24 or 32 bits of data per pixel. */
        bpp = mXImage->bits_per_pixel;
        if ((ximage_depth + 7) / 8 != (bpp + 7) / 8)
            ximage_depth = bpp;     // by A'rpi
        mask =
            mXImage->red_mask | mXImage->green_mask | mXImage->blue_mask;
        mp_msg(MSGT_VO, MSGL_V,
               "vo: X11 color mask:  %X  (R:%lX G:%lX B:%lX)\n", mask,
               mXImage->red_mask, mXImage->green_mask, mXImage->blue_mask);
        XDestroyImage(mXImage);
    }
    if (((ximage_depth + 7) / 8) == 2)
    {
        if (mask == 0x7FFF)
            ximage_depth = 15;
        else if (mask == 0xFFFF)
            ximage_depth = 16;
    }

    mp_msg(MSGT_VO, MSGL_V, "vo: X11 depth %d and %d bpp.\n", depth,
           ximage_depth);

    p->ximage_depth = ximage_depth;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (p->myximage[0])
        freeMyXImage(p, 0);
    if (p->myximage[1])
        freeMyXImage(p, 1);

    talloc_free(p->original_image);

    vo_x11_uninit(vo);

    sws_freeContext(p->swsContext);
}

static int preinit(struct vo *vo, const char *arg)
{
    struct priv *p = vo->priv;
    p->vo = vo;

    if (arg) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo_x11: Unknown subdevice: %s\n", arg);
        return ENOSYS;
    }

    if (!vo_x11_init(vo))
        return -1;              // Can't open X11
    find_x11_depth(vo);
    return 0;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;
    switch (request) {
    case VOCTRL_PAUSE:
        return p->int_pause = 1;
    case VOCTRL_RESUME:
        return p->int_pause = 0;
    case VOCTRL_FULLSCREEN:
        vo_x11_fullscreen(vo);
        vo_x11_clearwindow(vo, vo->x11->window);
        return VO_TRUE;
    case VOCTRL_SET_EQUALIZER:
    {
        struct voctrl_set_equalizer_args *args = data;
        return vo_x11_set_equalizer(vo, args->name, args->value);
    }
    case VOCTRL_GET_EQUALIZER:
    {
        struct voctrl_get_equalizer_args *args = data;
        return vo_x11_get_equalizer(vo, args->name, args->valueptr);
    }
    case VOCTRL_ONTOP:
        vo_x11_ontop(vo);
        return VO_TRUE;
    case VOCTRL_UPDATE_SCREENINFO:
        vo_x11_update_screeninfo(vo);
        return VO_TRUE;
    case VOCTRL_REDRAW_FRAME:
        return redraw_frame(vo);
    case VOCTRL_SCREENSHOT: {
        struct voctrl_screenshot_args *args = data;
        args->out_image = get_screenshot(vo);
        return true;
    }
    }
    return VO_NOTIMPL;
}

const struct vo_driver video_out_x11 = {
    .info = &(const vo_info_t) {
        "X11 ( XImage/Shm )",
        "x11",
        "Aaron Holtzman <aholtzma@ess.engr.uvic.ca>",
        ""
    },
    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv) {
        .srcW = -1,
        .srcH = -1,
        .old_vo_dwidth = -1,
        .old_vo_dheight = -1,
    },
    .preinit = preinit,
    .query_format = query_format,
    .config = config,
    .control = control,
    .draw_image = draw_image,
    .draw_osd = draw_osd,
    .flip_page = flip_page,
    .check_events = check_events,
    .uninit = uninit,
};
