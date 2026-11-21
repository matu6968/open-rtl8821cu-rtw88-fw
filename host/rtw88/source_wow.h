/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef __RTW_SOURCE_WOW_H__
#define __RTW_SOURCE_WOW_H__
bool rtw_source_wow_enabled(struct rtw_dev *rtwdev);
extern const struct wiphy_wowlan_support rtw_source_wowlan_support;
int rtw_source_wow_suspend(struct rtw_dev *rtwdev,
			   struct cfg80211_wowlan *wowlan);
int rtw_source_wow_resume(struct rtw_dev *rtwdev);
void rtw_source_wow_abort(struct rtw_dev *rtwdev);
#endif
