#ifndef MYUNIX_AUDIO_H
#define MYUNIX_AUDIO_H

#include <stddef.h>

#define AUDIO_HZ 22050u

/* SB16: unsigned 8-bit mono PCM out via /dev/audio at AUDIO_HZ. */
int audio_init(void);
int audio_write(const char *buf, size_t n);

#endif /* MYUNIX_AUDIO_H */