/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Registers of the Unisoc UMS9230 video codec (VSP), as far as they have been
 * found out from the vendor's AVC library (see VIDEO_CODEC.md in the port's
 * repository). Offsets are from the start of the 0x32000000 window. Nothing here
 * is documented by the vendor; entries marked "guess" are inferred and have not
 * been confirmed on hardware.
 */
#ifndef _SPRD_VSP_REGS_H
#define _SPRD_VSP_REGS_H

#include <linux/bits.h>

#define VSP_RESET		0x1010	/* ARM_VSP_RST writes 0xd0 */
#define VSP_BUS_IDLE		0x101c	/* mask 0x101, DDR bus idle */
#define VSP_INT_CTRL		0x1028	/* 0 = off, 1 = enable (written before start) */
#define VSP_START		0x1030	/* 0xa starts one slice */
#define VSP_INT_MASK		0x1038	/* 0xffffffff before start */
#define VSP_INT_STATUS		0x1120	/* read after the interrupt (guess) */

#define VSP_BSM_ADDR0		0x1080	/* bitstream buffer, bus address >> 4 */
#define VSP_BSM_ADDR1		0x1084

#define VSP_CUR_LUMA		0x1088	/* current picture, bus address >> 4 */
#define VSP_CUR_CHROMA		0x108c
#define VSP_CUR_AUX0		0x1090	/* per-MB side data (guess: motion vectors) */
#define VSP_CUR_AUX1		0x10b0	/* guess */

#define VSP_PARAM_SLICE_QP	0x103c
#define VSP_PARAM_SEQ		0x1044
#define VSP_PARAM_FLAGS		0x1048
#define VSP_PARAM_CHROMA_QP	0x104c
#define VSP_PARAM_UNKNOWN0	0x1034
#define VSP_PARAM_UNKNOWN1	0x1050
#define VSP_PARAM_UNKNOWN2	0x1054
#define VSP_PARAM_UNKNOWN3	0x1058
#define VSP_PARAM_UNKNOWN4	0x105c
#define VSP_PARAM_UNKNOWN5	0x1060
#define VSP_PARAM_REFIDX_FIRST	0x1064	/* 0x1064-0x107c, 0x1160-0x1170, 0x1184-0x1198 */
#define VSP_PARAM_REFIDX_LAST	0x107c
#define VSP_PARAM_PICSIZE	0x1180

/* bitstream module */
#define VSP_BSM_CTRL		0x8000
#define VSP_BSM_LEN		0x8004
#define VSP_BSM_CMD		0x8008	/* write n << 24 to look at n bits; |1 consumes them */
#define VSP_BSM_BUSY		0x8018	/* bit 27 set while busy */
#define VSP_BSM_READY		0x801c	/* bit 2 */
#define VSP_BSM_DATA		0x8010	/* result of a look-ahead */
#define VSP_BSM_STATE		0x8020	/* bit 0 set when a command can be given */

#define VSP_BSM_BUSY_BIT	BIT(27)
#define VSP_BSM_START		BIT(31)
#define VSP_BSM_STATE_READY	BIT(0)

/* VSP_PARAM_SLICE_QP */
#define VSP_QP_VALID		BIT(31)
#define VSP_QP_SHIFT		25

/* VSP_PARAM_SEQ */
#define VSP_SEQ_NOT_BASELINE	BIT(31)
#define VSP_SEQ_SCALING		BIT(29)
#define VSP_SEQ_FIRST_MB	GENMASK(28, 16)
#define VSP_SEQ_MB_ROW		GENMASK(15, 8)
#define VSP_SEQ_MB_COL		GENMASK(7, 0)

/* VSP_PARAM_FLAGS */
#define VSP_FL_EXPLICIT_WP	BIT(15)
#define VSP_FL_CABAC		BIT(14)
#define VSP_FL_T8X8		BIT(13)
#define VSP_FL_DIRECT8X8	BIT(12)
#define VSP_FL_DIRECT_SPATIAL	BIT(11)
#define VSP_FL_CONSTR_INTRA	BIT(10)
#define VSP_FL_WEIGHTED_PRED	BIT(16)
#define VSP_FL_BIPRED_IDC	GENMASK(18, 17)
#define VSP_FL_DEBLOCK_IDC	GENMASK(20, 19)
#define VSP_FL_ALPHA		GENMASK(9, 5)	/* guess: which of the two is which */
#define VSP_FL_BETA		GENMASK(4, 0)

/* VSP_PARAM_CHROMA_QP */
#define VSP_CQP_CB		GENMASK(4, 0)
#define VSP_CQP_CR		GENMASK(9, 5)
#define VSP_CQP_L0		GENMASK(14, 10)
#define VSP_CQP_L1		GENMASK(19, 15)

#define VSP_PICSIZE_H_MBS	GENMASK(31, 20)
#define VSP_PICSIZE_W_PIX	GENMASK(15, 4)

#endif
