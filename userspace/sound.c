/*
 * sound: play unsigned 8-bit mono PCM at 22050 Hz through /dev/audio.
 *   sound           - synth demo tones
 *   sound <file>    - stream a raw .pcm/.raw file byte by byte
 */
#include "libc.h"

#define RATE 22050
#define WND  512

static int audio_fd;

static void stream(const char *buf, long n) {
    long off = 0;
    while (off < n) {
        long w = write(audio_fd, buf + off, (unsigned long)(n - off));
        if (w <= 0) return;
        off += w;
    }
}

static void synth_tone(int hz, int ms) {
    long total = (long)RATE * (long)ms / 1000;
    unsigned inc = (unsigned)(((unsigned long)hz * 256u) / RATE);
    unsigned phase = 0;
    unsigned char chunk[WND];
    long i = 0;

    while (i < total) {
        long n = 0;
        while (i < total && n < WND) {
            unsigned t;
            if (phase < 128u)
                t = phase * 2u;
            else
                t = 512u - phase * 2u;
            chunk[n++] = (unsigned char)
                (128 + (int)((int)t - 128) * 96 / 128);
            phase = (phase + inc) & 0xFFu;
            i++;
        }
        stream((const char *)chunk, (long)n);
    }
}

int main(int argc, char **argv) {
    audio_fd = open("/dev/audio");
    if (audio_fd < 0) {
        print("sound: /dev/audio not available\n");
        return 1;
    }

    if (argc > 1) {
        int f = open(argv[1]);
        if (f < 0) {
            print("sound: cannot open ");
            print(argv[1]);
            print("\n");
            close(audio_fd);
            return 1;
        }
        print("sound: playing ");
        print(argv[1]);
        print("\n");
        {
            unsigned char buf[WND];
            for (;;) {
                long r = read(f, buf, WND);
                if (r <= 0) break;
                stream((const char *)buf, r);
            }
        }
        close(f);
    } else {
        print("sound: play demo\n");
        synth_tone(440, 600);
        synth_tone(554, 450);
        synth_tone(880, 900);
    }

    close(audio_fd);
    return 0;
}