/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/* Rendition Verite V1000 (machine=svga_verite): interface between the board emulation in
 * src/hardware/vga_verite.cpp and the VGA, INT 10h and VESA code. */

#ifndef DOSBOX_VERITE_H
#define DOSBOX_VERITE_H

class PageHandler;
struct VideoModeBlock;

void VERITE_Init(void);
void SVGA_Setup_Verite(void);

/* the VGA frame timer: vertical sync, display start latch */
void VERITE_VSync(void);

/* local memory window at A0000 (native mode with the VESA bit set), or NULL */
PageHandler *VERITE_WindowHandler(void);

/* INT 10h / VESA BIOS */
void VERITE_SetupRom(uint32_t rom_base, uint16_t &used); /* PCIR + VPD in the VGA BIOS ROM */
bool VERITE_BiosExtension(void); /* INT 10h AH=15h; false if the function is unknown */
void VERITE_BeginSetMode(void);
bool VERITE_FinishSetMode(const VideoModeBlock *mode, bool clearmem);
uint32_t VERITE_LfbBase(void);
void VERITE_SetBank(uint32_t offset);
uint32_t VERITE_GetBank(void);
void VERITE_SetDisplayStart(uint32_t offset);
uint32_t VERITE_GetDisplayStart(void);
void VERITE_SetPitch(uint32_t bytes);
uint32_t VERITE_GetPitch(void);

#endif
