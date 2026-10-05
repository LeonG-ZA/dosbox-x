/*
 *  HLE replacement for CPU B's rasteriser (draw32B / draw16B in MAINB.OUT).
 *  Interprets the draw list CPU A builds - see su2000/docs/findings/drawlist.md.
 */
#include "pixboard.h"

void PixRaster_Draw(PixBoard *b, M88110 *cpu, bool bpp32) {
    (void)b; (void)cpu; (void)bpp32;
}
