#!/bin/bash
# Sin and Punishment - PortMaster & NextUI H700 Launch Script
# Compatible with official PortMaster, NextUI (PORTS.pak), and MuOS / Knulli / AmberELEC

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

if [ -f "${controlfolder}/control.txt" ]; then
  source "${controlfolder}/control.txt"
fi

[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
if type get_controls >/dev/null 2>&1; then
  get_controls
fi

GAMEDIR="/$directory/ports/sinandpunishment"

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
if [ -d "$GAMEDIR" ]; then
  SNP_RUNDIR="$GAMEDIR"
elif [ -d "$SCRIPT_DIR/.ports/sinandpunishment" ]; then
  SNP_RUNDIR="$SCRIPT_DIR/.ports/sinandpunishment"
elif [ -d "$SCRIPT_DIR/sinandpunishment" ]; then
  SNP_RUNDIR="$SCRIPT_DIR/sinandpunishment"
else
  SNP_RUNDIR="$GAMEDIR"
fi

if [ ! -d "$SNP_RUNDIR" ]; then
  echo "Error: Game directory not found at $GAMEDIR or $SCRIPT_DIR/.ports/sinandpunishment"
  exit 1
fi

cd "$SNP_RUNDIR" || exit 1
: > "$SNP_RUNDIR/log.txt"

export LD_LIBRARY_PATH="$SNP_RUNDIR/lib:${LD_LIBRARY_PATH:-}"
export SDL_GAMECONTROLLERCONFIG="${SDL_GAMECONTROLLERCONFIG:-}"
if [ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ] && [ -f "${controlfolder}/gamecontrollerdb.txt" ]; then
  export SDL_GAMECONTROLLERCONFIG_FILE="${controlfolder}/gamecontrollerdb.txt"
fi
export SNP_ROM_DIR="$SNP_RUNDIR/roms"

# Locate valid Sin and Punishment ROM
ROM_FOUND=""
for candidate in "$SNP_RUNDIR/roms"/*.[zZ]64 "$SNP_RUNDIR/roms"/*.[nN]64 "$SNP_RUNDIR/roms"/*.[vV]64 \
                 "$SNP_RUNDIR"/*.[zZ]64 "$SNP_RUNDIR"/*.[nN]64 "$SNP_RUNDIR"/*.[vV]64; do
  if [ -f "$candidate" ]; then
    ROM_FOUND="$candidate"
    break
  fi
done

if [ -z "$ROM_FOUND" ]; then
  echo "No Sin and Punishment ROM found in $SNP_RUNDIR/roms/ or $SNP_RUNDIR/" | tee -a "$SNP_RUNDIR/log.txt"
  echo "Place a Sin and Punishment ROM (.z64, .n64, or .v64) in sinandpunishment/roms/" | tee -a "$SNP_RUNDIR/log.txt"
  if type text_viewer >/dev/null 2>&1; then
    text_viewer -f 24 -m "No Sin and Punishment ROM found!\n\nPlease place your .z64 ROM inside:\nsinandpunishment/roms/" -t 5
  fi
  exit 1
fi

echo "Found ROM: $ROM_FOUND" | tee -a "$SNP_RUNDIR/log.txt"

if [ -n "${ESUDO:-}" ]; then
  $ESUDO chmod +x "$SNP_RUNDIR/SnPRecompiled"
else
  chmod +x "$SNP_RUNDIR/SnPRecompiled"
fi

if type pm_platform_helper >/dev/null 2>&1; then
  pm_platform_helper "$SNP_RUNDIR/SnPRecompiled"
fi

./SnPRecompiled 2>&1 | tee -a "$SNP_RUNDIR/log.txt"

if type pm_finish >/dev/null 2>&1; then
  pm_finish
fi
