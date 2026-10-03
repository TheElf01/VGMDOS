#!/usr/bin/env python

import sys

def main():
    try:
        f = open("FONT.F16", "rb")
    except IOError:
        print("ERROR: no se encontro FONT.F16 en el directorio actual.")
        return 1

    data = bytearray(f.read())
    f.close()

    if len(data) != 4096:
        print("ERROR: FONT.F16 deberia pesar exactamente 4096 bytes "
              "(256 caracteres x 16 bytes), pero pesa %d." % len(data))
        return 1

    lines = []
    for i in range(0, len(data), 12):
        chunk = data[i:i+12]
        hexvals = ",".join("0x%02X" % b for b in chunk)
        lines.append("    " + hexvals + ",")

    if lines:
        lines[-1] = lines[-1].rstrip(",")

    out = open("vgafont_data.inc", "w")
    out.write("\n".join(lines) + "\n")
    out.close()

    print("vgafont_data.inc generado a partir de FONT.F16 (%d bytes)." % len(data))
    return 0

if __name__ == "__main__":
    sys.exit(main())
