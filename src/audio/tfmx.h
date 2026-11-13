/* tfmx.h - the TFMX player of the Amiga version (its TFMX.obj, translated from 68000 code to C) and
 * the four Paula channels it drives.
 *
 * The player runs at the Amiga's 50 Hz vertical blank, as the original did; tfmx_render makes
 * 44100 Hz stereo from it. */
#ifndef UGH_TFMX_H
#define UGH_TFMX_H

#include <stddef.h>
#include <stdint.h>

/* the module: mdat (song data) and smpl (samples), both kept by the caller for as long as the
 * player runs; smpl is written to (the player clears its first four bytes, its silent loop). */
int tfmx_init(const uint8_t *mdat, size_t mdat_len, uint8_t *smpl, size_t smpl_len);

void tfmx_song(int song);           /* start subsong 0..31 (the player's "play song") */
void tfmx_fx(int fx);               /* start a sound effect from the table at mdat+200h */
void tfmx_note(uint32_t note);     /* a note straight to NotePort (the game's effects: 14MM3300) */
void tfmx_fade(int speed, int vol); /* fade the master volume to vol (0..64), 1 step per speed calls */
void tfmx_stop(void);               /* all channels off, the song stopped */
int tfmx_playing(void);             /* 1 while a song runs */

void tfmx_render(int *left, int *right);   /* the next 44100 Hz sample pair */

#endif
