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
void CG_DrawBoxAt(vec3_t origin, qboolean ducked, vec4_t rgba, qboolean full, qhandle_t shader);

#endif // BBOX_H
