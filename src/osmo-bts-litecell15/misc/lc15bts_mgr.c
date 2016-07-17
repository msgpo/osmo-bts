/* Main program for NuRAN Wireless Litecell 1.5 BTS management daemon */

/* Copyright (C) 2015 by Yves Godin <support@nuranwireless.com>
 * 
 * Based on sysmoBTS:
 *     sysmobts_mgr.c
 *     (C) 2012 by Harald Welte <laforge@gnumonks.org>
 *     (C) 2014 by Holger Hans Peter Freyther
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
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <sys/signal.h>
#include <sys/stat.h>

#include <osmocom/core/talloc.h>
#include <osmocom/core/application.h>
#include <osmocom/core/timer.h>
#include <osmocom/core/msgb.h>
#include <osmocom/vty/telnet_interface.h>
#include <osmocom/vty/logging.h>
#include <osmocom/vty/ports.h>

#include "misc/lc15bts_misc.h"
#include "misc/lc15bts_mgr.h"
#include "misc/lc15bts_par.h"
#include "misc/lc15bts_bid.h"
#include "misc/lc15bts_power.h"

static int no_rom_write = 0;
static int daemonize = 0;
void *tall_mgr_ctx;

/* every 6 hours means 365*4 = 1460 rom writes per year (max) */
#define TEMP_TIMER_SECS		(6 * 3600)

/* every 1 hours means 365*24 = 8760 rom writes per year (max) */
#define HOURS_TIMER_SECS	(1 * 3600)


/* the initial state */
static struct lc15bts_mgr_instance manager = {
	.config_file	= "lc15bts-mgr.cfg",
	.temp = {
		.supply_limit	= {
			.thresh_warn	= 60,
			.thresh_crit	= 78,
		},
		.soc_limit	= {
			.thresh_warn	= 60,
			.thresh_crit	= 78,
		},
		.fpga_limit	= {
			.thresh_warn	= 60,
			.thresh_crit	= 78,
		},
		.logrf_limit	= {
			.thresh_warn	= 60,
			.thresh_crit	= 78,
		},
		.ocxo_limit	= {
			.thresh_warn	= 60,
			.thresh_crit	= 78,
		},
		.tx0_limit	= {
			.thresh_warn	= 60,
			.thresh_crit	= 78,
		},
		.tx1_limit	= {
			.thresh_warn	= 60,
			.thresh_crit	= 78,
		},
		.pa0_limit	= {
			.thresh_warn	= 60,
			.thresh_crit	= 78,
		},
		.pa1_limit	= {
			.thresh_warn	= 60,
			.thresh_crit	= 78,
		},
		.action_warn		= 0,
		.action_crit		= TEMP_ACT_PA0_OFF | TEMP_ACT_PA1_OFF,
		.state			= STATE_NORMAL,
	}
};

static struct osmo_timer_list temp_timer;
static void check_temp_timer_cb(void *unused)
{
	lc15bts_check_temp(no_rom_write);

	osmo_timer_schedule(&temp_timer, TEMP_TIMER_SECS, 0);
}

static struct osmo_timer_list hours_timer;
static void hours_timer_cb(void *unused)
{
	lc15bts_update_hours(no_rom_write);

	osmo_timer_schedule(&hours_timer, HOURS_TIMER_SECS, 0);
}

static void print_help(void)
{
	printf("lc15bts-mgr [-nsD] [-d cat]\n");
	printf(" -n Do not write to ROM\n");
	printf(" -s Disable color\n");
	printf(" -d CAT enable debugging\n");
	printf(" -D daemonize\n");
	printf(" -c Specify the filename of the config file\n");
}

static int parse_options(int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "nhsd:c:")) != -1) {
		switch (opt) {
		case 'n':
			no_rom_write = 1;
			break;
		case 'h':
			print_help();
			return -1;
		case 's':
			log_set_use_color(osmo_stderr_target, 0);
			break;
		case 'd':
			log_parse_category_mask(osmo_stderr_target, optarg);
			break;
		case 'D':
			daemonize = 1;
			break;
		case 'c':
			manager.config_file = optarg;
			break;
		default:
			return -1;
		}
	}

	return 0;
}

static void signal_handler(int signal)
{
	fprintf(stderr, "signal %u received\n", signal);

	switch (signal) {
	case SIGINT:
		lc15bts_check_temp(no_rom_write);
		lc15bts_update_hours(no_rom_write);
		exit(0);
		break;
	case SIGABRT:
	case SIGUSR1:
	case SIGUSR2:
		talloc_report_full(tall_mgr_ctx, stderr);
		break;
	default:
		break;
	}
}

static struct log_info_cat mgr_log_info_cat[] = {
	[DTEMP] = {
		.name = "DTEMP",
		.description = "Temperature monitoring",
		.color = "\033[1;35m",
		.enabled = 1, .loglevel = LOGL_INFO,
	},
	[DFW] =	{
		.name = "DFW",
		.description = "Firmware management",
		.color = "\033[1;36m",
		.enabled = 1, .loglevel = LOGL_INFO,
	},
	[DFIND] = {
		.name = "DFIND",
		.description = "ipaccess-find handling",
		.color = "\033[1;37m",
		.enabled = 1, .loglevel = LOGL_INFO,
	},
	[DCALIB] = {
		.name = "DCALIB",
		.description = "Calibration handling",
		.color = "\033[1;37m",
		.enabled = 1, .loglevel = LOGL_INFO,
	},
};

static const struct log_info mgr_log_info = {
	.cat = mgr_log_info_cat,
	.num_cat = ARRAY_SIZE(mgr_log_info_cat),
};

static int mgr_log_init(void)
{
	osmo_init_logging(&mgr_log_info);
	return 0;
}

int main(int argc, char **argv)
{
	void *tall_msgb_ctx;
	int rc;


	tall_mgr_ctx = talloc_named_const(NULL, 1, "bts manager");
	tall_msgb_ctx = talloc_named_const(tall_mgr_ctx, 1, "msgb");
	msgb_set_talloc_ctx(tall_msgb_ctx);

	mgr_log_init();

	osmo_init_ignore_signals();
	signal(SIGINT, &signal_handler);
	signal(SIGUSR1, &signal_handler);
	signal(SIGUSR2, &signal_handler);

	rc = parse_options(argc, argv);
	if (rc < 0)
		exit(2);

	lc15bts_mgr_vty_init();
	logging_vty_add_cmds(&mgr_log_info);
	rc = lc15bts_mgr_parse_config(&manager);
	if (rc < 0) {
		LOGP(DFIND, LOGL_FATAL, "Cannot parse config file\n");
		exit(1);
	}

	rc = telnet_init(tall_msgb_ctx, NULL, OSMO_VTY_PORT_BTSMGR);
	if (rc < 0) {
		fprintf(stderr, "Error initializing telnet\n");
		exit(1);
	}

	/* start temperature check timer */
	temp_timer.cb = check_temp_timer_cb;
	check_temp_timer_cb(NULL);

	/* start operational hours timer */
	hours_timer.cb = hours_timer_cb;
	hours_timer_cb(NULL);

 	/* Enable the PAs */
	rc = lc15bts_power_set(LC15BTS_POWER_PA0, 1);
	if (rc < 0) {
		exit(3);
	}

	rc = lc15bts_power_set(LC15BTS_POWER_PA1, 1);
	if (rc < 0) {
		exit(3);
	}
	

	/* handle broadcast messages for ipaccess-find */
	if (lc15bts_mgr_nl_init() != 0)
		exit(3);

	/* Initialize the temperature control */
	lc15bts_mgr_temp_init(&manager);

	if (lc15bts_mgr_calib_init(&manager) != 0)
		exit(3);

	if (daemonize) {
		rc = osmo_daemonize();
		if (rc < 0) {
			perror("Error during daemonize");
			exit(1);
		}
	}


	while (1) {
		log_reset_context();
		osmo_select_main(0);
	}
}