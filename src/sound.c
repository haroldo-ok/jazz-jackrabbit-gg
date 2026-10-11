/* Sound effects on the Game Gear PSG, stepped from the frame interrupt
 * right after the music (music.c), whose tone 2 / noise channel they borrow.
 *
 * The Genesis port plays the original JJ1 PCM clips (gun, jump, pickup)
 * through the YM2612's DAC.  The GG has only the SN76489 PSG, so each clip is
 * replaced by a short synthesized envelope with the same role: a rising chirp
 * for the jump, a noise crack for the gun, a two-tone blip for pickups and a
 * falling buzz for damage.  Effects use tone channel 2 and the noise channel;
 * while one plays, the music goes on silently on that channel and takes it
 * back afterwards. */

#include <SMSlib.h>
#include "sound.h"
#include "music.h"

__sfr __at 0x7F PSGPort;
__sfr __at 0x06 GGStereoPort;

enum { SFX_NONE, SFX_JUMP, SFX_FIRE, SFX_PICKUP, SFX_HURT, SFX_LIFE };

static volatile u8 sfx, sfxTime;
static u16 sfxTone;
static const u8 sfxChannels[6] = { 0, 4, 8, 4, 12, 4 };   /* bit 2 tone 2, bit 3 noise */

/* Take the channels the effect needs; give back the ones it no longer uses. */
static void sfx_claim(u8 mask)
{
    u8 drop = musicSfxMask & (u8)~mask;
    if (drop & 4) { PSGPort = 0xDF; music_release(2); }
    if (drop & 8) { PSGPort = 0xFF; music_release(3); }
    musicSfxMask = mask;
}

static void tone2(u16 n, u8 vol)
{
    PSGPort = (u8)(0xC0 | (n & 0x0F));
    PSGPort = (u8)((n >> 4) & 0x3F);
    PSGPort = (u8)(0xD0 | (vol & 0x0F));
}

static void noise(u8 mode, u8 vol)
{
    PSGPort = (u8)(0xE0 | (mode & 0x07));
    PSGPort = (u8)(0xF0 | (vol & 0x0F));
}

/* Stop the sound effect (the music is controlled with music_play). */
void sound_silence(void)
{
    __asm__("di");
    sfx = SFX_NONE;
    sfx_claim(0);
    __asm__("ei");
}

void sound_init(void)
{
    GGStereoPort = 0xFF;
    PSGPort = 0x9F;
    PSGPort = 0xBF;
    PSGPort = 0xDF;
    PSGPort = 0xFF;
}

void sound_events(u8 events)
{
    u8 want = SFX_NONE;
    if (events & JAZZ_EVENT_HURT) want = SFX_HURT;
    else if (events & JAZZ_EVENT_LIFE) want = SFX_LIFE;
    else if (events & JAZZ_EVENT_GEM) want = SFX_PICKUP;
    else if (events & JAZZ_EVENT_JUMP) want = SFX_JUMP;
    else if (events & JAZZ_EVENT_FIRE) want = SFX_FIRE;
    if (want == SFX_NONE) return;
    /* don't let a gunshot cut off damage or a pickup */
    if ((sfx == SFX_HURT || sfx == SFX_LIFE) && sfxTime > 4 && want != SFX_HURT) return;
    __asm__("di");
    sfx = want;
    sfxTime = 0;
    switch (want) {
    case SFX_JUMP: sfxTone = 380; break;
    case SFX_PICKUP: sfxTone = 120; break;
    case SFX_HURT: sfxTone = 500; break;
    case SFX_LIFE: sfxTone = 200; break;
    default: break;
    }
    sfx_claim(sfxChannels[want]);
    __asm__("ei");
}

/* One video frame of the current effect; called from the frame interrupt. */
void sound_update(void)
{
    u8 t;
    if (sfx == SFX_NONE) return;
    t = sfxTime++;
    switch (sfx) {
    case SFX_JUMP:
        if (t >= 12) break;
        tone2(sfxTone, (u8)(2 + (t >> 1)));
        sfxTone -= 20;
        return;
    case SFX_FIRE:
        if (t >= 6) break;
        noise(t < 2 ? 4 : 5, (u8)(1 + t * 2));
        return;
    case SFX_PICKUP:
        if (t >= 10) break;
        tone2(t < 4 ? 120 : 90, (u8)(2 + (t >> 1)));
        return;
    case SFX_HURT:
        if (t >= 14) break;
        tone2(sfxTone, (u8)(1 + (t >> 1)));
        noise(6, (u8)(6 + (t >> 1)));
        sfxTone += 24;
        return;
    case SFX_LIFE:
        if (t >= 24) break;
        tone2((t & 4) ? 160 : 107, (u8)(2 + (t >> 2)));
        return;
    }
    sfx = SFX_NONE;
    sfx_claim(0);
}
