/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright(c) 2007 - 2011 Realtek Corporation.
 * Adapted from vendor phydm_iqk.h; source under GPL version 2.
 * Gain-search dimension is 3: the algorithm indexes RXIQK1=1, RXIQK2=2.
 */
struct _IQK_INFORMATION {
	boolean		LOK_fail[NUM];
	boolean		IQK_fail[2][NUM];
	u32		iqc_matrix[2][NUM];
	u8      iqk_times;
	u32		rf_reg18;
	u32		lna_idx;
	u8		rxiqk_step;
	u8		tmp1bcc;
	u8		kcount;

	u32		iqk_channel[2];
	boolean		IQK_fail_report[2][4][2]; /*channel/path/TRX(TX:0, RX:1) */
	u32		IQK_CFIR_real[2][4][2][8]; /*channel / path / TRX(TX:0, RX:1) / CFIR_real*/
	u32		IQK_CFIR_imag[2][4][2][8]; /*channel / path / TRX(TX:0, RX:1) / CFIR_imag*/
	u8		retry_count[2][4][3]; /* channel / path / (TXK:0, RXK1:1, RXK2:2) */
	u8		gs_retry_count[2][4][3]; /* channel / path / RXIQK1=1, RXIQK2=2 */
	u8		RXIQK_fail_code[2][4]; /* channel / path 0:SRXK1 fail, 1:RXK1 fail 2:RXK2 fail */
	u32		LOK_IDAC[2][4];		/*channel / path*/
	u16		RXIQK_AGC[2][4];	 /*channel / path*/
	u32		bypass_iqk[2][4];	/*channel / 0xc94/0xe94*/
	u32		tmp_GNTWL;
	boolean		is_BTG;
	boolean		isbnd;
};
