/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef RTW88_SOURCE_FWLOG_H
#define RTW88_SOURCE_FWLOG_H

#include <linux/spinlock.h>
#include <linux/types.h>

#define RTW_FWLOG_LIMIT 55

struct rtw_dev;

struct rtw_fwlog {
	spinlock_t lock;
	bool saw_packet;
	u8 packet_sequence;
	s8 message_sequence;
	s8 discarded_sequence;
	u8 remaining;
	u8 length;
	char message[RTW_FWLOG_LIMIT];
};

void rtw_fwlog_reset(struct rtw_dev *rtwdev);
bool rtw_fwlog_rx(struct rtw_dev *rtwdev, const u8 *frame, size_t length);

#endif
