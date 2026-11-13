/* amiga_audio.h - the Amiga version's music and sound effects: its TFMX module (mdat + smpl) played
 * by tfmx.c through a four-channel Paula (paula.c), started by the DOS game's own sound calls
 * (game.c), and heard in place of the DOS sound when the player chooses it. */
#ifndef UGH_AMIGA_AUDIO_H
#define UGH_AMIGA_AUDIO_H

#include <stddef.h>
#include <stdint.h>

/* the TFMX module: mdat (song data) and smpl (samples); 1 when they are valid.  The data is copied. */
int amiga_audio_load(const uint8_t *mdat, size_t mdat_len, const uint8_t *smpl, size_t smpl_len);
int amiga_audio_ready(void);

/* the next 44100 Hz stereo sample */
void amiga_audio_sample(int *left, int *right);

/* the game's requests */
void amiga_audio_song(int song);      /* start subsong `song` (-1: stop the music) */
void amiga_audio_fx(int fx);          /* start sound effect `fx` */
void amiga_audio_stop(void);          /* everything quiet */

#endif
