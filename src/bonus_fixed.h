#ifndef BONUS_FIXED_H
#define BONUS_FIXED_H
/* Fixed-ROM services of the bonus levels (bonus_fixed.c). */
extern u8 bxBank;
extern u16 bxRefs, bxChunks, bxMasks, bxRows;
u8 bx_tile(u8 tx, u8 ty);
u8 bx_mask(u8 tile, u8 row);
u8 bx_event(u8 tx, u8 ty);
u8 bx_probe(u8 tx, u8 ty, u8 row, u8 col);
extern u8 bxProbeEvent;
u8 bx_row_events(u8 ty, u8 x0, u8 n, u8 *out, u8 max);
extern u32 fsU, fsV, fsDU, fsDV;
extern u8 *fsOut;
void bx_floor_row(void);
void bx_floor_reset(void);
void bx_floor_upload(const u8 *buf);
void bx_vram_copy(u8 bank, u16 src, u16 dst, u16 n);
void bx_sat(void);
#endif
