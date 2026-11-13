#ifndef UGH_AUDIO_WIN32_H
#define UGH_AUDIO_WIN32_H
#include <stdint.h>
int audio_open(void);
void audio_close(void);
void audio_push(const int16_t *stereo, int frames);
void audio_set_muted(int muted);
#endif
