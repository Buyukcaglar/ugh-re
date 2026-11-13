#ifndef UGH_PNGW_H
#define UGH_PNGW_H
#include <stdint.h>
int png_write_rgb(const char *path, int w, int h, const uint32_t *px);   /* px: 0x00RRGGBB */
#endif
