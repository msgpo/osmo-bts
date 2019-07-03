/*
 * OpenBTS-style TRX interface/protocol handling
 *
 * This file contains the BTS-side implementation of the OpenBTS-style
 * UDP TRX protocol.  It manages the clock, control + burst-data UDP
 * sockets and their respective protocol encoding/parsing.
 *
 * Copyright (C) 2013  Andreas Eversberg <jolly@eversberg.eu>
 * Copyright (C) 2016-2017  Harald Welte <laforge@gnumonks.org>
 * Copyright (C) 2019  Vadim Yanitskiy <axilirator@gmail.com>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <netinet/in.h>

#include <osmocom/core/select.h>
#include <osmocom/core/socket.h>
#include <osmocom/core/timer.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/bits.h>

#include <osmo-bts/phy_link.h>
#include <osmo-bts/logging.h>
#include <osmo-bts/bts.h>
#include <osmo-bts/scheduler.h>

#include "l1_if.h"
#include "trx_if.h"

/* enable to print RSSI level graph */
//#define TOA_RSSI_DEBUG

int transceiver_available = 0;

/*
 * socket helper functions
 */

/*! convenience wrapper to open socket + fill in osmo_fd */
static int trx_udp_open(void *priv, struct osmo_fd *ofd, const char *host_local,
			uint16_t port_local, const char *host_remote, uint16_t port_remote,
			int (*cb)(struct osmo_fd *fd, unsigned int what))
{
	int rc;

	/* Init */
	ofd->fd = -1;
	ofd->cb = cb;
	ofd->data = priv;

	/* Listen / Binds + Connect */
	rc = osmo_sock_init2_ofd(ofd, AF_UNSPEC, SOCK_DGRAM, IPPROTO_UDP, host_local, port_local,
				host_remote, port_remote, OSMO_SOCK_F_BIND | OSMO_SOCK_F_CONNECT);
	if (rc < 0)
		return rc;

	return 0;
}

/* close socket + unregister osmo_fd */
static void trx_udp_close(struct osmo_fd *ofd)
{
	if (ofd->fd >= 0) {
		osmo_fd_unregister(ofd);
		close(ofd->fd);
		ofd->fd = -1;
	}
}


/*
 * TRX clock socket
 */

/* get clock from clock socket */
static int trx_clk_read_cb(struct osmo_fd *ofd, unsigned int what)
{
	struct phy_link *plink = ofd->data;
	struct phy_instance *pinst = phy_instance_by_num(plink, 0);
	char buf[1500];
	int len;
	uint32_t fn;

	OSMO_ASSERT(pinst);

	len = recv(ofd->fd, buf, sizeof(buf) - 1, 0);
	if (len <= 0)
		return len;
	buf[len] = '\0';

	if (!!strncmp(buf, "IND CLOCK ", 10)) {
		LOGP(DTRX, LOGL_NOTICE, "Unknown message on clock port: %s\n",
			buf);
		return 0;
	}

	if (sscanf(buf, "IND CLOCK %u", &fn) != 1) {
		LOGP(DTRX, LOGL_ERROR, "Unable to parse '%s'\n", buf);
		return 0;
	}

	LOGP(DTRX, LOGL_INFO, "Clock indication: fn=%u\n", fn);

	if (fn >= GSM_HYPERFRAME) {
		fn %= GSM_HYPERFRAME;
		LOGP(DTRX, LOGL_ERROR, "Indicated clock's FN is not wrapping "
			"correctly, correcting to fn=%u\n", fn);
	}

	/* inform core TRX clock handling code that a FN has been received */
	trx_sched_clock(pinst->trx->bts, fn);

	return 0;
}


/*
 * TRX ctrl socket
 */

/* send first ctrl message and start timer */
static void trx_ctrl_send(struct trx_l1h *l1h)
{
	struct trx_ctrl_msg *tcm;
	char buf[1500];
	int len;

	/* get first command */
	if (llist_empty(&l1h->trx_ctrl_list))
		return;
	tcm = llist_entry(l1h->trx_ctrl_list.next, struct trx_ctrl_msg, list);

	len = snprintf(buf, sizeof(buf), "CMD %s%s%s", tcm->cmd, tcm->params_len ? " ":"", tcm->params);
	OSMO_ASSERT(len < sizeof(buf));

	LOGPPHI(l1h->phy_inst, DTRX, LOGL_DEBUG, "Sending control '%s'\n", buf);
	/* send command */
	send(l1h->trx_ofd_ctrl.fd, buf, len+1, 0);

	/* start timer */
	osmo_timer_schedule(&l1h->trx_ctrl_timer, 2, 0);
}

/* send first ctrl message and start timer */
static void trx_ctrl_timer_cb(void *data)
{
	struct trx_l1h *l1h = data;
	struct trx_ctrl_msg *tcm = NULL;

	/* get first command */
	OSMO_ASSERT(!llist_empty(&l1h->trx_ctrl_list));
	tcm = llist_entry(l1h->trx_ctrl_list.next, struct trx_ctrl_msg, list);

	LOGPPHI(l1h->phy_inst, DTRX, LOGL_NOTICE, "No satisfactory response from transceiver(CMD %s%s%s)\n",
		tcm->cmd, tcm->params_len ? " ":"", tcm->params);

	trx_ctrl_send(l1h);
}

void trx_if_init(struct trx_l1h *l1h)
{
	l1h->trx_ctrl_timer.cb = trx_ctrl_timer_cb;
	l1h->trx_ctrl_timer.data = l1h;
}

/*! Send a new TRX control command.
 *  \param[inout] l1h TRX Layer1 handle to which to send command
 *  \param[in] criticial
 *  \param[in] cb callback function to be called when valid response is
 *  		  received. Type of cb depends on type of message.
 *  \param[in] cmd zero-terminated string containing command
 *  \param[in] fmt Format string (+ variable list of arguments)
 *  \returns 0 on success; negative on error
 *
 *  The new ocommand will be added to the end of the control command
 *  queue.
 */
static int trx_ctrl_cmd_cb(struct trx_l1h *l1h, int critical, void *cb, const char *cmd,
	const char *fmt, ...)
{
	struct trx_ctrl_msg *tcm;
	struct trx_ctrl_msg *prev = NULL;
	va_list ap;
	int pending;

	if (!transceiver_available &&
	    !(!strcmp(cmd, "POWEROFF") || !strcmp(cmd, "POWERON"))) {
		LOGPPHI(l1h->phy_inst, DTRX, LOGL_ERROR, "CTRL %s ignored: No clock from "
			"transceiver, please fix!\n", cmd);
		return -EIO;
	}

	pending = !llist_empty(&l1h->trx_ctrl_list);

	/* create message */
	tcm = talloc_zero(tall_bts_ctx, struct trx_ctrl_msg);
	if (!tcm)
		return -ENOMEM;
	snprintf(tcm->cmd, sizeof(tcm->cmd)-1, "%s", cmd);
	tcm->cmd[sizeof(tcm->cmd)-1] = '\0';
	tcm->cmd_len = strlen(tcm->cmd);
	if (fmt && fmt[0]) {
		va_start(ap, fmt);
		vsnprintf(tcm->params, sizeof(tcm->params) - 1, fmt, ap);
		va_end(ap);
		tcm->params[sizeof(tcm->params)-1] = '\0';
		tcm->params_len = strlen(tcm->params);
	} else {
		tcm->params[0] ='\0';
		tcm->params_len = 0;
	}
	tcm->critical = critical;
	tcm->cb = cb;

	/* Avoid adding consecutive duplicate messages, eg: two consecutive POWEROFF */
	if(pending)
		prev = llist_entry(l1h->trx_ctrl_list.prev, struct trx_ctrl_msg, list);

	if (!pending ||
	    !(strcmp(tcm->cmd, prev->cmd) == 0 && strcmp(tcm->params, prev->params) == 0)) {
		LOGPPHI(l1h->phy_inst, DTRX, LOGL_INFO, "Enqueuing TRX control command 'CMD %s%s%s'\n",
			tcm->cmd, tcm->params_len ? " ":"", tcm->params);
		llist_add_tail(&tcm->list, &l1h->trx_ctrl_list);
	}

	/* send message, if we didn't already have pending messages */
	if (!pending)
		trx_ctrl_send(l1h);

	return 0;
}
#define trx_ctrl_cmd(l1h, critical, cmd, fmt, ...) trx_ctrl_cmd_cb(l1h, critical, NULL, cmd, fmt, ##__VA_ARGS__)

/*! Send "POWEROFF" command to TRX */
int trx_if_cmd_poweroff(struct trx_l1h *l1h)
{
	struct phy_instance *pinst = l1h->phy_inst;
	if (pinst->num == 0)
		return trx_ctrl_cmd(l1h, 1, "POWEROFF", "");
	else
		return 0;
}

/*! Send "POWERON" command to TRX */
int trx_if_cmd_poweron(struct trx_l1h *l1h)
{
	struct phy_instance *pinst = l1h->phy_inst;
	if (pinst->num == 0)
		return trx_ctrl_cmd(l1h, 1, "POWERON", "");
	else
		return 0;
}

/*! Send "SETTSC" command to TRX */
int trx_if_cmd_settsc(struct trx_l1h *l1h, uint8_t tsc)
{
	struct phy_instance *pinst = l1h->phy_inst;
	if (pinst->phy_link->u.osmotrx.use_legacy_setbsic)
		return 0;

	return trx_ctrl_cmd(l1h, 1, "SETTSC", "%d", tsc);
}

/*! Send "SETBSIC" command to TRX */
int trx_if_cmd_setbsic(struct trx_l1h *l1h, uint8_t bsic)
{
	struct phy_instance *pinst = l1h->phy_inst;
	if (!pinst->phy_link->u.osmotrx.use_legacy_setbsic)
		return 0;

	return trx_ctrl_cmd(l1h, 1, "SETBSIC", "%d", bsic);
}

/*! Send "SETRXGAIN" command to TRX */
int trx_if_cmd_setrxgain(struct trx_l1h *l1h, int db)
{
	return trx_ctrl_cmd(l1h, 0, "SETRXGAIN", "%d", db);
}

/*! Send "SETPOWER" command to TRX */
int trx_if_cmd_setpower(struct trx_l1h *l1h, int db)
{
	return trx_ctrl_cmd(l1h, 0, "SETPOWER", "%d", db);
}

/*! Send "SETMAXDLY" command to TRX, i.e. maximum delay for RACH bursts */
int trx_if_cmd_setmaxdly(struct trx_l1h *l1h, int dly)
{
	return trx_ctrl_cmd(l1h, 0, "SETMAXDLY", "%d", dly);
}

/*! Send "SETMAXDLYNB" command to TRX, i.e. maximum delay for normal bursts */
int trx_if_cmd_setmaxdlynb(struct trx_l1h *l1h, int dly)
{
	return trx_ctrl_cmd(l1h, 0, "SETMAXDLYNB", "%d", dly);
}

/*! Send "SETSLOT" command to TRX: Configure Channel Combination for TS */
int trx_if_cmd_setslot(struct trx_l1h *l1h, uint8_t tn, uint8_t type, trx_if_cmd_setslot_cb *cb)
{
	return trx_ctrl_cmd_cb(l1h, 1, cb, "SETSLOT", "%d %d", tn, type);
}

/*! Send "RXTUNE" command to TRX: Tune Receiver to given ARFCN */
int trx_if_cmd_rxtune(struct trx_l1h *l1h, uint16_t arfcn)
{
	struct phy_instance *pinst = l1h->phy_inst;
	uint16_t freq10;

	if (pinst->trx->bts->band == GSM_BAND_1900)
		arfcn |= ARFCN_PCS;

	freq10 = gsm_arfcn2freq10(arfcn, 1); /* RX = uplink */
	if (freq10 == 0xffff) {
		LOGP(DTRX, LOGL_ERROR, "Arfcn %d not defined.\n",
		     arfcn & ~ARFCN_FLAG_MASK);
		return -ENOTSUP;
	}

	return trx_ctrl_cmd(l1h, 1, "RXTUNE", "%d", freq10 * 100);
}

/*! Send "TXTUNE" command to TRX: Tune Transmitter to given ARFCN */
int trx_if_cmd_txtune(struct trx_l1h *l1h, uint16_t arfcn)
{
	struct phy_instance *pinst = l1h->phy_inst;
	uint16_t freq10;

	if (pinst->trx->bts->band == GSM_BAND_1900)
		arfcn |= ARFCN_PCS;

	freq10 = gsm_arfcn2freq10(arfcn, 0); /* TX = downlink */
	if (freq10 == 0xffff) {
		LOGP(DTRX, LOGL_ERROR, "Arfcn %d not defined.\n",
		     arfcn & ~ARFCN_FLAG_MASK);
		return -ENOTSUP;
	}

	return trx_ctrl_cmd(l1h, 1, "TXTUNE", "%d", freq10 * 100);
}

/*! Send "HANDOVER" command to TRX: Enable handover RACH Detection on timeslot/sub-slot */
int trx_if_cmd_handover(struct trx_l1h *l1h, uint8_t tn, uint8_t ss)
{
	return trx_ctrl_cmd(l1h, 1, "HANDOVER", "%d %d", tn, ss);
}

/*! Send "NOHANDOVER" command to TRX: Disable handover RACH Detection on timeslot/sub-slot */
int trx_if_cmd_nohandover(struct trx_l1h *l1h, uint8_t tn, uint8_t ss)
{
	return trx_ctrl_cmd(l1h, 1, "NOHANDOVER", "%d %d", tn, ss);
}

struct trx_ctrl_rsp {
	char cmd[50];
	char params[100];
	int status;
	void *cb;
};

static int parse_rsp(const char *buf_in, size_t len_in, struct trx_ctrl_rsp *rsp)
{
	char *p, *k;

	if (strncmp(buf_in, "RSP ", 4))
		goto parse_err;

	/* Get the RSP cmd name */
	if (!(p = strchr(buf_in + 4, ' ')))
		goto parse_err;

	if (p - buf_in >= sizeof(rsp->cmd)) {
		LOGP(DTRX, LOGL_ERROR, "cmd buffer too small %lu >= %lu\n",
			p - buf_in, sizeof(rsp->cmd));
		goto parse_err;
	}

	rsp->cmd[0] = '\0';
	strncat(rsp->cmd, buf_in + 4, p - buf_in - 4);

	/* Now comes the status code of the response */
	p++;
	if (sscanf(p, "%d", &rsp->status) != 1)
		goto parse_err;

	/* Now copy back the parameters */
	k = strchr(p, ' ');
	if (k)
		k++;
	else
		k = p + strlen(p);

	if (strlen(k) >= sizeof(rsp->params)) {
		LOGP(DTRX, LOGL_ERROR, "params buffer too small %lu >= %lu\n",
			strlen(k), sizeof(rsp->params));
		goto parse_err;
	}
	rsp->params[0] = '\0';
	strcat(rsp->params, k);
	return 0;

parse_err:
	LOGP(DTRX, LOGL_NOTICE, "Unknown message on ctrl port: %s\n",
		buf_in);
	return -1;
}

static bool cmd_matches_rsp(struct trx_ctrl_msg *tcm, struct trx_ctrl_rsp *rsp)
{
	if (strcmp(tcm->cmd, rsp->cmd))
		return false;

	/* For SETSLOT we also need to check if it's the response for the
	   specific timeslot. For other commands such as SETRXGAIN, it is
	   expected that they can return different values */
	if (strcmp(tcm->cmd, "SETSLOT") == 0 && strcmp(tcm->params, rsp->params))
		return false;

	return true;
}

static int trx_ctrl_rx_rsp_setslot(struct trx_l1h *l1h, struct trx_ctrl_rsp *rsp)
{
	trx_if_cmd_setslot_cb *cb = (trx_if_cmd_setslot_cb*) rsp->cb;
	struct phy_instance *pinst = l1h->phy_inst;
	unsigned int tn, ts_type;

	if (rsp->status)
		LOGPPHI(pinst, DTRX, LOGL_ERROR, "transceiver SETSLOT failed with status %d\n",
			rsp->status);

	/* Since message was already validated against CMD we sent, we know format
	 * of params is: "<TN> <TS_TYPE>" */
	if (sscanf(rsp->params, "%u %u", &tn, &ts_type) < 2) {
		LOGPPHI(pinst, DTRX, LOGL_ERROR, "transceiver SETSLOT unable to parse params\n");
		return -EINVAL;
	}

	if (cb)
		cb(l1h, tn, ts_type, rsp->status);

	return rsp->status == 0 ? 0 : -EINVAL;
}

/* -EINVAL: unrecoverable error, exit BTS
 * N > 0: try sending originating command again after N seconds
 * 0: Done with response, get originating command out from send queue
 */
static int trx_ctrl_rx_rsp(struct trx_l1h *l1h, struct trx_ctrl_rsp *rsp, bool critical)
{
	struct phy_instance *pinst = l1h->phy_inst;

	/* If TRX fails, try again after 1 sec */
	if (strcmp(rsp->cmd, "POWERON") == 0) {
		if (rsp->status == 0) {
			if (pinst->phy_link->state != PHY_LINK_CONNECTED)
				phy_link_state_set(pinst->phy_link, PHY_LINK_CONNECTED);
			return 0;
		} else {
			LOGPPHI(l1h->phy_inst, DTRX, LOGL_NOTICE,
				"transceiver rejected POWERON command (%d), re-trying in a few seconds\n",
				rsp->status);
			if (pinst->phy_link->state != PHY_LINK_SHUTDOWN)
				phy_link_state_set(pinst->phy_link, PHY_LINK_SHUTDOWN);
			return 5;
		}
	} else if (strcmp(rsp->cmd, "SETSLOT") == 0) {
		return trx_ctrl_rx_rsp_setslot(l1h, rsp);
	}

	if (rsp->status) {
		LOGPPHI(l1h->phy_inst, DTRX, critical ? LOGL_FATAL : LOGL_NOTICE,
			"transceiver rejected TRX command with response: '%s%s%s %d'\n",
			rsp->cmd, rsp->params[0] != '\0' ? " ":"",
			rsp->params, rsp->status);
		if (critical)
			return -EINVAL;
	}
	return 0;
}

/*! Get + parse response from TRX ctrl socket */
static int trx_ctrl_read_cb(struct osmo_fd *ofd, unsigned int what)
{
	struct trx_l1h *l1h = ofd->data;
	struct phy_instance *pinst = l1h->phy_inst;
	char buf[1500];
	struct trx_ctrl_rsp rsp;
	int len, rc;
	struct trx_ctrl_msg *tcm;

	len = recv(ofd->fd, buf, sizeof(buf) - 1, 0);
	if (len <= 0)
		return len;
	buf[len] = '\0';

	if (parse_rsp(buf, len, &rsp) < 0)
		return 0;

	LOGPPHI(l1h->phy_inst, DTRX, LOGL_INFO, "Response message: '%s'\n", buf);

	/* abort timer and send next message, if any */
	if (osmo_timer_pending(&l1h->trx_ctrl_timer))
		osmo_timer_del(&l1h->trx_ctrl_timer);

	/* get command for response message */
	if (llist_empty(&l1h->trx_ctrl_list)) {
		/* RSP from a retransmission, skip it */
		if (l1h->last_acked && cmd_matches_rsp(l1h->last_acked, &rsp)) {
			LOGPPHI(l1h->phy_inst, DTRX, LOGL_NOTICE, "Discarding duplicated RSP "
				"from old CMD '%s'\n", buf);
			return 0;
		}
		LOGPPHI(l1h->phy_inst, DTRX, LOGL_NOTICE, "Response message without command\n");
		return -EINVAL;
	}
	tcm = llist_entry(l1h->trx_ctrl_list.next, struct trx_ctrl_msg,
		list);

	/* check if response matches command */
	if (!cmd_matches_rsp(tcm, &rsp)) {
		/* RSP from a retransmission, skip it */
		if (l1h->last_acked && cmd_matches_rsp(l1h->last_acked, &rsp)) {
			LOGPPHI(l1h->phy_inst, DTRX, LOGL_NOTICE, "Discarding duplicated RSP "
				"from old CMD '%s'\n", buf);
			return 0;
		}
		LOGPPHI(l1h->phy_inst, DTRX, (tcm->critical) ? LOGL_FATAL : LOGL_NOTICE,
			"Response message '%s' does not match command "
			"message 'CMD %s%s%s'\n",
			buf, tcm->cmd, tcm->params_len ? " ":"", tcm->params);
		goto rsp_error;
	}

	rsp.cb = tcm->cb;

	/* check for response code */
	rc = trx_ctrl_rx_rsp(l1h, &rsp, tcm->critical);
	if (rc == -EINVAL)
		goto rsp_error;

	/* re-schedule last cmd in rc seconds time */
	if (rc > 0) {
		osmo_timer_schedule(&l1h->trx_ctrl_timer, rc, 0);
		return 0;
	}

	/* remove command from list, save it to last_acked and removed previous last_acked */
	llist_del(&tcm->list);
	talloc_free(l1h->last_acked);
	l1h->last_acked = tcm;

	trx_ctrl_send(l1h);

	return 0;

rsp_error:
	bts_shutdown(pinst->trx->bts, "TRX-CTRL-MSG: CRITICAL");
	/* keep tcm list, so process is stopped */
	return -EIO;
}


/*
 * TRX burst data socket
 */

/* Maximum DATA message length (header + burst) */
#define TRX_DATA_MSG_MAX_LEN	512

/* Common header length: 1/2 VER + 1/2 TDMA TN + 4 TDMA FN */
#define TRX_CHDR_LEN		(1 + 4)
/* Uplink v0 header length: 1 RSSI + 2 ToA256 */
#define TRX_UL_V0HDR_LEN	(TRX_CHDR_LEN + 1 + 2)
/* Uplink v1 header length: + 1 MTS + 2 C/I */
#define TRX_UL_V1HDR_LEN	(TRX_UL_V0HDR_LEN + 1 + 2)

/* TRXD header dissector for version 0 */
static int trx_data_handle_hdr_v0(struct trx_l1h *l1h,
				  struct trx_ul_burst_ind *bi,
				  const uint8_t *buf, size_t buf_len)
{
	/* Make sure we have enough data */
	if (buf_len < TRX_UL_V0HDR_LEN) {
		LOGPPHI(l1h->phy_inst, DTRX, LOGL_ERROR,
			"Short read on TRXD, missing version 0 header "
			"(len=%zu vs expected %d)\n", buf_len, TRX_UL_V0HDR_LEN);
		return -EIO;
	}

	bi->tn = buf[0] & 0b111;
	bi->fn = osmo_load32be(buf + 1);
	bi->rssi = -(int8_t)buf[5];
	bi->toa256 = (int16_t) osmo_load16be(buf + 6);

	if (bi->fn >= GSM_HYPERFRAME) {
		LOGPPHI(l1h->phy_inst, DTRX, LOGL_ERROR,
			"Illegal TDMA fn=%u\n", bi->fn);
		return -EINVAL;
	}

	return TRX_UL_V0HDR_LEN;
}

/* TRXD header dissector for version 0x01 */
static int trx_data_handle_hdr_v1(struct trx_l1h *l1h,
				  struct trx_ul_burst_ind *bi,
				  const uint8_t *buf, size_t buf_len)
{
	uint8_t mts;
	int rc;

	/* Make sure we have enough data */
	if (buf_len < TRX_UL_V1HDR_LEN) {
		LOGPPHI(l1h->phy_inst, DTRX, LOGL_ERROR,
			"Short read on TRXD, missing version 1 header "
			"(len=%zu vs expected %d)\n", buf_len, TRX_UL_V1HDR_LEN);
		return -EIO;
	}

	/* Parse v0 specific part */
	rc = trx_data_handle_hdr_v0(l1h, bi, buf, buf_len);
	if (rc < 0)
		return rc;

	/* Move closer to the v1 specific part */
	buf_len -= rc;
	buf += rc;

	/* IDLE / NOPE frame indication */
	if (buf[0] & (1 << 7)) {
		bi->flags |= TRX_BI_F_NOPE_IND;
		return TRX_UL_V1HDR_LEN;
	}

	/* Modulation info and TSC set */
	mts = (buf[0] >> 3) & 0b1111;
	if ((mts & 0b1100) == 0x00) {
		bi->bt = TRX_BURST_GMSK;
		bi->tsc_set = mts & 0b11;
		bi->flags |= TRX_BI_F_MOD_TYPE;
	} else if ((mts & 0b0100) == 0b0100) {
		bi->bt = TRX_BURST_8PSK;
		bi->tsc_set = mts & 0b1;
		bi->flags |= TRX_BI_F_MOD_TYPE;
	} else {
		LOGPPHI(l1h->phy_inst, DTRX, LOGL_ERROR,
			"Indicated modulation 0x%02x is not supported\n", mts & 0b1110);
		return -ENOTSUP;
	}

	/* Training Sequence Code */
	bi->tsc = buf[0] & 0b111;
	bi->flags |= TRX_BI_F_TS_INFO;

	/* C/I: Carrier-to-Interference ratio (in centiBels) */
	bi->ci_cb = (int16_t) osmo_load16be(buf + 1);
	bi->flags |= TRX_BI_F_CI_CB;

	return TRX_UL_V1HDR_LEN;
}

/* TRXD burst handler for header version 0 */
static int trx_data_handle_burst_v0(struct trx_l1h *l1h,
				    struct trx_ul_burst_ind *bi,
				    const uint8_t *buf, size_t buf_len)
{
	size_t i;

	/* Verify burst length */
	switch (buf_len) {
	/* Legacy transceivers append two padding bytes */
	case EGPRS_BURST_LEN + 2:
	case GSM_BURST_LEN + 2:
		bi->burst_len = buf_len - 2;
		break;
	case EGPRS_BURST_LEN:
	case GSM_BURST_LEN:
		bi->burst_len = buf_len;
		break;

	default:
		LOGPPHI(l1h->phy_inst, DTRX, LOGL_NOTICE,
			"Rx TRXD message with odd burst length %zu\n", buf_len);
		return -EINVAL;
	}

	/* Convert unsigned soft-bits [254..0] to soft-bits [-127..127] */
	for (i = 0; i < bi->burst_len; i++) {
		if (buf[i] == 255)
			bi->burst[i] = -127;
		else
			bi->burst[i] = 127 - buf[i];
	}

	return 0;
}

/* TRXD burst handler for header version 1 */
static int trx_data_handle_burst_v1(struct trx_l1h *l1h,
				    struct trx_ul_burst_ind *bi,
				    const uint8_t *buf, size_t buf_len)
{
	/* Modulation types defined in 3GPP TS 45.002 */
	static const size_t bl[] = {
		[TRX_BURST_GMSK] = 148, /* 1 bit per symbol */
		[TRX_BURST_8PSK] = 444, /* 3 bits per symbol */
	};

	/* Verify burst length */
	if (bl[bi->bt] != buf_len) {
		LOGPPHI(l1h->phy_inst, DTRX, LOGL_NOTICE,
			"Rx TRXD message with odd burst length %zu, "
			"expected %zu\n", buf_len, bl[bi->bt]);
		return -EINVAL;
	}

	/* The burst format is the same as for version 0.
	 * NOTE: other modulation types to be handled separately. */
	return trx_data_handle_burst_v0(l1h, bi, buf, buf_len);
}

/* Parse TRXD message from transceiver, compose an UL burst indication.
 *
 * This message contains a demodulated Uplink burst with fixed-size
 * header preceding the burst bits. The header consists of the common
 * and message specific part.
 *
 *   +---------------+-----------------+------------+
 *   | common header | specific header | burst bits |
 *   +---------------+-----------------+------------+
 *
 * Common header is the same as for Downlink message:
 *
 *   +-----------------+----------------+-------------------+
 *   | VER (1/2 octet) | TN (1/2 octet) | FN (4 octets, BE) |
 *   +-----------------+----------------+-------------------+
 *
 * and among with TDMA parameters, contains the version indicator:
 *
 *   +-----------------+------------------------+
 *   | 7 6 5 4 3 2 1 0 | bit numbers            |
 *   +-----------------+------------------------+
 *   | X X X X . . . . | header version (0..15) |
 *   +-----------------+------------------------+
 *   | . . . . . X X X | TDMA TN (0..7)         |
 *   +-----------------+------------------------+
 *   | . . . . X . . . | RESERVED (0)           |
 *   +-----------------+------------------------+
 *
 * which is encoded in 4 MSB bits of the first octet, which used to be
 * zero-initialized due to the value range of TDMA TN. Therefore, the
 * old header format has implicit version 0x00.
 *
 * The message specific header has the following structure:
 *
 * == Version 0x00
 *
 *   +------+-----+--------------------+
 *   | RSSI | ToA | soft-bits (254..0) |
 *   +------+-----+--------------------+
 *
 * == Version 0x01
 *
 *   +------+-----+-----+-----+--------------------+
 *   | RSSI | ToA | MTS | C/I | soft-bits (254..0) |
 *   +------+-----+-----+-----+--------------------+
 *
 * where:
 *
 *   - RSSI (1 octet) - Received Signal Strength Indication
 *     encoded without the negative sign.
 *   - ToA (2 octets) - Timing of Arrival in units of 1/256
 *     of symbol (big endian).
 *   - MTS (1 octet)  - Modulation and Training Sequence info.
 *   - C/I (2 octets) - Carrier-to-Interference ratio (big endian).
 *
 * == Coding of MTS: Modulation and Training Sequence info
 *
 * 3GPP TS 45.002 version 15.1.0 defines several modulation types,
 * and a few sets of training sequences for each type. The most
 * common are GMSK and 8-PSK (which is used in EDGE).
 *
 *   +-----------------+---------------------------------------+
 *   | 7 6 5 4 3 2 1 0 | bit numbers (value range)             |
 *   +-----------------+---------------------------------------+
 *   | . . . . . X X X | Training Sequence Code (0..7)         |
 *   +-----------------+---------------------------------------+
 *   | . X X X X . . . | Modulation, TS set number (see below) |
 *   +-----------------+---------------------------------------+
 *   | X . . . . . . . | IDLE / nope frame indication (0 or 1) |
 *   +-----------------+---------------------------------------+
 *
 * The bit number 7 (MSB) is set to high when either nothing has been
 * detected, or during IDLE frames, so we can deliver noise levels,
 * and avoid clock gaps on the L1 side. Other bits are ignored,
 * and should be set to low (0) in this case. L16 shall be set to 0x00.
 *
 * == Coding of modulation and TS set number
 *
 * GMSK has 4 sets of training sequences (see tables 5.2.3a-d),
 * while 8-PSK (see tables 5.2.3f-g) and the others have 2 sets.
 * Access and Synchronization bursts also have several synch.
 * sequences.
 *
 *   +-----------------+---------------------------------------+
 *   | 7 6 5 4 3 2 1 0 | bit numbers (value range)             |
 *   +-----------------+---------------------------------------+
 *   | . 0 0 X X . . . | GMSK, 4 TS sets (0..3)                |
 *   +-----------------+---------------------------------------+
 *   | . 0 1 0 X . . . | 8-PSK, 2 TS sets (0..1)               |
 *   +-----------------+---------------------------------------+
 *   | . 0 1 1 X . . . | AQPSK, 2 TS sets (0..1)               |
 *   +-----------------+---------------------------------------+
 *   | . 1 0 0 X . . . | 16QAM, 2 TS sets (0..1)               |
 *   +-----------------+---------------------------------------+
 *   | . 1 0 1 X . . . | 32QAM, 2 TS sets (0..1)               |
 *   +-----------------+---------------------------------------+
 *   | . 1 1 1 X . . . | RESERVED (0)                          |
 *   +-----------------+---------------------------------------+
 *
 * NOTE: we only support GMSK and 8-PSK.
 *
 * == C/I: Carrier-to-Interference ratio
 *
 * The C/I value can be computed from the training sequence of each
 * burst, where we can compare the "ideal" training sequence with
 * the actual training sequence and then express that in centiBels.
 *
 * == Coding of the burst bits
 *
 * Unlike to be transmitted bursts, the received bursts are designated
 * using the soft-bits notation, so the receiver can indicate its
 * assurance from 0 to -127 that a given bit is 1, and from 0 to +127
 * that a given bit is 0.
 *
 * Each soft-bit (-127..127) of the burst is encoded as an unsigned
 * value in range (254..0) respectively using the constant shift.
 *
 */
static int trx_data_read_cb(struct osmo_fd *ofd, unsigned int what)
{
	struct trx_l1h *l1h = ofd->data;
	uint8_t buf[TRX_DATA_MSG_MAX_LEN];
	struct trx_ul_burst_ind bi;
	ssize_t hdr_len, buf_len;
	uint8_t hdr_ver;
	int rc;

	buf_len = recv(ofd->fd, buf, sizeof(buf), 0);
	if (buf_len <= 0) {
		LOGPPHI(l1h->phy_inst, DTRX, LOGL_ERROR,
			"recv() failed on TRXD with rc=%zd\n", buf_len);
		return buf_len;
	}

	/* Pre-clean (initialize) the flags */
	bi.flags = 0x00;

	/* Parse the header depending on its version */
	hdr_ver = buf[0] >> 4;
	switch (hdr_ver) {
	case 0:
		/* Legacy protocol has no version indicator */
		hdr_len = trx_data_handle_hdr_v0(l1h, &bi, buf, buf_len);
		break;
	case 1:
		hdr_len = trx_data_handle_hdr_v1(l1h, &bi, buf, buf_len);
		break;
	default:
		LOGPPHI(l1h->phy_inst, DTRX, LOGL_ERROR,
			"TRXD header version %u is not supported\n", hdr_ver);
		return -ENOTSUP;
	}

	/* Header parsing error */
	if (hdr_len < 0)
		return hdr_len;

	/* TODO: we can use NOPE indications to get noise levels on IDLE
	 * TDMA frames, and properly drive scheduler if nothing has been
	 * detected on non-IDLE channels. */
	if (bi.flags & TRX_BI_F_NOPE_IND) {
		LOGPPHI(l1h->phy_inst, DTRX, LOGL_NOTICE,
			"IDLE / NOPE indications are not (yet) supported\n");
		return -ENOTSUP;
	}

	/* We're done with the header now */
	buf_len -= hdr_len;

	/* Handle burst bits */
	switch (hdr_ver) {
	case 0:
		rc = trx_data_handle_burst_v0(l1h, &bi, buf + hdr_len, buf_len);
		break;
	case 1:
		rc = trx_data_handle_burst_v1(l1h, &bi, buf + hdr_len, buf_len);
		break;
	default:
		/* Shall not happen, just to make GCC happy */
		OSMO_ASSERT(0);
	}

	/* Burst parsing error */
	if (rc < 0)
		return rc;

	/* TODO: also print TSC and C/I */
	LOGPPHI(l1h->phy_inst, DTRX, LOGL_DEBUG,
		"Rx %s (hdr_ver=%u): tn=%u fn=%u rssi=%d toa256=%d\n",
		(bi.flags & TRX_BI_F_NOPE_IND) ? "NOPE.ind" : "UL burst",
		hdr_ver, bi.tn, bi.fn, bi.rssi, bi.toa256);

#ifdef TOA_RSSI_DEBUG
	char deb[128];

	sprintf(deb, "|                                0              "
		"                 | rssi=%4d  toa=%5d fn=%u",
		bi.rssi, bi.toa256, bi.fn);
	deb[1 + (128 + bi.rssi) / 4] = '*';
	fprintf(stderr, "%s\n", deb);
#endif

	/* feed received burst into scheduler code */
	trx_sched_ul_burst(&l1h->l1s, &bi);

	return 0;
}

/*! Send burst data for given FN/timeslot to TRX
 *  \param[inout] l1h TRX Layer1 handle referring to TX
 *  \param[in] tn Timeslot Number (0..7)
 *  \param[in] fn GSM Frame Number
 *  \param[in] pwr Transmit Power to use
 *  \param[in] bits Unpacked bits to be transmitted
 *  \param[in] nbits Number of \a bits
 *  \returns 0 on success; negative on error */
int trx_if_send_burst(struct trx_l1h *l1h, uint8_t tn, uint32_t fn, uint8_t pwr,
	const ubit_t *bits, uint16_t nbits)
{
	uint8_t buf[TRX_DATA_MSG_MAX_LEN];

	if ((nbits != GSM_BURST_LEN) && (nbits != EGPRS_BURST_LEN)) {
		LOGPPHI(l1h->phy_inst, DTRX, LOGL_ERROR, "Tx burst length %u invalid\n", nbits);
		return -1;
	}

	LOGPPHI(l1h->phy_inst, DTRX, LOGL_DEBUG, "TX burst tn=%u fn=%u pwr=%u\n", tn, fn, pwr);

	buf[0] = tn;
	buf[1] = (fn >> 24) & 0xff;
	buf[2] = (fn >> 16) & 0xff;
	buf[3] = (fn >>  8) & 0xff;
	buf[4] = (fn >>  0) & 0xff;
	buf[5] = pwr;

	/* copy ubits {0,1} */
	memcpy(buf + 6, bits, nbits);

	/* we must be sure that we have clock, and we have sent all control
	 * data */
	if (transceiver_available && llist_empty(&l1h->trx_ctrl_list)) {
		send(l1h->trx_ofd_data.fd, buf, nbits + 6, 0);
	} else
		LOGPPHI(l1h->phy_inst, DTRX, LOGL_ERROR, "Ignoring TX data, transceiver offline.\n");

	return 0;
}


/*
 * open/close
 */

/*! flush (delete) all pending control messages */
void trx_if_flush(struct trx_l1h *l1h)
{
	struct trx_ctrl_msg *tcm;

	/* free ctrl message list */
	while (!llist_empty(&l1h->trx_ctrl_list)) {
		tcm = llist_entry(l1h->trx_ctrl_list.next, struct trx_ctrl_msg,
			list);
		llist_del(&tcm->list);
		talloc_free(tcm);
	}
	talloc_free(l1h->last_acked);
}

/*! close the TRX for given handle (data + control socket) */
void trx_if_close(struct trx_l1h *l1h)
{
	LOGPPHI(l1h->phy_inst, DTRX, LOGL_NOTICE, "Close transceiver\n");

	trx_if_flush(l1h);

	/* close sockets */
	trx_udp_close(&l1h->trx_ofd_ctrl);
	trx_udp_close(&l1h->trx_ofd_data);
}

/*! compute UDP port number used for TRX protocol */
static uint16_t compute_port(struct phy_instance *pinst, int remote, int is_data)
{
	struct phy_link *plink = pinst->phy_link;
	uint16_t inc = 1;

	if (is_data)
		inc = 2;

	if (remote)
		return plink->u.osmotrx.base_port_remote + (pinst->num << 1) + inc;
	else
		return plink->u.osmotrx.base_port_local + (pinst->num << 1) + inc;
}

/*! open a TRX interface. creates contro + data sockets */
static int trx_if_open(struct trx_l1h *l1h)
{
	struct phy_instance *pinst = l1h->phy_inst;
	struct phy_link *plink = pinst->phy_link;
	int rc;

	LOGPPHI(l1h->phy_inst, DTRX, LOGL_NOTICE, "Open transceiver\n");

	/* initialize ctrl queue */
	INIT_LLIST_HEAD(&l1h->trx_ctrl_list);

	/* open sockets */
	rc = trx_udp_open(l1h, &l1h->trx_ofd_ctrl,
			  plink->u.osmotrx.local_ip,
			  compute_port(pinst, 0, 0),
			  plink->u.osmotrx.remote_ip,
			  compute_port(pinst, 1, 0), trx_ctrl_read_cb);
	if (rc < 0)
		goto err;
	rc = trx_udp_open(l1h, &l1h->trx_ofd_data,
			  plink->u.osmotrx.local_ip,
			  compute_port(pinst, 0, 1),
			  plink->u.osmotrx.remote_ip,
			  compute_port(pinst, 1, 1), trx_data_read_cb);
	if (rc < 0)
		goto err;

	/* enable all slots */
	l1h->config.slotmask = 0xff;

	/* FIXME: why was this only for TRX0 ? */
	//if (l1h->trx->nr == 0)
	trx_if_cmd_poweroff(l1h);

	return 0;

err:
	trx_if_close(l1h);
	return rc;
}

/*! close the control + burst data sockets for one phy_instance */
static void trx_phy_inst_close(struct phy_instance *pinst)
{
	struct trx_l1h *l1h = pinst->u.osmotrx.hdl;

	trx_if_close(l1h);
	trx_sched_exit(&l1h->l1s);
}

/*! open the control + burst data sockets for one phy_instance */
static int trx_phy_inst_open(struct phy_instance *pinst)
{
	struct trx_l1h *l1h;
	int rc;

	l1h = pinst->u.osmotrx.hdl;
	if (!l1h)
		return -EINVAL;

	rc = trx_sched_init(&l1h->l1s, pinst->trx);
	if (rc < 0) {
		LOGPPHI(l1h->phy_inst, DL1C, LOGL_FATAL, "Cannot initialize scheduler\n");
		return -EIO;
	}

	rc = trx_if_open(l1h);
	if (rc < 0) {
		LOGPPHI(l1h->phy_inst, DL1C, LOGL_FATAL, "Cannot open TRX interface\n");
		trx_phy_inst_close(pinst);
		return -EIO;
	}

	return 0;
}

/*! open the PHY link using TRX protocol */
int bts_model_phy_link_open(struct phy_link *plink)
{
	struct phy_instance *pinst;
	int rc;

	phy_link_state_set(plink, PHY_LINK_CONNECTING);

	/* open the shared/common clock socket */
	rc = trx_udp_open(plink, &plink->u.osmotrx.trx_ofd_clk,
			  plink->u.osmotrx.local_ip,
			  plink->u.osmotrx.base_port_local,
			  plink->u.osmotrx.remote_ip,
			  plink->u.osmotrx.base_port_remote,
			  trx_clk_read_cb);
	if (rc < 0) {
		phy_link_state_set(plink, PHY_LINK_SHUTDOWN);
		return -1;
	}

	/* open the individual instances with their ctrl+data sockets */
	llist_for_each_entry(pinst, &plink->instances, list) {
		if (trx_phy_inst_open(pinst) < 0)
			goto cleanup;
	}
	/* FIXME: is there better way to check/report TRX availability? */
	transceiver_available = 1;
	return 0;

cleanup:
	phy_link_state_set(plink, PHY_LINK_SHUTDOWN);
	llist_for_each_entry(pinst, &plink->instances, list) {
		if (pinst->u.osmotrx.hdl) {
			trx_if_close(pinst->u.osmotrx.hdl);
			pinst->u.osmotrx.hdl = NULL;
		}
	}
	trx_udp_close(&plink->u.osmotrx.trx_ofd_clk);
	return -1;
}

/*! determine if the TRX for given handle is powered up */
int trx_if_powered(struct trx_l1h *l1h)
{
	return l1h->config.poweron;
}
