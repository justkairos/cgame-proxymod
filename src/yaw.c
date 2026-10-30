#include "yaw.h"

#include "cg_cvar.h"
#include "cg_draw.h"
#include "cg_utils.h"
#include "help.h"

#include <stdlib.h>

static vmCvar_t yaw;
static vmCvar_t yaw_ywh;
static vmCvar_t yaw_rgba;

static cvarTable_t yaw_cvars[] = {
  { &yaw, "mdd_yaw", "", CVAR_ARCHIVE_ND },
  { &yaw_ywh, "mdd_yaw_ywh", "236 1 8", CVAR_ARCHIVE_ND },
  { &yaw_rgba, "mdd_yaw_rgba", ".8 .8 .8 .8", CVAR_ARCHIVE_ND },
};

static help_t yaw_help[] = {
  {
    yaw_cvars + 1,
    Y | W | H,
    {
      "mdd_yaw_ywh X X X",
    },
  },
  {
    yaw_cvars + 2,
    RGBA,
    {
      "mdd_yaw_rgba X X X X",
    },
  },
};

void init_yaw(void)
{
  init_cvars(yaw_cvars, ARRAY_LEN(yaw_cvars));
  init_help(yaw_help, ARRAY_LEN(yaw_help));
}

void update_yaw(void)
{
  update_cvars(yaw_cvars, ARRAY_LEN(yaw_cvars));
}

// Marks one or more absolute yaw angles with a vertical line, like mdd_pitch does for pitch angles.
void draw_yaw(void)
{
  if (yaw.string[0] == '\0') return;

  vec3_t graph_ywh;
  vec4_t graph_rgba;
  ParseVec(yaw_ywh.string, graph_ywh, 3);
  ParseVec(yaw_rgba.string, graph_rgba, 4);

  float const v = DEG2RAD(getPs()->viewangles[YAW]);

  char const* str = yaw.string;
  char*       end;
  for (float angle = strtof(str, &end); str != end; angle = strtof(str, &end))
  {
    CG_DrawLineYaw(DEG2RAD(angle), v, graph_ywh[0], graph_ywh[1], graph_ywh[2], graph_rgba);
    str = end;
  }
}
