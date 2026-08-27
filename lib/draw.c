#include <string.h>
#include "draw.h"

void draw_init (draw_buf *b, unsigned int *px, int w, int h, int stride,
                int argb)
{
    b->px = px;
    b->w = w;
    b->h = h;
    b->stride = (stride > w) ? stride : w;
    b->argb = argb;
    b->clip_on = 0;
    draw_damage_reset (b);
}

void draw_damage_reset (draw_buf *b)
{
    b->dx0 = b->w;
    b->dy0 = b->h;
    b->dx1 = 0;
    b->dy1 = 0;
}

int draw_damaged (const draw_buf *b, int *x, int *y, int *w, int *h)
{
    if (b->dx1 <= b->dx0 || b->dy1 <= b->dy0)
    {
        return 0;
    }
    *x = b->dx0;
    *y = b->dy0;
    *w = b->dx1 - b->dx0;
    *h = b->dy1 - b->dy0;

    return 1;
}

void draw_note (draw_buf *b, int x0, int y0, int x1, int y1)
{
    if (x0 < b->dx0) b->dx0 = x0 < 0 ? 0 : x0;
    if (y0 < b->dy0) b->dy0 = y0 < 0 ? 0 : y0;
    if (x1 > b->dx1) b->dx1 = x1 > b->w ? b->w : x1;
    if (y1 > b->dy1) b->dy1 = y1 > b->h ? b->h : y1;
}

void draw_damage_all (draw_buf *b)
{
    b->dx0 = 0;
    b->dy0 = 0;
    b->dx1 = b->w;
    b->dy1 = b->h;
}

static unsigned int pixel_of (const draw_buf *b, unsigned long colour)
{
    unsigned int a, r, g, bl;

    if (!b->argb)
    {
        return 0xff000000u | (unsigned int) (colour & 0xffffff);
    }
    /* Wayland ARGB is premultiplied; X11's is not, so multiply through here */
    a = (unsigned int) (colour >> 24) & 0xff;
    r = ((unsigned int) (colour >> 16) & 0xff) * a / 255;
    g = ((unsigned int) (colour >> 8) & 0xff) * a / 255;
    bl = ((unsigned int) colour & 0xff) * a / 255;

    return (a << 24) | (r << 16) | (g << 8) | bl;
}

/* The box the caller may touch: the buffer cut down by the clip if it is on */
static void bounds (const draw_buf *b, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = 0;
    *y0 = 0;
    *x1 = b->w;
    *y1 = b->h;
    if (b->clip_on)
    {
        if (b->cx > *x0) *x0 = b->cx;
        if (b->cy > *y0) *y0 = b->cy;
        if (b->cx + b->cw < *x1) *x1 = b->cx + b->cw;
        if (b->cy + b->ch < *y1) *y1 = b->cy + b->ch;
    }
}

void draw_fill (draw_buf *b, unsigned long colour, int x, int y, int w, int h)
{
    unsigned int p = pixel_of (b, colour);
    int x0, y0, x1, y1, line, col;

    bounds (b, &x0, &y0, &x1, &y1);
    if (x > x0) x0 = x;
    if (y > y0) y0 = y;
    if (x + w < x1) x1 = x + w;
    if (y + h < y1) y1 = y + h;
    if (x0 >= x1 || y0 >= y1)
    {
        return;
    }
    for (col = x0; col < x1; col++)
    {
        b->px[(size_t) y0 * b->stride + col] = p;
    }
    /* The rest of the block is that line again: one memcpy a line beats a
       store loop the compiler will not vectorise at -O2 */
    for (line = y0 + 1; line < y1; line++)
    {
        memcpy (b->px + (size_t) line * b->stride + x0,
                b->px + (size_t) y0 * b->stride + x0,
                (size_t) (x1 - x0) * 4);
    }
    draw_note (b, x0, y0, x1, y1);
}

void draw_rect (draw_buf *b, unsigned long colour, int x, int y, int w, int h,
                int thick)
{
    /* X draws the outline on the rectangle, so the box is w+1 by h+1 */
    draw_fill (b, colour, x, y, w + 1, thick);
    draw_fill (b, colour, x, y + h + 1 - thick, w + 1, thick);
    draw_fill (b, colour, x, y, thick, h + 1);
    draw_fill (b, colour, x + w + 1 - thick, y, thick, h + 1);
}

/* Convex only, which is all the suite draws: triangles */
void draw_poly (draw_buf *b, unsigned long colour, const bw_point *p, int n)
{
    unsigned int px = pixel_of (b, colour);
    int x0, y0, x1, y1, line, i, col;
    int top = 0, bot = 0;

    if (n < 3)
    {
        return;
    }
    bounds (b, &x0, &y0, &x1, &y1);
    top = bot = p[0].y;
    for (i = 1; i < n; i++)
    {
        if (p[i].y < top) top = p[i].y;
        if (p[i].y > bot) bot = p[i].y;
    }
    if (top < y0) top = y0;
    if (bot > y1 - 1) bot = y1 - 1;

    for (line = top; line <= bot; line++)
    {
        int lo = b->w, hi = -1;

        for (i = 0; i < n; i++)
        {
            const bw_point *a = &p[i], *c = &p[(i + 1) % n];
            int ay = a->y, cy = c->y, ax = a->x, cx = c->x, t, xx;

            if (ay > cy)
            {
                t = ay; ay = cy; cy = t;
                t = ax; ax = cx; cx = t;
            }
            if (line < ay || line > cy)
            {
                continue;
            }
            xx = (cy == ay) ? (ax < cx ? ax : cx)
                            : ax + (cx - ax) * (line - ay) / (cy - ay);
            if (xx < lo) lo = xx;
            if (cy == ay)
            {
                xx = ax > cx ? ax : cx;
            }
            if (xx > hi) hi = xx;
        }
        if (lo < x0) lo = x0;
        if (hi > x1 - 1) hi = x1 - 1;
        if (lo > hi)
        {
            continue;
        }
        {
            unsigned int *out = b->px + (size_t) line * b->stride + lo;

            for (col = lo; col <= hi; col++)
            {
                *out++ = px;
            }
        }
        draw_note (b, lo, line, hi + 1, line + 1);
    }
}

/* One byte a column, top line in the low bit. Every session has to render
   the same pixels for the same string */
static const unsigned char font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x00,0x5f,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7f,0x14,0x7f,0x14}, /* # */
    {0x24,0x2a,0x7f,0x2a,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1c,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1c,0x00}, /* ) */
    {0x14,0x08,0x3e,0x08,0x14}, /* * */
    {0x08,0x08,0x3e,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3e,0x51,0x49,0x45,0x3e}, /* 0 */
    {0x00,0x42,0x7f,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4b,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7f,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3c,0x4a,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1e}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3e}, /* @ */
    {0x7e,0x11,0x11,0x11,0x7e}, /* A */
    {0x7f,0x49,0x49,0x49,0x36}, /* B */
    {0x3e,0x41,0x41,0x41,0x22}, /* C */
    {0x7f,0x41,0x41,0x22,0x1c}, /* D */
    {0x7f,0x49,0x49,0x49,0x41}, /* E */
    {0x7f,0x09,0x09,0x09,0x01}, /* F */
    {0x3e,0x41,0x49,0x49,0x7a}, /* G */
    {0x7f,0x08,0x08,0x08,0x7f}, /* H */
    {0x00,0x41,0x7f,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3f,0x01}, /* J */
    {0x7f,0x08,0x14,0x22,0x41}, /* K */
    {0x7f,0x40,0x40,0x40,0x40}, /* L */
    {0x7f,0x02,0x0c,0x02,0x7f}, /* M */
    {0x7f,0x04,0x08,0x10,0x7f}, /* N */
    {0x3e,0x41,0x41,0x41,0x3e}, /* O */
    {0x7f,0x09,0x09,0x09,0x06}, /* P */
    {0x3e,0x41,0x51,0x21,0x5e}, /* Q */
    {0x7f,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7f,0x01,0x01}, /* T */
    {0x3f,0x40,0x40,0x40,0x3f}, /* U */
    {0x1f,0x20,0x40,0x20,0x1f}, /* V */
    {0x3f,0x40,0x38,0x40,0x3f}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x7f,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x00,0x41,0x41,0x7f,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7f,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7f}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7e,0x09,0x01,0x02}, /* f */
    {0x0c,0x52,0x52,0x52,0x3e}, /* g */
    {0x7f,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7d,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3d,0x00}, /* j */
    {0x7f,0x10,0x28,0x44,0x00}, /* k */
    {0x00,0x41,0x7f,0x40,0x00}, /* l */
    {0x7c,0x04,0x18,0x04,0x78}, /* m */
    {0x7c,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7c,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7c}, /* q */
    {0x7c,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3f,0x44,0x40,0x20}, /* t */
    {0x3c,0x40,0x40,0x20,0x7c}, /* u */
    {0x1c,0x20,0x40,0x20,0x1c}, /* v */
    {0x3c,0x40,0x30,0x40,0x3c}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0c,0x50,0x50,0x50,0x3c}, /* y */
    {0x44,0x64,0x54,0x4c,0x44}, /* z */
    {0x00,0x08,0x36,0x41,0x00}, /* { */
    {0x00,0x00,0x7f,0x00,0x00}, /* | */
    {0x00,0x41,0x36,0x08,0x00}, /* } */
    {0x08,0x08,0x2a,0x1c,0x08}, /* ~ */
};

void draw_text (draw_buf *b, unsigned long colour, int x, int y, const char *s,
                int scale)
{
    unsigned int p = pixel_of (b, colour);
    int x0, y0, x1, y1, cx = x;

    bounds (b, &x0, &y0, &x1, &y1);
    /* The baseline sits under the 7 lines, the way X core text is placed */
    y -= 7 * scale;
    for (; *s != '\0'; s++, cx += 6 * scale)
    {
        int c = (unsigned char) *s, col, line;

        if (c < 0x20 || c > 0x7e)
        {
            continue;
        }
        for (col = 0; col < 5; col++)
        {
            unsigned char bits = font5x7[c - 0x20][col];

            for (line = 0; line < 7; line++)
            {
                int px0, py0, sy, sx;

                if (!(bits & (1u << line)))
                {
                    continue;
                }
                px0 = cx + col * scale;
                py0 = y + line * scale;
                for (sy = 0; sy < scale; sy++)
                {
                    unsigned int *out;

                    if (py0 + sy < y0 || py0 + sy >= y1)
                    {
                        continue;
                    }
                    out = b->px + (size_t) (py0 + sy) * b->stride;
                    for (sx = 0; sx < scale; sx++)
                    {
                        if (px0 + sx >= x0 && px0 + sx < x1)
                        {
                            out[px0 + sx] = p;
                        }
                    }
                }
            }
        }
    }
    draw_note (b, x, y, cx, y + 7 * scale);
}

void draw_clip (draw_buf *b, int x, int y, int w, int h)
{
    if (w < 0)
    {
        b->clip_on = 0;

        return;
    }
    b->clip_on = 1;
    b->cx = x;
    b->cy = y;
    b->cw = w;
    b->ch = h;
}

void draw_copy (draw_buf *dst, const draw_buf *src, int sx, int sy,
                int w, int h, int dx, int dy)
{
    int line;

    /* Trim to what both buffers really hold, and to the destination's clip,
       the way a server copy is cut by the GC's */
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    if (dst->clip_on)
    {
        if (dx < dst->cx) { w -= dst->cx - dx; sx += dst->cx - dx; dx = dst->cx; }
        if (dy < dst->cy) { h -= dst->cy - dy; sy += dst->cy - dy; dy = dst->cy; }
        if (dx + w > dst->cx + dst->cw) w = dst->cx + dst->cw - dx;
        if (dy + h > dst->cy + dst->ch) h = dst->cy + dst->ch - dy;
    }
    if (w <= 0 || h <= 0)
    {
        return;
    }
    /* Downwards within one buffer would smear, the way memcpy does and
       XCopyArea does not; those lines are carried bottom up instead */
    if (dst->px == src->px && dy > sy)
    {
        for (line = h - 1; line >= 0; line--)
        {
            memmove (dst->px + (size_t) (dy + line) * dst->stride + dx,
                     src->px + (size_t) (sy + line) * src->stride + sx,
                     (size_t) w * 4);
        }
    }
    else
    {
        for (line = 0; line < h; line++)
        {
            memmove (dst->px + (size_t) (dy + line) * dst->stride + dx,
                     src->px + (size_t) (sy + line) * src->stride + sx,
                     (size_t) w * 4);
        }
    }
    draw_note (dst, dx, dy, dx + w, dy + h);
}
