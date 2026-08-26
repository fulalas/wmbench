/* BENCH_CAPTURE_CMD gets one argument, a path, and must write a full-screen
   binary PPM (P6) there */
#ifndef BENCH_CAPTURE_H
#define BENCH_CAPTURE_H

typedef struct {
    int width, height;
    void *xim;                  /* an XImage, when the X server answered */
    unsigned char *rgb;         /* 3 bytes a pixel, when a screenshot file did */
} bw_image;

unsigned long bw_pixel (const bw_image *, int x, int y);
void bw_image_free (bw_image *);

/* True when the whole file is written */
int capture_write_ppm (const char *path, const bw_image *);

/*
 * The file must be exactly screen_w x screen_h or nothing is returned: a
 * check that quietly looks at the wrong place is worse than one that says it
 * could not look. scale maps the caller's coordinates onto the file's pixels.
 */
bw_image *capture_via_cmd (int x, int y, int w, int h, int scale,
                           int screen_w, int screen_h);
bw_image *capture_wrap_ximage (void *xim);

#endif
