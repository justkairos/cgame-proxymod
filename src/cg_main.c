/*
  ==============================
  Written by id software, nightmare and hk of mdd
  This file is part of mdd client proxymod.

  mdd client proxymod is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  mdd client proxymod is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with mdd client proxymod.  If not, see <http://www.gnu.org/licenses/>.
  ==============================
  Note: mdd client proxymod contains large quantities from the quake III arena source code
*/
#include "cg_main.h"

#include "cg_grenade.h"
#include "cg_hud.h"
#include "cg_input.h"
#include "cg_rocket.h"
#include "q_assert.h"
#include "version.h"

#include <stdlib.h>

static void CG_Init(int32_t clientNum);

static void CG_Shutdown(void);

/* CLIENT to VM */
intptr_t vmMain(
  int32_t cmd,
  int32_t arg0,
  int32_t arg1,
  int32_t arg2,
  int32_t arg3,
  int32_t arg4,
  int32_t arg5,
  int32_t arg6,
  int32_t arg7,
  int32_t arg8,
  int32_t arg9,
  int32_t arg10,
  int32_t arg11)
{
  intptr_t ret;

  /* PRE CALL */
  switch (cmd)
  {
  case CG_INIT: // void CG_Init( int32_t serverMessageNum, int32_t serverCommandSequence, int32_t clientNum )
    CG_Init(arg2);
    break;

  case CG_CONSOLE_COMMAND: // qboolean (*CG_ConsoleCommand)( void );
    break;

  case CG_DRAW_ACTIVE_FRAME: // void (*CG_DrawActiveFrame)( int32_t serverTime, stereoFrame_t stereoView, qboolean
                             // demoPlayback );
    CG_DrawActiveFrame(arg0, arg1, arg2);
    break;

  case CG_CROSSHAIR_PLAYER: // int32_t (*CG_CrosshairPlayer)( void );
    break;

  case CG_LAST_ATTACKER: // int32_t (*CG_LastAttacker)( void );
    break;

  case CG_KEY_EVENT: // void  (*CG_KeyEvent)( int32_t key, qboolean down );
    break;

  case CG_MOUSE_EVENT: // void  (*CG_MouseEvent)( int32_t dx, int32_t dy );
    break;

  case CG_EVENT_HANDLING: // void (*CG_EventHandling)(int32_t type);
    break;

  case CG_SHUTDOWN: // void (*CG_Shutdown)( void );
    break;

  case -1:
    setVMPtr(arg0);
    return 0;
    break;
  }

  /* call vmMain() in the VM (defrag) */
  ret = callVM(cmd, arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11);

  /* POST CALL */
  switch (cmd)
  {
  case CG_INIT: // void CG_Init( int32_t serverMessageNum, int32_t serverCommandSequence, int32_t clientNum )
    break;

  case CG_CONSOLE_COMMAND: // qboolean (*CG_ConsoleCommand)( void );
    if (!ret) ret = CG_ConsoleCommand();
    break;

  case CG_DRAW_ACTIVE_FRAME: // void (*CG_DrawActiveFrame)( int32_t serverTime, stereoFrame_t stereoView, qboolean
                             // demoPlayback )
    // after defrag predicted the player state for this frame
    input_frame_begin();
    run_grenade();
    run_rocket();
    input_frame_end();
    break;

  case CG_CROSSHAIR_PLAYER: // int32_t (*CG_CrosshairPlayer)( void )
    break;

  case CG_LAST_ATTACKER: // int32_t (*CG_LastAttacker)( void );
    break;

  case CG_KEY_EVENT: // void (*CG_KeyEvent)( int32_t key, qboolean down )
    break;

  case CG_MOUSE_EVENT: // void (*CG_MouseEvent)( int32_t dx, int32_t dy )
    break;

  case CG_EVENT_HANDLING: // void (*CG_EventHandling)(int32_t type)
    break;

  case CG_SHUTDOWN: // void (*CG_Shutdown)( void )
    CG_Shutdown();
    ASSERT_EQ(ret, 0);
    break;
  }

  return ret;
}

cg_t  cg;
cgs_t cgs;

/*
=================
CG_RegisterCvars
=================
*/
static void CG_RegisterCvars(void)
{
  init_hud();
}

/*
=================
CG_UpdateCvars
=================
*/
void CG_UpdateCvars(void)
{
  update_hud();
}

//===========================================================================

/*
=================
CG_ConfigString
=================
*/
char const* CG_ConfigString(int32_t index)
{
  if (index < 0 || index >= MAX_CONFIGSTRINGS)
  {
    trap_Error(vaf("CG_ConfigString: bad index: %i", index));
  }
  return cgs.gameState.stringData + cgs.gameState.stringOffsets[index];
}

//==================================================================

/*
=================
CG_Init

Called after every level change or subsystem restart
Will perform callbacks to make the loading info screen update.
=================
*/
static void CG_Init(int32_t clientNum)
{
  trap_Print(vaf("^7[^1m^3D^1d^7] cgame-proxy %s\n", VERSION));

  // clear everything
  memset(&cgs, 0, sizeof(cgs));
  memset(&cg, 0, sizeof(cg));

  cg.clientNum = clientNum;

  // load a few needed things before we do any screen updates
  cgs.media.charsetShader   = trap_R_RegisterShader("gfx/2d/bigchars");
  cgs.media.whiteShader     = trap_R_RegisterShader("white");
  cgs.media.charsetProp     = trap_R_RegisterShaderNoMip("menu/art/font1_prop.tga");
  cgs.media.charsetPropGlow = trap_R_RegisterShaderNoMip("menu/art/font1_prop_glo.tga");
  cgs.media.charsetPropB    = trap_R_RegisterShaderNoMip("menu/art/font2_prop.tga");

  CG_RegisterCvars();

  CG_InitConsoleCommands();

  // get the rendering configuration from the client system
  trap_GetGlconfig(&cgs.glconfig); // rendering configuration
  cgs.screenXScale = cgs.glconfig.vidWidth / (float)SCREEN_WIDTH;
  cgs.screenWidth  = SCREEN_WIDTH; // = cgs.glconfig.vidWidth / cgs.screenXScale)
  cgs.screenHeight = cgs.glconfig.vidHeight / cgs.screenXScale;

  // get the gamestate from the client system
  trap_GetGameState(&cgs.gameState);

  cgs.levelStartTime = atoi(CG_ConfigString(CS_LEVEL_START_TIME));

  // CG_RegisterGraphics
  cgs.media.deferShader = trap_R_RegisterShaderNoMip("gfx/2d/defer");

  initVM();
}

/*
=================
CG_Shutdown

Called before every level change or subsystem restart
=================
*/
void CG_Shutdown(void)
{
  // some mods may need to do cleanup work here,
  // like closing files or archiving session data

  del_hud();

  intptr_t const ret = callVM_Destroy();
  (void)ret;
  ASSERT_EQ(ret, 0);
}
