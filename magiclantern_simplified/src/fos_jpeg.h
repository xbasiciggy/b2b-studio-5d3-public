#ifndef _fos_jpeg_h_
#define _fos_jpeg_h_

#include <stdint.h>

/* what goes into the JPEG's EXIF (any may be NULL) */
struct fos_jpeg_meta
{
    const char *when;           /* "2026-10-22 17:45" */
    const char *desc;           /* the film */
    const char *fstop;          /* "2.8" */
    const uint8_t *exif_blob;
    int exif_length;
};

/* width x height RGB pixels (rows stride bytes apart) as a baseline JPEG
 * at path; quality 1..100. 1 when written. */
int fos_jpeg_write(const char *path, const uint8_t *rgb, int stride, int width, int height,
                   int quality, const struct fos_jpeg_meta *meta);

void *fos_jpeg_begin(const char *, int, int, int, const struct fos_jpeg_meta *);
int fos_jpeg_rows(void *, const uint8_t *, int, int);
int fos_jpeg_end(void *);
#include <stdint.h>
extern uintptr_t fos_jpeg_state_addr;     /* address of the encoder state just allocated (diagnostics) */
#endif
