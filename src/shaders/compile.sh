#!/bin/sh
# Need to be LF instead of CR-LF to run the shell script in WSL2.

# DXC_PATH="C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\dxc.exe"
DXC_PATH="/mnt/c/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64/dxc.exe"

echo Compiling shaders...
"$DXC_PATH"  -T vs_6_0 -E VSMain -Fo VSMain.cso shaders.hlsl
"$DXC_PATH"  -T ps_6_0 -E PSMain -Fo PSMain.cso shaders.hlsl

echo Converting to C includes.
xxd -i VSMain.cso VSMain.h
xxd -i PSMain.cso PSMain.h
