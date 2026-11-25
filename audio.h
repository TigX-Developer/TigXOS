#ifndef TIGX_AUDIO_H
#define TIGX_AUDIO_H
#include <stdint.h>
void audio_init(void);
int  audio_present(void);
const char *audio_backend_name(void);
void audio_beep(uint32_t freq_hz, uint32_t ms);
int  audio_play_tone(uint32_t freq_hz, uint32_t ms); /* AC97 PCM or speaker */
#endif
