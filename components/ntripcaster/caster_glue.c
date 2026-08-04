// caster_glue.c — the C side of the firmware ↔ Zig caster seam.
//
// The Zig caster logs through caster_console_write() instead of stderr (it has
// none on this target). Route it to stdout, which ESP-IDF wires to the console
// (USB-Serial-JTAG on the Tab5). The lines already carry their own timestamp +
// level prefix and a trailing newline, so we just emit the bytes verbatim.

#include "caster.h"
#include <stdio.h>

void caster_console_write(const char *data, size_t len)
{
    fwrite(data, 1, len, stdout);
    fflush(stdout);
}
