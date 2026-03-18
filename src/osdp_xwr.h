/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_XWR_H_
#define _OSDP_XWR_H_

#include "osdp_common.h"

int pd_stage_event_xwr_reply(struct osdp_pd *pd, struct osdp_cmd *cmd, int cb_ret);

int osdp_xwr_cmd_build(struct osdp_pd *pd, const struct osdp_cmd *cmd,
		uint8_t *buf, int max_len);
int osdp_xwr_cmd_decode(struct osdp_pd *pd, struct osdp_cmd *cmd,
		const uint8_t *buf, int len, bool *trigger_app);

int osdp_xrd_reply_build(struct osdp_pd *pd, uint8_t *buf, int max_len);
int osdp_xrd_reply_decode(struct osdp_pd *pd, struct osdp_event *event,
		const uint8_t *buf, int len);

#endif /* _OSDP_XWR_H_ */
