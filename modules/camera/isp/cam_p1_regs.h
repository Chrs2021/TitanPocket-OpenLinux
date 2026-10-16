/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT6771 ISP 5.0 pass 1 (CAM) registers.
 *
 * Offsets from each CAM block's base: CAM_A 0x1a004000, CAM_B 0x1a006000,
 * CAM_C 0x1a008000 (same layout); CAM_UNI (shared) 0x1a003000.
 *
 * Names and offsets: the stock HAL's libcamdrv_twin.so (its register dump
 * prints each name with the physical address; tools/re/elf_strrefs.py and
 * the Ghidra decompile in out/camera/re/twin.c), 309 per CAM block; plus the
 * registers ChromeOS's MT8183 driver uses (marked "cros":
 * mtk-isp/isp_50/cam/mtk_cam-regs.h, same ISP generation). Field layouts
 * aren't known yet. See docs/camera.md, "MediaTek ISP".
 */
#ifndef _TITAN_CAM_P1_REGS_H
#define _TITAN_CAM_P1_REGS_H

#define CAM_UNI_BASE	0x1a003000
#define CAM_A_BASE	0x1a004000
#define CAM_B_BASE	0x1a006000
#define CAM_C_BASE	0x1a008000
#define CAM_BLOCK_SIZE	0x2000


/* CTL */
#define CAM_CTL_EN                           0x0004
#define CAM_CTL_DMA_EN                       0x0008
#define CAM_CTL_FMT_SEL                      0x000c
#define CAM_CTL_SEL                          0x0010
#define CAM_CTL_EN2                          0x0018
#define CAM_CTL_RAW_INT_EN                   0x0020	/* cros */
#define CAM_CTL_RAW_INT_STAT                 0x0024	/* cros */
#define CAM_CTL_RAW_INT2_STAT                0x0034	/* cros */

/* AFO */
#define CAM_AFO_FBC_STATUS                   0x0134	/* cros */

/* AAO */
#define CAM_AAO_FBC_STATUS                   0x013c	/* cros */

/* CQ */
#define CAM_CQ_THR0_BASEADDR                 0x0198	/* cros */

/* TG */
#define CAM_TG_SEN_MODE                      0x0230	/* cros */
#define CAM_TG_VF_CON                        0x0234	/* cros */
#define CAM_TG_SEN_GRAB_PXL                  0x0238
#define CAM_TG_SEN_GRAB_LIN                  0x023c
#define CAM_TG_INTER_ST                      0x026c	/* cros */

/* DMX */
#define CAM_DMX_CTL                          0x0330
#define CAM_DMX_CROP                         0x0334
#define CAM_DMX_VSIZE                        0x0338

/* RMG */
#define CAM_RMG_HDR_CFG                      0x0350
#define CAM_RMG_HDR_GAIN                     0x0354
#define CAM_RMG_HDR_CFG2                     0x0358

/* RMM */
#define CAM_RMM_OSC                          0x0390
#define CAM_RMM_MC                           0x0394
#define CAM_RMM_REVG_1                       0x0398
#define CAM_RMM_REVG_2                       0x039c
#define CAM_RMM_LEOS                         0x03a0
#define CAM_RMM_MC2                          0x03a4
#define CAM_RMM_DIFF_LB                      0x03a8
#define CAM_RMM_MA                           0x03ac
#define CAM_RMM_TUNE                         0x03b0

/* OBC */
#define CAM_OBC_OFFST0                       0x03f0
#define CAM_OBC_OFFST1                       0x03f4
#define CAM_OBC_OFFST2                       0x03f8
#define CAM_OBC_OFFST3                       0x03fc
#define CAM_OBC_GAIN0                        0x0400
#define CAM_OBC_GAIN1                        0x0404
#define CAM_OBC_GAIN2                        0x0408
#define CAM_OBC_GAIN3                        0x040c

/* BNR */
#define CAM_BNR_BPC_CON                      0x0420
#define CAM_BNR_BPC_TH1                      0x0424
#define CAM_BNR_BPC_TH2                      0x0428
#define CAM_BNR_BPC_TH3                      0x042c
#define CAM_BNR_BPC_TH4                      0x0430
#define CAM_BNR_BPC_DTC                      0x0434
#define CAM_BNR_BPC_COR                      0x0438
#define CAM_BNR_BPC_TBLI1                    0x043c
#define CAM_BNR_BPC_TBLI2                    0x0440
#define CAM_BNR_BPC_TH1_C                    0x0444
#define CAM_BNR_BPC_TH2_C                    0x0448
#define CAM_BNR_BPC_TH3_C                    0x044c
#define CAM_BNR_NR1_CON                      0x0450
#define CAM_BNR_NR1_CT_CON                   0x0454
#define CAM_BNR_NR1_CT_CON2                  0x0458
#define CAM_BNR_NR1_CT_CON3                  0x045c
#define CAM_BNR_PDC_CON                      0x0460
#define CAM_BNR_PDC_GAIN_L0                  0x0464
#define CAM_BNR_PDC_GAIN_L1                  0x0468
#define CAM_BNR_PDC_GAIN_L2                  0x046c
#define CAM_BNR_PDC_GAIN_L3                  0x0470
#define CAM_BNR_PDC_GAIN_L4                  0x0474
#define CAM_BNR_PDC_GAIN_R0                  0x0478
#define CAM_BNR_PDC_GAIN_R1                  0x047c
#define CAM_BNR_PDC_GAIN_R2                  0x0480
#define CAM_BNR_PDC_GAIN_R3                  0x0484
#define CAM_BNR_PDC_GAIN_R4                  0x0488
#define CAM_BNR_PDC_TH_GB                    0x048c
#define CAM_BNR_PDC_TH_IA                    0x0490
#define CAM_BNR_PDC_TH_HD                    0x0494
#define CAM_BNR_PDC_SL                       0x0498
#define CAM_BNR_PDC_POS                      0x049c

/* SCM */
#define CAM_SCM_CFG0                         0x04b0
#define CAM_SCM_CFG1                         0x04b4

/* RPG */
#define CAM_RPG_SATU_1                       0x04c0
#define CAM_RPG_SATU_2                       0x04c4
#define CAM_RPG_GAIN_1                       0x04c8
#define CAM_RPG_GAIN_2                       0x04cc
#define CAM_RPG_OFST_1                       0x04d0
#define CAM_RPG_OFST_2                       0x04d4

/* RRZ */
#define CAM_RRZ_CTL                          0x04e0
#define CAM_RRZ_IN_IMG                       0x04e4
#define CAM_RRZ_OUT_IMG                      0x04e8
#define CAM_RRZ_HORI_STEP                    0x04ec
#define CAM_RRZ_VERT_STEP                    0x04f0
#define CAM_RRZ_HORI_INT_OFST                0x04f4
#define CAM_RRZ_HORI_SUB_OFST                0x04f8
#define CAM_RRZ_VERT_INT_OFST                0x04fc
#define CAM_RRZ_VERT_SUB_OFST                0x0500
#define CAM_RRZ_MODE_TH                      0x0504
#define CAM_RRZ_MODE_CTL                     0x0508
#define CAM_RRZ_RLB_AOFST                    0x050c
#define CAM_RRZ_LBLD_CFG                     0x0510
#define CAM_RRZ_NNIR_TBL_SEL                 0x0514

/* RMX */
#define CAM_RMX_CTL                          0x0540
#define CAM_RMX_CROP                         0x0544
#define CAM_RMX_VSIZE                        0x0548

/* BMX */
#define CAM_BMX_CTL                          0x0580
#define CAM_BMX_CROP                         0x0584
#define CAM_BMX_VSIZE                        0x0588

/* UFEG */
#define CAM_UFEG_CON                         0x05c0

/* LSC */
#define CAM_LSC_CTL1                         0x05d0
#define CAM_LSC_CTL2                         0x05d4
#define CAM_LSC_CTL3                         0x05d8
#define CAM_LSC_LBLOCK                       0x05dc
#define CAM_LSC_RATIO_0                      0x05e0
#define CAM_LSC_TPIPE_OFST                   0x05e4
#define CAM_LSC_TPIPE_SIZE                   0x05e8
#define CAM_LSC_GAIN_TH                      0x05ec
#define CAM_LSC_RATIO_1                      0x05f0

/* AF */
#define CAM_AF_CON                           0x0610
#define CAM_AF_TH_0                          0x0614
#define CAM_AF_TH_1                          0x0618
#define CAM_AF_FLT_1                         0x061c
#define CAM_AF_FLT_2                         0x0620
#define CAM_AF_FLT_3                         0x0624
#define CAM_AF_FLT_4                         0x0628
#define CAM_AF_FLT_5                         0x062c
#define CAM_AF_FLT_6                         0x0630
#define CAM_AF_FLT_7                         0x0634
#define CAM_AF_FLT_8                         0x0638
#define CAM_AF_SIZE                          0x0640
#define CAM_AF_VLD                           0x0644
#define CAM_AF_BLK_0                         0x0648
#define CAM_AF_BLK_1                         0x064c
#define CAM_AF_TH_2                          0x0650
#define CAM_AF_FLT_9                         0x0654
#define CAM_AF_FLT_10                        0x0658
#define CAM_AF_FLT_11                        0x065c
#define CAM_AF_FLT_12                        0x0660
#define CAM_AF_LUT_H0_0                      0x0670
#define CAM_AF_LUT_H0_1                      0x0674
#define CAM_AF_LUT_H0_2                      0x0678
#define CAM_AF_LUT_H0_3                      0x067c
#define CAM_AF_LUT_H0_4                      0x0680
#define CAM_AF_LUT_H1_0                      0x0690
#define CAM_AF_LUT_H1_1                      0x0694
#define CAM_AF_LUT_H1_2                      0x0698
#define CAM_AF_LUT_H1_3                      0x069c
#define CAM_AF_LUT_H1_4                      0x06a0
#define CAM_AF_LUT_V_0                       0x06b0
#define CAM_AF_LUT_V_1                       0x06b4
#define CAM_AF_LUT_V_2                       0x06b8
#define CAM_AF_LUT_V_3                       0x06bc
#define CAM_AF_LUT_V_4                       0x06c0
#define CAM_AF_CON2                          0x06c4
#define CAM_AF_BLK_2                         0x06c8

/* RCP */
#define CAM_RCP_CROP_CON1                    0x0800
#define CAM_RCP_CROP_CON2                    0x0804

/* SGG1 */
#define CAM_SGG1_PGN                         0x0810
#define CAM_SGG1_GMRC_1                      0x0814
#define CAM_SGG1_GMRC_2                      0x0818

/* VBN */
#define CAM_VBN_GAIN                         0x0a30
#define CAM_VBN_OFST                         0x0a34
#define CAM_VBN_TYPE                         0x0a38
#define CAM_VBN_SPARE                        0x0a3c

/* AMX */
#define CAM_AMX_CTL                          0x0a50
#define CAM_AMX_CROP                         0x0a54
#define CAM_AMX_VSIZE                        0x0a58

/* BIN */
#define CAM_BIN_CTL                          0x0a70
#define CAM_BIN_FTH                          0x0a74
#define CAM_BIN_SPARE                        0x0a78

/* DBN */
#define CAM_DBN_GAIN                         0x0a90
#define CAM_DBN_OFST                         0x0a94
#define CAM_DBN_SPARE                        0x0a98

/* RCP3 */
#define CAM_RCP3_CROP_CON1                   0x0ab0
#define CAM_RCP3_CROP_CON2                   0x0ab4

/* DBS */
#define CAM_DBS_SIGMA                        0x0ac0
#define CAM_DBS_BSTBL_0                      0x0ac4
#define CAM_DBS_BSTBL_1                      0x0ac8
#define CAM_DBS_BSTBL_2                      0x0acc
#define CAM_DBS_BSTBL_3                      0x0ad0
#define CAM_DBS_CTL                          0x0ad4
#define CAM_DBS_CTL_2                        0x0ad8
#define CAM_DBS_SIGMA_2                      0x0adc
#define CAM_DBS_YGN                          0x0ae0
#define CAM_DBS_SL_Y12                       0x0ae4
#define CAM_DBS_SL_Y34                       0x0ae8
#define CAM_DBS_SL_G12                       0x0aec
#define CAM_DBS_SL_G34                       0x0af0

/* SL2F */
#define CAM_SL2F_CEN                         0x0b00
#define CAM_SL2F_RR_CON0                     0x0b04
#define CAM_SL2F_RR_CON1                     0x0b08
#define CAM_SL2F_GAIN                        0x0b0c
#define CAM_SL2F_RZ                          0x0b10
#define CAM_SL2F_XOFF                        0x0b14
#define CAM_SL2F_YOFF                        0x0b18
#define CAM_SL2F_SLP_CON0                    0x0b1c
#define CAM_SL2F_SLP_CON1                    0x0b20
#define CAM_SL2F_SIZE                        0x0b2c

/* SGG2 */
#define CAM_SGG2_PGN                         0x0b90
#define CAM_SGG2_GMRC_1                      0x0b94
#define CAM_SGG2_GMRC_2                      0x0b98

/* RMB */
#define CAM_RMB_MODE                         0x0bc0

/* SGG5 */
#define CAM_SGG5_PGN                         0x0cb0
#define CAM_SGG5_GMRC_1                      0x0cb4
#define CAM_SGG5_GMRC_2                      0x0cb8

/* UFE */
#define CAM_UFE_CON                          0x0d20

/* ADBS */
#define CAM_ADBS_CTL                         0x0d50
#define CAM_ADBS_GRAY_BLD_0                  0x0d54
#define CAM_ADBS_GRAY_BLD_1                  0x0d58
#define CAM_ADBS_BIAS_LUT_R0                 0x0d5c
#define CAM_ADBS_BIAS_LUT_R1                 0x0d60
#define CAM_ADBS_BIAS_LUT_R2                 0x0d64
#define CAM_ADBS_BIAS_LUT_R3                 0x0d68
#define CAM_ADBS_BIAS_LUT_G0                 0x0d6c
#define CAM_ADBS_BIAS_LUT_G1                 0x0d70
#define CAM_ADBS_BIAS_LUT_G2                 0x0d74
#define CAM_ADBS_BIAS_LUT_G3                 0x0d78
#define CAM_ADBS_BIAS_LUT_B0                 0x0d7c
#define CAM_ADBS_BIAS_LUT_B1                 0x0d80
#define CAM_ADBS_BIAS_LUT_B2                 0x0d84
#define CAM_ADBS_BIAS_LUT_B3                 0x0d88
#define CAM_ADBS_GAIN_0                      0x0d8c
#define CAM_ADBS_GAIN_1                      0x0d90
#define CAM_ADBS_IVGN_0                      0x0d94
#define CAM_ADBS_IVGN_1                      0x0d98
#define CAM_ADBS_HDR                         0x0d9c
#define CAM_ADBS_CMDL_ONLY_1                 0x0da0

/* DCPN */
#define CAM_DCPN_HDR_EN                      0x0db0
#define CAM_DCPN_IN_IMG_SIZE                 0x0db4
#define CAM_DCPN_ALGO_PARAM1                 0x0db8
#define CAM_DCPN_ALGO_PARAM2                 0x0dbc
#define CAM_DCPN_GTM_X0                      0x0dc0
#define CAM_DCPN_GTM_Y0                      0x0dc4
#define CAM_DCPN_GTM_S0                      0x0dc8
#define CAM_DCPN_GTM_S1                      0x0dcc

/* CPN */
#define CAM_CPN_HDR_CTL_EN                   0x0df0
#define CAM_CPN_IN_IMG_SIZE                  0x0df4
#define CAM_CPN_ALGO_PARAM1                  0x0df8
#define CAM_CPN_ALGO_PARAM2                  0x0dfc
#define CAM_CPN_GTM_X0X1                     0x0e00
#define CAM_CPN_GTM_X2X3                     0x0e04
#define CAM_CPN_GTM_X4X5                     0x0e08
#define CAM_CPN_GTM_X6                       0x0e0c
#define CAM_CPN_GTM_Y0Y1                     0x0e10
#define CAM_CPN_GTM_Y2Y3                     0x0e14
#define CAM_CPN_GTM_Y4Y5                     0x0e18
#define CAM_CPN_GTM_Y6                       0x0e1c
#define CAM_CPN_GTM_S0S1                     0x0e20
#define CAM_CPN_GTM_S2S3                     0x0e24
#define CAM_CPN_GTM_S4S5                     0x0e28
#define CAM_CPN_GTM_S6S7                     0x0e2c

/* SGM */
#define CAM_SGM_R_OFST_TABLE0_3              0x0fa0
#define CAM_SGM_R_OFST_TABLE4_7              0x0fa4
#define CAM_SGM_R_OFST_TABLE8_11             0x0fa8
#define CAM_SGM_R_OFST_TABLE12_15            0x0fac
#define CAM_SGM_G_OFST_TABLE0_3              0x0fb0
#define CAM_SGM_G_OFST_TABLE4_7              0x0fb4
#define CAM_SGM_G_OFST_TABLE8_11             0x0fb8
#define CAM_SGM_G_OFST_TABLE12_15            0x0fbc
#define CAM_SGM_B_OFST_TABLE0_3              0x0fc0
#define CAM_SGM_B_OFST_TABLE4_7              0x0fc4
#define CAM_SGM_B_OFST_TABLE8_11             0x0fc8
#define CAM_SGM_B_OFST_TABLE12_15            0x0fcc

/* SPECIAL */
#define CAM_SPECIAL_FUN_EN                   0x1018

/* IMGO */
#define CAM_IMGO_BASE_ADDR                   0x1020
#define CAM_IMGO_OFST_ADDR                   0x1028
#define CAM_IMGO_XSIZE                       0x1030
#define CAM_IMGO_YSIZE                       0x1034
#define CAM_IMGO_STRIDE                      0x1038
#define CAM_IMGO_CON                         0x103c
#define CAM_IMGO_CON2                        0x1040
#define CAM_IMGO_CON3                        0x1044
#define CAM_IMGO_CROP                        0x1048
#define CAM_IMGO_CON4                        0x104c

/* RRZO */
#define CAM_RRZO_BASE_ADDR                   0x1050
#define CAM_RRZO_OFST_ADDR                   0x1058
#define CAM_RRZO_XSIZE                       0x1060
#define CAM_RRZO_YSIZE                       0x1064
#define CAM_RRZO_STRIDE                      0x1068
#define CAM_RRZO_CON                         0x106c
#define CAM_RRZO_CON2                        0x1070
#define CAM_RRZO_CON3                        0x1074
#define CAM_RRZO_CROP                        0x1078
#define CAM_RRZO_CON4                        0x107c

/* AFO */
#define CAM_AFO_BASE_ADDR                    0x10b0
#define CAM_AFO_OFST_ADDR                    0x10b8
#define CAM_AFO_DRS                          0x10bc
#define CAM_AFO_XSIZE                        0x10c0
#define CAM_AFO_YSIZE                        0x10c4
#define CAM_AFO_STRIDE                       0x10c8
#define CAM_AFO_CON                          0x10cc
#define CAM_AFO_CON2                         0x10d0
#define CAM_AFO_CON3                         0x10d4
#define CAM_AFO_CON4                         0x10dc

/* UFEO */
#define CAM_UFEO_BASE_ADDR                   0x1110
#define CAM_UFEO_OFST_ADDR                   0x1118
#define CAM_UFEO_XSIZE                       0x1120
#define CAM_UFEO_YSIZE                       0x1124
#define CAM_UFEO_STRIDE                      0x1128
#define CAM_UFEO_CON                         0x112c
#define CAM_UFEO_CON2                        0x1130
#define CAM_UFEO_CON3                        0x1134
#define CAM_UFEO_CON4                        0x113c

/* BPCI */
#define CAM_BPCI_XSIZE                       0x1180
#define CAM_BPCI_YSIZE                       0x1184
#define CAM_BPCI_STRIDE                      0x1188
#define CAM_BPCI_CON                         0x118c
#define CAM_BPCI_CON2                        0x1190
#define CAM_BPCI_CON3                        0x1194
#define CAM_BPCI_CON4                        0x119c

/* LSCI */
#define CAM_LSCI_OFST_ADDR                   0x11d8
#define CAM_LSCI_XSIZE                       0x11e0
#define CAM_LSCI_YSIZE                       0x11e4
#define CAM_LSCI_STRIDE                      0x11e8
#define CAM_LSCI_CON                         0x11ec
#define CAM_LSCI_CON2                        0x11f0
#define CAM_LSCI_CON3                        0x11f4
#define CAM_LSCI_CON4                        0x11fc

/* UFGO */
#define CAM_UFGO_BASE_ADDR                   0x1320
#define CAM_UFGO_OFST_ADDR                   0x1324
#define CAM_UFGO_XSIZE                       0x1330
#define CAM_UFGO_YSIZE                       0x1334
#define CAM_UFGO_STRIDE                      0x1338
#define CAM_UFGO_CON                         0x133c
#define CAM_UFGO_CON2                        0x1340
#define CAM_UFGO_CON3                        0x1344
#define CAM_UFGO_CON4                        0x134c

/* IMGO */
#define CAM_IMGO_ERR_STAT                    0x1360	/* cros */

/* RRZO */
#define CAM_RRZO_ERR_STAT                    0x1364	/* cros */

/* AAO */
#define CAM_AAO_ERR_STAT                     0x1368	/* cros */

/* AFO */
#define CAM_AFO_ERR_STAT                     0x136c	/* cros */

/* LCSO */
#define CAM_LCSO_ERR_STAT                    0x1370	/* cros */

/* BPCI */
#define CAM_BPCI_ERR_STAT                    0x137c	/* cros */

/* LSCI */
#define CAM_LSCI_ERR_STAT                    0x1384	/* cros */

/* LMVO */
#define CAM_LMVO_ERR_STAT                    0x1390	/* cros */

/* FLKO */
#define CAM_FLKO_ERR_STAT                    0x1394	/* cros */

/* PSO */
#define CAM_PSO_ERR_STAT                     0x13a0	/* cros */

/* FRAME */
#define CAM_FRAME_SEQ_NUM                    0x13b8	/* cros */

/* META0 */
#define CAM_META0_SEQ_NUM                    0x14dc	/* cros */

/* META1 */
#define CAM_META1_SEQ_NUM                    0x151c	/* cros */

/* CAM_UNI (offsets from CAM_UNI_BASE) */
#define CAM_UNI_TOP_MOD_EN                   0x0010
#define CAM_UNI_TOP_FMT_SEL                  0x001c
#define CAM_UNI_RAWI_YSIZE                   0x0134

#endif
