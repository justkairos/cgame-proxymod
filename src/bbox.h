#ifndef BBOX_H
#define BBOX_H

#include "q_shared.h"

void init_bbox(void);

void update_bbox(void);

void draw_bbox(void);

// Draws a standalone player-sized box at an arbitrary origin, independent of mdd_bbox/getPs() -
// used by player_trajectory.c to draw predicted-position ghosts. Draws all 6 faces if full is
// true, otherwise just the bottom quad (a flat "footprint" square). shader is the caller's own
// registered handle (player_trajectory.c drives it from mdd_player_trajectory_shader_box) -
// independent of mdd_bbox's own "bbox_nocull" shader, not shared with draw_bbox()'s own drawing.
// rgba (floats 0..1) as the bytes a vertex colour takes, with the alpha folded into the colour: see bbox.c
void rgba_to_bytes(vec4_t const rgba, byte out[4]);

void CG_DrawBoxAt(vec3_t origin, qboolean ducked, vec4_t rgba, qboolean full, qhandle_t shader);

#endif // BBOX_H
