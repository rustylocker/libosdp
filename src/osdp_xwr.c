/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include "osdp_xwr.h"

#define CMD_XWR_HEADER_LEN              2
#define CMD_XWR_MODE_SET_MIN_LEN        1
#define CMD_XWR_TRANS_SEND_MIN_LEN      1  /* TODO: Determine the minimum APDU size */
#define CMD_XWR_SC_DISCONNECT_LEN       1
#define CMD_XWR_SC_SCAN_LEN             1

#define EVENT_XRD_HEADER_LEN            CMD_XWR_HEADER_LEN
#define EVENT_XRD_ERROR_LEN             1
#define EVENT_XRD_MODE_REPORT_LEN       2
#define EVENT_XRD_CARD_REPORT_MIN_LEN   3
#define EVENT_XRD_SC_PRESENT_LEN        2
#define EVENT_XRD_CARD_DATA_MIN_LEN     2


enum osdp_pd_error_e {
	OSDP_PD_ERR_NONE = 0,
	OSDP_PD_ERR_WAIT = -1,
	OSDP_PD_ERR_GENERIC = -2,
	OSDP_PD_ERR_REPLY = -3,
	OSDP_PD_ERR_IGNORE = -4,
	OSDP_PD_ERR_NO_DATA = -5,
};


int pd_stage_event_xwr_reply(struct osdp_pd *pd, struct osdp_cmd *cmd, int cb_result)
{
	int ret = cb_result;

	if (cmd->id == OSDP_CMD_XWRITE) {
		struct osdp_event ev;
		bool stage_reply = false;

		ev.type = OSDP_EVENT_XREAD;
		ev.flags = 0;
		ev.xread.mode = cmd->xwrite.mode;
		ev.xread.reply = cmd->xwrite.command;

		switch (cmd->xwrite.mode) {
		case 0:
			switch (cmd->xwrite.command) {
			case 0x01:
				// Mode report
				ev.xread.mode_report.mode_code = (uint8_t)pd->state;
				ev.xread.mode_report.mode_config = 0x0;
				stage_reply = true;
				break;
			default:
				break;
			}
			break;
		case 1:
			switch (cmd->xwrite.command) {
			case 0x01:
				if (cb_result > 0) {
					ev.xread.reply = 0x02;  // Transparent card data
					ev.xread.card_data.reader = cmd->xwrite.transp_send.reader;
					ev.xread.card_data.status = cmd->xwrite.transp_send.status;
					ev.xread.card_data.apdu_length = cmd->xwrite.transp_send.apdu_length;
					memcpy(ev.xread.card_data.apdu, cmd->xwrite.transp_send.apdu, cmd->xwrite.transp_send.apdu_length);
					stage_reply = true;
				}
				else if (cb_result < 0) {
					ev.xread.reply = 0x00;  // Error notification
					ev.xread.error_reply.error_code = (uint8_t)(-cb_result);
					ret = 0;  // Patch the result
					stage_reply = true;
				}
				break;
			case 0x4:
				ev.xread.reply = 0x01;  // Card present notification
				ev.xread.card_present.reader = cmd->xwrite.sc_scan.reader;
				ev.xread.card_present.status = cmd->xwrite.sc_scan.status;
				stage_reply = true;
				break;
			default:
				break;
			}
			break;
		default:
			break;
		}

		if (stage_reply) {
			pd->reply_id = REPLY_XRD;
			memcpy(pd->ephemeral_data, &ev, sizeof(ev));
		}
	}
	else {
		ret = -1;
	}
	return ret;
}

int osdp_xwr_cmd_build(struct osdp_pd *pd, const struct osdp_cmd *cmd,
		uint8_t *buf, int max_len)
{
	int len = 0;

	// Write the common "header"
	if (max_len < (CMD_XWR_HEADER_LEN+1)) {
		LOG_ERR("Cmd length error");
		return -1;
	}
	bwrite_u8(CMD_XWR, buf, &len);
	bwrite_u8(cmd->xwrite.mode, buf, &len);
	bwrite_u8(cmd->xwrite.command, buf, &len);

	switch (cmd->xwrite.mode) {
	// Support the read back and the setting of the PD’s behaviour mode.
	case 0:
		// Mode 0 commands are available in all modes.
		switch (cmd->xwrite.command) {
		case 0x02:
			// En-/Disable the specified mode
			if ((max_len-len) < CMD_XWR_MODE_SET_MIN_LEN) {
				LOG_ERR("Cmd length error");
				return -1;
			}
			bwrite_u8(cmd->xwrite.mode_set.mode_code, buf, &len);
			bwrite_u8(cmd->xwrite.mode_set.mode_config, buf, &len);
			break;
		case 0x01:
		default:
			break;
		}
		break;
	// Support transparent operations between the ACU and a Smart Card.
	case 1:
		switch (cmd->xwrite.command) {
		case 1:
			// Transparent content send
			if ((max_len-len) < (cmd->xwrite.transp_send.apdu_length+CMD_XWR_TRANS_SEND_MIN_LEN)) {
				LOG_ERR("Cmd length error");
				return -1;
			}
			bwrite_u8(0, buf, &len);  // reader
			memcpy(buf + len, cmd->xwrite.transp_send.apdu, cmd->xwrite.transp_send.apdu_length);
			len += cmd->xwrite.transp_send.apdu_length;
			break;
		case 2:
			// Disconnect from smart card
			if ((max_len-len) < CMD_XWR_SC_DISCONNECT_LEN) {
				LOG_ERR("Cmd length error");
				return -1;
			}
			bwrite_u8(0, buf, &len);  // reader
			break;
		case 4:
			// Trigger smart card scan
			if ((max_len-len) < CMD_XWR_SC_SCAN_LEN) {
				LOG_ERR("Cmd length error");
				return -1;
			}
			bwrite_u8(0, buf, &len);  // reader
			break;
		default:
			break;
		}
		break;

	default:
		LOG_ERR("Invalid XWR mode");
		break;
	}

	return len;
}

int osdp_xwr_cmd_decode(struct osdp_pd *pd, struct osdp_cmd *cmd,
		const uint8_t *buf, int len, bool *trigger_app)
{
	int pos = 0;

	if (trigger_app != NULL) {
		*trigger_app = false;
	}
	// Parse the common "header"
	if (len < CMD_XWR_HEADER_LEN) {
		LOG_ERR("Cmd length error");
		return -2;
	}
	cmd->id = OSDP_CMD_XWRITE;
	cmd->xwrite.mode = buf[pos++];
	cmd->xwrite.command = buf[pos++];

	// Parse the mode and command dependent data and set the right reply (code).
	switch (cmd->xwrite.mode) {
	// Support the read back and the setting of the PD’s behaviour mode.
	case 0:
		// Mode 0 commands are available in all modes.
		switch (cmd->xwrite.command) {
		case 0x01:
			// Return the current mode in effect
			//   by replying with osdp_XRD (XRW_Mode=0, XRD_REPLY=0x01)
			(void)pd_stage_event_xwr_reply(pd, cmd, 0);
			if (trigger_app != NULL) {
				*trigger_app = false;
			}
			return 0;
		case 0x02:
			// En-/Disable the specified mode
			if ((len-pos) < CMD_XWR_MODE_SET_MIN_LEN) {
				LOG_ERR("Cmd length error");
				return -2;
			}
			cmd->xwrite.mode_set.mode_code = buf[pos++];
			cmd->xwrite.mode_set.mode_config = 0x00;  // Default
			// Parser mode configuration only if enough data available
			if ((len-pos) > 0) {
				cmd->xwrite.mode_set.mode_config = buf[pos++];
				// Verify the new mode
				if ((cmd->xwrite.mode_set.mode_config != 0x00) && (cmd->xwrite.mode_set.mode_code > 1)) {
					LOG_ERR("Unkown mode code");
					return -2;
				}
			}
			// Store the new mode
			pd->state = (cmd->xwrite.mode_set.mode_config != 0x00) ? cmd->xwrite.mode_set.mode_code : 0;
			pd->reply_id = REPLY_ACK;
			if (trigger_app != NULL) {
				*trigger_app = true;
			}
			break;
		default:
			break;
		}
		break;

	// Support transparent operations between the ACU and a Smart Card.
	case 1:
		// Verify that we are in the right mode
		if (pd->state != 0x01) {
			LOG_ERR("Wrong XWR behavior mode");
			return -2;
		}

		switch (cmd->xwrite.command) {
		case 1:
			// Transparent content send
			if ((len-pos) < CMD_XWR_TRANS_SEND_MIN_LEN) {
				LOG_ERR("Cmd length error");
				return -1;
			}
			cmd->xwrite.transp_send.reader = buf[pos++];
			cmd->xwrite.transp_send.apdu_length = len-pos;
			memcpy(cmd->xwrite.transp_send.apdu, buf+pos, len-pos);
			pd->reply_id = REPLY_ACK;
			if (trigger_app != NULL) {
				*trigger_app = true;
			}
			break;
		case 2:
			// Disconnect from smart card
			if ((len-pos) < CMD_XWR_SC_DISCONNECT_LEN) {
				LOG_ERR("Cmd length error");
				return -1;
			}
			cmd->xwrite.sc_disco.reader = buf[pos++];
			pd->reply_id = REPLY_ACK;
			if (trigger_app != NULL) {
				*trigger_app = true;
			}
			break;
		case 4:
			// Trigger smart card scan
			if ((len-pos) < CMD_XWR_SC_SCAN_LEN) {
				LOG_ERR("Cmd length error");
				return -1;
			}
			cmd->xwrite.sc_scan.reader = buf[pos++];
			pd->reply_id = REPLY_ACK;
			if (trigger_app != NULL) {
				*trigger_app = true;
			}
			break;
		default:
			break;
		}
		break;

	default:
		LOG_ERR("Invalid XWR mode");
		break;
	}

	return 0;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int osdp_xrd_reply_build(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	const struct osdp_event *event = (struct osdp_event *)pd->ephemeral_data;
	int len = 0;

	// Assemble the common reply "header"
	if (max_len < (EVENT_XRD_HEADER_LEN+1)) {
		LOG_ERR("Event length error");
		return -1;
	}
	bwrite_u8(pd->reply_id, buf, &len);
	bwrite_u8(event->xread.mode, buf, &len);
	bwrite_u8(event->xread.reply, buf, &len);

	// Assemble the mode specific data
	switch (event->xread.mode) {
	// Support the read back and the setting of the PD’s behaviour mode.
	case 0:
		// Mode 0 commands are available in all modes.
		switch (event->xread.reply) {
		case 0x00:
			// General error indication
			if ((max_len-len) < EVENT_XRD_ERROR_LEN) {
				LOG_ERR("Event length error");
				return -1;
			}
			bwrite_u8(event->xread.error_reply.error_code, buf, &len);
			break;
		case 0x01:
			// Return the current extended write mode in effect.
			if ((max_len-len) < EVENT_XRD_MODE_REPORT_LEN) {
				LOG_ERR("Event length error");
				return -1;
			}
			bwrite_u8((uint8_t)pd->state, buf, &len);
			bwrite_u8(event->xread.mode_report.mode_config, buf, &len);
			break;
		case 0x02:
			// Card information report on smart card detection
			if ((max_len-len) < (EVENT_XRD_CARD_REPORT_MIN_LEN
					+ event->xread.card_report.csn_length + event->xread.card_report.length)) {
				LOG_ERR("Event length error");
				return -1;
			}
			bwrite_u8(event->xread.card_report.reader, buf, &len);
			bwrite_u8(event->xread.card_report.protocol, buf, &len);
			bwrite_u8(event->xread.card_report.csn_length, buf, &len);
			memcpy(buf + len, event->xread.card_report.csn,
					event->xread.card_report.csn_length);
			len += event->xread.card_report.csn_length;
			memcpy(buf + len, event->xread.card_report.data,
					event->xread.card_report.length);
			len += event->xread.card_report.length;
			break;
		default:
			return -1;
		}
		break;

	// Support transparent operations between the ACU and a Smart Card.
	case 1:
		// Verify that we are in the right mode
		if (pd->state != 0x01) {
			LOG_ERR("Wrong XRD behavior mode");
			return -1;
		}

		switch (event->xread.reply) {
		case 0x00:
			// General error indication
			if ((max_len-len) < EVENT_XRD_ERROR_LEN) {
				LOG_ERR("Event length error");
				return -1;
			}
			bwrite_u8(event->xread.error_reply.error_code, buf, &len);
			break;
		case 0x01:
			// Card present notification.
			if ((max_len-len) < EVENT_XRD_SC_PRESENT_LEN) {
				LOG_ERR("Event length error");
				return -1;
			}
			bwrite_u8(event->xread.card_present.reader, buf, &len);
			bwrite_u8(event->xread.card_present.status, buf, &len);
			break;
		case 0x02:
			// Transparent card data.
			if ((max_len-len) < (EVENT_XRD_CARD_DATA_MIN_LEN + event->xread.card_data.apdu_length)) {
				LOG_ERR("Event length error");
				return -1;
			}
			bwrite_u8(event->xread.card_data.reader, buf, &len);
			bwrite_u8(event->xread.card_data.status, buf, &len);
			memcpy(buf + len, event->xread.card_data.apdu,
					event->xread.card_data.apdu_length);
			len += event->xread.card_data.apdu_length;
			break;
		default:
			return -1;
		}
		break;

	default:
		LOG_ERR("Invalid XWR mode");
		break;
	}

	return len;
}

int osdp_xrd_reply_decode(struct osdp_pd *pd, struct osdp_event *event,
		const uint8_t *buf, int len)
{
	int pos = 0;

	// Parse the common "header"
	if (len < EVENT_XRD_HEADER_LEN) {
		LOG_ERR("Event length error");
		return -2;
	}
	event->type = OSDP_EVENT_XREAD;
	event->xread.mode = buf[pos++];
	event->xread.reply = buf[pos++];

	// Parse the mode specific data
	switch (event->xread.mode) {
	// Support the read back and the setting of the PD’s behaviour mode.
	case 0:
		switch (event->xread.reply) {
		case 0x00:
			// General error indication
			if ((len-pos) < EVENT_XRD_ERROR_LEN) {
				LOG_ERR("Event length error");
				return -2;
			}
			event->xread.error_reply.error_code = buf[pos++];
			break;
		case 0x01:
			// Return the current extended write mode in effect.
			if ((len-pos) < EVENT_XRD_MODE_REPORT_LEN) {
				LOG_ERR("Event length error");
				return -2;
			}
			event->xread.mode_report.mode_code = buf[pos++];
			event->xread.mode_report.mode_config =
					((len-pos) > 0) ? buf[pos++] : 0x0;
			break;
		case 0x02: {
			// Card information report on smart card detection
			if ((len-pos) < EVENT_XRD_CARD_REPORT_MIN_LEN) {
				LOG_ERR("Event length error");
				return -2;
			}
			event->xread.card_report.reader = buf[pos++];
			event->xread.card_report.protocol = buf[pos++];
			event->xread.card_report.csn_length = buf[pos++];
			if ((len-pos) < event->xread.card_report.csn_length) {
				LOG_ERR("Event length error");
				return -2;
			}
			memcpy(event->xread.card_report.csn, buf + pos,
					event->xread.card_report.csn_length);
			pos += event->xread.card_report.csn_length;
			event->xread.card_report.length = MAX(len - pos, 0);
			memcpy(event->xread.card_report.data, buf + pos, event->xread.card_report.length);
			pos += event->xread.card_report.length;
		}	break;
		default:
			return -1;
		}
		break;

	// Support transparent operations between the ACU and a Smart Card.
	case 1:
		switch (event->xread.reply) {
		case 0x00:
			// General error indication
			if ((len-pos) < EVENT_XRD_ERROR_LEN) {
				LOG_ERR("Event length error");
				return -2;
		}
			event->xread.error_reply.error_code = buf[pos++];
			break;
		case 0x01:
			// Card present notification.
			if ((len-pos) < EVENT_XRD_SC_PRESENT_LEN) {
				LOG_ERR("Event length error");
				return -2;
			}
			event->xread.card_present.reader = buf[pos++];
			event->xread.card_present.status = buf[pos++];
			break;
		case 0x02: {
			// Transparent card data.
			if ((len-pos) < EVENT_XRD_CARD_DATA_MIN_LEN) {
				LOG_ERR("Event length error");
				return -2;
			}
			event->xread.card_data.reader = buf[pos++];
			event->xread.card_data.status = buf[pos++];
			event->xread.card_data.apdu_length = MAX(len - pos, 0);
			memcpy(event->xread.card_data.apdu, buf + pos, event->xread.card_data.apdu_length);
			pos += event->xread.card_data.apdu_length;
		}	break;
		default:
		return -1;
		}
		break;

	default:
		LOG_ERR("Invalid XWR mode");
		break;
	}

	return 0;
}

