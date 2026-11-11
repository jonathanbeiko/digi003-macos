#!/bin/zsh
# SPDX-License-Identifier: GPL-2.0-only
# Removes the Digi 003 driver, the menu bar app and its login item, then restarts Core Audio (asks for your password).
cd "${0:A:h}"
pkill -f Digi003Menu 2>/dev/null
rm -f ~/Library/LaunchAgents/io.github.jonathanbeiko.digi003.menu.plist
rm -rf ~/Applications/"Digi 003.app"
sudo rm -rf /Library/Audio/Plug-Ins/HAL/Digi003.driver
sudo killall coreaudiod
sleep 2
launchctl kickstart gui/$(id -u)/com.uaudio.ua_mixer_engine 2>/dev/null   # Universal Audio users: bring the mixer back
echo "Digi 003 driver and menu bar app removed."
