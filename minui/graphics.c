/*
 * Copyright (C) 2007 The Android Open Source Project
 * Copyright (C) 2011 Freescale Semiconductor, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include <fcntl.h>
#include <stdio.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <linux/fb.h>
#include <linux/kd.h>
#ifdef FSL_EPDC_FB
#include <linux/mxcfb.h>
#endif

#include <pixelflinger/pixelflinger.h>

#include "font_10x18.h"
#include "minui.h"

#if defined(RECOVERY_BGRA)
#define PIXEL_FORMAT GGL_PIXEL_FORMAT_BGRA_8888
#define PIXEL_SIZE   4
#elif defined(RECOVERY_RGBX)
#define PIXEL_FORMAT GGL_PIXEL_FORMAT_RGBX_8888
#define PIXEL_SIZE   4
#else
#define PIXEL_FORMAT GGL_PIXEL_FORMAT_RGB_565
#define PIXEL_SIZE   2
#endif

typedef struct {
    GGLSurface texture;
    unsigned cwidth;
    unsigned cheight;
    unsigned ascent;
} GRFont;

static GRFont *gr_font = 0;
static GGLContext *gr_context = 0;
static GGLSurface gr_font_texture;
static GGLSurface gr_framebuffer[2];
static GGLSurface gr_mem_surface;
static unsigned gr_active_fb = 0;

static int gr_fb_fd = -1;
static int gr_vt_fd = -1;

static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;

#ifdef FSL_EPDC_FB
static unsigned int marker_val = 1;
static int epdc_fd;

static unsigned int update_to_display(int left, int top, int width, int height, int wave_mode,
	int wait_for_complete, uint flags)
{
	struct mxcfb_update_data upd_data;
	int retval;

	upd_data.update_mode = UPDATE_MODE_PARTIAL;
	upd_data.waveform_mode = wave_mode;
	upd_data.update_region.left = left;
	upd_data.update_region.width = width;
	upd_data.update_region.top = top;
	upd_data.update_region.height = height;
	upd_data.temp = TEMP_USE_AMBIENT;
	upd_data.flags = flags;

	if (wait_for_complete) {
		/* Get unique marker value */
		upd_data.update_marker = marker_val++;
	} else {
		upd_data.update_marker = 0;
	}

	retval = ioctl(epdc_fd, MXCFB_SEND_UPDATE, &upd_data);
	while (retval < 0) {
		/* We have limited memory available for updates, so wait and
+		 * then try again after some updates have completed */
		sleep(1);
		retval = ioctl(epdc_fd, MXCFB_SEND_UPDATE, &upd_data);
	}

	if (wait_for_complete) {
		/* Wait for update to complete */
		retval = ioctl(epdc_fd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &upd_data.update_marker);
		if (retval < 0) {
			printf("Wait for update complete failed.  Error = 0x%x", retval);
		}
	}

	return upd_data.waveform_mode;
}
#endif


static int get_framebuffer(GGLSurface *fb)
{
    int fd;
    void *bits;

#ifdef FSL_EPDC_FB
    int auto_update_mode;
    struct mxcfb_waveform_modes wv_modes;
    int scheme = UPDATE_SCHEME_QUEUE_AND_MERGE;
#endif


    fd = open("/dev/graphics/fb0", O_RDWR);
    if (fd < 0) {
        perror("cannot open fb0");
        return -1;
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0) {
        perror("failed to get fb0 info");
        close(fd);
        return -1;
    }

    vi.bits_per_pixel = PIXEL_SIZE * 8;
    if (PIXEL_FORMAT == GGL_PIXEL_FORMAT_BGRA_8888) {
      vi.red.offset     = 8;
      vi.red.length     = 8;
      vi.green.offset   = 16;
      vi.green.length   = 8;
      vi.blue.offset    = 24;
      vi.blue.length    = 8;
      vi.transp.offset  = 0;
      vi.transp.length  = 8;
    } else if (PIXEL_FORMAT == GGL_PIXEL_FORMAT_RGBX_8888) {
      vi.red.offset     = 24;
      vi.red.length     = 8;
      vi.green.offset   = 16;
      vi.green.length   = 8;
      vi.blue.offset    = 8;
      vi.blue.length    = 8;
      vi.transp.offset  = 0;
      vi.transp.length  = 8;
    } else { /* RGB565*/
      vi.red.offset     = 11;
      vi.red.length     = 5;
      vi.green.offset   = 5;
      vi.green.length   = 6;
      vi.blue.offset    = 0;
      vi.blue.length    = 5;
      vi.transp.offset  = 0;
      vi.transp.length  = 0;
    }
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &vi) < 0) {
        perror("failed to put fb0 info");
        close(fd);
        return -1;
    }

    if (ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        perror("failed to get fb0 info");
        close(fd);
        return -1;
    }

#ifdef FSL_EPDC_FB
    vi.bits_per_pixel = 16;
    vi.grayscale = 0;
    vi.yoffset = 0;
    vi.rotate = FB_ROTATE_UR;
    vi.activate = FB_ACTIVATE_FORCE;
    epdc_fd = fd;

    if (ioctl(fd, FBIOPUT_VSCREENINFO, &vi) < 0) {
        perror("failed to put fb0 info");
        close(fd);
        return -1;
    }
#endif

    bits = mmap(0, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (bits == MAP_FAILED) {
        perror("failed to mmap framebuffer");
        close(fd);
        return -1;
    }

    fb->version = sizeof(*fb);
    fb->width = vi.xres;
    fb->height = vi.yres;
    fb->stride = fi.line_length/PIXEL_SIZE;
    fb->data = bits;
    fb->format = PIXEL_FORMAT;
    memset(fb->data, 0, vi.yres * fi.line_length);

    fb++;

    fb->version = sizeof(*fb);
    fb->width = vi.xres;
    fb->height = vi.yres;
    fb->stride = fi.line_length/PIXEL_SIZE;
    fb->data = (void*) (((unsigned) bits) + vi.yres * fi.line_length);
    fb->format = PIXEL_FORMAT;
    memset(fb->data, 0, vi.yres * fi.line_length);

#ifdef FSL_EPDC_FB
    auto_update_mode = AUTO_UPDATE_MODE_REGION_MODE;
    if (ioctl(fd, MXCFB_SET_AUTO_UPDATE_MODE, &auto_update_mode) < 0) {
        perror("set auto update mode failed\n");
        return -1;
    }

    wv_modes.mode_init = 0;
    wv_modes.mode_du = 1;
    wv_modes.mode_gc4 = 3;
    wv_modes.mode_gc8 = 2;
    wv_modes.mode_gc16 = 2;
    wv_modes.mode_gc32 = 2;
    if (ioctl(fd, MXCFB_SET_WAVEFORM_MODES, &wv_modes) < 0) {
        perror("set waveform modes failed\n");
        return -1;
    }

    if (ioctl(fd, MXCFB_SET_UPDATE_SCHEME, &scheme) < 0) {
        perror("set update scheme failed\n");
        return -1;
    }
#endif

    return fd;
}

static void get_memory_surface(GGLSurface* ms) {
  ms->version = sizeof(*ms);
  ms->width = vi.xres;
  ms->height = vi.yres;
  ms->stride = fi.line_length/PIXEL_SIZE;
  ms->data = malloc(fi.line_length * vi.yres);
  ms->format = PIXEL_FORMAT;
}

static void set_active_framebuffer(unsigned n)
{
    if (n > 1) return;
    vi.yres_virtual = vi.yres * PIXEL_SIZE;
    vi.yoffset = n * vi.yres;
    vi.bits_per_pixel = PIXEL_SIZE * 8;
    if (ioctl(gr_fb_fd, FBIOPAN_DISPLAY, &vi) < 0) {
    /* if (ioctl(gr_fb_fd, FBIOPUT_VSCREENINFO, &vi) < 0) { */
        perror("active fb swap failed");
    }
#ifdef FSL_EPDC_FB
    update_to_display(0, 0, vi.xres, vi.yres, WAVEFORM_MODE_AUTO, 1, 0);
#endif
}

void gr_flip(void)
{
    GGLContext *gl = gr_context;

    /* swap front and back buffers */
    gr_active_fb = (gr_active_fb + 1) & 1;

    /* copy data from the in-memory surface to the buffer we're about
     * to make active. */
    memcpy(gr_framebuffer[gr_active_fb].data, gr_mem_surface.data,
           fi.line_length * vi.yres);

    /* inform the display driver */
    set_active_framebuffer(gr_active_fb);
}

void gr_color(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    GGLContext *gl = gr_context;
    GGLint color[4];
    color[0] = ((r << 8) | r) + 1;
    color[1] = ((g << 8) | g) + 1;
    color[2] = ((b << 8) | b) + 1;
    color[3] = ((a << 8) | a) + 1;
    gl->color4xv(gl, color);
}

int gr_measure(const char *s)
{
    return gr_font->cwidth * strlen(s);
}

void gr_font_size(int *x, int *y)
{
    *x = gr_font->cwidth;
    *y = gr_font->cheight;
}

int gr_text(int x, int y, const char *s)
{
    GGLContext *gl = gr_context;
    GRFont *font = gr_font;
    unsigned off;

    y -= font->ascent;

    gl->bindTexture(gl, &font->texture);
    gl->texEnvi(gl, GGL_TEXTURE_ENV, GGL_TEXTURE_ENV_MODE, GGL_REPLACE);
    gl->texGeni(gl, GGL_S, GGL_TEXTURE_GEN_MODE, GGL_ONE_TO_ONE);
    gl->texGeni(gl, GGL_T, GGL_TEXTURE_GEN_MODE, GGL_ONE_TO_ONE);
    gl->enable(gl, GGL_TEXTURE_2D);

    while((off = *s++)) {
        off -= 32;
        if (off < 96) {
            gl->texCoord2i(gl, (off * font->cwidth) - x, 0 - y);
            gl->recti(gl, x, y, x + font->cwidth, y + font->cheight);
        }
        x += font->cwidth;
    }

    return x;
}

void gr_fill(int x, int y, int w, int h)
{
    GGLContext *gl = gr_context;
    gl->disable(gl, GGL_TEXTURE_2D);
    gl->recti(gl, x, y, w, h);
}

void gr_blit(gr_surface source, int sx, int sy, int w, int h, int dx, int dy) {
    if (gr_context == NULL) {
        return;
    }
    GGLContext *gl = gr_context;

    gl->bindTexture(gl, (GGLSurface*) source);
    gl->texEnvi(gl, GGL_TEXTURE_ENV, GGL_TEXTURE_ENV_MODE, GGL_REPLACE);
    gl->texGeni(gl, GGL_S, GGL_TEXTURE_GEN_MODE, GGL_ONE_TO_ONE);
    gl->texGeni(gl, GGL_T, GGL_TEXTURE_GEN_MODE, GGL_ONE_TO_ONE);
    gl->enable(gl, GGL_TEXTURE_2D);
    gl->texCoord2i(gl, sx - dx, sy - dy);
    gl->recti(gl, dx, dy, dx + w, dy + h);
}

unsigned int gr_get_width(gr_surface surface) {
    if (surface == NULL) {
        return 0;
    }
    return ((GGLSurface*) surface)->width;
}

unsigned int gr_get_height(gr_surface surface) {
    if (surface == NULL) {
        return 0;
    }
    return ((GGLSurface*) surface)->height;
}

static void gr_init_font(void)
{
    GGLSurface *ftex;
    unsigned char *bits, *rle;
    unsigned char *in, data;

    gr_font = calloc(sizeof(*gr_font), 1);
    ftex = &gr_font->texture;

    bits = malloc(font.width * font.height);

    ftex->version = sizeof(*ftex);
    ftex->width = font.width;
    ftex->height = font.height;
    ftex->stride = font.width;
    ftex->data = (void*) bits;
    ftex->format = GGL_PIXEL_FORMAT_A_8;

    in = font.rundata;
    while((data = *in++)) {
        memset(bits, (data & 0x80) ? 255 : 0, data & 0x7f);
        bits += (data & 0x7f);
    }

    gr_font->cwidth = font.cwidth;
    gr_font->cheight = font.cheight;
    gr_font->ascent = font.cheight - 2;
}

int gr_init(void)
{
    gglInit(&gr_context);
    GGLContext *gl = gr_context;

    gr_init_font();
    gr_vt_fd = open("/dev/tty0", O_RDWR | O_SYNC);
    if (gr_vt_fd < 0) {
        // This is non-fatal; post-Cupcake kernels don't have tty0.
        perror("can't open /dev/tty0");
    } else if (ioctl(gr_vt_fd, KDSETMODE, (void*) KD_GRAPHICS)) {
        // However, if we do open tty0, we expect the ioctl to work.
        perror("failed KDSETMODE to KD_GRAPHICS on tty0");
        gr_exit();
        return -1;
    }

    gr_fb_fd = get_framebuffer(gr_framebuffer);
    if (gr_fb_fd < 0) {
        gr_exit();
        return -1;
    }

    get_memory_surface(&gr_mem_surface);

    fprintf(stderr, "framebuffer: fd %d (%d x %d)\n",
            gr_fb_fd, gr_framebuffer[0].width, gr_framebuffer[0].height);

        /* start with 0 as front (displayed) and 1 as back (drawing) */
    gr_active_fb = 0;
    set_active_framebuffer(0);
    gl->colorBuffer(gl, &gr_mem_surface);

    gl->activeTexture(gl, 0);
    gl->enable(gl, GGL_BLEND);
    gl->blendFunc(gl, GGL_SRC_ALPHA, GGL_ONE_MINUS_SRC_ALPHA);

    gr_fb_blank(true);
    gr_fb_blank(false);

    return 0;
}

void gr_exit(void)
{
    close(gr_fb_fd);
    gr_fb_fd = -1;

    free(gr_mem_surface.data);

    ioctl(gr_vt_fd, KDSETMODE, (void*) KD_TEXT);
    close(gr_vt_fd);
    gr_vt_fd = -1;
}

int gr_fb_width(void)
{
    return gr_framebuffer[0].width;
}

int gr_fb_height(void)
{
    return gr_framebuffer[0].height;
}

gr_pixel *gr_fb_data(void)
{
    return (unsigned short *) gr_mem_surface.data;
}

void gr_fb_blank(bool blank)
{
    int ret;

    ret = ioctl(gr_fb_fd, FBIOBLANK, blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK);
    if (ret < 0)
        perror("ioctl(): blank");
}
