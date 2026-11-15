/* Music player: the original MS-DOS songs, arranged for the PSG offline by
 * tools/psgmusic.py (whose RefPlayer class is the exact model of this file).
 *
 * Runs from the frame interrupt, one step per video frame, so the tempo does
 * not depend on how long the game loop takes.  A song's home block (order
 * table and envelopes) sits in one ROM bank and its pattern segments in any
 * banks; the step pages the segment's bank into slot 2 while it reads events
 * and the home bank while it steps the envelopes, then puts the previous
 * mapping back.
 *
 * Home block: [orders][restart (0xFF = play once)][envelope table ptr]
 *             [order -> segment ptr, segment bank]... [envelope ptrs]... envelopes.
 * Segment bytes: 0 end, 1..63 wait n frames, 0x40+ch note (note, env, base),
 * 0x44+ch note joined late (+ frames to skip), 0x48+ch off, 0x4C+ch base,
 * 0x50+ch new note keeping the envelope, 0x54+ch exact tone period (2 bytes,
 * kept until the next note; slides, vibrato and bends), 0x58+ch exact period
 * plus a signed byte.  Envelope bytes: att << 4 | frames,
 * 0x00 end, 0x10 hold.
 *
 * Tone 2 and the noise channel are shared with the sound effects: while an
 * effect owns one (musicSfxMask) the music keeps running silently on it and
 * rewrites the registers once it is handed back (music_release). */

#include <SMSlib.h>
#include "gg_data.h"
#include "music.h"

__sfr __at 0x7F MusicPSG;

/* 16 bytes: the assembly below indexes channels with shifts and uses
 * these offsets (on 0, note 1, arp 2, phase 3, base 4, att 5, run 6,
 * hold 7, env 8, lastAtt 10, lastPer 11, fix 13, per 14). */
typedef struct {
    unsigned char on, note, arp, phase, base, att, run, hold;
    const unsigned char *env;
    unsigned char lastAtt;
    unsigned int lastPer;
    unsigned char fix;          /* 1: play `per` instead of the note */
    unsigned int per;
} MusChan;

MusChan mc[4];
unsigned char musBank;                 /* home bank, 0 = no music */
unsigned char musSegBank;              /* bank of the segment being read */
const unsigned int *musOrdTab;          /* the song's order table (header + 4) */
unsigned char musOrders, musRestart, musOrder, musWait, musPause, musSave, musTmpMask;
const unsigned char *musPtr;
const unsigned int *musEnvTab;
unsigned char musicSfxMask;

const unsigned char arpOfs[12] = { 0, 0, 0,  0, 4, 7,  0, 3, 7,  0, 7, 12 };

#define BANK_REG (*(volatile unsigned char *)0xFFFF)

void silence_all(void)
{
    unsigned char i;
    for (i = 0; i < 4; i++) {
        mc[i].on = 0;
        if (!(musicSfxMask & (1 << i)) && mc[i].lastAtt != 15) {
            MusicPSG = (unsigned char)(0x9F | (i << 5));
        }
        mc[i].lastAtt = 15;
    }
}

void music_release(unsigned char ch)
{
    mc[ch].lastAtt = 0xFF;
    mc[ch].lastPer = 0xFFFF;
}

/* Start a song (SONG_*), or stop with 0xFF. */
void music_play(unsigned char song)
{
    unsigned char save, i, bank = 0;
    __asm__("di");
    const unsigned char *hdr = 0;
    if (song < SONG_COUNT) { bank = gg_song[song].bank; hdr = (const unsigned char *)gg_song[song].addr; }
    musBank = 0;
    silence_all();
    for (i = 0; i < 4; i++) {
        mc[i].on = 0;
        mc[i].phase = 0;
        mc[i].fix = 0;
        mc[i].lastAtt = 15;
        mc[i].lastPer = 0xFFFF;
    }
    if (bank) {
        save = BANK_REG;
        BANK_REG = bank;
        musOrders = hdr[0];
        musRestart = hdr[1];
        musEnvTab = *(const unsigned int *const *)(hdr + 2);
        musOrdTab = (const unsigned int *)(hdr + 4);
        musOrder = 0;
        musPtr = (const unsigned char *)(hdr[4] | (hdr[5] << 8));
        musSegBank = hdr[6];
        BANK_REG = save;
        musWait = 0;
        musBank = bank;
    }
    __asm__("ei");
}

void music_pause(unsigned char on)
{
    unsigned char i;
    if (on == musPause) return;
    musPause = on;
    if (on) {
        silence_all();
    } else {
        for (i = 0; i < 4; i++) music_release(i);
    }
}

/* Advance channel IX's envelope by one frame (uses A, HL). */
static void step_env_ix(void) __naked
{
    __asm
    ld  a, 7(ix)
    or  a, a
    ret nz
    ld  a, 6(ix)
    or  a, a
    jr  nz, 09701$
    ld  l, 8(ix)
    ld  h, 9(ix)
    ld  a, (hl)
    or  a, a
    jr  nz, 09702$
    ld  0(ix), a
    ret
09702$:
    cp  a, #0x10
    jr  nz, 09703$
    ld  7(ix), #1
    ret
09703$:
    inc hl
    ld  8(ix), l
    ld  9(ix), h
    ld  h, a
    rrca
    rrca
    rrca
    rrca
    and a, #15
    ld  5(ix), a
    ld  a, h
    and a, #15
09701$:
    dec a
    ld  6(ix), a
    ret
    __endasm;
}

/* One video frame of music; called from the frame interrupt.  Same
 * behaviour as RefPlayer.frame() in tools/psgmusic.py. */
void music_frame(void) __naked
{
    __asm
    ld  a, (_musBank)
    or  a, a
    ret z
    ld  a, (_musPause)
    or  a, a
    ret nz
    push ix
    ld  a, (0xFFFF)
    ld  (_musSave), a
    ld  a, (_musBank)
    ld  (0xFFFF), a
    ld  a, (_musWait)
    or  a, a
    jr  z, 09710$
    dec a
    ld  (_musWait), a
    jp  09730$
    ; ---------------------------------------------------------- events
09710$:
    ld  a, (_musSegBank)
    ld  (0xFFFF), a
    ld  hl, (_musPtr)
09711$:
    ld  a, (hl)
    inc hl
    or  a, a
    jr  nz, 09713$
    ; end of segment: next order (or restart / stop)
    ld  a, (_musOrders)
    ld  b, a
    ld  a, (_musOrder)
    inc a
    cp  a, b
    jr  c, 09712$
    ld  a, (_musRestart)
    cp  a, #0xFF
    jp  z, 09760$
09712$:
    ; order entries are 3 bytes (address, bank) in the home bank
    ld  (_musOrder), a
    ld  l, a
    ld  h, #0
    ld  e, l
    ld  d, h
    add hl, hl
    add hl, de
    ld  de, (_musOrdTab)
    add hl, de
    ld  a, (_musBank)
    ld  (0xFFFF), a
    ld  e, (hl)
    inc hl
    ld  d, (hl)
    inc hl
    ld  a, (hl)
    ld  (_musSegBank), a
    ld  (0xFFFF), a
    ex  de, hl
    jp  09711$
09713$:
    cp  a, #0x40
    jr  nc, 09714$
    dec a
    ld  (_musWait), a
    ld  (_musPtr), hl
    jp  09730$
09714$:
    ld  c, a
    and a, #3
    add a, a
    add a, a
    add a, a
    add a, a
    ld  e, a
    ld  d, #0
    ld  ix, #_mc
    add ix, de
    ld  a, c
    and a, #0xFC
    cp  a, #0x48
    jr  nz, 09715$
    ld  0(ix), #0
    jr  09711$
09715$:
    cp  a, #0x4C
    jr  nz, 09716$
    ld  a, (hl)
    inc hl
    and a, #15
    ld  4(ix), a
    jr  09711$
09716$:
    cp  a, #0x50
    jr  nz, 09721$
    ld  a, (hl)
    inc hl
    ld  1(ix), a
    ld  13(ix), #0
    jp  09711$
09721$:
    cp  a, #0x58
    jr  nz, 09722$
    ld  a, (hl)
    inc hl
    ld  e, a
    rla
    sbc a, a
    ld  d, a
    ld  a, 14(ix)
    add a, e
    ld  14(ix), a
    ld  a, 15(ix)
    adc a, d
    ld  15(ix), a
    jp  09711$
09722$:
    cp  a, #0x54
    jr  nz, 09717$
    ld  a, (hl)
    inc hl
    ld  14(ix), a
    ld  a, (hl)
    inc hl
    ld  15(ix), a
    ld  13(ix), #1
    jp  09711$
09717$:
    ; note on: note, envelope, base | arp << 4 [, frames to skip]
    ld  a, (hl)
    inc hl
    ld  1(ix), a
    ld  a, (hl)
    inc hl
    push hl
    ld  l, a
    ld  h, #0
    add hl, hl
    ld  de, (_musEnvTab)
    add hl, de
    ld  a, (_musBank)
    ld  (0xFFFF), a
    ld  a, (hl)
    inc hl
    ld  h, (hl)
    ld  8(ix), a
    ld  9(ix), h
    ld  a, (_musSegBank)
    ld  (0xFFFF), a
    pop hl
    ld  a, (hl)
    inc hl
    ld  b, a
    and a, #15
    ld  4(ix), a
    ld  a, b
    rrca
    rrca
    rrca
    rrca
    and a, #15
    ld  2(ix), a
    ld  0(ix), #1
    ld  13(ix), #0
    ld  6(ix), #0
    ld  7(ix), #0
    ld  3(ix), #0
    ld  5(ix), #15
    bit 2, c
    jr  z, 09719$
    ld  b, (hl)
    inc hl
    push hl
    ld  a, (_musBank)
    ld  (0xFFFF), a
09718$:
    ld  a, b
    or  a, a
    jr  z, 09720$
    ld  a, 0(ix)
    or  a, a
    jr  z, 09720$
    ld  a, 7(ix)
    or  a, a
    jr  nz, 09720$
    call _step_env_ix
    dec b
    jr  09718$
09720$:
    ld  a, (_musSegBank)
    ld  (0xFFFF), a
    pop hl
09719$:
    ld  a, c
    and a, #3
    cp  a, #3
    jp  nz, 09711$
    ld  11(ix), #0xFF
    ld  12(ix), #0xFF
    jp  09711$
    ; ---------------------------------------------------------- channels
09730$:
    ld  a, (_musBank)
    ld  (0xFFFF), a
    ld  a, (_musicSfxMask)
    ld  (_musTmpMask), a
    ld  ix, #_mc
    ld  b, #0
09731$:
    ld  a, 0(ix)
    or  a, a
    jr  nz, 09732$
    ; silent channel: only make sure its volume is off
    ld  hl, #_musTmpMask
    rrc (hl)
    jp  c, 09740$
    ld  a, 10(ix)
    cp  a, #15
    jp  z, 09740$
    ld  10(ix), #15
    ld  a, b
    rrca
    rrca
    rrca
    or  a, #0x9F
    out (0x7F), a
    jp  09740$
09732$:
    call _step_env_ix
    ld  a, 0(ix)
    or  a, a
    ld  a, #15
    jr  z, 09733$
    ld  a, 5(ix)
    add a, 4(ix)
    cp  a, #16
    jr  c, 09733$
    ld  a, #15
09733$:
    ld  c, a
    ld  a, b
    cp  a, #3
    jr  z, 09735$
    ; period = the exact one, or gg_note_period[note + arpOfs[arp * 3 + phase]]
    ld  a, 13(ix)
    or  a, a
    jr  z, 09742$
    ld  e, 14(ix)
    ld  d, 15(ix)
    jr  09736$
09742$:
    ld  a, 2(ix)
    or  a, a
    ld  a, 1(ix)
    jr  z, 09741$
    ld  a, 2(ix)
    ld  e, a
    add a, a
    add a, e
    add a, 3(ix)
    ld  e, a
    ld  d, #0
    ld  hl, #_arpOfs
    add hl, de
    ld  a, (hl)
    add a, 1(ix)
09741$:
    ld  l, a
    ld  h, #0
    add hl, hl
    ld  de, #_gg_note_period
    add hl, de
    ld  e, (hl)
    inc hl
    ld  d, (hl)
    ld  a, 3(ix)
    inc a
    cp  a, #3
    jr  c, 09734$
    xor a, a
09734$:
    ld  3(ix), a
    jr  09736$
09735$:
    ld  e, 1(ix)
    ld  d, #0
09736$:
    ; channel lent to a sound effect?
    ld  hl, #_musTmpMask
    rrc (hl)
    jp  c, 09740$
    ld  a, 0(ix)
    or  a, a
    jr  z, 09738$
    ld  a, 11(ix)
    cp  a, e
    jr  nz, 09737$
    ld  a, 12(ix)
    cp  a, d
    jr  z, 09738$
09737$:
    ld  11(ix), e
    ld  12(ix), d
    ld  a, b
    cp  a, #3
    jr  z, 09739$
    rrca
    rrca
    rrca
    or  a, #0x80
    ld  h, a
    ld  a, e
    and a, #15
    or  a, h
    out (0x7F), a
    ld  a, e
    rrca
    rrca
    rrca
    rrca
    and a, #0x0F
    ld  h, a
    ld  a, d
    rlca
    rlca
    rlca
    rlca
    and a, #0x30
    or  a, h
    out (0x7F), a
    jr  09738$
09739$:
    ld  a, e
    or  a, #0xE0
    out (0x7F), a
09738$:
    ld  a, 10(ix)
    cp  a, c
    jp  z, 09740$
    ld  10(ix), c
    ld  a, b
    rrca
    rrca
    rrca
    or  a, #0x90
    or  a, c
    out (0x7F), a
09740$:
    ld  de, #16
    add ix, de
    inc b
    ld  a, b
    cp  a, #4
    jp  nz, 09731$
    ld  a, (_musSave)
    ld  (0xFFFF), a
    pop ix
    ret
    ; ---------------------------------------------------------- jingle over
09760$:
    call _silence_all
    xor a, a
    ld  (_musBank), a
    ld  a, (_musSave)
    ld  (0xFFFF), a
    pop ix
    ret
    __endasm;
}
