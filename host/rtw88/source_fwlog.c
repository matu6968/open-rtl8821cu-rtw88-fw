// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include "main.h"
#include "debug.h"

#define FWLOG_CHUNK 11

static void fwlog_discard(struct rtw_fwlog *log, int sequence)
{
	if (sequence >= 0)
		log->discarded_sequence = sequence;
	else if (log->message_sequence >= 0)
		log->discarded_sequence = log->message_sequence;
	log->message_sequence = -1;
	log->length = 0;
}

void rtw_fwlog_reset(struct rtw_dev *rtwdev)
{
	struct rtw_fwlog *log = &rtwdev->source_fwlog;
	unsigned long flags;

	spin_lock_irqsave(&log->lock, flags);
	log->saw_packet = false;
	log->message_sequence = -1;
	log->discarded_sequence = -1;
	log->length = 0;
	spin_unlock_irqrestore(&log->lock, flags);
}

bool rtw_fwlog_rx(struct rtw_dev *rtwdev, const u8 *frame, size_t length)
{
	struct rtw_fwlog *log = &rtwdev->source_fwlog;
	char text[RTW_FWLOG_LIMIT];
	unsigned long flags;
	int complete = -1;
	u8 sequence, remaining, bytes;
	bool gap, debug;
	size_t i;

	if (rtwdev->chip->id != RTW_CHIP_TYPE_8821C ||
	    rtwdev->hci.type != RTW_HCI_TYPE_USB)
		return false;

	debug = frame[0] == 0;
	if (!(READ_ONCE(rtw_debug_mask) & RTW_DBG_FW)) {
		if (READ_ONCE(log->saw_packet))
			rtw_fwlog_reset(rtwdev);
		return debug;
	}

	spin_lock_irqsave(&log->lock, flags);
	gap = log->saw_packet && frame[1] != (u8)(log->packet_sequence + 1);
	log->saw_packet = true;
	log->packet_sequence = frame[1];
	if (gap)
		fwlog_discard(log, -1);
	if (!debug)
		goto unlock;
	if (length < 3) {
		fwlog_discard(log, -1);
		goto unlock;
	}
	sequence = frame[2] >> 4;
	remaining = frame[2] & 15;
	if (gap || sequence == log->discarded_sequence ||
	    remaining > RTW_FWLOG_LIMIT / FWLOG_CHUNK ||
	    length > 3 + FWLOG_CHUNK)
		goto invalid;
	bytes = length - 3;
	if (remaining ? bytes != FWLOG_CHUNK : bytes >= FWLOG_CHUNK)
		goto invalid;
	for (i = 3; i < length; i++) {
		u8 c = frame[i];

		if ((c < 32 || c >= 127) && c != '\t' && c != '\n' && c != '\r')
			goto invalid;
	}
	if (sequence != log->message_sequence) {
		log->message_sequence = sequence;
		log->discarded_sequence = -1;
		log->length = 0;
	} else if (log->remaining != remaining + 1) {
		goto invalid;
	}
	if (log->length + bytes > RTW_FWLOG_LIMIT)
		goto invalid;
	memcpy(log->message + log->length, frame + 3, bytes);
	log->length += bytes;
	log->remaining = remaining;
	if (!remaining) {
		complete = log->length;
		memcpy(text, log->message, complete);
		log->message_sequence = -1;
		log->length = 0;
	}
	goto unlock;

invalid:
	fwlog_discard(log, sequence);
unlock:
	spin_unlock_irqrestore(&log->lock, flags);
	if (complete >= 0)
		rtw_dbg(rtwdev, RTW_DBG_FW, "FW: %.*s%s", complete, text,
			complete && text[complete - 1] == '\n' ? "" : "\n");
	return debug;
}
