/* Shared by the differential test drivers (Genesis reference on the host,
 * GG logic on the host) and the GG_TRACE ROM build: one deterministic input
 * generator and one per-frame state record, so the three can be compared
 * byte for byte. */
#ifndef DIFF_COMMON_H
#define DIFF_COMMON_H

#define DIFF_REC 20

static unsigned short diffLfsr = 0xACE1;
static unsigned char diffHold;
static unsigned short diffInput;

static unsigned short diff_rand(void)
{
    unsigned short bit = (unsigned short)(((diffLfsr >> 0) ^ (diffLfsr >> 2) ^ (diffLfsr >> 3) ^ (diffLfsr >> 5)) & 1);
    diffLfsr = (unsigned short)((diffLfsr >> 1) | (bit << 15));
    return diffLfsr;
}

/* Inputs held for 4..35 frames: mostly running right with jumps and shots,
 * sometimes left, occasionally up/down/weapon switch. */
static unsigned short diff_next_input(void)
{
    if (!diffHold) {
        unsigned short r = diff_rand();
        unsigned short in = 0;
        unsigned char dir = (unsigned char)(r & 7);
        if (dir < 5) in |= 0x0002;          /* right */
        else if (dir < 7) in |= 0x0001;     /* left */
        if (r & 0x0018) in |= 0x0004;       /* jump (often) */
        if ((r & 0x0060) == 0x0060) in |= 0x0008;   /* fire */
        if ((r & 0x0700) == 0x0700) in |= 0x0040;   /* down */
        if ((r & 0x3800) == 0x3800) in |= 0x0080;   /* up */
        if ((r & 0xC000) == 0xC000) in |= 0x0020;   /* weapon switch */
        diffInput = in;
        diffHold = (unsigned char)(4 + ((r >> 3) & 31));
    }
    diffHold--;
    return diffInput;
}

/* rec: frame, x, y, vx, vy, score (u16 LE), health, lives, stage, gems&255,
 * enemy count, enemy position checksum (u16), bullets */
static void diff_pack(unsigned char *rec, unsigned short frame, short x, short y, short vx, short vy,
                      unsigned short score, unsigned char health, unsigned char lives,
                      unsigned char stage, unsigned char gems, unsigned char nen,
                      unsigned short esum, unsigned char nbul)
{
    rec[0] = (unsigned char)frame; rec[1] = (unsigned char)(frame >> 8);
    rec[2] = (unsigned char)x; rec[3] = (unsigned char)((unsigned short)x >> 8);
    rec[4] = (unsigned char)y; rec[5] = (unsigned char)((unsigned short)y >> 8);
    rec[6] = (unsigned char)vx; rec[7] = (unsigned char)((unsigned short)vx >> 8);
    rec[8] = (unsigned char)vy; rec[9] = (unsigned char)((unsigned short)vy >> 8);
    rec[10] = (unsigned char)score; rec[11] = (unsigned char)(score >> 8);
    rec[12] = health; rec[13] = lives; rec[14] = stage; rec[15] = gems;
    rec[16] = nen; rec[17] = (unsigned char)esum; rec[18] = (unsigned char)(esum >> 8);
    rec[19] = nbul;
}
#endif
