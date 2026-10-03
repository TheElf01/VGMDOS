@echo off
wcc386 -zq -ox -bt=dos vgmtool.c
wcc386 -zq -ox -bt=dos vgmlib.c
wcc386 -zq -ox -bt=dos inflate.c
wcc386 -zq -ox -bt=dos pred.c
wcc386 -zq -ox -bt=dos mdconv.c
wcc386 -zq -ox -bt=dos ngconv.c
wcc386 -zq -ox -bt=dos ngopl.c
wcc386 -zq -ox -bt=dos mdpcm.c
wcc386 -zq -ox -bt=dos snesconv.c
wcc386 -zq -ox -bt=dos ym3438.c
wcc386 -zq -ox -bt=dos opl3.c
wpp386 -zq -ox -bt=dos -DBLARGG_COMPILER_HAS_BOOL=1 -Ispc spc\SNES_SPC.cpp
wpp386 -zq -ox -bt=dos -DBLARGG_COMPILER_HAS_BOOL=1 -Ispc spc\SNES_SPC_misc.cpp
wpp386 -zq -ox -bt=dos -DBLARGG_COMPILER_HAS_BOOL=1 -Ispc spc\SPC_DSP.cpp
wpp386 -zq -ox -bt=dos -DBLARGG_COMPILER_HAS_BOOL=1 -Ispc spc\spcemu.cpp
if errorlevel 1 exit /b 1
wlink system causeway option quiet file vgmtool.obj,vgmlib.obj,inflate.obj,pred.obj,mdconv.obj,ngconv.obj,ngopl.obj,mdpcm.obj,snesconv.obj,ym3438.obj,opl3.obj,SNES_SPC.obj,SNES_SPC_misc.obj,SPC_DSP.obj,spcemu.obj name vgmtoold.exe
if errorlevel 1 exit /b 1
del *.obj
