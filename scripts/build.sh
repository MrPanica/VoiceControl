#! /bin/bash
set -e

rm -rf build
mkdir build
cd build
python3 ../configure.py -s tf2,sdk2013 --voicecontrol-only --sm-path /sourcemod --mms-path /metamod-source --hl2sdk-root /sdks --enable-optimize
ambuild
