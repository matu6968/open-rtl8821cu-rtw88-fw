// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Experimental unified source-firmware contract, not a vendor WoW ABI. */
#include <linux/module.h>
#include <linux/delay.h>
#include "main.h"
#include "debug.h"
#include "fw.h"
#include "ps.h"
#include "reg.h"
#include "source_wow.h"
#include "led.h"

#define SOURCE_WOW_CTRL 0x0690
#define SOURCE_WOW_ARMED 0x26
#define SOURCE_WOW_DBG6 0x10f8
#define SOURCE_WOW_DBG7 0x10fc
#define SOURCE_WOW_MAGIC_REASON 0x21

static bool source_wowlan;
module_param(source_wowlan, bool, 0444);
MODULE_PARM_DESC(source_wowlan,
		 "Opt in to RTL8821CU source firmware 0.11.0 magic-packet WoW ABI");

const struct wiphy_wowlan_support rtw_source_wowlan_support = {
	.flags = WIPHY_WOWLAN_MAGIC_PKT,
};

bool rtw_source_wow_enabled(struct rtw_dev *rtwdev)
{
	return source_wowlan && rtwdev->hci.type == RTW_HCI_TYPE_USB &&
	       rtwdev->chip->id == RTW_CHIP_TYPE_8821C;
}
EXPORT_SYMBOL_GPL(rtw_source_wow_enabled);

static int rtw_source_wow_command(struct rtw_dev *rtwdev, bool enable)
{
	u8 h2c[8] = { 0xf0, enable, 0, 0, 'W', 'O', 'W', '1' };
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);
	u32 expected, ack;
	u16 cookie;

	/* Fail closed on stock images and on a concurrent diagnostic read. */
	if (rtwdev->fw.version || rtwdev->fw.sub_version != 11 ||
	    rtwdev->fw.sub_index > 1 || (rtw_read32(rtwdev, SOURCE_WOW_DBG6) & 1))
		return -EPROTO;
	cookie = ++rtwdev->source_wow_cookie;
	if (!cookie)
		cookie = ++rtwdev->source_wow_cookie;
	h2c[2] = cookie;
	h2c[3] = cookie >> 8;
	expected = 0x57000000 | ((u32)cookie << 8);
	rtw_write32(rtwdev, SOURCE_WOW_DBG7, 0);
	rtw_fw_h2c_cmd_dbg(rtwdev, h2c);
	do {
		ack = rtw_read32(rtwdev, SOURCE_WOW_DBG7);
		if ((ack & 0xffffff00) == expected) {
			if ((ack & 0xff) != enable)
				return -EREMOTEIO;
			if (enable &&
			    (rtw_read8(rtwdev, SOURCE_WOW_CTRL) & SOURCE_WOW_ARMED) !=
			    SOURCE_WOW_ARMED)
				return -EREMOTEIO;
			if (!enable &&
			    (rtw_read8(rtwdev, SOURCE_WOW_CTRL) & SOURCE_WOW_ARMED))
				return -EREMOTEIO;
			return 0;
		}
		usleep_range(1000, 2000);
	} while (time_before(jiffies, deadline));
	rtw_err(rtwdev, "source WoW %s ACK timeout, cookie=%u ack=%08x\n",
		enable ? "arm" : "disarm", cookie, ack);
	return -ETIMEDOUT;
}

/* Called after releasing rtwdev->mutex, still inside cfg80211 suspend.
 * mac80211 rejects its queue helpers while local->quiescing is set. Use a
 * system queue; the recovery worker waits for the wiphy lock before touching
 * driver state, which cannot happen until the failed suspend has returned.
 */
void rtw_source_wow_abort(struct rtw_dev *rtwdev)
{
	bool recover, schedule;

	mutex_lock(&rtwdev->mutex);
	recover = rtwdev->source_wow_recovery;
	schedule = recover && !rtwdev->source_wow_recovery_queued;
	if (schedule)
		rtwdev->source_wow_recovery_queued = true;
	mutex_unlock(&rtwdev->mutex);
	if (recover) {
		if (schedule)
			queue_work(system_unbound_wq, &rtwdev->fw_recovery_work);
		return;
	}
	rtw_led_resume(rtwdev);
	/* The ordinary watchdog runs after the failed suspend unwinds. */
	queue_delayed_work(system_freezable_wq, &rtwdev->watch_dog_work,
			   RTW_WATCH_DOG_DELAY_TIME);
}

struct source_wow_vif {
	struct ieee80211_vif *vif;
	unsigned int count;
};

static void rtw_source_wow_vif_iter(void *data, u8 *mac,
				    struct ieee80211_vif *vif)
{
	struct source_wow_vif *found = data;
	struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;

	found->count++;
	if (vif->type == NL80211_IFTYPE_STATION &&
	    rtwvif->net_type == RTW_NET_MGD_LINKED && !rtwvif->port)
		found->vif = vif;
}

int rtw_source_wow_suspend(struct rtw_dev *rtwdev,
			   struct cfg80211_wowlan *wowlan)
{
	struct source_wow_vif found = {};
	int ret, rollback;

	lockdep_assert_held(&rtwdev->mutex);
	if (rtwdev->source_wow_recovery)
		return -EBUSY;
	if (!wowlan || !wowlan->magic_pkt || wowlan->any || wowlan->disconnect ||
	    wowlan->gtk_rekey_failure || wowlan->eap_identity_req ||
	    wowlan->four_way_handshake || wowlan->rfkill_release ||
	    wowlan->n_patterns || wowlan->tcp || wowlan->nd_config)
		return -EOPNOTSUPP;
	/* A system-USB callback error cannot reliably veto suspend. Reject
	 * missing interface/parent wake authorization here, before firmware ARM.
	 */
	if (!rtwdev->dev || !rtwdev->dev->parent ||
	    !device_may_wakeup(rtwdev->dev) ||
	    !device_may_wakeup(rtwdev->dev->parent)) {
		++rtwdev->source_wow_auth_errors;
		return -EACCES;
	}
	if (test_bit(RTW_FLAG_SCANNING, rtwdev->flags))
		return -EBUSY;
	rtw_iterate_vifs_atomic(rtwdev, rtw_source_wow_vif_iter, &found);
	if (found.count != 1 || !found.vif)
		return -ENOTCONN;

	rtw_leave_lps(rtwdev);
	ret = rtw_source_wow_command(rtwdev, true);
	if (ret) {
		++rtwdev->source_wow_arm_errors;
		rtwdev->source_wow_arm_errno = ret;
		rtwdev->source_wow_rollback_errno = 0;
		/* EPROTO is returned only before submitting an H2C command. */
		if (ret == -EPROTO)
			return ret;
		/* A consumed command may have armed before its ACK was lost. */
		rollback = rtw_source_wow_command(rtwdev, false);
		if (rollback) {
			++rtwdev->source_wow_rollback_errors;
			rtwdev->source_wow_rollback_errno = rollback;
			rtwdev->source_wow_recovery = true;
			/* Abort restores mac80211 queues; block normal driver TX. */
			clear_bit(RTW_FLAG_RUNNING, rtwdev->flags);
			rtw_err(rtwdev,
				"source WoW ARM failed=%d rollback failed=%d; hardware recovery required (arm_errors=%u rollback_errors=%u)\n",
				ret, rollback, rtwdev->source_wow_arm_errors,
				rtwdev->source_wow_rollback_errors);
		}
		return ret;
	}
	rtwdev->wow.wow_vif = found.vif;
	rtwdev->source_wow_suspended = true;
	rtwdev->source_wow_reset = false;
	set_bit(RTW_FLAG_WOWLAN, rtwdev->flags);
	rtw_info(rtwdev, "source WoW armed: magic packet, cookie=%u, no firmware swap\n",
		 rtwdev->source_wow_cookie);
	return 0;
}

int rtw_source_wow_resume(struct rtw_dev *rtwdev)
{
	struct cfg80211_wowlan_wakeup wakeup = { .pattern_idx = -1 };
	struct ieee80211_vif *vif = rtwdev->wow.wow_vif;
	u8 reason;
	int ret;

	lockdep_assert_held(&rtwdev->mutex);
	if (!rtwdev->source_wow_suspended)
		return 0;
	reason = rtw_read8(rtwdev, REG_WOWLAN_WAKE_REASON);
	ret = rtw_source_wow_command(rtwdev, false);
	if (rtwdev->source_wow_reset)
		ret = -EIO;
	if (!ret && vif && reason == SOURCE_WOW_MAGIC_REASON) {
		wakeup.magic_pkt = true;
		ieee80211_report_wowlan_wakeup(vif, &wakeup, GFP_KERNEL);
	}
	rtw_info(rtwdev, "source WoW resumed: reason=%02x disarm=%d reset=%u cookie=%u\n",
		 reason, ret, rtwdev->source_wow_reset, rtwdev->source_wow_cookie);
	rtwdev->source_wow_suspended = false;
	rtwdev->source_wow_reset = false;
	rtwdev->wow.wow_vif = NULL;
	clear_bit(RTW_FLAG_WOWLAN, rtwdev->flags);
	return ret;
}
