// SPDX-License-Identifier: GPL-2.0-only
/*
 * xapi_gui.c — XJ380 XAPI graphic subset (window/draw/message, 7392–7463)
 * implemented in-kernel on top of the Linux framebuffer (fbdev).
 *
 * Ported from XJ380 kernel/syscall/xapi/xgui.cpp.  Differences from the
 * XJ380 original:
 *   - windows are software sheets composited onto the first framebuffer
 *     (32bpp assumed); compositing happens on XAPI_REFRESH_WINDOW /
 *     XAPI_REFRESH_PART_WINDOW only;
 *   - text rendering uses the kernel console font (scaled), not TTF;
 *   - XAPI color format is 0xRRGGBBAA (as in XJ380);
 *   - message callbacks cannot be invoked directly from kernel context on
 *     Linux: XAPI_SET_MSH_PROCOR stores the callback and queues messages;
 *     input events queue MSG_* entries and SIGUSR2-wake the owner task.
 *     PNG/SVG/FA decode and the widget set (P4.6) are not implemented yet
 *     and return XAPI_ERR.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/fb.h>
#include <linux/font.h>
#include <linux/keyboard.h>
#include <linux/sched/signal.h>
#include <asm/xapi.h>

/* forward declarations */
static u32 xapi_text_width(u64 str, u32 size);
#include <asm/uaccess.h>

/* ----- XAPI GUI syscall numbers (P4.x of the XAPI spec) ----- */
#define XAPI_CREATE_WINDOW       7392
#define XAPI_SET_WINDOW_TITLE    7393
#define XAPI_CLOSE_WINDOW        7394
#define XAPI_SET_ICON            7395
#define XAPI_DRAW_POINT          7396
#define XAPI_DRAW_LINE           7397
#define XAPI_DRAW_RECT           7398
#define XAPI_DRAW_RECT_FILL      7399
#define XAPI_DRAW_CIRCLE         7400
#define XAPI_DRAW_CIRCLE_FILL    7401
#define XAPI_DRAW_TEXT           7402
#define XAPI_DRAW_BMP            7403
#define XAPI_DRAW_PNG            7404
#define XAPI_SET_MSH_PROCOR      7405
#define XAPI_READBUFFER          7406
#define XAPI_WRITEBUFFER         7407
#define XAPI_WRITEBUFFERA        7408
#define XAPI_REFRESH_WINDOW      7409
#define XAPI_BUTTON              7410
#define XAPI_BUTTON_EMP          7411
#define XAPI_DRAW_TEXT_L         7414
#define XAPI_DRAW_TEXT_SW        7415
#define XAPI_READBUFFERA         7417
#define XAPI_DRAW_PICTURE        7419
#define XAPI_GET_WIN_SZIE        7426
#define XAPI_CALC_TEXT_WIDTH     7431
#define XAPI_REFRESH_PART_WINDOW 7438
#define XAPI_GET_PIC_SIZE        7440
#define XAPI_DRAW_SVG            7446
#define XAPI_DRAW_FA             7447
#define XAPI_LOAD_PICTURE        7463

/* window type bits (XWINDOW.sets) */
#define XWIN_NORMAL              0
#define XWIN_FRAME_OFF           1
#define XWIN_FULL_SCR            2
#define XWIN_DESKTOP             3
#define XWIN_DOCK                4
#define XWIN_LOGIN               5
#define XWIN_TYPE_MASK           0x0f
#define XWIN_SUPPORT_RESIZEABLE  0x80

/* window frame metrics (same as XJ380 XWIN_NORMAL content offsets) */
#define XGUI_BORDER_X  12
#define XGUI_BORDER_T  27
#define XGUI_BORDER_B  20

#define XAPI_MSGQ_LEN  64

#define XAPI_OK  0
#define XAPI_ERR (~0ULL)

struct xapi_win {
	struct list_head list;
	u32 w, h;              /* full window size incl. frame */
	u32 x, y;              /* screen position */
	u32 type;
	u32 *buf;              /* w*h pixels, 0xRRGGBBAA */
	char *title;
	struct task_struct *owner;
	u64 msg_func;          /* user MsgPrcor (not callable from kernel) */
	u64 msg[XAPI_MSGQ_LEN * 3]; /* flat ring: type,hData,lData */
	u32 msg_head, msg_tail;
	bool visible;
};

static LIST_HEAD(xapi_wins);
static DEFINE_MUTEX(xapi_gui_lock);
static struct fb_info *xapi_fb;
static u32 xapi_next_slot;     /* cascade position counter */

/* ----- framebuffer capture (lazy: /dev/fb0 keeps fb_info in private_data) ----- */

static struct fb_info *xapi_fb_get(void)
{
	struct file *f;

	if (xapi_fb)
		return xapi_fb;
	f = filp_open("/dev/fb0", O_RDWR, 0);
	if (!IS_ERR(f)) {
		struct fb_info *info = (struct fb_info *)f->private_data;

		if (info && info->fbops && info->fbops->fb_imageblit)
			xapi_fb = info;
		fput(f);
	}
	return xapi_fb;
}

/* ----- pixel helpers ----- */

static inline u8 xapi_px_r(u32 c) { return (u8)(c >> 24); }
static inline u8 xapi_px_g(u32 c) { return (u8)(c >> 16); }
static inline u8 xapi_px_b(u32 c) { return (u8)(c >> 8); }
static inline u8 xapi_px_a(u32 c) { return (u8)c; }

#define XGUI_BG   0x202830FFu  /* desktop background, 0xRRGGBBAA */
#define XGUI_BAR  0x3A3F44FFu  /* title bar / frame */
#define XGUI_FG   0xFFFFFFFFu  /* title text */

/* convert 0xRRGGBBAA to the framebuffer's truecolor layout */
static inline u32 xapi_px_to_fb(struct xapi_win *w, struct fb_info *info, u32 c)
{
	struct fb_var_screeninfo *var = &info->var;
	u32 r = xapi_px_r(c), g = xapi_px_g(c), b = xapi_px_b(c), a = xapi_px_a(c);
	u32 out = 0;

	if (var->green.length >= 8) {
		out |= (r >> (8 - var->red.length)) << var->red.offset;
		out |= (g >> (8 - var->green.length)) << var->green.offset;
		out |= (b >> (8 - var->blue.length)) << var->blue.offset;
		if (var->transp.length)
			out |= (a >> (8 - var->transp.length)) << var->transp.offset;
	} else {
		/* narrow 16bpp style layouts: 5/6/5 */
		out |= ((r >> 3) & 0x1f) << var->red.offset;
		out |= ((g >> 2) & 0x3f) << var->green.offset;
		out |= ((b >> 3) & 0x1f) << var->blue.offset;
	}
	return out;
}

/* ----- window management ----- */

static void xapi_win_frame_draw(struct xapi_win *w);
static void xapi_win_draw_text_raw(struct xapi_win *w, const char *str, u32 x, u32 y, u32 color, u32 scale);

static void xapi_win_frame_draw(struct xapi_win *w)
{
	u32 x, y;

	if (w->type != XWIN_NORMAL)
		return;
	/* border + title bar + bottom strip */
	for (y = 0; y < w->h; y++)
		for (x = 0; x < w->w; x++)
			w->buf[y * w->w + x] = XGUI_BAR;
	for (y = XGUI_BORDER_T; y < w->h - XGUI_BORDER_B; y++)
		for (x = XGUI_BORDER_X; x < w->w - XGUI_BORDER_X; x++)
			w->buf[y * w->w + x] = 0x101014FFu;
	if (w->title)
		xapi_win_draw_text_raw(w, w->title, XGUI_BORDER_X, 6, XGUI_FG, 1);
}

static struct xapi_win *xapi_win_get(u64 handle)
{
	struct xapi_win *w;

	if (!handle)
		return NULL;
	list_for_each_entry(w, &xapi_wins, list)
		if ((u64)(unsigned long)w == handle)
			return w;
	return NULL;
}

static int xapi_copy_str(char **out, u64 user_str)
{
	char kbuf[256];
	size_t len;

	if (!user_str) {
		*out = NULL;
		return 0;
	}
	if (strncpy_from_user(kbuf, (char __user *)(unsigned long)user_str, sizeof(kbuf) - 1) < 0)
		return -1;
	kbuf[sizeof(kbuf) - 1] = 0;
	len = strlen(kbuf) + 1;
	*out = kmemdup(kbuf, len, GFP_KERNEL);
	return *out ? 0 : -1;
}

static u64 xapi_gui_create_window(u64 out_handle, u64 xwin_p)
{
	struct xapi_win *w;
	struct fb_info *info;
	u32 width, height;
	u8 sets;

	if (get_user(width, (u32 __user *)(unsigned long)xwin_p) ||
	    get_user(height, (u32 __user *)(unsigned long)(xwin_p + 4)) ||
	    get_user(sets, (u8 __user *)(unsigned long)(xwin_p + 12)))
		return XAPI_ERR;
	if (width == 0 || height == 0 || width > 8192 || height > 8192)
		return XAPI_ERR;
	info = xapi_fb_get();
	if (!info)
		return XAPI_ERR;

	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return XAPI_ERR;
	w->w = width;
	w->h = height;
	w->type = sets & XWIN_TYPE_MASK;
	w->owner = get_task_struct(current);
	w->visible = true;
	w->buf = vzalloc((size_t)w->w * w->h * 4);
	if (!w->buf) {
		kfree(w);
		return XAPI_ERR;
	}

	mutex_lock(&xapi_gui_lock);
	/* cascade position */
	w->x = 40 + (xapi_next_slot % 10) * 24;
	w->y = 40 + (xapi_next_slot % 10) * 24;
	xapi_next_slot++;
	if (w->x + w->w > info->var.xres)
		w->x = w->w >= info->var.xres ? 0 : info->var.xres - w->w;
	if (w->y + w->h > info->var.yres)
		w->y = w->h >= info->var.yres ? 0 : info->var.yres - w->h;

	if (xapi_copy_str(&w->title, *(u64 __user *)(unsigned long)(xwin_p + 8)) < 0) {
		mutex_unlock(&xapi_gui_lock);
		vfree(w->buf);
		kfree(w);
		return XAPI_ERR;
	}
	xapi_win_frame_draw(w);
	list_add_tail(&w->list, &xapi_wins);
	mutex_unlock(&xapi_gui_lock);

	if (put_user((u64)(unsigned long)w, (u64 __user *)(unsigned long)out_handle))
		return XAPI_ERR;
	return XAPI_OK;
}

static u64 xapi_gui_set_title(u64 handle, u64 str)
{
	struct xapi_win *w;
	char *t = NULL;

	mutex_lock(&xapi_gui_lock);
	w = xapi_win_get(handle);
	if (!w || xapi_copy_str(&t, str) < 0) {
		mutex_unlock(&xapi_gui_lock);
		return XAPI_ERR;
	}
	kfree(w->title);
	w->title = t;
	xapi_win_frame_draw(w);
	mutex_unlock(&xapi_gui_lock);
	return XAPI_OK;
}

static u64 xapi_gui_close_window(u64 handle)
{
	struct xapi_win *w;

	mutex_lock(&xapi_gui_lock);
	w = xapi_win_get(handle);
	if (!w) {
		mutex_unlock(&xapi_gui_lock);
		return XAPI_ERR;
	}
	list_del(&w->list);
	mutex_unlock(&xapi_gui_lock);
	vfree(w->buf);
	kfree(w->title);
	put_task_struct(w->owner);
	kfree(w);
	return XAPI_OK;
}

static u64 xapi_gui_get_window_size(u64 handle, u64 w_p, u64 h_p)
{
	struct xapi_win *w;
	u64 ret = XAPI_ERR;

	mutex_lock(&xapi_gui_lock);
	w = xapi_win_get(handle);
	if (w) {
		if (!w_p || !put_user(w->w, (u32 __user *)(unsigned long)w_p))
			if (!h_p || !put_user(w->h, (u32 __user *)(unsigned long)h_p))
				ret = XAPI_OK;
	}
	mutex_unlock(&xapi_gui_lock);
	return ret;
}

/* content-area bounds of a window (frame excluded for XWIN_NORMAL) */
static void xapi_win_content(struct xapi_win *w, u32 *cx, u32 *cy, u32 *cw, u32 *ch)
{
	if (w->type == XWIN_NORMAL) {
		*cx = XGUI_BORDER_X;
		*cy = XGUI_BORDER_T;
		*cw = w->w - 2 * XGUI_BORDER_X;
		*ch = w->h - XGUI_BORDER_T - XGUI_BORDER_B;
	} else {
		*cx = 0;
		*cy = 0;
		*cw = w->w;
		*ch = w->h;
	}
	if (*cw > w->w)
		*cw = w->w;
	if (*ch > w->h)
		*ch = w->h;
}

/* ----- drawing into the window backbuffer (content coordinates) ----- */

static bool xapi_win_px(struct xapi_win *w, u32 cx, u32 cy, u32 ccw, u32 cch, u32 x, u32 y, u32 c)
{
	if (x >= ccw || y >= cch)
		return false;
	w->buf[(y + cy) * w->w + (x + cx)] = c;
	return true;
}

static u64 xapi_gui_draw_point(u64 handle, u32 x, u32 y, u32 color)
{
	struct xapi_win *w;
	u32 cx, cy, cw, ch;
	u64 ret = XAPI_ERR;

	mutex_lock(&xapi_gui_lock);
	w = xapi_win_get(handle);
	if (w) {
		xapi_win_content(w, &cx, &cy, &cw, &ch);
		xapi_win_px(w, cx, cy, cw, ch, x, y, color);
		ret = XAPI_OK;
	}
	mutex_unlock(&xapi_gui_lock);
	return ret;
}

static u64 xapi_gui_draw_line(u64 handle, u32 x1, u32 y1, u32 x2, u32 y2, u32 color)
{
	struct xapi_win *w;
	u32 cx, cy, cw, ch;
	int dx = abs((int)x2 - (int)x1), sx = x1 < x2 ? 1 : -1;
	int dy = -abs((int)y2 - (int)y1), sy = y1 < y2 ? 1 : -1;
	int err = dx + dy, e2;
	u64 ret = XAPI_ERR;

	mutex_lock(&xapi_gui_lock);
	w = xapi_win_get(handle);
	if (w) {
		xapi_win_content(w, &cx, &cy, &cw, &ch);
		for (;;) {
			xapi_win_px(w, cx, cy, cw, ch, x1, y1, color);
			if (x1 == x2 && y1 == y2)
				break;
			e2 = 2 * err;
			if (e2 >= dy) { err += dy; x1 += sx; }
			if (e2 <= dx) { err += dx; y1 += sy; }
		}
		ret = XAPI_OK;
	}
	mutex_unlock(&xapi_gui_lock);
	return ret;
}

static u64 xapi_gui_draw_rect(u64 handle, u32 x1, u32 y1, u32 x2, u32 y2, u32 color, bool fill)
{
	struct xapi_win *w;
	u32 cx, cy, cw, ch, x, y;
	u64 ret = XAPI_ERR;

	if (x2 < x1) swap(x1, x2);
	if (y2 < y1) swap(y1, y2);
	mutex_lock(&xapi_gui_lock);
	w = xapi_win_get(handle);
	if (w) {
		xapi_win_content(w, &cx, &cy, &cw, &ch);
		if (fill) {
			for (y = y1; y <= y2 && y < ch; y++)
				for (x = x1; x <= x2 && x < cw; x++)
					xapi_win_px(w, cx, cy, cw, ch, x, y, color);
		} else {
			for (x = x1; x <= x2 && x < cw; x++) {
				xapi_win_px(w, cx, cy, cw, ch, x, y1, color);
				if (y2 < ch)
					xapi_win_px(w, cx, cy, cw, ch, x, y2, color);
			}
			for (y = y1; y <= y2 && y < ch; y++) {
				xapi_win_px(w, cx, cy, cw, ch, x1, y, color);
				if (x2 < cw)
					xapi_win_px(w, cx, cy, cw, ch, x2, y, color);
			}
		}
		ret = XAPI_OK;
	}
	mutex_unlock(&xapi_gui_lock);
	return ret;
}

static u64 xapi_gui_draw_circle(u64 handle, u32 xc, u32 yc, u32 r, u32 color, bool fill)
{
	struct xapi_win *w;
	u32 cx, cy, cw, ch;
	int x = r, y = 0, err = 1 - r;
	u64 ret = XAPI_ERR;

	mutex_lock(&xapi_gui_lock);
	w = xapi_win_get(handle);
	if (w) {
		xapi_win_content(w, &cx, &cy, &cw, &ch);
		while (x >= y) {
			if (fill) {
				int i;

				for (i = -x; i <= x; i++) {
					xapi_win_px(w, cx, cy, cw, ch, xc + i, yc + y, color);
					xapi_win_px(w, cx, cy, cw, ch, xc + i, yc - y, color);
				}
				for (i = -y; i <= y; i++) {
					xapi_win_px(w, cx, cy, cw, ch, xc + i, yc + x, color);
					xapi_win_px(w, cx, cy, cw, ch, xc + i, yc - x, color);
				}
			} else {
				xapi_win_px(w, cx, cy, cw, ch, xc + x, yc + y, color);
				xapi_win_px(w, cx, cy, cw, ch, xc - x, yc + y, color);
				xapi_win_px(w, cx, cy, cw, ch, xc + x, yc - y, color);
				xapi_win_px(w, cx, cy, cw, ch, xc - x, yc - y, color);
				xapi_win_px(w, cx, cy, cw, ch, xc + y, yc + x, color);
				xapi_win_px(w, cx, cy, cw, ch, xc - y, yc + x, color);
				xapi_win_px(w, cx, cy, cw, ch, xc + y, yc - x, color);
				xapi_win_px(w, cx, cy, cw, ch, xc - y, yc - x, color);
			}
			y++;
			if (err < 0) {
				err += 2 * y + 1;
			} else {
				x--;
				err += 2 * (y - x) + 1;
			}
		}
		ret = XAPI_OK;
	}
	mutex_unlock(&xapi_gui_lock);
	return ret;
}

/* ----- text rendering (kernel console font, nearest-neighbor scaled) ----- */

static u32 xapi_font_scale(u32 size)
{
	u32 s = size / 16;

	if (s == 0)
		s = 1;
	if (s > 8)
		s = 8;
	return s;
}

static const struct font_desc *xapi_font(void)
{
	static const struct font_desc *fd;

	if (!fd)
		fd = get_default_font(1024, 768, NULL, NULL);
	return fd;
}

static u32 xapi_text_width(u64 str, u32 size)
{
	char kbuf[256];
	int n;
	const struct font_desc *fd = xapi_font();

	if (!fd || !str)
		return 0;
	n = strncpy_from_user(kbuf, (char __user *)(unsigned long)str, sizeof(kbuf) - 1);
	if (n < 0)
		return 0;
	kbuf[n] = 0;
	return (u32)strlen(kbuf) * fd->width * xapi_font_scale(size);
}

/* raw text draw in window-buffer coordinates (used by the frame painter) */
static void xapi_win_draw_text_raw(struct xapi_win *w, const char *str, u32 x, u32 y, u32 color, u32 scale)
{
	const struct font_desc *fd = xapi_font();
	const u8 *fdata;
	u32 fw, fh, s;

	if (!fd || !str)
		return;
	fdata = fd->data;
	fw = fd->width;
	fh = fd->height;
	s = scale ? scale : 1;
	for (; *str && x + fw * s <= w->w; str++, x += fw * s) {
		const u8 *glyph = fdata + (u8)*str * fh;
		u32 i, j, kx, ky;

		for (j = 0; j < fh; j++) {
			u8 row = glyph[j];

			for (i = 0; i < fw && i < 8; i++) {
				if (!(row & (0x80 >> i)))
					continue;
				for (kx = 0; kx < s; kx++)
					for (ky = 0; ky < s; ky++) {
						u32 px = x + i * s + kx;
						u32 py = y + j * s + ky;

						if (px < w->w && py < w->h)
							w->buf[py * w->w + px] = color;
					}
			}
		}
	}
}

static u64 xapi_gui_draw_text(u64 handle, u32 x, u32 y, u64 str, u32 size, u32 color)
{
	struct xapi_win *w;
	char *t = NULL;
	u32 cx, cy, cw, ch, s;
	u64 ret = XAPI_ERR;

	mutex_lock(&xapi_gui_lock);
	w = xapi_win_get(handle);
	if (w && xapi_copy_str(&t, str) >= 0) {
		const struct font_desc *fd = xapi_font();
		const u8 *fdata = fd->data;
		u32 fw = fd->width, fh = fd->height;
		const char *p;

		xapi_win_content(w, &cx, &cy, &cw, &ch);
		if (cw)
			x %= cw;
		if (ch)
			y %= ch;
		s = xapi_font_scale(size);
		for (p = t; *p && x < cw; p++, x += fw * s) {
			const u8 *glyph = fdata + (u8)*p * fh;
			u32 i, j, kx, ky;

			for (j = 0; j < fh; j++) {
				u8 row = glyph[j];

				for (i = 0; i < fw && i < 8; i++) {
					if (!(row & (0x80 >> i)))
						continue;
					for (kx = 0; kx < s; kx++)
						for (ky = 0; ky < s; ky++) {
							u32 px = x + i * s + kx;
							u32 py = y + j * s + ky;

							if (px < cw && py < ch)
								w->buf[(py + cy) * w->w + px + cx] = color;
						}
				}
			}
		}
		kfree(t);
		ret = XAPI_OK;
	} else {
		kfree(t);
	}
	mutex_unlock(&xapi_gui_lock);
	return ret;
}

/* DrawText_L: x in the high 32 bits of "place", y in the low 32 bits */
static u64 xapi_gui_draw_text_l(u64 handle, u64 place, u64 str, u32 size, u32 color, u64 width_out)
{
	u32 x = (u32)(place >> 32);
	u32 y = (u32)place;
	u64 ret;

	ret = xapi_gui_draw_text(handle, x, y, str, size, color);
	if (ret == XAPI_OK && width_out) {
		u32 tw = xapi_text_width(str, size);

		if (put_user(tw, (u32 __user *)(unsigned long)width_out))
			return XAPI_ERR;
	}
	return ret;
}

/* DrawText_SW: "smooth" wide-char variant in XJ380; same bitmap rendering here */
static u64 xapi_gui_draw_text_sw(u64 handle, u32 x, u32 y, u64 str, u32 size, u32 color)
{
	return xapi_gui_draw_text(handle, x, y, str, size, color);
}

/* ----- pixel buffer I/O (content coordinates, stride = window width) ----- */

static bool xapi_buf_region(struct xapi_win *w, u32 x, u32 y, u32 width, u32 height,
			    u32 *bx, u32 *by, u32 *bw, u32 *bh)
{
	u32 cx, cy, cw, ch;

	xapi_win_content(w, &cx, &cy, &cw, &ch);
	if (width == 0 || height == 0 || x >= cw || y >= ch)
		return false;
	if (width > cw - x)
		width = cw - x;
	if (height > ch - y)
		height = ch - y;
	*bx = x + cx;
	*by = y + cy;
	*bw = width;
	*bh = height;
	return true;
}

static u64 xapi_gui_readbuffer(u64 handle, u32 x, u32 y, u32 width, u32 height, u64 buf, bool with_alpha)
{
	struct xapi_win *w;
	u32 bx, by, bw, bh, i, j;
	u64 ret = XAPI_ERR;

	mutex_lock(&xapi_gui_lock);
	w = xapi_win_get(handle);
	if (w && xapi_buf_region(w, x, y, width, height, &bx, &by, &bw, &bh)) {
		u8 __user *dst = (u8 __user *)(unsigned long)buf;
		size_t step = with_alpha ? 4 : 3;
		bool ok = true;

		for (j = 0; ok && j < bh; j++) {
			const u32 *row = &w->buf[(size_t)(j + by) * w->w + bx];

			for (i = 0; ok && i < bw; i++) {
				u8 px[4] = { xapi_px_r(row[i]), xapi_px_g(row[i]),
					     xapi_px_b(row[i]), xapi_px_a(row[i]) };

				ok = !copy_to_user(dst + ((size_t)j * bw + i) * step, px,
						   with_alpha ? 4 : 3);
			}
		}
		ret = ok ? XAPI_OK : XAPI_ERR;
	}
	mutex_unlock(&xapi_gui_lock);
	return ret;
}

static u64 xapi_gui_writebuffer(u64 handle, u32 x, u32 y, u32 width, u32 height, u64 buf, bool with_alpha)
{
	struct xapi_win *w;
	u32 bx, by, bw, bh, i, j;
	u64 ret = XAPI_ERR;

	mutex_lock(&xapi_gui_lock);
	w = xapi_win_get(handle);
	if (w && xapi_buf_region(w, x, y, width, height, &bx, &by, &bw, &bh)) {
		u8 __user *src = (u8 __user *)(unsigned long)buf;
		size_t step = with_alpha ? 4 : 3;
		bool ok = true;

		for (j = 0; ok && j < bh; j++) {
			u32 *row = &w->buf[(size_t)(j + by) * w->w + bx];

			for (i = 0; ok && i < bw; i++) {
				u8 px[4];

				if (copy_from_user(px, src + ((size_t)j * bw + i) * step,
						   with_alpha ? 4 : 3)) {
					ok = false;
					break;
				}
				row[i] = ((u32)px[0] << 24) | ((u32)px[1] << 16) |
					 ((u32)px[2] << 8) | (with_alpha ? px[3] : 0xFF);
			}
		}
		ret = ok ? XAPI_OK : XAPI_ERR;
	}
	mutex_unlock(&xapi_gui_lock);
	return ret;
}

/* ----- compositing / refresh ----- */

static bool xapi_win_covers(struct xapi_win *w, u32 gx, u32 gy)
{
	return w->visible && gx >= w->x && gy >= w->y &&
	       gx < w->x + w->w && gy < w->y + w->h;
}

/* composite rows [rx1,rx2) x [ry1,ry2) of the screen and blit them out */
static void xapi_composite(u32 rx1, u32 ry1, u32 rx2, u32 ry2)
{
	struct fb_info *info = xapi_fb;
	u32 *rowbuf;
	struct xapi_win *w;
	u32 y, x, sw, sh;

	if (!info)
		return;
	sw = info->var.xres;
	sh = info->var.yres;
	if (rx2 > sw)
		rx2 = sw;
	if (ry2 > sh)
		ry2 = sh;
	if (rx1 >= rx2 || ry1 >= ry2)
		return;

	rowbuf = kmalloc_array(sw, 4, GFP_KERNEL | __GFP_ZERO);
	if (!rowbuf)
		return;

	for (y = ry1; y < ry2; y++) {
		for (x = rx1; x < rx2; x++) {
			u32 c = XGUI_BG;

			/* painter's algorithm: bottom -> top, alpha blend */
			list_for_each_entry(w, &xapi_wins, list) {
				u32 *p, a, r, g, b;

				if (!xapi_win_covers(w, x, y))
					continue;
				p = &w->buf[(size_t)(y - w->y) * w->w + (x - w->x)];
				a = xapi_px_a(*p);
				if (a == 0xFF) {
					c = *p;
				} else if (a != 0) {
					r = (xapi_px_r(*p) * a + xapi_px_r(c) * (255 - a)) / 255;
					g = (xapi_px_g(*p) * a + xapi_px_g(c) * (255 - a)) / 255;
					b = (xapi_px_b(*p) * a + xapi_px_b(c) * (255 - a)) / 255;
					c = (r << 24) | (g << 16) | (b << 8) | 0xFF;
				}
			}
			rowbuf[x - rx1] = xapi_px_to_fb(NULL, info, c);
		}
		/* blit one composed row via fb ops (handles dirty tracking on
		 * DRM fbdev and direct HW on classic fbdev) */
		{
			struct fb_image img;

			memset(&img, 0, sizeof(img));
			img.dx = rx1;
			img.dy = y;
			img.width = rx2 - rx1;
			img.height = 1;
			img.depth = 32;
			img.data = (u8 *)rowbuf;
			cfb_imageblit(info, &img);
		}
	}
	kfree(rowbuf);
}

static u64 xapi_gui_refresh(u64 handle, bool part, u32 x1, u32 y1, u32 x2, u32 y2)
{
	struct xapi_win *w;
	u64 ret = XAPI_ERR;

	mutex_lock(&xapi_gui_lock);
	w = xapi_win_get(handle);
	if (w) {
		u32 rx1 = w->x, ry1 = w->y, rx2 = w->x + w->w, ry2 = w->y + w->h;

		if (part) {
			rx1 = w->x + min(x1, w->w - 1);
			ry1 = w->y + min(y1, w->h - 1);
			rx2 = w->x + min(x2 + 1, w->w);
			ry2 = w->y + min(y2 + 1, w->h);
		}
		xapi_composite(rx1, ry1, rx2, ry2);
		ret = XAPI_OK;
	}
	mutex_unlock(&xapi_gui_lock);
	return ret;
}

/* ----- BMP loading (uncompressed 24/32bpp, nearest-neighbor scale) ----- */

struct xapi_bmp {
	u32 *px;         /* width*height, 0xRRGGBBAA */
	u32 width, height;
};

static void xapi_bmp_free(struct xapi_bmp *b)
{
	if (b) {
		vfree(b->px);
		kfree(b);
	}
}

static struct xapi_bmp *xapi_bmp_load(u64 path)
{
	struct file *f;
	char *p = NULL;
	__le32 hdr[13]; /* BITMAPFILEHEADER(14) minus type + DIB header core */
	u32 data_off, dib, width, height, bpp, compression, row_size, i, j;
	u8 *row;
	struct xapi_bmp *b;
	loff_t pos = 0;
	bool topdown = false;
	int n;

	if (xapi_copy_str(&p, path) < 0)
		return NULL;
	if (!p) {
		kfree(p);
		return NULL;
	}
	f = filp_open(p, O_RDONLY, 0);
	kfree(p);
	if (IS_ERR(f))
		return NULL;

	n = kernel_read(f, hdr, sizeof(hdr), &pos);
	if (n != (int)sizeof(hdr))
		goto fail;
	if (hdr[0] != cpu_to_le32(0x4D42)) /* 'BM' + size low word */
		goto fail;
	data_off = le32_to_cpu(hdr[3]);
	dib = le32_to_cpu(hdr[4]);
	if (dib < 40)
		goto fail;
	{
		__le32 info[6];
		loff_t p2 = 14;

		if (kernel_read(f, info, sizeof(info), &p2) != (int)sizeof(info))
			goto fail;
		width = le32_to_cpu(info[1]);
		height = le32_to_cpu(info[2]);
		bpp = le16_to_cpu((__force __le16)info[4]);
		compression = le32_to_cpu(info[5]);
	}
	if (width == 0 || width > 8192)
		goto fail;
	if (height & 0x80000000) { /* top-down */
		topdown = true;
		height = -(__s32)height;
	}
	if (height == 0 || height > 8192 || bpp != 24 || compression != 0)
		goto fail;

	b = kmalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		goto fail;
	b->width = width;
	b->height = height;
	b->px = vzalloc((size_t)width * height * 4);
	row_size = (width * 3 + 3) & ~3u;
	row = kmalloc(row_size, GFP_KERNEL);
	if (!b->px || !row) {
		kfree(row);
		xapi_bmp_free(b);
		goto fail;
	}
	pos = data_off;
	for (j = 0; j < height; j++) {
		u32 dy = topdown ? j : height - 1 - j;

		if (kernel_read(f, row, row_size, &pos) != (int)row_size) {
			kfree(row);
			xapi_bmp_free(b);
			goto fail;
		}
		for (i = 0; i < width; i++) {
			u8 *q = row + i * 3;

			b->px[(size_t)dy * width + i] =
				((u32)q[2] << 24) | ((u32)q[1] << 16) | ((u32)q[0] << 8) | 0xFF;
		}
	}
	kfree(row);
	fput(f);
	return b;
fail:
	fput(f);
	return NULL;
}

static void xapi_bmp_blit(struct xapi_win *w, struct xapi_bmp *b, u32 x, u32 y, u32 width, u32 height)
{
	u32 cx, cy, cw, ch, i, j;

	xapi_win_content(w, &cx, &cy, &cw, &ch);
	if (width == 0)
		width = b->width;
	if (height == 0)
		height = b->height;
	for (j = 0; j < height; j++) {
		u32 sy = j * b->height / height;

		for (i = 0; i < width; i++) {
			u32 sx = i * b->width / width;
			u32 dx = x + i, dy = y + j;

			if (dx < cw && dy < ch)
				w->buf[(size_t)(dy + cy) * w->w + dx + cx] = b->px[(size_t)sy * b->width + sx];
		}
	}
}

static u64 xapi_gui_draw_bmp(u64 handle, u32 x, u32 y, u32 width, u32 height, u64 path)
{
	struct xapi_win *w;
	struct xapi_bmp *b;
	u64 ret = XAPI_ERR;

	b = xapi_bmp_load(path);
	mutex_lock(&xapi_gui_lock);
	w = xapi_win_get(handle);
	if (w && b) {
		xapi_bmp_blit(w, b, x, y, width, height);
		ret = XAPI_OK;
	}
	mutex_unlock(&xapi_gui_lock);
	xapi_bmp_free(b);
	return ret;
}

/* ----- message queue + callback registration ----- */

static bool xapi_msgq_push(struct xapi_win *w, u64 type, u64 h, u64 l)
{
	u32 next = (w->msg_tail + 1) % XAPI_MSGQ_LEN;

	if (next == w->msg_head)
		return false; /* queue full */
	w->msg[w->msg_tail * 3] = type;
	w->msg[w->msg_tail * 3 + 1] = h;
	w->msg[w->msg_tail * 3 + 2] = l;
	w->msg_tail = next;
	return true;
}

static u64 xapi_gui_set_msg_proc(u64 handle, u64 func)
{
	struct xapi_win *w;
	u64 ret = XAPI_ERR;

	mutex_lock(&xapi_gui_lock);
	w = xapi_win_get(handle);
	if (w) {
		w->msg_func = func;
		ret = XAPI_OK;
	}
	mutex_unlock(&xapi_gui_lock);
	return ret;
}

/*
 * Keyboard events -> MSG_KEYDOWN for all windows with a registered message
 * callback.  Linux cannot invoke a user-space function pointer from kernel
 * context, so the callback is NOT called here; instead the message is queued
 * and the owner task is SIGUSR2-woken (the xdesktop-side trampoline drains
 * the queue via the notification handler it installs in user space).
 */
static int xapi_kbd_event(struct notifier_block *nb, unsigned long event, void *_param)
{
	struct keyboard_notifier_param *param = _param;
	struct xapi_win *w;

	if (event != KBD_KEYCODE || !param)
		return NOTIFY_DONE;

	mutex_lock(&xapi_gui_lock);
	list_for_each_entry(w, &xapi_wins, list) {
		if (!w->msg_func || !w->owner)
			continue;
		if (xapi_msgq_push(w, 9 /* MSG_KEYDOWN */, param->value, param->down ? 1 : 0))
			send_sig(SIGUSR2, w->owner, 1);
	}
	mutex_unlock(&xapi_gui_lock);
	return NOTIFY_DONE;
}

static struct notifier_block xapi_kbd_nb = {
	.notifier_call = xapi_kbd_event,
};

/* ----- syscall dispatch ----- */

long xapi_gui_handle_syscall(const struct pt_regs *regs, unsigned int nr)
{
	u64 a1 = regs->si;
	u64 a2 = regs->dx;
	u64 a3 = regs->cx;
	u64 a4 = regs->r8;
	u64 a5 = regs->r9;
	u64 a6 = 0;

	/* 7th C argument lives at [rsp+8] (return address was saved). */
	if (user_access_begin(regs, sizeof(*regs))) {
		unsigned long sp = regs->sp;

		if (sp)
			a6 = (u64)*(unsigned long __user *)(sp + 8);
		user_access_end();
	}

	switch (nr) {
	case XAPI_CREATE_WINDOW:       return xapi_gui_create_window(a1, a2);
	case XAPI_SET_WINDOW_TITLE:    return xapi_gui_set_title(a1, a2);
	case XAPI_CLOSE_WINDOW:        return xapi_gui_close_window(a1);
	case XAPI_SET_ICON:            return XAPI_OK; /* TODO: WM icon support */
	case XAPI_GET_WIN_SZIE:        return xapi_gui_get_window_size(a1, a2, a3);
	case XAPI_DRAW_POINT:          return xapi_gui_draw_point(a1, (u32)a2, (u32)a3, (u32)a4);
	case XAPI_DRAW_LINE:           return xapi_gui_draw_line(a1, (u32)a2, (u32)a3, (u32)a4, (u32)a5, (u32)a6);
	case XAPI_DRAW_RECT:           return xapi_gui_draw_rect(a1, (u32)a2, (u32)a3, (u32)a4, (u32)a5, (u32)a6, false);
	case XAPI_DRAW_RECT_FILL:      return xapi_gui_draw_rect(a1, (u32)a2, (u32)a3, (u32)a4, (u32)a5, (u32)a6, true);
	case XAPI_DRAW_CIRCLE:         return xapi_gui_draw_circle(a1, (u32)a2, (u32)a3, (u32)a4, (u32)a5, false);
	case XAPI_DRAW_CIRCLE_FILL:    return xapi_gui_draw_circle(a1, (u32)a2, (u32)a3, (u32)a4, (u32)a5, true);
	case XAPI_DRAW_TEXT:           return xapi_gui_draw_text(a1, (u32)a2, (u32)a3, a4, (u32)a5, (u32)a6);
	case XAPI_DRAW_TEXT_L:         return xapi_gui_draw_text_l(a1, a2, a3, (u32)a4, (u32)a5, a6);
	case XAPI_DRAW_TEXT_SW:        return xapi_gui_draw_text_sw(a1, (u32)a2, (u32)a3, a4, (u32)a5, (u32)a6);
	case XAPI_CALC_TEXT_WIDTH:     return xapi_text_width(a2, (u32)a3);
	case XAPI_READBUFFER:          return xapi_gui_readbuffer(a1, (u32)a2, (u32)a3, (u32)a4, (u32)a5, a6, false);
	case XAPI_READBUFFERA:         return xapi_gui_readbuffer(a1, (u32)a2, (u32)a3, (u32)a4, (u32)a5, a6, true);
	case XAPI_WRITEBUFFER:         return xapi_gui_writebuffer(a1, (u32)a2, (u32)a3, (u32)a4, (u32)a5, a6, false);
	case XAPI_WRITEBUFFERA:        return xapi_gui_writebuffer(a1, (u32)a2, (u32)a3, (u32)a4, (u32)a5, a6, true);
	case XAPI_REFRESH_WINDOW:      return xapi_gui_refresh(a1, false, 0, 0, 0, 0);
	case XAPI_REFRESH_PART_WINDOW: return xapi_gui_refresh(a1, true, (u32)a2, (u32)a3, (u32)a4, (u32)a5);
	case XAPI_DRAW_BMP:            return xapi_gui_draw_bmp(a1, (u32)a2, (u32)a3, (u32)a4, (u32)a5, a6);
	case XAPI_DRAW_PICTURE:        return xapi_gui_draw_bmp(a1, (u32)a2, (u32)a3, (u32)a4, (u32)a5, a6);
	case XAPI_SET_MSH_PROCOR:      return xapi_gui_set_msg_proc(a1, a2);
	case XAPI_DRAW_PNG:
	case XAPI_DRAW_SVG:
	case XAPI_DRAW_FA:
	case XAPI_LOAD_PICTURE:
	case XAPI_GET_PIC_SIZE:
	case XAPI_BUTTON:
	case XAPI_BUTTON_EMP:
	default:
		return XAPI_ERR; /* P4.6 widgets + PNG/SVG/FA: not implemented yet */
	}
}

static int __init xapi_gui_init(void)
{
	register_keyboard_notifier(&xapi_kbd_nb);
	return 0;
}
subsys_initcall(xapi_gui_init);




