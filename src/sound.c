/* Sound effects on the Game Gear PSG, stepped from the frame interrupt
 * right after the music (music.c), whose tone 2 / noise channel they borrow.
 *
 * The Genesis port plays the original SOUNDS.000 clips and the four weapon
 * clips through its PCM channels.  Here every clip has been converted
 * offline (tools/psgsfx.py) into a frame-by-frame PSG part: loudness, pitch
 * on tone channel 2, and the unpitched frames on the noise channel.  The
 * events pick clips the way the Genesis update_sfx() does; a clip only
 * interrupts one of lower or equal priority.  While a clip plays, the music
 * goes on silently on the borrowed channels and takes them back afterwards. */

#include <SMSlib.h>
#include "gg_data.h"
#include "sound.h"
#include "music.h"
#include "fe.h"

__sfr __at 0x7F PSGPort;
__sfr __at 0x06 GGStereoPort;

static volatile u8 sfxOn, sfxPrio;
static const u8 *volatile sfxPtr;
static u8 sfxUsed;      /* channels this clip has used: bit 2 tone 2, bit 3 noise */

static void sfx_claim(u8 mask)
{
    u8 drop = musicSfxMask & (u8)~mask;
    if (drop & 4) { PSGPort = 0xDF; music_release(2); }
    if (drop & 8) { PSGPort = 0xFF; music_release(3); }
    musicSfxMask = mask;
}

void sound_silence(void)
{
    __asm__("di");
    sfxOn = 0;
    sfxPrio = 0;
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

/* Start clip `id` (SFX_*) at priority `prio` (1..3). */
void sound_play(u8 id, u8 prio)
{
    u8 save;
    if (!id || id > SFX_COUNT || !feOptions.sfx) return;
    if (sfxOn && prio < sfxPrio) return;
    __asm__("di");
    save = SMS_getROMBank();
    SMS_mapROMBank(SFX_BANK);
    sfxPtr = (const u8 *)SFX_TABLE[id - 1];
    SMS_mapROMBank(save);
    sfxPrio = prio;
    sfxOn = 1;
    sfxUsed = 0;
    __asm__("ei");
}

/* Weapon clips: blaster, toaster, upward (photon), bouncer (rocket); the
 * fifth shot has none in the original. */
static const u8 shotSfx[JAZZ_SHOT_TYPES] = {
    SFX_SHOT_BLASTER, SFX_SHOT_TOASTER, SFX_SHOT_UPWARD, SFX_SHOT_BOUNCER, 0
};
static u8 prevShield, prevBird;

/* The Genesis update_sfx(), event for event. */
void sound_events(u16 events, u8 sfxEvent)
{
    if (game.player.shield && !prevShield) sound_play(SFX_INVULN, 2);
    if (game.player.bird && !prevBird) sound_play(SFX_BIRDY, 2);
    prevShield = game.player.shield ? 1 : 0;
    prevBird = game.player.bird ? 1 : 0;
    if (events & JAZZ_EVENT_HURT) sound_play(SFX_OW, 2);
    else if (events & JAZZ_EVENT_KILL) sound_play(sfxEvent ? sfxEvent : SFX_BOOM, 2);
    else if (events & JAZZ_EVENT_FIRE) sound_play(shotSfx[game.player.shotType], 1);
    if (events & JAZZ_EVENT_EXIT) sound_play(SFX_YEAHOO, 3);
    else if (events & JAZZ_EVENT_SPRING) sound_play(SFX_SPRING, 1);
    else if (events & JAZZ_EVENT_JUMP) sound_play(SFX_JUMP, 1);
    if (events & JAZZ_EVENT_LIFE) sound_play(SFX_ONEUP, 3);
    else if (sfxEvent && !(events & (JAZZ_EVENT_HURT | JAZZ_EVENT_KILL))) sound_play(sfxEvent, 1);
    else if (events & (JAZZ_EVENT_GEM | JAZZ_EVENT_HEALTH)) sound_play(SFX_YUM, 1);
}

/* One video frame of the current clip; called from the frame interrupt. */
void sound_update(void)
{
    u8 save, b0, b1, b2;
    if (!sfxOn) return;
    save = SMS_getROMBank();
    SMS_mapROMBank(SFX_BANK);
    b0 = sfxPtr[0];
    b1 = sfxPtr[1];
    b2 = sfxPtr[2];
    SMS_mapROMBank(save);
    sfxPtr += 3;
    if (b0 & 0x10) {
        /* noise frame: tone 2 rests */
        sfx_claim(sfxUsed | 8 | 4);
        sfxUsed |= 12;
        PSGPort = 0xDF;
        PSGPort = (u8)(0xE0 | (b1 & 7));
        PSGPort = (u8)(0xF0 | (b0 & 15));
    } else {
        sfx_claim(sfxUsed | 4);
        sfxUsed |= 4;
        if (sfxUsed & 8) PSGPort = 0xFF;
        PSGPort = (u8)(0xC0 | (b1 & 0x0F));
        PSGPort = (u8)(((b2 << 4) | (b1 >> 4)) & 0x3F);
        PSGPort = (u8)(0xD0 | (b0 & 15));
    }
    if (b0 & 0x80) {
        sfxOn = 0;
        sfxPrio = 0;
        sfx_claim(0);
    }
}
