#include "bbox.h"

#include "cg_cvar.h"
#include "cg_local.h"
#include "cg_utils.h"
#include "help.h"

static vmCvar_t bbox;
static vmCvar_t bbox_rgba;

static cvarTable_t bbox_cvars[] = {
  { &bbox, "mdd_bbox", "0", CVAR_ARCHIVE_ND },
  { &bbox_rgba, "mdd_bbox_rgba", ".9 .5 .7 .7", CVAR_ARCHIVE_ND },
};

static help_t bbox_help[] = {
  {
    bbox_cvars + 1,
    RGBA,
    {
      "mdd_bbox_rgba X X X X",
    },
  },
};

typedef struct
{
  vec4_t               bbox_rgba;
  playerState_t const* pm_ps;

  qhandle_t bboxShader_nocull;

  vec3_t mins;
  vec3_t maxs;

  float extx;
  float exty;
  float extz;

  polyVert_t verts[4];
} bbox_t;

static bbox_t s;

void init_bbox(void)
{
  init_cvars(bbox_cvars, ARRAY_LEN(bbox_cvars));
  init_help(bbox_help, ARRAY_LEN(bbox_help));

  // get the shader handle
  s.bboxShader_nocull = trap_R_RegisterShader("bbox_nocull");

  s.mins[0] = s.mins[1] = -15;
  s.mins[2]             = -24;
  s.maxs[0] = s.maxs[1] = 15;

  // get the extents (size)
  s.extx = s.maxs[0] - s.mins[0];
  s.exty = s.maxs[1] - s.mins[1];

  // set the polygon's texture coordinates
  s.verts[0].st[0] = 0;
  s.verts[0].st[1] = 0;
  s.verts[1].st[0] = 0;
  s.verts[1].st[1] = 1;
  s.verts[2].st[0] = 1;
  s.verts[2].st[1] = 1;
  s.verts[3].st[0] = 1;
  s.verts[3].st[1] = 0;
}

void update_bbox(void)
{
  update_cvars(bbox_cvars, ARRAY_LEN(bbox_cvars));
}

static void CG_AddBoundingBox(void);

void draw_bbox(void)
{
  if (!bbox.integer) return;

  ParseVec(bbox_rgba.string, s.bbox_rgba, 4);

  s.pm_ps = getPs();

  CG_AddBoundingBox();
}

static void CG_AddBoundingBox(void)
{
  int    i;
  vec3_t corners[8];
  //             4 ----- 7
  //   z        /|      /|
  //   | _ y   5 ----- 6 |
  //  /        | |     | |
  // x         | 0 ----| 3
  //           |/      |/
  //           1 ----- 2

  // TODO
  // don't draw it in third-person
  // if (!cg_thirdPerson.integer)
  //   return;

  // don't draw it for dead players
  if (s.pm_ps->pm_type >= PM_SPECTATOR) return;

  // if they don't exist, forget it
  if (!s.bboxShader_nocull) return;

  if (s.pm_ps->pm_flags & PMF_DUCKED)
  {
    s.maxs[2] = 16;
  }
  else
  {
    s.maxs[2] = 32;
  }

  // get the extents (size)
  s.extz = s.maxs[2] - s.mins[2];

  // set the polygon's vertex colors
  for (i = 0; i < 4; ++i)
  {
    s.verts[i].modulate[0] = (byte)(s.bbox_rgba[0] * 255);
    s.verts[i].modulate[1] = (byte)(s.bbox_rgba[1] * 255);
    s.verts[i].modulate[2] = (byte)(s.bbox_rgba[2] * 255);
    s.verts[i].modulate[3] = (byte)(s.bbox_rgba[3] * 255);
  }

  VectorAdd(s.pm_ps->origin, s.mins, corners[0]);

  VectorCopy(corners[0], corners[1]);
  corners[1][0] += s.extx;

  VectorCopy(corners[1], corners[2]);
  corners[2][1] += s.exty;

  VectorCopy(corners[2], corners[3]);
  corners[3][0] -= s.extx;

  // bottom
  VectorCopy(corners[0], s.verts[0].xyz);
  VectorCopy(corners[1], s.verts[1].xyz);
  VectorCopy(corners[2], s.verts[2].xyz);
  VectorCopy(corners[3], s.verts[3].xyz);
  trap_R_AddPolyToScene(s.bboxShader_nocull, 4, s.verts);

  if (bbox.integer == 1)
  {
    for (i = 0; i < 4; ++i)
    {
      VectorCopy(corners[i], corners[i + 4]);
      corners[i + 4][2] += s.extz;
    }

    // top
    VectorCopy(corners[7], s.verts[0].xyz);
    VectorCopy(corners[6], s.verts[1].xyz);
    VectorCopy(corners[5], s.verts[2].xyz);
    VectorCopy(corners[4], s.verts[3].xyz);
    trap_R_AddPolyToScene(s.bboxShader_nocull, 4, s.verts);

    // top side
    VectorCopy(corners[3], s.verts[0].xyz);
    VectorCopy(corners[2], s.verts[1].xyz);
    VectorCopy(corners[6], s.verts[2].xyz);
    VectorCopy(corners[7], s.verts[3].xyz);
    trap_R_AddPolyToScene(s.bboxShader_nocull, 4, s.verts);

    // left side
    VectorCopy(corners[2], s.verts[0].xyz);
    VectorCopy(corners[1], s.verts[1].xyz);
    VectorCopy(corners[5], s.verts[2].xyz);
    VectorCopy(corners[6], s.verts[3].xyz);
    trap_R_AddPolyToScene(s.bboxShader_nocull, 4, s.verts);

    // right side
    VectorCopy(corners[0], s.verts[0].xyz);
    VectorCopy(corners[3], s.verts[1].xyz);
    VectorCopy(corners[7], s.verts[2].xyz);
    VectorCopy(corners[4], s.verts[3].xyz);
    trap_R_AddPolyToScene(s.bboxShader_nocull, 4, s.verts);

    // bottom side
    VectorCopy(corners[1], s.verts[0].xyz);
    VectorCopy(corners[0], s.verts[1].xyz);
    VectorCopy(corners[4], s.verts[2].xyz);
    VectorCopy(corners[5], s.verts[3].xyz);
    trap_R_AddPolyToScene(s.bboxShader_nocull, 4, s.verts);
  }
}

// Draw bbox or the bottom square of bbox at an arbitrary origin.
// Used by mdd_player_trajectory_* feature
void CG_DrawBoxAt(vec3_t origin, qboolean ducked, vec4_t rgba, qboolean full, qhandle_t shader)
{
  int        i;
  vec3_t     mins, maxs;
  vec3_t     corners[8];
  float      extx, exty, extz;
  polyVert_t verts[4];

  if (!shader) return;

  mins[0] = mins[1] = -15;
  mins[2]           = -24;
  maxs[0] = maxs[1] = 15;
  maxs[2]           = ducked ? 16 : 32;

  extx = maxs[0] - mins[0];
  exty = maxs[1] - mins[1];
  extz = maxs[2] - mins[2];

  verts[0].st[0] = 0;
  verts[0].st[1] = 0;
  verts[1].st[0] = 0;
  verts[1].st[1] = 1;
  verts[2].st[0] = 1;
  verts[2].st[1] = 1;
  verts[3].st[0] = 1;
  verts[3].st[1] = 0;

  for (i = 0; i < 4; ++i)
  {
    verts[i].modulate[0] = (byte)(rgba[0] * 255);
    verts[i].modulate[1] = (byte)(rgba[1] * 255);
    verts[i].modulate[2] = (byte)(rgba[2] * 255);
    verts[i].modulate[3] = (byte)(rgba[3] * 255);
  }

  VectorAdd(origin, mins, corners[0]);

  VectorCopy(corners[0], corners[1]);
  corners[1][0] += extx;

  VectorCopy(corners[1], corners[2]);
  corners[2][1] += exty;

  VectorCopy(corners[2], corners[3]);
  corners[3][0] -= extx;

  // bottom
  VectorCopy(corners[0], verts[0].xyz);
  VectorCopy(corners[1], verts[1].xyz);
  VectorCopy(corners[2], verts[2].xyz);
  VectorCopy(corners[3], verts[3].xyz);
  trap_R_AddPolyToScene(shader, 4, verts);

  if (!full) return;

  for (i = 0; i < 4; ++i)
  {
    VectorCopy(corners[i], corners[i + 4]);
    corners[i + 4][2] += extz;
  }

  // top
  VectorCopy(corners[7], verts[0].xyz);
  VectorCopy(corners[6], verts[1].xyz);
  VectorCopy(corners[5], verts[2].xyz);
  VectorCopy(corners[4], verts[3].xyz);
  trap_R_AddPolyToScene(shader, 4, verts);

  // top side
  VectorCopy(corners[3], verts[0].xyz);
  VectorCopy(corners[2], verts[1].xyz);
  VectorCopy(corners[6], verts[2].xyz);
  VectorCopy(corners[7], verts[3].xyz);
  trap_R_AddPolyToScene(shader, 4, verts);

  // left side
  VectorCopy(corners[2], verts[0].xyz);
  VectorCopy(corners[1], verts[1].xyz);
  VectorCopy(corners[5], verts[2].xyz);
  VectorCopy(corners[6], verts[3].xyz);
  trap_R_AddPolyToScene(shader, 4, verts);

  // right side
  VectorCopy(corners[0], verts[0].xyz);
  VectorCopy(corners[3], verts[1].xyz);
  VectorCopy(corners[7], verts[2].xyz);
  VectorCopy(corners[4], verts[3].xyz);
  trap_R_AddPolyToScene(shader, 4, verts);

  // bottom side
  VectorCopy(corners[1], verts[0].xyz);
  VectorCopy(corners[0], verts[1].xyz);
  VectorCopy(corners[4], verts[2].xyz);
  VectorCopy(corners[5], verts[3].xyz);
  trap_R_AddPolyToScene(shader, 4, verts);
}
