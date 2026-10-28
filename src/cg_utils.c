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
#include "cg_utils.h"

#include "cg_cvar.h"
#include "cg_local.h"
#include "cg_vm.h"
#include "defrag.h"
#include "ExportImport.h"

snapshot_t const* getSnap(void)
{
  static snapshot_t snapshot;
  int32_t           curSnapNum;
  int32_t           servertime;
  trap_GetCurrentSnapshotNumber(&curSnapNum, &servertime);
  trap_GetSnapshot(curSnapNum, &snapshot);
  return &snapshot;
}

playerState_t const* getPs(void)
{
  if (cvar_getInteger("g_synchronousClients")) return &getSnap()->ps;
  return (playerState_t const*)VM_ArgPtr(defrag()->pps_offset);
}

// Export a handle to getPs() for clients that want to read pps from cgame through proxymod.
EXPORT playerState_t const* proxymod_getPs(void);
EXPORT playerState_t const* proxymod_getPs(void)
{
  return getPs();
}
