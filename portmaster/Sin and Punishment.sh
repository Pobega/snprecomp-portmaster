#!/bin/bash

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

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/sinandpunishment"
BINARY="snprecompiled.${DEVICE_ARCH}"
cd "$GAMEDIR" || exit 1

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SNP_ROM_DIR="$GAMEDIR/roms"
export ANALOG_STICKS

shopt -s nullglob nocaseglob
rom_files=("$GAMEDIR/roms/"*.{z64,n64,v64} "$GAMEDIR/"*.{z64,n64,v64})
shopt -u nullglob nocaseglob
if [ ${#rom_files[@]} -eq 0 ]; then
  pm_show_error "Copy your legally obtained Sin and Punishment Nintendo 64 ROM (.z64, .n64, or .v64) into ports/sinandpunishment/roms/ and launch again." --title "ROM missing" --type warning
  pm_finish
  exit 1
fi

$ESUDO chmod +x "$GAMEDIR/$BINARY"

$GPTOKEYB2 "snprecompiled" > /dev/null 2>&1 &

pm_platform_helper "$GAMEDIR/$BINARY"

./$BINARY

pm_finish
