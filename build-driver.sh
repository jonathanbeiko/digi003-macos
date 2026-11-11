#!/bin/zsh
# SPDX-License-Identifier: GPL-2.0-only
# Builds Digi003.driver (AudioServerPlugIn) into ./build
set -e
cd "${0:A:h}"
B=build/Digi003.driver/Contents
rm -rf build/Digi003.driver
mkdir -p $B/MacOS
cp driver/Info.plist $B/Info.plist
clang -O2 -Wall -Wno-deprecated-declarations -arch arm64 -bundle -mmacosx-version-min=14.0 \
  -o $B/MacOS/Digi003 driver/Digi003Driver.c engine/d003engine.c \
  -framework CoreAudio -framework CoreFoundation -framework IOKit
codesign --force --sign - --timestamp=none build/Digi003.driver
echo "built build/Digi003.driver"
