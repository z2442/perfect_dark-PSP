#!/bin/bash

# Remove old build directory 
rm -rf build-psp

# Generate build system files for PSP
# -DCMAKE_BUILD_TYPE=Debug to build the debug with symbols aka larger 
psp-cmake -S . -B build-psp \
    -DBUILD_PSP=ON \
    -DROMID=ntsc-final \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_PRX=1

# Build 
cmake --build build-psp -j 4

#copy the game data executable directory
cp -r data build-psp/