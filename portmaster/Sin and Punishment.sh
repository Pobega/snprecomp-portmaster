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

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
if [ -n "${directory:-}" ] && [ -d "$directory/ports/sinandpunishment" ]; then
  GAMEDIR="$directory/ports/sinandpunishment"
elif [ -d "$SCRIPT_DIR/sinandpunishment" ]; then
  GAMEDIR="$SCRIPT_DIR/sinandpunishment"
else
  GAMEDIR="$SCRIPT_DIR"
fi

cd "$GAMEDIR"
> "$GAMEDIR/log.txt" 2>&1

export LD_LIBRARY_PATH="$GAMEDIR/lib:${LD_LIBRARY_PATH:-}"
export SDL_GAMECONTROLLERCONFIG="${SDL_GAMECONTROLLERCONFIG:-}"
if [ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ] && [ -f "${controlfolder}/gamecontrollerdb.txt" ]; then
  export SDL_GAMECONTROLLERCONFIG_FILE="${controlfolder}/gamecontrollerdb.txt"
fi
export SNP_ROM_DIR="$GAMEDIR/roms"

# Locate valid Sin and Punishment ROM
ROM_FOUND=""
for candidate in "$GAMEDIR/roms"/*.[zZ]64 "$GAMEDIR/roms"/*.[nN]64 "$GAMEDIR/roms"/*.[vV]64 \
                 "$GAMEDIR"/*.[zZ]64 "$GAMEDIR"/*.[nN]64 "$GAMEDIR"/*.[vV]64; do
  if [ -f "$candidate" ]; then
    ROM_FOUND="$candidate"
    break
  fi
done

if [ -z "$ROM_FOUND" ]; then
  echo "No Sin and Punishment ROM found in $GAMEDIR/roms/ or $GAMEDIR/" >> "$GAMEDIR/log.txt"
  echo "Place a Sin and Punishment ROM (.z64, .n64, or .v64) in sinandpunishment/roms/" >> "$GAMEDIR/log.txt"
  if type text_viewer >/dev/null 2>&1; then
    text_viewer -f 24 -m "No Sin and Punishment ROM found!\n\nPlease place your .z64 ROM inside:\nports/sinandpunishment/roms/" -t 5
  fi
  exit 1
fi

echo "Found ROM: $ROM_FOUND" >> "$GAMEDIR/log.txt"

if [ -n "${ESUDO:-}" ]; then
  $ESUDO chmod +x "$GAMEDIR/SnPRecompiled"
else
  chmod +x "$GAMEDIR/SnPRecompiled"
fi

if type pm_platform_helper >/dev/null 2>&1; then
  pm_platform_helper "$GAMEDIR/SnPRecompiled"
fi

./SnPRecompiled >> "$GAMEDIR/log.txt" 2>&1

if type pm_finish >/dev/null 2>&1; then
  pm_finish
fi
