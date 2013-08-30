/*
 * Copyright (C) 2007 The Android Open Source Project
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/msm_mdp.h>
#include <linux/msm_ion.h>

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

#define NUM_BUFFERS 2
#define MDP_V4_0 400
#define ALIGN(x, align) (((x) + ((align)-1)) & ~((align)-1))

typedef struct {
    GGLSurface* texture;
    unsigned cwidth;
    unsigned cheight;
} GRFont;

typedef struct {
    unsigned char *mem_buf;
    int size;
    int ion_fd;
    int mem_fd;
    struct ion_handle_data handle_data;
} memInfo;

static GRFont *gr_font = 0;
static GGLContext *gr_context = 0;
static GGLSurface gr_font_texture;
static GGLSurface gr_framebuffer[NUM_BUFFERS];
static GGLSurface gr_mem_surface;
static unsigned gr_active_fb = 0;
static unsigned double_buffering = 0;
static int overscan_percent = OVERSCAN_PERCENT;
static int overscan_offset_x = 0;
static int overscan_offset_y = 0;

static int gr_fb_fd = -1;
static int gr_vt_fd = -1;

static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;

static int overlay_id = MSMFB_NEW_REQUEST;
static bool has_overlay = false;
static memInfo mem_info;

static int map_mdp_pixel_format()
{
#if defined(RECOVERY_BGRA)
    return MDP_BGRA_8888;
#elif defined(RECOVERY_RGBX)
    return MDP_RGBA_8888;
#else
    return MDP_RGB_565;
#endif
}

static int free_ion_mem(void) {
    int ret = 0;

    if (mem_info.mem_buf)
        munmap(mem_info.mem_buf, mem_info.size);

    if (mem_info.ion_fd >= 0) {
        ret = ioctl(mem_info.ion_fd, ION_IOC_FREE, &mem_info.handle_data);
        if (ret < 0)
            perror("free_mem failed ");
    }

    if (mem_info.mem_fd >= 0)
        close(mem_info.mem_fd);
    if (mem_info.ion_fd >= 0)
        close(mem_info.ion_fd);

    memset(&mem_info, 0, sizeof(mem_info));
    mem_info.mem_fd = -1;
    mem_info.ion_fd = -1;
    return 0;
}

static int alloc_ion_mem(unsigned int size)
{
    int result;
    struct ion_fd_data fd_data;
    struct ion_allocation_data ionAllocData;

    mem_info.ion_fd = open("/dev/ion", O_RDWR|O_DSYNC);
    if (mem_info.ion_fd < 0) {
        perror("ERROR: Can't open ion ");
        return -errno;
    }

    ionAllocData.flags = 0;
    ionAllocData.len = size;
    ionAllocData.align = sysconf(_SC_PAGESIZE);
    ionAllocData.heap_mask =
            ION_HEAP(ION_IOMMU_HEAP_ID) |
            ION_HEAP(ION_SYSTEM_CONTIG_HEAP_ID);

    result = ioctl(mem_info.ion_fd, ION_IOC_ALLOC,  &ionAllocData);
    if(result){
        perror("ION_IOC_ALLOC Failed ");
        close(mem_info.ion_fd);
        return result;
    }

    fd_data.handle = ionAllocData.handle;
    mem_info.handle_data.handle = ionAllocData.handle;
    result = ioctl(mem_info.ion_fd, ION_IOC_MAP, &fd_data);
    if (result) {
        perror("ION_IOC_MAP Failed ");
        free_ion_mem();
        return result;
    }
    mem_info.mem_buf = (unsigned char *)mmap(NULL, size, PROT_READ |
                PROT_WRITE, MAP_SHARED, fd_data.fd, 0);
    mem_info.mem_fd = fd_data.fd;

    if (!mem_info.mem_buf) {
        perror("ERROR: mem_buf MAP_FAILED ");
        free_ion_mem();
        return -ENOMEM;
    }

    return 0;
}

static int target_has_overlay(char *version)
{
    int ret;
    int mdp_version;

    if (strlen(version) >= 8) {
        if(!strncmp(version, "msmfb", strlen("msmfb"))) {
            char str_ver[4];
            memcpy(str_ver, version + strlen("msmfb"), 3);
            str_ver[3] = '\0';
            mdp_version = atoi(str_ver);
            if (mdp_version >= MDP_V4_0)
                return 1;
            return 0;
        } else if (!strncmp(version, "mdssfb", strlen("mdssfb"))) {
            return 1;
        }
    }

    return 0;
}

static int allocate_overlay(int fd)
{
    struct mdp_overlay overlay;
    int ret = 0;

    memset(&overlay, 0 , sizeof (struct mdp_overlay));

    /* Fill Overlay Data */

    overlay.src.width  = ALIGN(gr_framebuffer[0].width, 32);
    overlay.src.height = gr_framebuffer[0].height;
    overlay.src.format = map_mdp_pixel_format();
    overlay.src_rect.w = gr_framebuffer[0].width;
    overlay.src_rect.h = gr_framebuffer[0].height;
    overlay.dst_rect.w = gr_framebuffer[0].width;
    overlay.dst_rect.h = gr_framebuffer[0].height;
    overlay.alpha = 0xFF;
    overlay.transp_mask = MDP_TRANSP_NOP;
    overlay.id = MSMFB_NEW_REQUEST;
    ret = ioctl(fd, MSMFB_OVERLAY_SET, &overlay);
    if (ret < 0) {
        perror("Overlay Set Failed");
        return ret;
    }

    overlay_id = overlay.id;
    return 0;
}

static int free_overlay(int fd)
{
    int ret = 0;
    struct mdp_display_commit ext_commit;

    if (overlay_id != MSMFB_NEW_REQUEST) {
        ret = ioctl(fd, MSMFB_OVERLAY_UNSET, &overlay_id);
        if (ret) {
            perror("Overlay Unset Failed");
            overlay_id = MSMFB_NEW_REQUEST;
            return ret;
        }

        memset(&ext_commit, 0, sizeof(struct mdp_display_commit));
        ext_commit.flags = MDP_DISPLAY_COMMIT_OVERLAY;
        ext_commit.wait_for_finish = 1;
        ret = ioctl(fd, MSMFB_DISPLAY_COMMIT, &ext_commit);
        if (ret < 0) {
            perror("ERROR: Clear MSMFB_DISPLAY_COMMIT failed!");
            overlay_id = MSMFB_NEW_REQUEST;
            return ret;
        }

        overlay_id = MSMFB_NEW_REQUEST;
    }
    return 0;
}

static int overlay_display_frame(int fd, int memory_id)
{
    int ret = 0;
    struct msmfb_overlay_data ovdata;
    struct mdp_display_commit ext_commit;

    if (overlay_id == MSMFB_NEW_REQUEST) {
        perror("display_frame failed, no overlay\n");
        return -1;
    }

    memset(&ovdata, 0, sizeof(struct msmfb_overlay_data));

    ovdata.id = overlay_id;
    ovdata.data.flags = 0;
    ovdata.data.offset = 0;
    ovdata.data.memory_id = memory_id;
    ret = ioctl(fd, MSMFB_OVERLAY_PLAY, &ovdata);
    if (ret < 0) {
        perror("overlay_display_frame failed, overlay play Failed\n");
        return ret;
    }

    memset(&ext_commit, 0, sizeof(struct mdp_display_commit));
    ext_commit.flags = MDP_DISPLAY_COMMIT_OVERLAY;
    ext_commit.wait_for_finish = 1;
    ret = ioctl(fd, MSMFB_DISPLAY_COMMIT, &ext_commit);
    if (ret < 0) {
        perror("overlay_display_frame failed, overlay commit Failed\n!");
        return ret;
    }

    return 0;
}

static int get_framebuffer(GGLSurface *fb)
{
    int fd;
    void *bits;

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

    has_overlay = target_has_overlay(fi.id);

    if (!has_overlay) {
        bits = mmap(0, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (bits == MAP_FAILED) {
            perror("failed to mmap framebuffer");
            close(fd);
            return -1;
        }
    }
    overscan_offset_x = vi.xres * overscan_percent / 100;
    overscan_offset_y = vi.yres * overscan_percent / 100;

    fb->version = sizeof(*fb);
    fb->width = vi.xres;
    fb->height = vi.yres;
    fb->stride = fi.line_length/PIXEL_SIZE;
    fb->format = PIXEL_FORMAT;
    if (!has_overlay) {
        fb->data = bits;
        memset(fb->data, 0, vi.yres * fi.line_length);
    }

    fb++;

    /* check if we can use double buffering */
    if (vi.yres * fi.line_length * 2 > fi.smem_len)
        return fd;

    double_buffering = 1;

    fb->version = sizeof(*fb);
    fb->width = vi.xres;
    fb->height = vi.yres;
    fb->stride = fi.line_length/PIXEL_SIZE;
    fb->format = PIXEL_FORMAT;
    if (!has_overlay) {
        fb->data = (void*) (((unsigned) bits) + vi.yres * fi.line_length);
        memset(fb->data, 0, vi.yres * fi.line_length);
    }

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
    if (n > 1 || !double_buffering) return;
    vi.yres_virtual = vi.yres * NUM_BUFFERS;
    vi.yoffset = n * vi.yres;
    vi.bits_per_pixel = PIXEL_SIZE * 8;
    if (ioctl(gr_fb_fd, FBIOPUT_VSCREENINFO, &vi) < 0) {
        perror("active fb swap failed");
    }
}

void gr_flip(void)
{
    if (!has_overlay) {
        GGLContext *gl = gr_context;

        /* swap front and back buffers */
        if (double_buffering)
            gr_active_fb = (gr_active_fb + 1) & 1;

        /* copy data from the in-memory surface to the buffer we're about
         * to make active. */
        memcpy(gr_framebuffer[gr_active_fb].data, gr_mem_surface.data,
               fi.line_length * vi.yres);

        /* inform the display driver */
        set_active_framebuffer(gr_active_fb);
    } else {
        memcpy(mem_info.mem_buf, gr_mem_surface.data,
                fi.line_length * vi.yres);
        overlay_display_frame(gr_fb_fd, mem_info.mem_fd);
    }
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

int gr_text(int x, int y, const char *s, int bold)
{
    GGLContext *gl = gr_context;
    GRFont *font = gr_font;
    unsigned off;

    if (!font->texture) return x;

    bold = bold && (font->texture->height != font->cheight);

    x += overscan_offset_x;
    y += overscan_offset_y;

    gl->bindTexture(gl, font->texture);
    gl->texEnvi(gl, GGL_TEXTURE_ENV, GGL_TEXTURE_ENV_MODE, GGL_REPLACE);
    gl->texGeni(gl, GGL_S, GGL_TEXTURE_GEN_MODE, GGL_ONE_TO_ONE);
    gl->texGeni(gl, GGL_T, GGL_TEXTURE_GEN_MODE, GGL_ONE_TO_ONE);
    gl->enable(gl, GGL_TEXTURE_2D);

    while((off = *s++)) {
        off -= 32;
        if (off < 96) {
            gl->texCoord2i(gl, (off * font->cwidth) - x,
                           (bold ? font->cheight : 0) - y);
            gl->recti(gl, x, y, x + font->cwidth, y + font->cheight);
        }
        x += font->cwidth;
    }

    return x;
}

void gr_texticon(int x, int y, gr_surface icon) {
    if (gr_context == NULL || icon == NULL) {
        return;
    }
    GGLContext* gl = gr_context;

    x += overscan_offset_x;
    y += overscan_offset_y;

    gl->bindTexture(gl, (GGLSurface*) icon);
    gl->texEnvi(gl, GGL_TEXTURE_ENV, GGL_TEXTURE_ENV_MODE, GGL_REPLACE);
    gl->texGeni(gl, GGL_S, GGL_TEXTURE_GEN_MODE, GGL_ONE_TO_ONE);
    gl->texGeni(gl, GGL_T, GGL_TEXTURE_GEN_MODE, GGL_ONE_TO_ONE);
    gl->enable(gl, GGL_TEXTURE_2D);

    int w = gr_get_width(icon);
    int h = gr_get_height(icon);

    gl->texCoord2i(gl, -x, -y);
    gl->recti(gl, x, y, x+gr_get_width(icon), y+gr_get_height(icon));
}

void gr_fill(int x1, int y1, int x2, int y2)
{
    x1 += overscan_offset_x;
    y1 += overscan_offset_y;

    x2 += overscan_offset_x;
    y2 += overscan_offset_y;

    GGLContext *gl = gr_context;
    gl->disable(gl, GGL_TEXTURE_2D);
    gl->recti(gl, x1, y1, x2, y2);
}

void gr_blit(gr_surface source, int sx, int sy, int w, int h, int dx, int dy) {
    if (gr_context == NULL || source == NULL) {
        return;
    }
    GGLContext *gl = gr_context;

    dx += overscan_offset_x;
    dy += overscan_offset_y;

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
    gr_font = calloc(sizeof(*gr_font), 1);

    int res = res_create_surface("font", (void**)&(gr_font->texture));
    if (res == 0) {
        // The font image should be a 96x2 array of character images.  The
        // columns are the printable ASCII characters 0x20 - 0x7f.  The
        // top row is regular text; the bottom row is bold.
        gr_font->cwidth = gr_font->texture->width / 96;
        gr_font->cheight = gr_font->texture->height / 2;
    } else {
        printf("failed to read font: res=%d\n", res);

        // fall back to the compiled-in font.
        gr_font->texture = malloc(sizeof(*gr_font->texture));
        gr_font->texture->width = font.width;
        gr_font->texture->height = font.height;
        gr_font->texture->stride = font.width;

        unsigned char* bits = malloc(font.width * font.height);
        gr_font->texture->data = (void*) bits;

        unsigned char data;
        unsigned char* in = font.rundata;
        while((data = *in++)) {
            memset(bits, (data & 0x80) ? 255 : 0, data & 0x7f);
            bits += (data & 0x7f);
        }

        gr_font->cwidth = font.cwidth;
        gr_font->cheight = font.cheight;
    }

    // interpret the grayscale as alpha
    gr_font->texture->format = GGL_PIXEL_FORMAT_A_8;
}

int gr_init(void)
{
    gglInit(&gr_context);
    GGLContext *gl = gr_context;
    int result = 0;

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
    if (!has_overlay)
        set_active_framebuffer(0);
    gl->colorBuffer(gl, &gr_mem_surface);

    gl->activeTexture(gl, 0);
    gl->enable(gl, GGL_BLEND);
    gl->blendFunc(gl, GGL_SRC_ALPHA, GGL_ONE_MINUS_SRC_ALPHA);

    gr_fb_blank(true);
    gr_fb_blank(false);

    if (has_overlay) {
        result = alloc_ion_mem(fi.line_length * vi.yres);
        if (result) {
            perror("fail to allocate buffer\n");
            return result;
        }
        allocate_overlay(gr_fb_fd);
    }

    return 0;
}

void gr_exit(void)
{
    if (has_overlay) {
        free_overlay(gr_fb_fd);
        free_ion_mem();
    }

    close(gr_fb_fd);
    gr_fb_fd = -1;

    free(gr_mem_surface.data);

    ioctl(gr_vt_fd, KDSETMODE, (void*) KD_TEXT);
    close(gr_vt_fd);
    gr_vt_fd = -1;
}

int gr_fb_width(void)
{
    return gr_framebuffer[0].width - 2*overscan_offset_x;
}

int gr_fb_height(void)
{
    return gr_framebuffer[0].height - 2*overscan_offset_y;
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

void gr_get_memory_surface(gr_surface surface)
{
    get_memory_surface( (GGLSurface*) surface);
}
