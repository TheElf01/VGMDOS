@echo off
python font2c.py
if errorlevel 1 exit /b 1
if not exist build mkdir build
del *.obj
wcc -zq -2 -ox -oa -ol -mm -oneatx -bt=dos nesapu.c
wcc -zq -2 -ox -oa -ol -mm -oneatx -bt=dos psg_full.c
wcc -zq -2 -ox -oa -ol -mm -oneatx -bt=dos ay8910.c
wcc -zq -2 -ox -oa -ol -mm -oneatx -bt=dos scc.c
wcc -zq -2 -ox -oa -ol -mm -oneatx -bt=dos gbapu.c
wcc -zq -2 -ox -oa -ol -mm -oneatx -bt=dos wsapu.c
wcc -zq -2 -ox -oa -ol -mm -oneatx -bt=dos pcengine.c
wcc -zq -2 -ox -oa -ol -mm -oneatx -bt=dos fds.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos vgmgui.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos textui.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos vgm.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos sbdetect.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos opl2.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos ym2413_opl.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos ym3812_opl.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos ym2413.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos dosmem.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos dma.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos dsp.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos swaudio.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos pgus_tandy.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos ym2203_opl.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos pctimer.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos irq.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos vgafont.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos rf5c164.c
wcc -zq -2 -ox -oa -ol -mm -bt=dos emu8000.c
if errorlevel 1 exit /b 1
wlink system dos option quiet option stack=13k file vgmgui.obj,textui.obj,vgm.obj,sbdetect.obj,psg_full.obj,nesapu.obj,ay8910.obj,scc.obj,opl2.obj,ym2413_opl.obj,ym3812_opl.obj,ym2413.obj,gbapu.obj,wsapu.obj,pcengine.obj,fds.obj,dosmem.obj,dma.obj,dsp.obj,swaudio.obj,pgus_tandy.obj,ym2203_opl.obj,pctimer.obj,irq.obj,vgafont.obj,rf5c164.obj,emu8000.obj name build\vgmdos.exe
if errorlevel 1 exit /b 1
del *.obj
