# VGMDOS 2026 - TheElf

(English: README.md)

VGMDOS es un reproductor de VGM para DOS. Funciona en un 286 o superior.
Los temas con chips FM suenan en el OPL de tu tarjeta de sonido. El resto de chips se emulan por software y salen por el DAC de la tarjeta.

Soporta musica de muchos sistemas: Master System, Game Gear, Neo Geo Pocket, Mega Drive, Mega CD, 32X, Neo Geo, SNES, NES, Famicom Disk System, PC Engine, Game Boy, WonderSwan, MSX (MSX-MUSIC, MSX-AUDIO, SCC), ZX Spectrum, Amstrad CPC, PC-88 y maquinas con OPL.

Los sistemas mas "pesados" (Mega Drive, Neo Geo, SNES, PC Engine) se preparan antes con **vgmtool**, un conversor que corre en tu PC moderno.

## Que necesitas

- Un 286 o superior. Con una CPU mas rapida puedes subir la calidad, pero un 286 rapido ya es capaz de 22 kHz estereo.

- Sound Blaster, SB Pro, SB16, AWE32/64 o compatible, con la variable BLASTER

- O un DAC en el puerto de impresora (tipo Covox): `OUTPUT=LPT1` en el INI

- PicoGUS en modo Tandy tambien sirve para el SN76489.

## Archivos

| Archivo | Que es |
|---------|--------|
| bin/VGMDOS.EXE | el reproductor |
| bin/VGMDOS.INI | opciones |
| bin/VGMTOOL.EXE | conversor para Windows (32 bits) |
| bin/VGMTOOLD.EXE | el mismo conversor para DOS (386 o superior) |

## Como se usa

1. Pon tus VGM/VGZ en directorios en el PC moderno. Lo mejor es un directorio por juego.
2. Pasa vgmtool por toda la coleccion:

       vgmtool C:\VGM C:\VGMDOS

3. Copia `C:\VGMDOS` a la maquina DOS y a disfrutar. Pon `PATH=` en `VGMDOS.INI` si quieres que cargue otro directorio o unidad, como un CD-ROM o la red.

Tambien puedes poner VGM sin pasar por vgmtool, pero los de Mega Drive y Neo Geo hay que convertirlos si o si, y los archivos con mucho PCM (PC Engine, algunos de NES) van muy lentos en un 286 si no los conviertes.

## Teclas

| Tecla | Accion |
|-------|--------|
| Flechas, RePag, AvPag, Inicio, Fin | moverse |
| Enter | entrar en directorio / reproducir |
| Espacio | reproducir el directorio entero |
| S | crear FILES.LST del directorio (titulos sacados del VGM) |
| I | acerca de |
| ESC | parar el tema / salir |

## VGMDOS.INI

    LANGUAGE=ES        EN o ES
    PATH=C:\VGM        directorio que abre al arrancar (si no, el del EXE)
    NOSHORT=1          muestra los titulos en vez de los nombres 8.3, leidos de VGM.DIR y FILES.LST
    QUALITY=22         frecuencia de los chips por software: 8, 11, 16, 22 o 44
    QUALITY_NES=11     limite de un chip (PSG, AY, SCC, NES, FDS, GB, WS, PCE, MCD)
    OUTPUT=LPT1        DAC en el puerto de impresora (LPT1 a LPT4)
    PGUS_TNDY=1        PicoGUS Tandy para el SN76489 (puerto C0, o el puerto en hex)
    BITS16=1           salida de 16 bits en SB16/AWE (necesita H en BLASTER)
    PGUSPATH=C:\PGUS   directorio de PGUSINIT.EXE (ver coleccion PicoGUS)
    PGUSDRIVE=E        letra del CD de PicoGUS

**QUALITY** es la frecuencia que usan los chips que se emulan por software. Mas alta suena mas limpio pero gasta mas CPU. Si un tema va lento o a saltos, baja QUALITY, o baja solo el chip que da problemas con `QUALITY_xxx`. Por ejemplo, un 286 a 12 MHz puede con el PSG a 16 kHz pero quizas el NES solo a 11 u 8. Un Harris 25 MHz puede con TODOS a 22 kHz.

**BITS16** pone la salida en 16 bits para los temas con PCM (Mega CD, Neo Geo, SNES). Las partes flojas y los fades ganan mucho sobre 8 bits. Gasta algo mas de CPU. Necesita SB16 o superior.

## VGMTOOL

    vgmtool C:\VGM C:\VGMDOS          todo el arbol de directorios
    vgmtool entrada.vgz salida.vgm    un archivo

Con cada archivo hace esto:

- **Mega Drive (YM2612):** el FM va a OPL3. El PSG queda igual. La bateria PCM va como streams, lista para sonar en DOS. Si el tema tambien tiene PCM de Mega CD (RF5C164), queda igual.
- **32X:** el chip PWM (voces, bateria) se pasa solo a PCM.
- **SNES (archivos .spc):** se emula el SPC y cada voz pasa a ser una nota del PCM (8 canales), con sus muestras BRR pasadas a PCM. La duracion sale de la etiqueta del SPC (180 s si no tiene). El eco (reverb) del SNES no suena. Recomiendo AWE32+ ya que tiene reverb por hardware
- **Neo Geo (YM2610):** el FM va a OPL3 como en Mega Drive, el SSG suena como AY y las muestras ADPCM se pasan a PCM a frecuencia completa. Los juegos grandes (Metal Slug) piden 2 a 4 MB: lo que cabe va a memoria (convencional + XMS) y el resto se lee del archivo mientras suena. vgmtool pone primero las muestras que mas suenan, asi con 512 KB de memoria casi todo esta en memoria.
- **PC Engine y NES con PCM:** el PCM se preconvierte (mas abajo).
- **El resto** queda igual.
- Y todos los archivos se comprimen.

Al convertir un directorio ademas pone nombres 8.3 (`01TITLE.VGM`...), un `VGM.DIR` en cada directorio con los nombres reales de los subdirectorios y un `FILES.LST` con los titulos. Asi el reproductor muestra nombres de verdad y no los 8.3.

### Opciones

    -n        no comprimir
    -c        solo comprimir, mismos nombres y todo el arbol igual
    -d        solo descomprimir (vuelve a VGM normal, para VGMPlay etc)
    -l        Neo Geo: PCM a media calidad (ocupa casi la mitad)
    -mdlow    Mega Drive: todo por OPL3
    -mdmid    Mega Drive: PCM en 2 canales, el resto OPL3 (286 rapido, por defecto)
    -mdhigh   Mega Drive: todo el FM por PCM (386DX o superior)
    -jN       (Windows NT+) N temas a la vez, por ejemplo -j4
    -en / -es idioma

### Calidad de Mega Drive

El OPL3 no puede copiar el YM2612 exacto, asi que hay tres niveles:

- **-mdlow:** el FM se traduce a OPL3. Archivos chicos, casi nada de CPU, va en cualquier 286. Suena parecido, pero algunos instrumentos no son iguales a la consola y algunas notas duran mas de lo que deberian.
- **-mdmid:** vgmtool graba cada instrumento del YM2612 con un emulador exacto (Nuked-OPN2) y tambien con el OPL3 (Nuked-OPL3). Los instrumentos que el OPL3 copia peor suenan como notas grabadas en el PCM, pero nunca mas de 2 a la vez. Todo lo demas se queda en OPL3. Es el mejor equilibrio para un 286.
  El conversor hace dos pasadas: primero mira cuando empieza y cuando acaba cada nota, y despues reparte los 2 canales PCM entre los sonidos que mas los necesitan. Una nota que ya esta sonando no se corta nunca: si los 2 canales estan ocupados, la nota nueva suena por OPL3. Tarda el doble en convertir.
- **-mdhigh:** todas las notas FM se graban y suenan por PCM. Es lo mas parecido a la consola, pero pide un 386DX o superior y los archivos son mas grandes. En un Cyrix 486DLC33 va perfecto.

Si todos los instrumentos de un tema suenan parecido en OPL3, el tema queda como -mdlow, sin PCM. Los temas de 32X siempre pasan el PWM a PCM, elijas el nivel que elijas.

Convertir con -mdmid o -mdhigh es lento, asi que usa VGMTOOL.EXE en Windows y `-jN` para usar todos los nucleos de tu PC, por ejemplo:

    vgmtool -mdmid -j4 C:\VGM C:\VGMDOS

Con `-jN` los archivos convertidos, el VGM.DIR y el FILES.LST son iguales que sin el. Solo funciona con VGMTOOL.EXE de Windows y al convertir un directorio.

### Compresion

Los comandos del VGM se comprimen con LZSS (ventana de 4 KB). La cabecera, el GD3 y los bancos de PCM quedan sin comprimir. Los archivos quedan en un 25-40% del tamano, y el reproductor los descomprime al vuelo mientras suenan, cuesta casi nada incluso en un 286. Solo VGMDOS lee este formato. Si quieres archivos normales para otros reproductores, usa `-d`.

### Preconvertir

Algunos juegos de PC Engine y NES tocan voces y bateria escribiendo cada muestra de PCM en el chip, miles de escrituras por segundo. En un 286 solo leer eso ya no llega. vgmtool saca esas muestras, guarda cada trozo una sola vez en un banco dentro del archivo y pone un "toca este trozo" donde empezaba. Suena igual y cuesta mucho menos. Estos archivos solo suenan en VGMDOS.

## Coleccion PicoGUS

Puedes armar una coleccion virtual con las imagenes de CD del PicoGUS.

En `VGMDOS.INI`:

    PGUSPATH=C:\PGUS
    PGUSDRIVE=E

`PGUSPATH` es el directorio donde esta `PGUSINIT.EXE` (dejalo vacio si esta en el PATH). `PGUSDRIVE` es la letra de la unidad de CD de PicoGUS.

En el disco duro haces directorios vacios, y dentro de cada uno un archivo `PICOGUS.DIR` con el numero de CD en la primera linea:

    51

Al entrar en ese directorio, VGMDOS ejecuta `PGUSINIT.EXE /cdload 51`, espera a que el CD este listo y muestra lo que hay en la unidad de CD, con su `VGM.DIR` y su `FILES.LST` si los tiene. Con ".." vuelves al directorio del disco duro.

Si ese CD ya esta cargado (mismo numero), no se carga otra vez. VGMDOS solo recuerda el ultimo numero mientras esta abierto.

La segunda linea es opcional. Es un directorio dentro del CD, que se muestra en vez de la raiz. Asi una sola imagen de CD puede tener muchas colecciones:

    51
    MD

## Modo AWE (experimental, no va bien; lo terminaria de implementar si a alguien le interesa)

Para tarjetas AWE32/AWE64 con RAM, puedes dejar que el chip EMU8000 de la tarjeta toque el PCM de los temas de Mega Drive convertidos, y asi la CPU trabaja menos.

    SET AWE32=1          (usa 512 KB de la RAM de la tarjeta)
    SET AWE32=2048       (o los KB que quieras usar)

Necesita `E=` en la variable BLASTER (por ejemplo `E620`), y solo vale para temas convertidos con este vgmtool.

Este modo no esta terminado. El uso de CPU baja mucho, pero el sonido no es tan bueno como el del PCM por software, asi que esta apagado salvo que pongas `AWE32`.

## Compilar

Necesitas Open Watcom 1.9 o 2.0.

- Reproductor: entra en el directorio `vgmdos` y ejecuta `build.bat`. Genera `build\vgmdos.exe`. Python se usa para pasar `FONT.F16` a `vgafont_data.inc`.
- Conversor para Windows: entra en el directorio `vgmtool` y ejecuta `build_win.bat`.
- Conversor para DOS: entra en el directorio `vgmtool` y ejecuta `build_dos.bat`.

## Creditos

vgmtool usa Nuked OPL3 y Nuked OPN2 de Nuke.YKT, y snes_spc de Shay Green. Mira `THIRD_PARTY.md`.

Mega Drive/NeoGeo en OPL3 es una conversion, no emulacion: suena muy parecido pero no exactamente igual al original.
