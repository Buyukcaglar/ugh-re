/* amiga_data.h - finds the Amiga version's TFMX module (mdat.UGH!-All_Sounds.o and
 * smpl.UGH!-All_Sounds) in the player's Amiga disk image (an AmigaDOS OFS/FFS .adf) and hands it
 * to amiga_audio.c. */
#ifndef UGH_AMIGA_DATA_H
#define UGH_AMIGA_DATA_H

#include <stddef.h>
#include <stdint.h>

/* 1 when the disk held the module and the Amiga sound is ready */
int amiga_data_load_adf(const uint8_t *adf, size_t len);

/* a file of an AmigaDOS disk image by name (case-insensitive, in any directory); a malloc'd copy
 * the caller frees, or NULL */
uint8_t *adf_read_file(const uint8_t *adf, size_t len, const char *name, size_t *out_len);

#endif
