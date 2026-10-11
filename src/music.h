#ifndef MUSIC_H
#define MUSIC_H
/* Original JJ1 songs arranged for the PSG (see music.c). */
void music_play(unsigned char song);   /* SONG_* from gg_data.h, 0xFF = stop */
void music_frame(void);                /* frame interrupt */
void music_pause(unsigned char on);
void music_release(unsigned char ch);  /* a sound effect hands channel ch back */
extern unsigned char musicSfxMask;     /* channels owned by sound effects */
#endif
