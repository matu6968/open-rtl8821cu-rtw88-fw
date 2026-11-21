/* SPDX-License-Identifier: GPL-2.0-only
 * Realtek PHYDM RTL8821C IQK algorithm, vendor v5.2.5.3 reference.
 * Adapted for a standalone, one-path firmware environment. See NOTICES.TXT.
 */
static boolean iqk_wait_indirect_ready(struct PHY_DM_STRUCT *p_dm_odm)
{
	for (u32 budget = 0; budget < 30000; ++budget) {
		if (odm_read_1byte(p_dm_odm, 0x1703) & BIT(5)) return true;
		if (!(budget & 255)) {
			diag_mailbox_poll();
			foundation_poll();
		}
	}
	access_error |= 32;
	return false;
}
u32
_iqk_indirect_read_reg(
	struct PHY_DM_STRUCT	*p_dm_odm,
	u16 reg_addr
)
{
	u32 j = 0;

	odm_write_4byte(p_dm_odm, 0x1700, 0x800f0000 | reg_addr);

	if (!iqk_wait_indirect_ready(p_dm_odm)) return 0;

	return odm_read_4byte(p_dm_odm, 0x1708);

}

void
_iqk_indirect_write_reg(
	struct PHY_DM_STRUCT	*p_dm_odm,
	u16 reg_addr,
	u32 bit_mask,
	u32 reg_value
)
{
	u32 val, i = 0, j = 0, bitpos = 0;

	if (bit_mask == 0x0)
		return;
	if (bit_mask == 0xffffffff) {
		odm_write_4byte(p_dm_odm, 0x1704, reg_value);

		if (!iqk_wait_indirect_ready(p_dm_odm)) return;

		odm_write_4byte(p_dm_odm, 0x1700, 0xc00f0000 | reg_addr);
	} else {
		for (i = 0; i <= 31; i++) {
			if (((bit_mask >> i) & 0x1) == 0x1) {
				bitpos = i;
				break;
			}
		}

		val = _iqk_indirect_read_reg(p_dm_odm, reg_addr);
		if (access_error & 32) return;
		val = (val & (~bit_mask)) | (reg_value << bitpos);

		odm_write_4byte(p_dm_odm, 0x1704, val);

		if (!iqk_wait_indirect_ready(p_dm_odm)) return;

		odm_write_4byte(p_dm_odm, 0x1700, 0xc00f0000 | reg_addr);
	}
}

void
_iqk_set_gnt_wl_high(
	struct PHY_DM_STRUCT	*p_dm_odm
)
{
	u32 val = 0;
	u8 state = 0x1, sw_control = 0x1;

	val = (sw_control) ? ((state << 1) | 0x1) : 0;
	_iqk_indirect_write_reg(p_dm_odm, 0x38, 0x3000, val);
	_iqk_indirect_write_reg(p_dm_odm, 0x38, 0x0300, val);
}

void _iqk_set_gnt_bt_low(
	struct PHY_DM_STRUCT	*p_dm_odm
)
{
	u32 val = 0;
	u8 state = 0x0, sw_control = 0x1;

	val = (sw_control) ? ((state << 1) | 0x1) : 0;
	_iqk_indirect_write_reg(p_dm_odm, 0x38, 0xc000, val);
	_iqk_indirect_write_reg(p_dm_odm, 0x38, 0x0c00, val);
}

void _iqk_set_gnt_wl_gnt_bt(
	struct PHY_DM_STRUCT	*p_dm_odm,
	boolean beforeK
)
{
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;

	if (beforeK) {
		_iqk_set_gnt_wl_high(p_dm_odm);
		_iqk_set_gnt_bt_low(p_dm_odm);
	} else
		_iqk_indirect_write_reg(p_dm_odm, 0x38, MASKDWORD, p_iqk_info->tmp_GNTWL);
}

void
_iqk_fill_iqk_report_8821c(
	void		*p_dm_void,
	u8			channel
)
{
	struct PHY_DM_STRUCT	*p_dm_odm = (struct PHY_DM_STRUCT *)p_dm_void;
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;
	u32		tmp1 = 0x0, tmp2 = 0x0, tmp3 = 0x0;
	u8		i;

	for (i = 0; i < SS_8821C; i++) {
		tmp1 = tmp1 + ((p_iqk_info->IQK_fail_report[channel][i][TX_IQK] & 0x1) << i);
		tmp2 = tmp2 + ((p_iqk_info->IQK_fail_report[channel][i][RX_IQK] & 0x1) << (i + 4));
		tmp3 = tmp3 + ((p_iqk_info->RXIQK_fail_code[channel][i] & 0x3) << (i * 2 + 8));
	}
	odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008);
	odm_set_bb_reg(p_dm_odm, 0x1bf0, 0x00ffffff, tmp1 | tmp2 | tmp3);

	for (i = 0; i < SS_8821C; i++)
		odm_write_4byte(p_dm_odm, 0x1be8 + (i * 4), (p_iqk_info->RXIQK_AGC[channel][(i * 2) + 1] << 16) | p_iqk_info->RXIQK_AGC[channel][i * 2]);
}

void
_iqk_iqk_fail_report_8821c(
	struct PHY_DM_STRUCT	*p_dm_odm
)
{
	u32		tmp1bf0 = 0x0;
	u8		i;

	tmp1bf0 = odm_read_4byte(p_dm_odm, 0x1bf0);

	for (i = 0; i < 4; i++) {
		if (tmp1bf0 & (0x1 << i))
			((void)0);
		if (tmp1bf0 & (0x1 << (i + 12)))
			((void)0);

	}
}

void
_iqk_backup_mac_bb_8821c(
	struct PHY_DM_STRUCT	*p_dm_odm,
	u32		*MAC_backup,
	u32		*BB_backup,
	u32		*backup_mac_reg,
	u32		*backup_bb_reg,
	u8		num_backup_bb_reg
)
{
	u32 i;

	for (i = 0; i < MAC_REG_NUM_8821C; i++)
		MAC_backup[i] = odm_read_4byte(p_dm_odm, backup_mac_reg[i]);

	for (i = 0; i < num_backup_bb_reg; i++)
		BB_backup[i] = odm_read_4byte(p_dm_odm, backup_bb_reg[i]);

}

void
_iqk_backup_rf_8821c(
	struct PHY_DM_STRUCT	*p_dm_odm,
	u32		RF_backup[][SS_8821C],
	u32		*backup_rf_reg
)
{
	u32 i, j;

	for (i = 0; i < RF_REG_NUM_8821C; i++)
		for (j = 0; j < SS_8821C; j++)
			RF_backup[i][j] = odm_get_rf_reg(p_dm_odm, j, backup_rf_reg[i], RFREGOFFSETMASK);

}

void
_iqk_agc_bnd_int_8821c(
	struct PHY_DM_STRUCT	*p_dm_odm
)
{

	odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008);
	odm_write_4byte(p_dm_odm, 0x1b00, 0xf80a7008);
	odm_write_4byte(p_dm_odm, 0x1b00, 0xf8015008);
	odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008);

}

void
_iqk_bb_reset_8821c(
	struct PHY_DM_STRUCT	*p_dm_odm
)
{
	boolean		cca_ing = false;
	u32		count = 0;

	odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0x0, RFREGOFFSETMASK, 0x10000);

	while (1) {
		odm_write_4byte(p_dm_odm, 0x8fc, 0x0);
		odm_set_bb_reg(p_dm_odm, 0x198c, 0x7, 0x7);
		cca_ing = (boolean) odm_get_bb_reg(p_dm_odm, 0xfa0, BIT(3));

		if (count > 30)
			cca_ing = false;

		if (cca_ing) {
			ODM_delay_ms(1);
			count++;
		} else {
			odm_write_1byte(p_dm_odm, 0x808, 0x0);
			odm_set_bb_reg(p_dm_odm, 0xa04, BIT(27) | BIT26 | BIT25 | BIT24, 0x0);

			odm_set_bb_reg(p_dm_odm, 0x0, BIT(16), 0x0);
			odm_set_bb_reg(p_dm_odm, 0x0, BIT(16), 0x1);

			if (odm_get_bb_reg(p_dm_odm, 0x660, BIT(16)))
				odm_write_4byte(p_dm_odm, 0x6b4, 0x89000006);
			((void)0);
			break;
		}
	}
}

void
_iqk_afe_setting_8821c(
	struct PHY_DM_STRUCT	*p_dm_odm,
	boolean		do_iqk
)
{
	if (do_iqk) {

		odm_write_4byte(p_dm_odm, 0xc60, 0x50000000);
		odm_write_4byte(p_dm_odm, 0xc60, 0x700F0040);

		odm_write_4byte(p_dm_odm, 0xc58, 0xd8000402);
		odm_write_4byte(p_dm_odm, 0xc5c, 0xd1000120);
		odm_write_4byte(p_dm_odm, 0xc6c, 0x00000a15);
		_iqk_bb_reset_8821c(p_dm_odm);

	} else {

		odm_write_4byte(p_dm_odm, 0xc60, 0x50000000);
		odm_write_4byte(p_dm_odm, 0xc60, 0x700B8040);

		odm_write_4byte(p_dm_odm, 0xc58, 0xd8020402);
		odm_write_4byte(p_dm_odm, 0xc5c, 0xde000120);
		odm_write_4byte(p_dm_odm, 0xc6c, 0x0000122a);

	}
}

void
_iqk_restore_mac_bb_8821c(
	struct PHY_DM_STRUCT		*p_dm_odm,
	u32		*MAC_backup,
	u32		*BB_backup,
	u32		*backup_mac_reg,
	u32		*backup_bb_reg,
	u8		num_backup_bb_reg
)
{
	u32 i;

	for (i = 0; i < MAC_REG_NUM_8821C; i++)
		odm_write_4byte(p_dm_odm, backup_mac_reg[i], MAC_backup[i]);
	for (i = 0; i < num_backup_bb_reg; i++)
		odm_write_4byte(p_dm_odm, backup_bb_reg[i], BB_backup[i]);

}

void
_iqk_restore_rf_8821c(
	struct PHY_DM_STRUCT			*p_dm_odm,
	u32			*backup_rf_reg,
	u32			RF_backup[][SS_8821C]
)
{
	u32 i;

	odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0xef, RFREGOFFSETMASK, 0x0);
	odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0xee, RFREGOFFSETMASK, 0x0);
	odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0xdf, RFREGOFFSETMASK, RF_backup[0][ODM_RF_PATH_A] & (~BIT(4)));

	odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0xde, RFREGOFFSETMASK, RF_backup[1][ODM_RF_PATH_A] & (~BIT(4)));

	for (i = 2; i < (RF_REG_NUM_8821C-1); i++)
		odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, backup_rf_reg[i], RFREGOFFSETMASK, RF_backup[i][ODM_RF_PATH_A]);

	odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0x1, RFREGOFFSETMASK, (RF_backup[5][ODM_RF_PATH_A] & (~BIT(0))));

}

void
_iqk_backup_iqk_8821c(
	struct PHY_DM_STRUCT			*p_dm_odm,
	u8				step
)
{
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;
	u8		i, j, k, path, idx;
	u32		tmp;
	u16		iqk_apply[2] = {0xc94, 0xe94};

	if (step == 0x0) {
		p_iqk_info->iqk_channel[1] = p_iqk_info->iqk_channel[0];
		for (i = 0; i < SS_8821C; i++) {
			p_iqk_info->LOK_IDAC[1][i] = p_iqk_info->LOK_IDAC[0][i];
			p_iqk_info->RXIQK_AGC[1][i] = p_iqk_info->RXIQK_AGC[0][i];
			p_iqk_info->bypass_iqk[1][i] = p_iqk_info->bypass_iqk[0][i];
			p_iqk_info->RXIQK_fail_code[1][i] = p_iqk_info->RXIQK_fail_code[0][i];
			for (j = 0; j < 2; j++) {
				p_iqk_info->IQK_fail_report[1][i][j] = p_iqk_info->IQK_fail_report[0][i][j];
				for (k = 0; k < 8; k++) {
					p_iqk_info->IQK_CFIR_real[1][i][j][k] = p_iqk_info->IQK_CFIR_real[0][i][j][k];
					p_iqk_info->IQK_CFIR_imag[1][i][j][k] = p_iqk_info->IQK_CFIR_imag[0][i][j][k];
				}
			}
		}

		for (i = 0; i < 4; i++) {
			p_iqk_info->RXIQK_fail_code[0][i] = 0x0;
			p_iqk_info->RXIQK_AGC[0][i] = 0x0;
			for (j = 0; j < 2; j++) {
				p_iqk_info->IQK_fail_report[0][i][j] = true;

			}
			for (j = 0; j < 3; j++) {
				p_iqk_info->retry_count[0][i][j] = 0x0;
				p_iqk_info->gs_retry_count[0][i][j] = 0x0;
			}
		}
	} else {
		p_iqk_info->iqk_channel[0] = p_iqk_info->rf_reg18;
		for (path = 0; path < SS_8821C; path++) {
			p_iqk_info->LOK_IDAC[0][path] = odm_get_rf_reg(p_dm_odm, path, 0x58, RFREGOFFSETMASK);
			p_iqk_info->bypass_iqk[0][path] = odm_get_bb_reg(p_dm_odm, iqk_apply[path], MASKDWORD);

			for (idx = 0; idx < 2; idx++) {
				odm_set_bb_reg(p_dm_odm, 0x1b00, MASKDWORD, 0xf8000008 | path << 1);

				if (idx == 0)
					odm_set_bb_reg(p_dm_odm, 0x1b0c, BIT(13) | BIT(12), 0x3);
				else
					odm_set_bb_reg(p_dm_odm, 0x1b0c, BIT(13) | BIT(12), 0x1);

				odm_set_bb_reg(p_dm_odm, 0x1bd4, BIT(20) | BIT(19) | BIT(18) | BIT(17) | BIT(16), 0x10);

				for (i = 0; i < 8; i++) {
					odm_set_bb_reg(p_dm_odm, 0x1bd8, MASKDWORD, 0xe0000001 + (i * 4));
					tmp = odm_get_bb_reg(p_dm_odm, 0x1bfc, MASKDWORD);
					p_iqk_info->IQK_CFIR_real[0][path][idx][i] = (tmp & 0x0fff0000) >> 16;
					p_iqk_info->IQK_CFIR_imag[0][path][idx][i] = tmp & 0xfff;
				}
			}
			odm_set_bb_reg(p_dm_odm, 0x1bd8, MASKDWORD, 0x0);
			odm_set_bb_reg(p_dm_odm, 0x1b0c, BIT(13) | BIT(12), 0x0);
		}
	}
}

void
_iqk_reload_iqk_setting_8821c(
	struct PHY_DM_STRUCT			*p_dm_odm,
	u8				channel,
	u8				reload_idx
)
{
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;
	u8 i, path, idx;
	u16		iqk_apply[2] = {0xc94, 0xe94};

	for (path = 0; path < SS_8821C; path++) {
		for (idx = 0; idx < reload_idx; idx++) {
			odm_set_bb_reg(p_dm_odm, 0x1b00, MASKDWORD, 0xf8000008 | path << 1);
			odm_set_bb_reg(p_dm_odm, 0x1b2c, MASKDWORD, 0x7);
			odm_set_bb_reg(p_dm_odm, 0x1b38, MASKDWORD, 0x20000000);
			odm_set_bb_reg(p_dm_odm, 0x1b3c, MASKDWORD, 0x20000000);
			odm_set_bb_reg(p_dm_odm, 0x1bcc, MASKDWORD, 0x00000000);

			if (idx == 0)
				odm_set_bb_reg(p_dm_odm, 0x1b0c, BIT(13) | BIT(12), 0x3);
			else
				odm_set_bb_reg(p_dm_odm, 0x1b0c, BIT(13) | BIT(12), 0x1);

			odm_set_bb_reg(p_dm_odm, 0x1bd4, BIT(20) | BIT(19) | BIT(18) | BIT(17) | BIT(16), 0x10);

			for (i = 0; i < 8; i++) {
				odm_write_4byte(p_dm_odm, 0x1bd8,	((0xc0000000 >> idx) + 0x3) + (i * 4) + (p_iqk_info->IQK_CFIR_real[channel][path][idx][i] << 9));
				odm_write_4byte(p_dm_odm, 0x1bd8, ((0xc0000000 >> idx) + 0x1) + (i * 4) + (p_iqk_info->IQK_CFIR_imag[channel][path][idx][i] << 9));
			}
		}
		odm_set_bb_reg(p_dm_odm, iqk_apply[path], MASKDWORD, p_iqk_info->bypass_iqk[channel][path]);

		odm_set_bb_reg(p_dm_odm, 0x1bd8, MASKDWORD, 0x0);
		odm_set_bb_reg(p_dm_odm, 0x1b0c, BIT(13) | BIT(12), 0x0);
	}
}

boolean
_iqk_reload_iqk_8821c(
	struct PHY_DM_STRUCT			*p_dm_odm,
	boolean			reset
)
{
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;
	u8 i;
	boolean reload = false;

	/* A clear request still needs the current RF18 cache key. The original
	 * reference only refreshed it in the non-clear branch, which can associate
	 * newly calibrated coefficients with the previous channel/bandwidth.
	 */
	p_iqk_info->rf_reg18 = odm_get_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0x18, RFREGOFFSETMASK);
	if (reset) {
		for (i = 0; i < 2; i++)
			p_iqk_info->iqk_channel[i] = 0x0;
	} else {

		for (i = 0; i < 2; i++) {
			if (p_iqk_info->rf_reg18 == p_iqk_info->iqk_channel[i]) {
				_iqk_reload_iqk_setting_8821c(p_dm_odm, i, 2);
				_iqk_fill_iqk_report_8821c(p_dm_odm, i);
				((void)0);
				reload = true;
			}
		}
	}
	return reload;
}

void
_iqk_rfe_setting_8821c(
	struct PHY_DM_STRUCT	*p_dm_odm,
	boolean		ext_pa_on
)
{
	if (ext_pa_on) {

		odm_write_4byte(p_dm_odm, 0xcb0, 0x77777777);
		odm_write_4byte(p_dm_odm, 0xcb4, 0x00007777);
		odm_write_4byte(p_dm_odm, 0xcbc, 0x0000083B);

		((void)0);
	} else {

		odm_write_4byte(p_dm_odm, 0xcb0, 0x77171117);
		odm_write_4byte(p_dm_odm, 0xcb4, 0x00001177);
		odm_write_4byte(p_dm_odm, 0xcbc, 0x00000404);

	}
}

void
_iqk_rfsetting_8821c(
	struct PHY_DM_STRUCT	*p_dm_odm
)
{
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;

	u8 path;
	u32 tmp;

	odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008);
	odm_write_4byte(p_dm_odm, 0x1bb8, 0x00000000);

	for (path = 0; path < SS_8821C; path++) {

		tmp = odm_get_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0xdf, RFREGOFFSETMASK);
		tmp = (tmp & (~BIT(4))) | BIT(1) | BIT(11);
		odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0xdf, RFREGOFFSETMASK, tmp);

		if (p_iqk_info->is_BTG) {
			tmp = odm_get_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0xde, RFREGOFFSETMASK);
			tmp = (tmp & (~BIT(4))) | BIT(15);

			odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0xde, RFREGOFFSETMASK, tmp);
		}

		if (!p_iqk_info->is_BTG) {

			odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0xef, RFREGOFFSETMASK, 0x80000);
			odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x33, RFREGOFFSETMASK, 0x00024);
			odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x3e, RFREGOFFSETMASK, 0x0003f);
			odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x3f, RFREGOFFSETMASK, 0x60fde);
			odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0xef, RFREGOFFSETMASK, 0x00000);
			if (*p_dm_odm->p_band_type == ODM_BAND_5G) {
				odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0xef, BIT(19), 0x1);
				odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x33, RFREGOFFSETMASK, 0x00026);
				odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x3e, RFREGOFFSETMASK, 0x00037);
				odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x3f, RFREGOFFSETMASK, 0xdefce);
				odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0xef, BIT(19), 0x0);
			} else {
				odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0xef, BIT(19), 0x1);
				odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x33, RFREGOFFSETMASK, 0x00026);
				odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x3e, RFREGOFFSETMASK, 0x00037);
				odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x3f, RFREGOFFSETMASK, 0x5efce);
				odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0xef, BIT(19), 0x0);
			}
		} else {

			odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0xee, RFREGOFFSETMASK, 0x01000);
			odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x33, RFREGOFFSETMASK, 0x00004);
			odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x3f, RFREGOFFSETMASK, 0x01ec1);
			odm_set_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0xee, RFREGOFFSETMASK, 0x00000);
		}
	}
}

void
_iqk_configure_macbb_8821c(
	struct PHY_DM_STRUCT		*p_dm_odm
)
{

	odm_write_1byte(p_dm_odm, 0x522, 0x7f);
	odm_set_bb_reg(p_dm_odm, 0x1518, BIT(16), 0x1);
	odm_set_bb_reg(p_dm_odm, 0x550, BIT(11) | BIT(3), 0x0);
	odm_set_bb_reg(p_dm_odm, 0x90c, BIT(15), 0x1);
	odm_set_bb_reg(p_dm_odm, 0x9a4, BIT(31), 0x0);

	odm_set_bb_reg(p_dm_odm, 0xc94, BIT(0), 0x1);

	odm_write_4byte(p_dm_odm, 0xc00, 0x00000004);

	odm_set_bb_reg(p_dm_odm, 0xb00, BIT(8), 0x0);

}

void
_iqk_lok_setting_8821c(
	struct PHY_DM_STRUCT	*p_dm_odm,
	u8 path,
	u8          uPADindex
)
{
	u32 LOK0x56_2G = 0x50ef3;
	u32 LOK0x56_5G = 0x50ee8;
	u32 LOK0x33 = 0;
	u32 LOK0x78 = 0xbcbba;
	u32 tmp = 0;

	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;

	LOK0x33 = uPADindex;

	if (p_dm_odm->mp_mode)
		odm_set_bb_reg(p_dm_odm, 0x810, BIT(7)|BIT(6)|BIT(5)|BIT(4), 0x8);

	if (p_iqk_info->is_BTG) {
		tmp = (LOK0x78 & 0x1c000) >> 14;
		odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008 | path << 1);
		odm_write_4byte(p_dm_odm, 0x1bcc, 0x1b);
		odm_write_1byte(p_dm_odm, 0x1b23, 0x00);
		odm_write_1byte(p_dm_odm, 0x1b2b, 0x80);

		LOK0x78 = LOK0x78 & (0xe3fff | ((u32)uPADindex << 14));
		odm_set_rf_reg(p_dm_odm, path, 0x78, RFREGOFFSETMASK, LOK0x78);
		odm_set_rf_reg(p_dm_odm, path, 0x5c, RFREGOFFSETMASK, 0x05320);
		odm_set_rf_reg(p_dm_odm, path, 0x8f, RFREGOFFSETMASK, 0xac018);
		odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0xee, BIT(4), 0x1);
		odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0x33, BIT(3), 0x0);
		((void)0);
	} else {

		odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008 | path << 1);
		odm_write_4byte(p_dm_odm, 0x1bcc, 0x9);
		odm_write_1byte(p_dm_odm, 0x1b23, 0x00);

		switch (*p_dm_odm->p_band_type) {
		case ODM_BAND_2_4G:
			odm_write_1byte(p_dm_odm, 0x1b2b, 0x00);
			LOK0x56_2G = LOK0x56_2G & (0xfff1f | ((u32)uPADindex << 5));
			odm_set_rf_reg(p_dm_odm, path, 0x56, RFREGOFFSETMASK, LOK0x56_2G);
			odm_set_rf_reg(p_dm_odm, path, 0x8f, RFREGOFFSETMASK, 0xadc18);
			odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0xef, BIT(4), 0x1);
			odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0x33, BIT(3), 0x0);
			break;
		case ODM_BAND_5G:
			odm_write_1byte(p_dm_odm, 0x1b2b, 0x00);
			LOK0x56_5G = LOK0x56_5G & (0xfff1f | ((u32)uPADindex << 5));
			odm_set_rf_reg(p_dm_odm, path, 0x56, RFREGOFFSETMASK, LOK0x56_5G);
			odm_set_rf_reg(p_dm_odm, path, 0x8f, RFREGOFFSETMASK, 0xadc18);
			odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0xef, BIT(4), 0x1);
			odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0x33, BIT(3), 0x1);
			break;
		}
	}

	odm_set_rf_reg(p_dm_odm, path, 0x33, BIT(2) | BIT(1) | BIT(0), LOK0x33);
	((void)0);

}

void
_iqk_txk_setting_8821c(
	struct PHY_DM_STRUCT	*p_dm_odm,
	u8 path
)
{
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;

	if (p_iqk_info->is_BTG) {
		odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008 | path << 1);
		odm_write_4byte(p_dm_odm, 0x1bcc, 0x1b);
		odm_write_4byte(p_dm_odm, 0x1b20, 0x00840008);

		odm_set_rf_reg(p_dm_odm, path, 0x78, RFREGOFFSETMASK, 0xbcbba);
		odm_set_rf_reg(p_dm_odm, path, 0x5c, RFREGOFFSETMASK, 0x04320);
		odm_set_rf_reg(p_dm_odm, path, 0x8f, RFREGOFFSETMASK, 0xac018);
		odm_write_1byte(p_dm_odm, 0x1b2b, 0x80);
	} else {
		odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008 | path << 1);
		odm_write_4byte(p_dm_odm, 0x1bcc, 0x9);
		odm_write_4byte(p_dm_odm, 0x1b20, 0x01440008);

		switch (*p_dm_odm->p_band_type) {
		case ODM_BAND_2_4G:
			odm_set_rf_reg(p_dm_odm, path, 0x56, RFREGOFFSETMASK, 0x50EF3);
			odm_set_rf_reg(p_dm_odm, path, 0x8f, RFREGOFFSETMASK, 0xadc18);
			odm_write_1byte(p_dm_odm, 0x1b2b, 0x00);
			break;
		case ODM_BAND_5G:
			odm_set_rf_reg(p_dm_odm, path, 0x56, RFREGOFFSETMASK, 0x50EF0);
			odm_set_rf_reg(p_dm_odm, path, 0x8f, RFREGOFFSETMASK, 0xa9c18);
			odm_write_1byte(p_dm_odm, 0x1b2b, 0x00);
			break;
		}

	}

}

void
_iqk_rxk1setting_8821c(
	struct PHY_DM_STRUCT	*p_dm_odm,
	u8 path
)
{
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;

	if (p_iqk_info->is_BTG) {
		odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008 | path << 1);
		odm_write_1byte(p_dm_odm, 0x1b2b, 0x80);
		odm_write_4byte(p_dm_odm, 0x1bcc, 0x09);
		odm_write_4byte(p_dm_odm, 0x1b20, 0x01450008);
		odm_write_4byte(p_dm_odm, 0x1b24, 0x01460c88);

		odm_set_rf_reg(p_dm_odm, path, 0x78, RFREGOFFSETMASK, 0x8cbba);
		odm_set_rf_reg(p_dm_odm, path, 0x5c, RFREGOFFSETMASK, 0x00320);
		odm_set_rf_reg(p_dm_odm, path, 0x8f, RFREGOFFSETMASK, 0xa8018);
	} else {
		odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008 | path << 1);
		switch (*p_dm_odm->p_band_type) {
		case ODM_BAND_2_4G:
			odm_write_1byte(p_dm_odm, 0x1bcc, 0x12);
			odm_write_1byte(p_dm_odm, 0x1b2b, 0x00);
			odm_write_4byte(p_dm_odm, 0x1b20, 0x01450008);
			odm_write_4byte(p_dm_odm, 0x1b24, 0x01461068);

			odm_set_rf_reg(p_dm_odm, path, 0x56, RFREGOFFSETMASK, 0x510f3);
			odm_set_rf_reg(p_dm_odm, path, 0x8f, RFREGOFFSETMASK, 0xa9c00);
			break;
		case ODM_BAND_5G:
			odm_write_1byte(p_dm_odm, 0x1bcc, 0x9);
			odm_write_1byte(p_dm_odm, 0x1b2b, 0x00);
			odm_write_4byte(p_dm_odm, 0x1b20, 0x00450008);
			odm_write_4byte(p_dm_odm, 0x1b24, 0x00461468);

			odm_set_rf_reg(p_dm_odm, path, 0x56, RFREGOFFSETMASK, 0x510f3);
			odm_set_rf_reg(p_dm_odm, path, 0x8f, RFREGOFFSETMASK, 0xa9c00);
			break;
		}
	}

}

static	u8	btg_lna[5] = {0x0, 0x4, 0x8, 0xc, 0xf};
static	u8	wlg_lna[5] = {0x0, 0x1, 0x2, 0x3, 0x5};
static	u8	wla_lna[5] = {0x0, 0x1, 0x3, 0x4, 0x5};

void
_iqk_rxk2setting_8821c(
	struct PHY_DM_STRUCT	*p_dm_odm,
	u8 path,
	boolean is_gs
)
{
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;

	if (p_iqk_info->is_BTG) {
		if (is_gs) {
			p_iqk_info->tmp1bcc = 0x1b;
			p_iqk_info->lna_idx = 2;
		}
		odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008 | path << 1);
		odm_write_1byte(p_dm_odm, 0x1b2b, 0x80);
		odm_write_4byte(p_dm_odm, 0x1bcc, p_iqk_info->tmp1bcc);
		odm_write_4byte(p_dm_odm, 0x1b20, 0x01450008);
		odm_write_4byte(p_dm_odm, 0x1b24, (0x01460048 | (btg_lna[p_iqk_info->lna_idx] << 10)));

		odm_set_rf_reg(p_dm_odm, path, 0x78, RFREGOFFSETMASK, 0x8cbba);
		odm_set_rf_reg(p_dm_odm, path, 0x5c, RFREGOFFSETMASK, 0x00320);
		odm_set_rf_reg(p_dm_odm, path, 0x8f, RFREGOFFSETMASK, 0xa8018);
	} else {
		odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008 | path << 1);
		switch (*p_dm_odm->p_band_type) {
		case ODM_BAND_2_4G:
			if (is_gs) {
				p_iqk_info->tmp1bcc = 0x12;
				p_iqk_info->lna_idx = 2;
			}
			odm_write_1byte(p_dm_odm, 0x1bcc, p_iqk_info->tmp1bcc);
			odm_write_1byte(p_dm_odm, 0x1b2b, 0x00);
			odm_write_4byte(p_dm_odm, 0x1b20, 0x01450008);
			odm_write_4byte(p_dm_odm, 0x1b24, (0x01460048 | (wlg_lna[p_iqk_info->lna_idx] << 10)));
			odm_set_rf_reg(p_dm_odm, path, 0x56, RFREGOFFSETMASK, 0x510f3);
			odm_set_rf_reg(p_dm_odm, path, 0x8f, RFREGOFFSETMASK, 0xa9c00);
			break;
		case ODM_BAND_5G:
			if (is_gs) {
				p_iqk_info->tmp1bcc = 0x12;
				p_iqk_info->lna_idx = 2;
			}
			odm_write_1byte(p_dm_odm, 0x1bcc, p_iqk_info->tmp1bcc);
			odm_write_1byte(p_dm_odm, 0x1b2b, 0x00);
			odm_write_4byte(p_dm_odm, 0x1b20, 0x00450008);
			odm_write_4byte(p_dm_odm, 0x1b24, (0x01460048 | (wla_lna[p_iqk_info->lna_idx] << 10)));
			odm_set_rf_reg(p_dm_odm, path, 0x56, RFREGOFFSETMASK, 0x51000);
			odm_set_rf_reg(p_dm_odm, path, 0x8f, RFREGOFFSETMASK, 0xa9c00);
			break;
		}
	}

}

boolean
_iqk_check_cal_8821c(
	struct PHY_DM_STRUCT			*p_dm_odm,
	u32				IQK_CMD
)
{
	boolean		notready = true, fail = true;
	u32		delay_count = 0x0;

	while (notready) {
		if (odm_read_4byte(p_dm_odm, 0x1b00) == (IQK_CMD & 0xffffff0f)) {
			fail = (boolean) odm_get_bb_reg(p_dm_odm, 0x1b08, BIT(26));
			notready = false;
		} else {
			ODM_delay_ms(1);
			delay_count++;
		}

		if (delay_count >= 50) {
			access_error |= STATION_IQK_TIMEOUT;
			fail = true;

			break;
		}
	}
	((void)0);
	return fail;
}

boolean
_iqk_rx_iqk_gain_search_fail_8821c(
	struct PHY_DM_STRUCT			*p_dm_odm,
	u8		path,
	u8		step
)
{
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;
	boolean		fail = true;
	u32	IQK_CMD = 0x0, rf_reg0, tmp, rxbb;
	u8	IQMUX[4] = {0x9, 0x12, 0x1b, 0x24}, *plna;
	u8	idx;
	u8	lna_setting[5];

	if (p_iqk_info->is_BTG)
		plna = btg_lna;
	else if (*p_dm_odm->p_band_type == ODM_BAND_2_4G)
		plna = wlg_lna;
	else
		plna = wla_lna;

	for (idx = 0; idx < 4; idx++)
		if (p_iqk_info->tmp1bcc == IQMUX[idx])
			break;

	odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008 | path << 1);
	odm_write_4byte(p_dm_odm, 0x1bcc, p_iqk_info->tmp1bcc);

	if (step == RXIQK1)
		((void)0);

	if (step == RXIQK1)
		IQK_CMD = 0xf8000208 | (1 << (path + 4));
	else
		IQK_CMD = 0xf8000308 | (1 << (path + 4));

	((void)0);

	_iqk_set_gnt_wl_gnt_bt(p_dm_odm, true);
	odm_write_4byte(p_dm_odm, 0x1b00, IQK_CMD);
	odm_write_4byte(p_dm_odm, 0x1b00, IQK_CMD + 0x1);
	ODM_delay_ms(GS_delay_8821C);
	fail = _iqk_check_cal_8821c(p_dm_odm, IQK_CMD);
	((void)0);
	_iqk_set_gnt_wl_gnt_bt(p_dm_odm, false);

	if (step == RXIQK2) {
		rf_reg0 = odm_get_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x0, RFREGOFFSETMASK);
		odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008 | path << 1);
		((void)0);
		tmp = (rf_reg0 & 0x1fe0) >> 5;
		rxbb = tmp & 0x1f;

		if (rxbb == 0x1) {
			if (idx != 3)
				idx++;
			else if (p_iqk_info->lna_idx != 0x0)
				p_iqk_info->lna_idx--;
			else
				p_iqk_info->isbnd = true;
			fail = true;
		} else if (rxbb == 0xa) {
			if (idx != 0)
				idx--;
			else if (p_iqk_info->lna_idx != 0x4)
				p_iqk_info->lna_idx++;
			else
				p_iqk_info->isbnd = true;
			fail = true;
		} else
			fail = false;

		if (p_iqk_info->isbnd == true)
			fail = false;

		p_iqk_info->tmp1bcc = IQMUX[idx];

		if (fail) {
			odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008 | path << 1);
			odm_write_4byte(p_dm_odm, 0x1b24, (odm_read_4byte(p_dm_odm, 0x1b24) & 0xffffc3ff) | (*(plna + p_iqk_info->lna_idx) << 10));
		}
	}
	return fail;
}

boolean
_lok_one_shot_8821c(
	struct PHY_DM_STRUCT	*p_dm_void,
	u8 path,
	u8          uPADindex

)
{
	struct PHY_DM_STRUCT	*p_dm_odm = (struct PHY_DM_STRUCT *)p_dm_void;
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;
	u8		delay_count = 0, i;
	boolean		LOK_notready = false;
	u32		LOK_temp1 = 0, LOK_temp2 = 0, LOK_temp3 = 0;
	u32		IQK_CMD = 0x0;
	u8		LOKreg[] = {0x58, 0x78};

	((void)0);

	IQK_CMD = 0xf8000008 | (1 << (4 + path));

	((void)0);

	_iqk_set_gnt_wl_gnt_bt(p_dm_odm, true);
	odm_write_4byte(p_dm_odm, 0x1b00, IQK_CMD);
	odm_write_4byte(p_dm_odm, 0x1b00, IQK_CMD + 1);

	ODM_delay_ms(LOK_delay_8821C);

	delay_count = 0;
	LOK_notready = true;

	while (LOK_notready) {
		if (odm_read_4byte(p_dm_odm, 0x1b00) == (IQK_CMD & 0xffffff0f))
			LOK_notready = false;
		else
			LOK_notready = true;

		if (LOK_notready) {
			ODM_delay_ms(1);
			delay_count++;
		}

		if (delay_count >= 50) {
			access_error |= STATION_IQK_TIMEOUT;

			break;
		}
	}

	_iqk_set_gnt_wl_gnt_bt(p_dm_odm, false);


	if (!LOK_notready) {
		LOK_temp2 = odm_get_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x8, RFREGOFFSETMASK);
		LOK_temp3 = odm_get_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x58, RFREGOFFSETMASK);

		((void)0);
	} else {
		((void)0);
	}
	p_iqk_info->LOK_fail[path] = LOK_notready;

	return LOK_notready;
}

boolean
_iqk_one_shot_8821c(
	void		*p_dm_void,
	u8		path,
	u8		idx
)
{
	struct PHY_DM_STRUCT	*p_dm_odm = (struct PHY_DM_STRUCT *)p_dm_void;
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;
	u8		delay_count = 0;
	boolean		notready = true, fail = true, search_fail = true;
	u32		IQK_CMD = 0x0, tmp;
	u16		iqk_apply[2] = {0xc94, 0xe94};

	if (idx == TX_IQK)
		((void)0);
	else if (idx == RXIQK1)
		((void)0);
	else
		((void)0);

	if (idx == TXIQK) {
		IQK_CMD = 0xf8000008 | ((*p_dm_odm->p_band_width + 4) << 8) | (1 << (path + 4));
		((void)0);

	} else if (idx == RXIQK1) {
		if (*p_dm_odm->p_band_width == 2)
			IQK_CMD = 0xf8000808 | (1 << (path + 4));
		else
			IQK_CMD = 0xf8000708 | (1 << (path + 4));
		((void)0);

	} else if (idx == RXIQK2) {
		IQK_CMD = 0xf8000008 | ((*p_dm_odm->p_band_width + 9) << 8) | (1 << (path + 4));
		((void)0);

	}

	_iqk_set_gnt_wl_gnt_bt(p_dm_odm, true);

	odm_write_4byte(p_dm_odm, 0x1bc8, 0x80000000);
	odm_write_4byte(p_dm_odm, 0x8f8, 0x41400080);

	odm_write_4byte(p_dm_odm, 0x1b00, IQK_CMD);
	odm_write_4byte(p_dm_odm, 0x1b00, IQK_CMD + 0x1);
	ODM_delay_ms(WBIQK_delay_8821C);

	while (notready) {
		if (odm_read_4byte(p_dm_odm, 0xfa0) & BIT(27))
			notready = false;
		else
			notready = true;

		if (notready) {
			ODM_delay_ms(1);
			delay_count++;
		} else {
			fail = (boolean) odm_get_bb_reg(p_dm_odm, 0x1b08, BIT(26));
			break;
		}

		if (delay_count >= 50) {
			access_error |= STATION_IQK_TIMEOUT;

			break;
		}
	}

	((void)0);
	_iqk_set_gnt_wl_gnt_bt(p_dm_odm, false);

	if (p_dm_odm->debug_components && ODM_COMP_CALIBRATION) {
		odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008 | path << 1);
		((void)0);
		((void)0);
		if (idx != TXIQK)
			((void)0);
	}

	odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008 | path << 1);
	if (idx == TXIQK)
		if (fail)
			odm_set_bb_reg(p_dm_odm, iqk_apply[path], BIT(0), 0x0);

	if (idx == RXIQK2) {
		p_iqk_info->RXIQK_AGC[0][path] =
			(u16)(((odm_get_rf_reg(p_dm_odm, (enum odm_rf_radio_path_e)path, 0x0, RFREGOFFSETMASK) >> 5) & 0xff) |
			      (p_iqk_info->tmp1bcc << 8));

		odm_write_4byte(p_dm_odm, 0x1b38, 0x20000000);

		if (!fail)
			odm_set_bb_reg(p_dm_odm, iqk_apply[path], (BIT(11) | BIT(10)), 0x1);
		else
			odm_set_bb_reg(p_dm_odm, iqk_apply[path], (BIT(11) | BIT(10)), 0x0);
	}

	if (idx == TXIQK)
		p_iqk_info->IQK_fail_report[0][path][TXIQK] = fail;
	else
		p_iqk_info->IQK_fail_report[0][path][RXIQK] = fail;

	return fail;
}

boolean
_iqk_rxiqkbystep_8821c(
	void		*p_dm_void,
	u8		path
)
{
	struct PHY_DM_STRUCT	*p_dm_odm = (struct PHY_DM_STRUCT *)p_dm_void;
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;
	boolean		KFAIL = true, gonext;

	switch (p_iqk_info->rxiqk_step) {
	case 1:
		_iqk_rxk1setting_8821c(p_dm_odm, path);
		gonext = false;
		while (1) {
			KFAIL = _iqk_rx_iqk_gain_search_fail_8821c(p_dm_odm, path, RXIQK1);
			if (KFAIL && (p_iqk_info->gs_retry_count[0][path][RXIQK1] < 2))
				p_iqk_info->gs_retry_count[0][path][RXIQK1]++;
			else if (KFAIL) {
				p_iqk_info->RXIQK_fail_code[0][path] = 0;
				p_iqk_info->rxiqk_step = 5;
				gonext = true;
			} else {
				p_iqk_info->rxiqk_step++;
				gonext = true;
			}
			if (gonext)
				break;
		}
		break;
	case 2:
		_iqk_rxk2setting_8821c(p_dm_odm, path, true);
		p_iqk_info->isbnd = false;
		while (1) {
			KFAIL = _iqk_rx_iqk_gain_search_fail_8821c(p_dm_odm, path, RXIQK2);
			if (KFAIL && (p_iqk_info->gs_retry_count[0][path][RXIQK2] < rxiqk_gs_limit))
				p_iqk_info->gs_retry_count[0][path][RXIQK2]++;
			else {
				p_iqk_info->rxiqk_step++;
				break;
			}
		}
		break;
	case 3:
		_iqk_rxk1setting_8821c(p_dm_odm, path);
		gonext = false;
		while (1) {
			KFAIL = _iqk_one_shot_8821c(p_dm_odm, path, RXIQK1);
			if (KFAIL && (p_iqk_info->retry_count[0][path][RXIQK1] < 2))
				p_iqk_info->retry_count[0][path][RXIQK1]++;
			else if (KFAIL) {
				p_iqk_info->RXIQK_fail_code[0][path] = 1;
				p_iqk_info->rxiqk_step = 5;
				gonext = true;
			} else {
				p_iqk_info->rxiqk_step++;
				gonext = true;
			}
			if (gonext)
				break;
		}
		break;
	case 4:
		_iqk_rxk2setting_8821c(p_dm_odm, path, false);
		gonext = false;
		while (1) {
			KFAIL = _iqk_one_shot_8821c(p_dm_odm, path,	RXIQK2);
			if (KFAIL && (p_iqk_info->retry_count[0][path][RXIQK2] < 2))
				p_iqk_info->retry_count[0][path][RXIQK2]++;
			else if (KFAIL) {
				p_iqk_info->RXIQK_fail_code[0][path] = 2;
				p_iqk_info->rxiqk_step = 5;
				gonext = true;
			} else {
				p_iqk_info->rxiqk_step++;
				gonext = true;
			}
			if (gonext)
				break;
		}
		break;
	}
	return KFAIL;
}

void
_iqk_iqk_by_path_8821c(
	void		*p_dm_void,
	boolean		segment_iqk
)
{
	struct PHY_DM_STRUCT	*p_dm_odm = (struct PHY_DM_STRUCT *)p_dm_void;
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;
	boolean		KFAIL = true;
	u8		i, kcount_limit;
	u32		cnt_iqk_fail = 0;

	if (*p_dm_odm->p_band_width == 2)
		kcount_limit = kcount_limit_80m;
	else
		kcount_limit = kcount_limit_others;

	for (u32 stage_budget = 0; stage_budget < 32; ++stage_budget) {


		switch (p_dm_odm->rf_calibrate_info.iqk_step) {
		case 1:
			for (i = 0; i < 8 ; i++) {
				_iqk_lok_setting_8821c(p_dm_odm, ODM_RF_PATH_A, i);
				_lok_one_shot_8821c(p_dm_odm, ODM_RF_PATH_A, i);
			}
			p_dm_odm->rf_calibrate_info.iqk_step++;
			break;
		case 2:
			_iqk_txk_setting_8821c(p_dm_odm, ODM_RF_PATH_A);
			KFAIL = _iqk_one_shot_8821c(p_dm_odm, ODM_RF_PATH_A, TXIQK);
			p_iqk_info->kcount++;
			((void)0);
			if (KFAIL)
				cnt_iqk_fail++;
			if (KFAIL && (p_iqk_info->retry_count[0][ODM_RF_PATH_A][TXIQK] < 3))
				p_iqk_info->retry_count[0][ODM_RF_PATH_A][TXIQK]++;
			else
				p_dm_odm->rf_calibrate_info.iqk_step++;
			break;
		case 3:
			while (1) {
				KFAIL = _iqk_rxiqkbystep_8821c(p_dm_odm, ODM_RF_PATH_A);
				((void)0);
				if (p_iqk_info->rxiqk_step == 5) {
					p_dm_odm->rf_calibrate_info.iqk_step++;
					p_iqk_info->rxiqk_step = 1;
					if (KFAIL) {
						cnt_iqk_fail++;
						((void)0);
					}
					break;
				}
			}
			p_iqk_info->kcount++;
			break;
		}

		if (p_dm_odm->rf_calibrate_info.iqk_step == 4) {






			((void)0);

			for (i = 0; i < SS_8821C; i++) {
				odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008 | i << 1);
				odm_write_4byte(p_dm_odm, 0x1b2c, 0x7);
				odm_write_4byte(p_dm_odm, 0x1bcc, 0x0);
				odm_write_4byte(p_dm_odm, 0x1b38, 0x20000000);
			}
			break;
		}

		p_dm_odm->n_iqk_cnt++;

		if (cnt_iqk_fail == 0)
			p_dm_odm->n_iqk_ok_cnt++;
		else
			p_dm_odm->n_iqk_fail_cnt = p_dm_odm->n_iqk_fail_cnt + cnt_iqk_fail;

		((void)0);

		if ((segment_iqk == true) && (p_iqk_info->kcount == kcount_limit))
			break;

	}
}

void
_iqk_start_iqk_8821c(
	struct PHY_DM_STRUCT		*p_dm_odm,
	boolean			segment_iqk
)
{
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;

	u32 tmp;

	odm_write_4byte(p_dm_odm, 0x1b00, 0xf8000008);
	odm_write_4byte(p_dm_odm, 0x1bb8, 0x00000000);

	if (p_iqk_info->is_BTG) {
		tmp = odm_get_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0x1, RFREGOFFSETMASK);
		tmp = (tmp & (~BIT(3))) | BIT(0) | BIT(2);
		odm_set_rf_reg(p_dm_odm, ODM_RF_PATH_A, 0x1, RFREGOFFSETMASK, tmp);
	}

	_iqk_iqk_by_path_8821c(p_dm_odm, segment_iqk);
}

void
_iq_calibrate_8821c_init(
	void		*p_dm_void
)
{
	struct PHY_DM_STRUCT	*p_dm_odm = (struct PHY_DM_STRUCT *)p_dm_void;
	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;
	u8	i, j, k, m;

	if (p_iqk_info->iqk_times == 0x0) {


		for (i = 0; i < SS_8821C; i++) {
			for (j = 0; j < 2; j++) {
				p_iqk_info->LOK_fail[i] = true;
				p_iqk_info->IQK_fail[j][i] = true;
				p_iqk_info->iqc_matrix[j][i] = 0x20000000;
			}
		}

		for (i = 0; i < 2; i++) {
			p_iqk_info->iqk_channel[i] = 0x0;

			for (j = 0; j < SS_8821C; j++) {
				p_iqk_info->LOK_IDAC[i][j] = 0x0;
				p_iqk_info->RXIQK_AGC[i][j] = 0x0;
				p_iqk_info->bypass_iqk[i][j] = 0x0;

				for (k = 0; k < 2; k++) {
					p_iqk_info->IQK_fail_report[i][j][k] = true;
					for (m = 0; m < 8; m++) {
						p_iqk_info->IQK_CFIR_real[i][j][k][m] = 0x0;
						p_iqk_info->IQK_CFIR_imag[i][j][k][m] = 0x0;
					}
				}

				for (k = 0; k < 3; k++)
					p_iqk_info->retry_count[i][j][k] = 0x0;
			}
		}
	}
}
void
_phy_iq_calibrate_8821c(
	struct PHY_DM_STRUCT		*p_dm_odm,
	boolean			reset
)
{

	u32	MAC_backup[MAC_REG_NUM_8821C], BB_backup[BB_REG_NUM_8821C], RF_backup[RF_REG_NUM_8821C][1];
	u32	backup_mac_reg[MAC_REG_NUM_8821C] = {0x520, 0x550, 0x1518};
	u32	backup_bb_reg[BB_REG_NUM_8821C] = {0x808, 0x90c, 0xc00, 0xcb0, 0xcb4, 0xcbc, 0x1990, 0x9a4, 0xa04, 0xb00};
	u32	backup_rf_reg[RF_REG_NUM_8821C] = {0xdf,  0xde, 0x8f, 0x65, 0x0, 0x1};
	u8	i, j;
	boolean segment_iqk = false, is_mp = false;

	struct _IQK_INFORMATION	*p_iqk_info = &p_dm_odm->IQK_info;

	if (p_dm_odm->mp_mode)
		is_mp = true;
	else if (p_dm_odm->is_linked)
		segment_iqk = false;

	p_iqk_info->is_BTG = (boolean)odm_get_bb_reg(p_dm_odm, 0xcb8, BIT(16));

	if (!is_mp)
		if (_iqk_reload_iqk_8821c(p_dm_odm, reset))
			return;

	((void)0);

	((void)0);
	((void)0);

	p_iqk_info->tmp_GNTWL = _iqk_indirect_read_reg(p_dm_odm, 0x38);
	if (access_error & 32) return; /* grant state cannot be saved/restored */

	p_iqk_info->iqk_times++;
	p_iqk_info->kcount = 0;
	p_dm_odm->rf_calibrate_info.iqk_total_progressing_time = 0;
	p_dm_odm->rf_calibrate_info.iqk_step = 1;
	p_iqk_info->rxiqk_step = 1;

	_iqk_backup_iqk_8821c(p_dm_odm, 0);
	_iqk_backup_mac_bb_8821c(p_dm_odm, MAC_backup, BB_backup, backup_mac_reg, backup_bb_reg,BB_REG_NUM_8821C);
	_iqk_backup_rf_8821c(p_dm_odm, RF_backup, backup_rf_reg);

	for (u32 segment_budget = 0; segment_budget < 8; ++segment_budget) {
		if (!is_mp)
			p_dm_odm->rf_calibrate_info.iqk_start_time = odm_get_current_time(p_dm_odm);

		g_station.iqk_stage = 1;
		_iqk_configure_macbb_8821c(p_dm_odm);
		_iqk_afe_setting_8821c(p_dm_odm, true);
		_iqk_rfe_setting_8821c(p_dm_odm, false);
		_iqk_agc_bnd_int_8821c(p_dm_odm);
		_iqk_rfsetting_8821c(p_dm_odm);

		g_station.iqk_stage = 2;
		_iqk_start_iqk_8821c(p_dm_odm, segment_iqk);

		g_station.iqk_stage = 3;
		_iqk_afe_setting_8821c(p_dm_odm, false);
		_iqk_restore_mac_bb_8821c(p_dm_odm, MAC_backup, BB_backup, backup_mac_reg, backup_bb_reg,BB_REG_NUM_8821C);
		_iqk_restore_rf_8821c(p_dm_odm, backup_rf_reg, RF_backup);

		if (!is_mp) {
			p_dm_odm->rf_calibrate_info.iqk_progressing_time = odm_get_progressing_time(p_dm_odm, p_dm_odm->rf_calibrate_info.iqk_start_time);
			p_dm_odm->rf_calibrate_info.iqk_total_progressing_time += odm_get_progressing_time(p_dm_odm, p_dm_odm->rf_calibrate_info.iqk_start_time);
			((void)0);
		}

		if (p_dm_odm->rf_calibrate_info.iqk_step == 4)
			break;

		p_iqk_info->kcount = 0;
		((void)0);
		ODM_delay_ms(50);
	};

	if (p_dm_odm->rf_calibrate_info.iqk_step != 4) access_error |= 64;
	_iqk_backup_iqk_8821c(p_dm_odm, 1);
	_iqk_fill_iqk_report_8821c(p_dm_odm, 0);

	if (!is_mp)
		((void)0);

	((void)0);

	((void)0);
}
