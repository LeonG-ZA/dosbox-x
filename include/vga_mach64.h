/*
 *  ATI mach64 GT (3D RAGE) emulation, shared declarations.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Register reference: ATI mach64 Register Reference Guide, ATI-264VT and
 *  3D RAGE, RRG-G02700 rev 0.10 (1996), referred to as [RRG] below.
 */

#ifndef DOSBOX_VGA_MACH64_H
#define DOSBOX_VGA_MACH64_H

#include <stdint.h>
#include <iostream>

#include "dosbox.h"

/* Block 0 register dword offsets ([RRG] chapter 3). Block 1 registers are
 * offset by 0x100 in the combined register index used internally. */
enum {
	/* Accelerator CRTC */
	M64_CRTC_H_TOTAL_DISP     = 0x00,
	M64_CRTC_H_SYNC_STRT_WID  = 0x01,
	M64_CRTC_V_TOTAL_DISP     = 0x02,
	M64_CRTC_V_SYNC_STRT_WID  = 0x03,
	M64_CRTC_VLINE_CRNT_VLINE = 0x04,
	M64_CRTC_OFF_PITCH        = 0x05,
	M64_CRTC_INT_CNTL         = 0x06,
	M64_CRTC_GEN_CNTL         = 0x07,
	M64_OVR_CLR               = 0x10,
	M64_OVR_WID_LEFT_RIGHT    = 0x11,
	M64_OVR_WID_TOP_BOTTOM    = 0x12,
	M64_CUR_CLR0              = 0x18,
	M64_CUR_CLR1              = 0x19,
	M64_CUR_OFFSET            = 0x1A,
	M64_CUR_HORZ_VERT_POSN    = 0x1B,
	M64_CUR_HORZ_VERT_OFF     = 0x1C,
	M64_GP_IO                 = 0x1E,
	M64_GP_IO_CNTL            = 0x1F,
	M64_SCRATCH_REG0          = 0x20,
	M64_SCRATCH_REG1          = 0x21,
	M64_CLOCK_CNTL            = 0x24,
	M64_BUS_CNTL              = 0x28,
	M64_MEM_CNTL              = 0x2C,
	M64_MEM_VGA_WP_SEL        = 0x2D,
	M64_MEM_VGA_RP_SEL        = 0x2E,
	M64_DAC_REGS              = 0x30,
	M64_DAC_CNTL              = 0x31,
	M64_GEN_TEST_CNTL         = 0x34,
	M64_CONFIG_CNTL           = 0x37,
	M64_CONFIG_CHIP_ID        = 0x38,
	M64_CONFIG_STAT0          = 0x39,
	M64_CRC_SIG               = 0x3A,

	/* Draw engine: destination trajectory (0x40-0x53) */
	M64_DST_OFF_PITCH         = 0x40,
	M64_DST_X                 = 0x41,
	M64_DST_Y                 = 0x42,
	M64_DST_Y_X               = 0x43,
	M64_DST_WIDTH             = 0x44,
	M64_DST_HEIGHT            = 0x45,
	M64_DST_HEIGHT_WIDTH      = 0x46,
	M64_DST_X_WIDTH           = 0x47,
	M64_DST_BRES_LNTH         = 0x48,
	M64_DST_BRES_ERR          = 0x49,
	M64_DST_BRES_INC          = 0x4A,
	M64_DST_BRES_DEC          = 0x4B,
	M64_DST_CNTL              = 0x4C,
	M64_DST_Y_X_ALIAS         = 0x4D,  /* GT */
	M64_TRAIL_BRES_ERR        = 0x4E,  /* GT */
	M64_TRAIL_BRES_INC        = 0x4F,  /* GT */
	M64_TRAIL_BRES_DEC        = 0x50,  /* GT */
	M64_LEAD_BRES_LNTH        = 0x51,  /* GT, alias of DST_BRES_LNTH */
	M64_Z_OFF_PITCH           = 0x52,  /* GT */
	M64_Z_CNTL                = 0x53,  /* GT */

	/* Source trajectory (0x60-0x6D) */
	M64_SRC_OFF_PITCH         = 0x60,
	M64_SRC_X                 = 0x61,
	M64_SRC_Y                 = 0x62,
	M64_SRC_Y_X               = 0x63,
	M64_SRC_WIDTH1            = 0x64,
	M64_SRC_HEIGHT1           = 0x65,
	M64_SRC_HEIGHT1_WIDTH1    = 0x66,
	M64_SRC_X_START           = 0x67,
	M64_SRC_Y_START           = 0x68,
	M64_SRC_Y_X_START         = 0x69,
	M64_SRC_WIDTH2            = 0x6A,
	M64_SRC_HEIGHT2           = 0x6B,
	M64_SRC_HEIGHT2_WIDTH2    = 0x6C,
	M64_SRC_CNTL              = 0x6D,

	/* Scaler / texture offsets (GT, 0x70-0x7F) */
	M64_TEX_0_OFF             = 0x70,  /* also SCALE_Y_OFF */
	M64_TEX_7_OFF             = 0x77,  /* also SCALE_WIDTH */
	M64_TEX_8_OFF             = 0x78,  /* also SCALE_HEIGHT */
	M64_TEX_10_OFF            = 0x7A,
	M64_SCALE_Y_PITCH         = 0x7B,
	M64_SCALE_X_INC           = 0x7C,
	M64_SCALE_Y_INC           = 0x7D,
	M64_SCALE_VACC            = 0x7E,
	M64_SCALE_3D_CNTL         = 0x7F,

	/* Host data, pattern, scissors (0x80-0xAD) */
	M64_HOST_DATA0            = 0x80,
	M64_HOST_DATA15           = 0x8F,
	M64_HOST_CNTL             = 0x90,
	M64_PAT_REG0              = 0xA0,
	M64_PAT_REG1              = 0xA1,
	M64_PAT_CNTL              = 0xA2,
	M64_SC_LEFT               = 0xA8,
	M64_SC_RIGHT              = 0xA9,
	M64_SC_LEFT_RIGHT         = 0xAA,
	M64_SC_TOP                = 0xAB,
	M64_SC_BOTTOM             = 0xAC,
	M64_SC_TOP_BOTTOM         = 0xAD,

	/* Data path (0xB0-0xB6), colour compare (0xC0-0xC2) */
	M64_DP_BKGD_CLR           = 0xB0,
	M64_DP_FRGD_CLR           = 0xB1,  /* also DP_FOG_CLR on GT */
	M64_DP_WRITE_MSK          = 0xB2,
	M64_DP_CHAIN_MSK          = 0xB3,
	M64_DP_PIX_WIDTH          = 0xB4,
	M64_DP_MIX                = 0xB5,
	M64_DP_SRC                = 0xB6,
	M64_CLR_CMP_CLR           = 0xC0,
	M64_CLR_CMP_MSK           = 0xC1,
	M64_CLR_CMP_CNTL          = 0xC2,
	M64_FIFO_STAT             = 0xC4,
	M64_CONTEXT_MASK          = 0xC8,
	M64_CONTEXT_LOAD_CNTL     = 0xCB,
	M64_GUI_TRAJ_CNTL         = 0xCC,
	M64_DST_X_Y               = 0xBA,  /* 3D RAGE II (GT-B) only: DST_Y_X with the halves swapped */
	M64_DST_WIDTH_HEIGHT      = 0xBB,  /* 3D RAGE II (GT-B) only: DST_HEIGHT_WIDTH swapped, initiator */
	M64_DP_SET_GUI_ENGINE     = 0xBF,  /* 3D RAGE II (GT-B) only */
	M64_TEX_PALETTE           = 0xDF,  /* 3D RAGE II (GT-B) only: index 31:24, RGB888 23:0 */
	M64_GUI_STAT              = 0xCE,

	/* Texture coordinates (GT, 0xD0-0xDC) */
	M64_S_X_INC2              = 0xD0,
	M64_S_Y_INC2              = 0xD1,
	M64_S_XY_INC2             = 0xD2,
	M64_S_XINC_START          = 0xD3,
	M64_S_Y_INC               = 0xD4,
	M64_S_START               = 0xD5,
	M64_T_X_INC2              = 0xD6,
	M64_T_Y_INC2              = 0xD7,
	M64_T_XY_INC2             = 0xD8,
	M64_T_XINC_START          = 0xD9,
	M64_T_Y_INC               = 0xDA,
	M64_T_START               = 0xDB,
	M64_TEX_SIZE_PITCH        = 0xDC,

	/* Colour, Z and alpha interpolators (GT, 0xF0-0xFE) */
	M64_RED_X_INC             = 0xF0,
	M64_RED_Y_INC             = 0xF1,
	M64_RED_START             = 0xF2,
	M64_GREEN_X_INC           = 0xF3,
	M64_GREEN_Y_INC           = 0xF4,
	M64_GREEN_START           = 0xF5,
	M64_BLUE_X_INC            = 0xF6,
	M64_BLUE_Y_INC            = 0xF7,
	M64_BLUE_START            = 0xF8,
	M64_Z_X_INC               = 0xF9,
	M64_Z_Y_INC               = 0xFA,
	M64_Z_START               = 0xFB,
	M64_ALPHA_X_INC           = 0xFC,
	M64_ALPHA_Y_INC           = 0xFD,
	M64_ALPHA_START           = 0xFE,

	/* Block 1 (combined index 0x100 + dword offset, [RRG] chapter 5) */
	M64_OVERLAY_Y_X_START     = 0x100,
	M64_OVERLAY_Y_X_END       = 0x101,
	M64_OVERLAY_VIDEO_KEY_CLR = 0x102,
	M64_OVERLAY_VIDEO_KEY_MSK = 0x103,
	M64_OVERLAY_GRAPHICS_KEY_CLR = 0x104,
	M64_OVERLAY_GRAPHICS_KEY_MSK = 0x105,
	M64_OVERLAY_KEY_CNTL      = 0x106,
	M64_OVERLAY_SCALE_INC     = 0x108,
	M64_OVERLAY_SCALE_CNTL    = 0x109,
	M64_SCALER_HEIGHT_WIDTH   = 0x10A,
	M64_OVERLAY_TEST          = 0x10B,
	M64_SCALER_THRESHOLD      = 0x10C,
	M64_CAPTURE_Y_X           = 0x110,
	M64_CAPTURE_HEIGHT_WIDTH  = 0x111,
	M64_VIDEO_FORMAT          = 0x112,
	M64_VIDEO_CONFIG          = 0x113,
	M64_CAPTURE_CONFIG        = 0x114,
	M64_TRIG_CNTL             = 0x115,
	M64_VIDEO_SYNC_TEST       = 0x116,
	M64_EXT_CRTC_GEN_CNTL_R   = 0x117,
	M64_VMC_CONFIG            = 0x118,
	M64_VMC_STATUS            = 0x119,
	M64_VMC_CMD               = 0x11A,
	M64_VMC_ARG0              = 0x11B,
	M64_VMC_ARG1              = 0x11C,
	M64_VMC_SNOOP_ARG0        = 0x11D,
	M64_VMC_SNOOP_ARG1        = 0x11E,
	M64_BUF0_OFFSET           = 0x120,
	M64_BUF0_PITCH            = 0x123,
	M64_BUF1_OFFSET           = 0x126,
	M64_BUF1_PITCH            = 0x129,
	M64_BUF0_CAP_ODD_OFFSET   = 0x12B,
	M64_BUF1_CAP_ODD_OFFSET   = 0x12C,
	M64_HW_DEBUG              = 0x150,

	M64_NUM_REGS              = 0x200
};

/* CRTC_GEN_CNTL bits ([RRG] 4-28) */
#define M64_CRTC_DBL_SCAN_EN    (1u << 0)
#define M64_CRTC_INTERLACE_EN   (1u << 1)
#define M64_CRTC_DISPLAY_DIS    (1u << 6)
#define M64_CRTC_EXT_DISP_EN    (1u << 24)
#define M64_CRTC_ENABLE         (1u << 25)
#define M64_VGA_ATI_LINEAR      (1u << 27)

/* DST_CNTL bits ([RRG] 4-48) */
#define M64_DST_X_DIR           (1u << 0)
#define M64_DST_Y_DIR           (1u << 1)
#define M64_DST_Y_MAJOR         (1u << 2)
#define M64_DST_X_TILE          (1u << 3)
#define M64_DST_Y_TILE          (1u << 4)
#define M64_DST_LAST_PEL        (1u << 5)
#define M64_DST_POLYGON_EN      (1u << 6)
#define M64_DST_24_ROT_EN       (1u << 7)
#define M64_DST_BRES_SIGN       (1u << 11)
#define M64_DST_POLYGON_RTEDGE_DIS (1u << 12)
#define M64_TRAIL_X_DIR         (1u << 13)
#define M64_TRAP_FILL_DIR       (1u << 14)
#define M64_TRAIL_BRES_SIGN     (1u << 15)
#define M64_BRES_SIGN_AUTO      (1u << 17)

/* SRC_CNTL bits ([RRG] 4-62) */
#define M64_SRC_PATT_EN         (1u << 0)
#define M64_SRC_PATT_ROT_EN     (1u << 1)
#define M64_SRC_LINEAR_EN       (1u << 2)
#define M64_SRC_BYTE_ALIGN      (1u << 3)
#define M64_SRC_LINE_X_DIR      (1u << 4)
#define M64_SRC_TRACK_DST       (1u << 7)

/* DP_PIX_WIDTH bits */
#define M64_DP_BYTE_PIX_ORDER   (1u << 24)

/* Pixel width codes (DP_*_PIX_WIDTH, CRTC_PIX_WIDTH) */
enum {
	M64_PIX_MONO   = 0,
	M64_PIX_4BPP   = 1,
	M64_PIX_8BPP   = 2,
	M64_PIX_15BPP  = 3,
	M64_PIX_16BPP  = 4,
	M64_PIX_24BPP  = 5,
	M64_PIX_32BPP  = 6,
	M64_PIX_RGB332 = 7,
	M64_PIX_Y8     = 8,
	M64_PIX_YUV422 = 11,
	M64_PIX_AYUV444 = 14,
	M64_PIX_ARGB4444 = 15
};

/* The GUI engine's operation in progress. The engine runs synchronously;
 * an operation stays "in progress" only while it waits for host data. */
struct Mach64Accel {
	int32_t  op;            /* M64_OP_* */
	bool     busy;

	/* pixel sizes: 0=mono, 1=4bpp, else bytes (1,2,4) encoded as log2+2 */
	int32_t  dst_size, src_size, host_size;
	uint32_t dst_offset, src_offset, poly_offset;   /* in pixels of the respective size */
	uint32_t dst_pitch, src_pitch;                   /* in pixels */

	int32_t  dst_x, dst_y, dst_x_start, dst_y_start;
	int32_t  src_x, src_y, src_x_start, src_y_start;
	int32_t  xinc, yinc;
	int32_t  x_count, dst_width, dst_height;
	int32_t  src_x_count, src_y_count;
	int32_t  src_width1, src_height1, src_width2, src_height2;

	int32_t  mix_fg, mix_bg, source_fg, source_bg, source_mix;
	bool     source_host;

	int32_t  sc_left, sc_right, sc_top, sc_bottom;
	int32_t  err, inc, dec;
	int32_t  rot0, rot;
	bool     row_ended;
	bool     poly_draw;

	uint8_t  pattern[8][8];
	uint8_t  pattern_clr4x2[2][4];
	uint8_t  pattern_clr8x1[8];

	/* 3D (GT) state of a trapezoid in progress */
	int32_t  trail_x;
	int32_t  trail_err, trail_inc, trail_dec;
	int32_t  lines_left;
};

enum {
	M64_OP_RECT = 0,
	M64_OP_LINE = 1,
	M64_OP_TRAP = 2
};

struct Mach64State {
	uint32_t reg[M64_NUM_REGS];     /* register file as it reads back */
	uint8_t  pll[16];               /* PLL registers ([RRG] appendix B) */

	/* PCI configuration */
	uint32_t pci_lfb_base;          /* BAR0, 16 MB aligned */
	uint32_t pci_io_base;           /* BAR1, block I/O, 256 bytes */
	uint32_t pci_rom_bar;
	uint16_t pci_command;
	uint8_t  pci_int_line;
	uint8_t  pci_cfg40;             /* user defined configuration: I/O decode */
	uint32_t pci_adapter_id;

	/* I/O decode as currently registered */
	uint32_t io_sparse_base_active; /* 0 when not registered */
	uint32_t io_block_base_active;

	Mach64Accel accel;

	uint32_t vblank_seen;           /* frames counted for CRTC_INT_CNTL */
	uint32_t reserved[16];
};

extern Mach64State mach64;

/* true when the emulated card is a mach64 GT (machine=svga_ati_3drage) */
bool Mach64_Present(void);

/* Setup, called from SVGA_Setup_ATI */
void SVGA_Setup_Mach64(void);

/* Register access by combined register index (0x000-0x0FF block 0, 0x100-0x1FF block 1) */
uint32_t Mach64_ReadReg(unsigned int reg);
void     Mach64_WriteReg(unsigned int reg, uint32_t val, uint32_t mask);

/* Scanout */
struct Mach64_Display {
	Bitu htotal, hdend, hbstart, hbend, hrstart, hrend;   /* pixels */
	Bitu vtotal, vdend, vbstart, vbend, vrstart, vrend;   /* lines */
	Bitu pixclock;                                        /* Hz */
	Bitu pitch_bytes;
	Bitu start_bytes;
	bool doublescan;
	bool interlace;
};
bool     Mach64_ExtDisplayActive(void);
bool     Mach64_GetDisplay(Mach64_Display &d);
Bitu     Mach64_DisplayStart(void);
uint8_t* Mach64_DrawLine(Bitu vidstart, Bitu line, uint8_t *templine);
void     Mach64_VerticalRetrace(void);
bool     Mach64_InVerticalBlank(void);

/* INT 10h / VESA support (src/ints/int10_mach64.cpp) */
struct VideoModeBlock;
void     Mach64_INT10_SetMode(const VideoModeBlock *mode);
void     Mach64_INT10_SetDisplayStart(uint32_t byte_offset);
uint32_t Mach64_INT10_GetDisplayStart(void);
void     Mach64_INT10_SetPitch(uint32_t pitch_pixels);
void     Mach64_INT10_SetBank(unsigned int bank);
unsigned int Mach64_INT10_GetBank(void);
void     Mach64_INT10_Extended(void);     /* INT 10h AH=A0h and ROM:0064h */
void     Mach64_SetupROM(uint32_t rom_base);

/* helpers for the BIOS side (vga_mach64.cpp) */
void     Mach64_IOWriteReg(unsigned int reg, uint32_t val);  /* through the decoded I/O ports */
uint32_t Mach64_IOReadReg(unsigned int reg);
void     Mach64_LFBInfo(uint32_t &base, bool &enabled);
void     Mach64_GetBanks(uint32_t &rd, uint32_t &wr);
void     Mach64_SetupHandlers(void);
class PageHandler;
PageHandler *Mach64_GetMMIOHandler(void);

/* PCI */
void     Mach64_PCI_ConfigWrite(uint8_t regnum, uint8_t val);
uint8_t  Mach64_PCI_ConfigRead(uint8_t regnum);

/* GUI engine (vga_mach64_accel.cpp) */
void     Mach64_Accel_Reset(void);
void     Mach64_Accel_Write(unsigned int reg, uint32_t val, uint32_t mask);
void     Mach64_Accel_HostData(uint32_t val, int bits);
bool     Mach64_Accel_Busy(void);

/* 3D / scaler pipe (vga_mach64_3d.cpp) */
bool     Mach64_3D_Enabled(void);
void     Mach64_3D_TrapBegin(void);
void     Mach64_3D_EdgeStepX(void);
void     Mach64_3D_EdgeStepY(void);
void     Mach64_3D_SpanBegin(int32_t n, int32_t dir);   /* n: pixels from the lead edge in DST_X_DIR */
void     Mach64_3D_TrapEnd(void);
void     Mach64_3D_SpanStep(void);
bool     Mach64_3D_Pixel(int32_t x, int32_t y, uint32_t dst_addr, int dst_size, uint32_t dst, uint32_t &out);
void     Mach64_Scale_Begin(void);
bool     Mach64_Scale_Pixel(int32_t col, int32_t row, uint32_t dst, uint32_t &out);
uint32_t Mach64_ReadPixel(uint32_t addr, int sz);
void     Mach64_WritePixel(uint32_t addr, int sz, uint32_t v);

/* savestate */
void     POD_Save_VGA_Mach64(std::ostream &stream);
void     POD_Load_VGA_Mach64(std::istream &stream);

#endif
