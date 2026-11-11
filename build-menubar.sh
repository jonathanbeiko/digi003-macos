#!/bin/zsh
# SPDX-License-Identifier: GPL-2.0-only
# Builds "Digi 003.app" (menu bar) into ./build and copies it to ~/Applications
set -e
cd "${0:A:h}"
A="build/Digi 003.app/Contents"
rm -rf "build/Digi 003.app"; mkdir -p "$A/MacOS"
cat > "$A/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>Digi003Menu</string>
<key>CFBundleIdentifier</key><string>io.github.jonathanbeiko.digi003.menu</string>
<key>CFBundleName</key><string>Digi 003</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.1</string>
<key>LSMinimumSystemVersion</key><string>14.0</string>
<key>LSUIElement</key><true/>
</dict></plist>
PL
swiftc -O -target arm64-apple-macos14 -o "$A/MacOS/Digi003Menu" menubar/main.swift -framework AppKit -framework CoreAudio -framework CoreMIDI -framework IOKit -framework UserNotifications
codesign --force --sign - "build/Digi 003.app"
mkdir -p ~/Applications
rm -rf ~/Applications/"Digi 003.app"; cp -R "build/Digi 003.app" ~/Applications/
echo "built and copied to ~/Applications/Digi 003.app"
