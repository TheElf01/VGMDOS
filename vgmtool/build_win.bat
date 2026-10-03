@echo off
wcc386 -zq -ox -bt=nt vgmtool.c
wcc386 -zq -ox -bt=nt vgmlib.c
wcc386 -zq -ox -bt=nt inflate.c
wcc386 -zq -ox -bt=nt pred.c
wcc386 -zq -ox -bt=nt mdconv.c
wcc386 -zq -ox -bt=nt ngconv.c
wcc386 -zq -ox -bt=nt ngopl.c
wcc386 -zq -ox -bt=nt mdpcm.c
wcc386 -zq -ox -bt=nt snesconv.c
wcc386 -zq -ox -bt=nt ym3438.c
wcc386 -zq -ox -bt=nt opl3.c
wpp386 -zq -ox -bt=nt -DBLARGG_COMPILER_HAS_BOOL=1 -Ispc spc\SNES_SPC.cpp
wpp386 -zq -ox -bt=nt -DBLARGG_COMPILER_HAS_BOOL=1 -Ispc spc\SNES_SPC_misc.cpp
wpp386 -zq -ox -bt=nt -DBLARGG_COMPILER_HAS_BOOL=1 -Ispc spc\SPC_DSP.cpp
wpp386 -zq -ox -bt=nt -DBLARGG_COMPILER_HAS_BOOL=1 -Ispc spc\spcemu.cpp
if errorlevel 1 exit /b 1
wlink system nt option quiet file vgmtool.obj,vgmlib.obj,inflate.obj,pred.obj,mdconv.obj,ngconv.obj,ngopl.obj,mdpcm.obj,snesconv.obj,ym3438.obj,opl3.obj,SNES_SPC.obj,SNES_SPC_misc.obj,SPC_DSP.obj,spcemu.obj name vgmtool.exe
if errorlevel 1 exit /b 1
del *.obj
