// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Associated deep-clock power save. Included by usb.c so the
 * existing URB lifecycle stays private to the USB transport. No system sleep.
 */
#include "led.h"
static bool source_deep_auto;
module_param(source_deep_auto, bool, 0444);
MODULE_PARM_DESC(source_deep_auto, "Opt in to associated USB deep LPS with source firmware 0.11.1");
struct source_deep_io {
	struct rtw_usb *usb;
	__le32 *buffer;
	u8 rpwm;
	u32 awake_clock;
};

static int source_deep_raw(struct source_deep_io *io, bool read, u16 reg,
			   unsigned int length, u32 *value)
{
	struct usb_device *udev = io->usb->udev;
	int ret;

	if (!read)
		*io->buffer = cpu_to_le32(*value);
	ret = usb_control_msg(udev, read ? usb_rcvctrlpipe(udev, 0) :
			      usb_sndctrlpipe(udev, 0), RTW_USB_CMD_REQ,
			      read ? RTW_USB_CMD_READ : RTW_USB_CMD_WRITE,
			      reg, 0, io->buffer, length, 200);
	if (ret != length)
		return ret < 0 ? ret : -EIO;
	if (read)
		*value = le32_to_cpu(*io->buffer);
	return 0;
}

static int source_deep_rpwm(struct source_deep_io *io, bool enter, bool awake)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(250);
	u32 before, value, clock;
	int ret;

	ret = source_deep_raw(io, true, 0xfe57, 1, &before);
	if (ret)
		return ret;
	io->rpwm = ((io->rpwm ^ 0x80) & 0x80) | 0x40 | enter;
	value = io->rpwm;
	ret = source_deep_raw(io, false, 0xfe58, 1, &value);
	if (ret)
		return ret;
	/* FE58 aliases an always-on MCU byte. The bare bridge can hold a
	 * concurrent MAC load until a dummy off-domain transaction releases it.
	 * Before COMMIT clocks are known awake. After COMMIT, raw SYS_CLKR must
	 * first prove normal CPU/HCI/MAC/SEC clocks and MACSLP clear.
	 */
	while (!awake && time_before(jiffies, deadline)) {
		ret = source_deep_raw(io, true, 8, 4, &clock);
		if (ret)
			return ret;
		awake = (clock & 0x7c33) == (io->awake_clock & 0x7c33);
		if (!awake)
			usleep_range(1000, 1500);
	}
	if (!awake) {
		rtw_err(io->usb->rtwdev, "source deep clock timeout: SYS_CLKR=%08x\n", clock);
		return -ETIMEDOUT;
	}
	value = 0;
	ret = source_deep_raw(io, false, 0x4e0, 1, &value);
	if (ret)
		return ret;
	do {
		ret = source_deep_raw(io, true, 0xfe57, 1, &value);
		if (ret)
			return ret;
		if ((value ^ before) & 0x80)
			return 0;
		usleep_range(1000, 1500);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

static int source_deep_command(struct rtw_dev *rtwdev, u8 mode, u16 cookie,
			       bool commit)
{
	u8 h2c[8] = { commit ? 0xf2 : 0xf1, mode, cookie, cookie >> 8,
		      'D', 'P', 'S', '1' };
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);
	u32 expected = 0x44000000 | ((u32)cookie << 8) | !!mode;

	rtw_fw_h2c_cmd_dbg(rtwdev, h2c);
	/* Commit is deliberately one-way. DBG reads add an off-domain SEC
	 * write and must not occur once clocks may have stopped.
	 */
	if (commit)
		return 0;
	do {
		if (rtw_read32(rtwdev, 0x10fc) == expected)
			return 0;
		usleep_range(1000, 1500);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

static int source_deep_reference_enter(struct source_deep_io *io)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(250);
	u32 before, value;
	int ret;

	ret = source_deep_raw(io, true, 0xfe57, 1, &before);
	if (ret)
		return ret;
	io->rpwm = ((io->rpwm ^ 0x80) & 0x80) | 0x41;
	value = io->rpwm;
	ret = source_deep_raw(io, false, 0xfe58, 1, &value);
	if (ret)
		return ret;
	/* Stock has no separate commit: clocks can stop on this write. Only
	 * USB-local CPWM is safe here; do not issue an off-domain SEC flush. */
	do {
		ret = source_deep_raw(io, true, 0xfe57, 1, &value);
		if (ret)
			return ret;
		if ((value ^ before) & 0x80)
			return 0;
		usleep_range(1000, 1500);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

struct source_deep_vif { unsigned int count; bool linked; };
static void source_deep_vif_iter(void *data, u8 *mac, struct ieee80211_vif *vif)
{
	struct source_deep_vif *found = data;
	struct rtw_vif *rvif = (void *)vif->drv_priv;

	found->count++;
	if (vif->type == NL80211_IFTYPE_STATION &&
	    rvif->net_type == RTW_NET_MGD_LINKED && !rvif->port)
		found->linked = true;
}

static int source_deep_observe(struct source_deep_io *io, unsigned int ms, bool automatic)
{
	unsigned long end = jiffies + msecs_to_jiffies(ms);
	struct rtw_usb *usb = io->usb;
	bool gated = false;

	do {
		u32 sys, marker;
		int ret;

		ret = source_deep_raw(io, true, 8, 4, &sys);
		if (ret)
			return ret;
		ret = source_deep_raw(io, true, 0x10fc, 4, &marker);
		if (ret)
			return ret;

		if (marker == 0xd505c000 && (sys & 0x7c33) == (io->awake_clock & 0x7c33))
			return -ENODATA; /* firmware reported a missed or late beacon */

		/* Both reads are always-on. Only an observed HOLD followed by the
		 * restored clock word and AWAKE marker completes an epoch. */
		if (sys != 0xeaeaeaea && sys != 0xffffffff &&
		    (sys & 0x4010) == 0x10 && marker == 0xd5050200) {
			if (!gated)
				usb->source_deep_gated++;
			gated = true;
		} else if ((sys & 0x7c33) == (io->awake_clock & 0x7c33) &&
			   (marker == 0xd5050000 || marker == 0xd5054000 ||
			    marker == 0xd5058000)) {
			if (gated)
				usb->source_deep_autonomous++;
			gated = false;
			if (marker != 0xd5050000 && usb->source_deep_autonomous)
				usb->source_deep_beacon = true;
			if (marker == 0xd5058000) {
				usb->source_deep_delivery = true;
				break;
			}
			if (automatic && usb->source_deep_beacon)
				break;
		}
		usleep_range(2000, 3000);
	} while (time_before(jiffies, end));
	return 0;
}

static ssize_t source_deep_cycle_ms_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct ieee80211_hw *hw = usb_get_intfdata(to_usb_interface(dev));
	struct rtw_usb *usb = rtw_get_usb_priv(hw->priv);

	return sysfs_emit(buf, "cycles=%u error=%d failed=%u committed=%u gated=%u autonomous=%u observation_error=%d delivery=%u beacon=%u auto_wakes=%u auto_errors=%u\n",
			  READ_ONCE(usb->source_deep_cycles),
			  READ_ONCE(usb->source_deep_errno),
			  READ_ONCE(usb->source_deep_failed),
			  READ_ONCE(usb->source_deep_committed),
			  READ_ONCE(usb->source_deep_gated),
			  READ_ONCE(usb->source_deep_autonomous),
			  READ_ONCE(usb->source_deep_observation_error),
			  READ_ONCE(usb->source_deep_delivery),
			  READ_ONCE(usb->source_deep_beacon),
			  READ_ONCE(usb->source_deep_auto_wakes),
			  READ_ONCE(usb->source_deep_auto_errors));
}

static int source_deep_run(struct ieee80211_hw *hw, unsigned int ms,
			   bool reference, bool automatic)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_usb *usb = rtw_get_usb_priv(rtwdev);
	struct source_deep_vif found = {};
	struct source_deep_io io = { .usb = usb };
	unsigned long deadline;
	u32 value;
	u16 cookie;
	bool paused = false, committed = false, prepared = false, locked = false;
	int ret, i;

	if (!rtw_source_wow_enabled(rtwdev) ||
	    (reference ? (rtwdev->fw.version != 24 || rtwdev->fw.sub_version != 11 || rtwdev->fw.sub_index) :
	     (rtwdev->fw.version || rtwdev->fw.sub_version != 11 || rtwdev->fw.sub_index > 1)))
		return -EOPNOTSUPP;
	if (automatic && rtwdev->fw.sub_index != 1)
		return -EOPNOTSUPP;
	io.buffer = kmalloc(sizeof(*io.buffer), GFP_KERNEL);
	if (!io.buffer)
		return -ENOMEM;
	wiphy_lock(hw->wiphy);
	if (usb->source_deep_failed || usb->suspended || rtwdev->source_wow_suspended ||
	    READ_ONCE(usb->source_deep_disconnecting) ||
	    test_bit(RTW_FLAG_SCANNING, rtwdev->flags)) {
		ret = -EBUSY;
		goto out_wiphy;
	}
	if (automatic && (!READ_ONCE(rtwdev->ps_enabled) ||
			  !test_bit(RTW_FLAG_RUNNING, rtwdev->flags) ||
			  !test_bit(RTW_FLAG_LEISURE_PS, rtwdev->flags))) {
		ret = -EBUSY;
		goto out_wiphy;
	}
	rtw_iterate_vifs_atomic(rtwdev, source_deep_vif_iter, &found);
	if (found.count != 1 || !found.linked) {
		ret = -ENOTCONN;
		goto out_wiphy;
	}
	usb->source_deep_gated = 0;
	usb->source_deep_autonomous = 0;
	usb->source_deep_observation_error = 0;
	usb->source_deep_delivery = false;
	usb->source_deep_beacon = false;
	ieee80211_stop_queues(hw);
	cancel_delayed_work_sync(&rtwdev->watch_dog_work);
	rtw_led_suspend(rtwdev); /* outside mutex: LED worker also takes it */
	flush_workqueue(usb->txwq);
	ret = -EBUSY;
	for (i = 0; i < ARRAY_SIZE(usb->tx_queue); i++)
		if (!skb_queue_empty(&usb->tx_queue[i]))
			goto restore;
	if (!usb_wait_anchor_empty_timeout(&usb->tx_anchor, 500))
		goto restore;
	mutex_lock(&rtwdev->mutex);
	locked = true;
	rtw_leave_lps(rtwdev);
	io.awake_clock = rtw_read32(rtwdev, 8);
	if ((io.awake_clock & 0x7c33) != 0x7c23) {
		ret = -EPROTO;
		goto restore;
	}
	io.rpwm = rtw_read8(rtwdev, 0xfe58);
	value = 0;
	ret = source_deep_raw(&io, false, 0x4e0, 1, &value);
	if (ret)
		goto restore;
	cookie = ++usb->source_deep_cookie;
	if (!cookie)
		cookie = ++usb->source_deep_cookie;
	ret = reference ? 0 : source_deep_command(rtwdev, 5, cookie, false);
	if (ret)
		goto restore;
	prepared = !reference;
	ret = source_deep_rpwm(&io, false, true);
	if (ret)
		goto restore;
	rtw_enter_lps(rtwdev, 0);
	deadline = jiffies + msecs_to_jiffies(250);
	while (!(rtw_read32(rtwdev, REG_TCR) & BIT(7))) {
		if (time_after_eq(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			goto restore;
		}
		usleep_range(1000, 1500);
	}
	/* RX callbacks may schedule workers taking mutex. Drain without it. */
	mutex_unlock(&rtwdev->mutex);
	locked = false;
	WRITE_ONCE(usb->suspended, true);
	cancel_work_sync(&usb->rx_urb_work);
	rtw_usb_cancel_rx_bufs(usb);
	flush_workqueue(usb->rxwq);
	paused = true;
	mutex_lock(&rtwdev->mutex);
	locked = true;
	if (reference)
		committed = true;
	ret = reference ? source_deep_reference_enter(&io) :
		source_deep_rpwm(&io, true, true);
	if (ret) {
		if (reference && !source_deep_rpwm(&io, false, false))
			committed = false;
		goto restore;
	}
	if (!reference)
		source_deep_command(rtwdev, 0, cookie, true);
	committed = true;
	WRITE_ONCE(usb->source_deep_committed, true);
	if (automatic)
		rtw_dbg(rtwdev, RTW_DBG_PS, "source deep committed: ms=%u cookie=%u\n", ms, cookie);
	else
		rtw_info(rtwdev, "source deep committed: ms=%u cookie=%u\n", ms, cookie);
	if (reference) {
		unsigned int normal = 0, asleep = 0;
		for (i = 0; i < ms; i += 2) {
			msleep(2);
			ret = source_deep_raw(&io, true, 8, 4, &value);
			if (ret)
				break;
			if ((value & 0x7c33) == (io.awake_clock & 0x7c33)) normal++;
			else asleep++;
		}
		rtw_info(rtwdev, "source deep stock reference before ACTIVE: normal=%u asleep=%u clock=%08x\n", normal, asleep, value);
	} else {
		usb->source_deep_observation_error = source_deep_observe(&io, ms, automatic);
	}
	WRITE_ONCE(usb->source_deep_committed, false);
	ret = source_deep_rpwm(&io, false, false);
	if (ret) {
		u32 debug = 0, ctrl = 0, fatal = 0;
		source_deep_raw(&io, true, 0x10fc, 4, &debug);
		source_deep_raw(&io, true, 0x10f8, 4, &fatal);
		source_deep_raw(&io, true, 0x90, 4, &ctrl);
		usb->source_deep_failed = true;
		rtw_err(rtwdev, "source deep wake failed: %d debug=%08x fatal=%08x WLLPS=%08x; cold recovery required\n", ret, debug, fatal, ctrl);
		goto restore;
	}
	committed = false;
	if (automatic) {
		ret = usb->source_deep_observation_error;
		if (!ret && usb->source_deep_gated && !usb->source_deep_beacon)
			ret = -ENODATA;
	}
restore:
	if (locked && !committed) {
		if (prepared) {
			int active = source_deep_rpwm(&io, false, true);
			int disarm = source_deep_command(rtwdev, 0, ++usb->source_deep_cookie ?: ++usb->source_deep_cookie, false);
			if (!ret)
				ret = active ?: disarm;
		}
		rtw_leave_lps(rtwdev);
	}
	if (locked)
		mutex_unlock(&rtwdev->mutex);
	usb->source_deep_errno = ret;
	if (!committed) {
		if (paused) {
			WRITE_ONCE(usb->suspended, false);
			rtw_usb_setup_rx(rtwdev);
		}
		rtw_led_resume(rtwdev);
		ieee80211_wake_queues(hw);
		queue_delayed_work(system_freezable_wq, &rtwdev->watch_dog_work,
				   RTW_WATCH_DOG_DELAY_TIME);
		if (!ret) {
			++usb->source_deep_cycles;
			if (automatic && usb->source_deep_beacon)
				usb->source_deep_auto_wakes += usb->source_deep_autonomous;
			if (automatic)
				rtw_dbg(rtwdev, RTW_DBG_PS, "source deep restored: cookie=%u cycles=%u\n",
					cookie, usb->source_deep_cycles);
			else
				rtw_info(rtwdev, "source deep restored: cookie=%u cycles=%u\n",
					 cookie, usb->source_deep_cycles);
		}
	}
out_wiphy:
	wiphy_unlock(hw->wiphy);
	kfree(io.buffer);
	return ret;
}

static ssize_t source_deep_cycle_ms_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct ieee80211_hw *hw = usb_get_intfdata(to_usb_interface(dev));
	bool reference = !strncmp(buf, "ref ", 4);
	unsigned int ms;
	int ret;

	ret = kstrtouint(reference ? buf + 4 : buf, 10, &ms);
	if (ret || ms < 100 || ms > 3000)
		return -EINVAL;
	ret = source_deep_run(hw, ms, reference, false);
	return ret ? ret : count;
}

static DEVICE_ATTR_RW(source_deep_cycle_ms);

static void source_deep_auto_work(struct work_struct *work)
{
	struct rtw_usb *usb = container_of(work, struct rtw_usb, source_deep_work);
	int ret;

	WRITE_ONCE(usb->source_deep_auto_running, true);
	ret = source_deep_run(usb->rtwdev->hw, 200, false, true);
	if (ret && ret != -EBUSY && ret != -ENOTCONN) {
		usb->source_deep_auto_errors++;
		rtw_warn(usb->rtwdev, "source deep automatic entry disabled: %d\n", ret);
	}
	WRITE_ONCE(usb->source_deep_auto_running, false);
}

static void source_deep_auto_schedule(struct rtw_dev *rtwdev)
{
	struct rtw_usb *usb = rtw_get_usb_priv(rtwdev);

	if (source_deep_auto && rtw_source_wow_enabled(rtwdev) &&
	    !rtwdev->fw.version && rtwdev->fw.sub_version == 11 && rtwdev->fw.sub_index == 1 &&
	    !READ_ONCE(usb->source_deep_auto_running) &&
	    !READ_ONCE(usb->source_deep_disconnecting) &&
	    !READ_ONCE(usb->source_deep_auto_errors) &&
	    !READ_ONCE(usb->source_deep_failed))
		queue_work(system_freezable_wq, &usb->source_deep_work);
}
