#!/bin/zsh
# SPDX-License-Identifier: GPL-2.0-only
# Installs the Digi 003 driver and restarts Core Audio (asks for your password).
cd "${0:A:h}"
# stop the 003 stream cleanly first (killing coreaudiod mid-stream can hang the 003's firmware)
ROUTE=""
if [ -x tools/d3ctl ]; then ROUTE=$(tools/d3ctl status 2>&1 | awk '/route =/ {gsub(/;/,""); print $3}'); tools/d3ctl route 0 >/dev/null 2>&1; tools/d3ctl keepalive 0 >/dev/null 2>&1; sleep 1; fi
pkill -f Digi003Menu 2>/dev/null
sudo rm -rf /Library/Audio/Plug-Ins/HAL/Digi003.driver
sudo cp -R build/Digi003.driver /Library/Audio/Plug-Ins/HAL/
sudo chown -R root:wheel /Library/Audio/Plug-Ins/HAL/Digi003.driver
sudo killall coreaudiod
sleep 2
# restarting coreaudiod stops the UA Mixer Engine (Apollo inputs go silent); bring it back
launchctl kickstart gui/$(id -u)/com.uaudio.ua_mixer_engine 2>/dev/null
sleep 3
[ -n "$ROUTE" ] && [ "$ROUTE" != "0" ] && tools/d3ctl route "$ROUTE" >/dev/null 2>&1
open ~/Applications/"Digi 003.app" 2>/dev/null
echo "Listo. El 003 deberia aparecer en Configuracion de Audio MIDI."
