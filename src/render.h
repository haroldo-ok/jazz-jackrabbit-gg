#ifndef RENDER_H
#define RENDER_H
#include "jazz_game.h"
extern s16 camX, camY;
extern u8 cacheMisses;
void render_init(void);
void render_load_font_bg(void);
void render_stage_begin(void);
void render_camera_snap(void);
void render_vblank(void);
void render_frame(u8 steps);
extern u8 spriteUploads;
#endif
