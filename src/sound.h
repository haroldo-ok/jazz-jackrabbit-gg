#ifndef SOUND_H
#define SOUND_H
#include "jazz_game.h"
void sound_init(void);
void sound_play(u8 id, u8 prio);              /* SFX_* clip, priority 1..3 */
void sound_events(u16 events, u8 sfxEvent);  /* the game's events of one pass */
void sound_update(void);                      /* frame interrupt */
void sound_silence(void);
#endif
