// SPDX-License-Identifier: GPL-2.0
/*
 * openipc-motor-ctl.c - Fast userspace CLI tool for /dev/openipc-motor.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "openipc-motor.h"

#define DEV_PATH "/dev/" OPENIPC_MOTOR_DEV_NAME

static int open_dev(void)
{
	int fd = open(DEV_PATH, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Error opening %s: [%d] %s\n", DEV_PATH, errno, strerror(errno));
		fprintf(stderr, "Ensure the openipc-motor kernel module is loaded (insmod openipc-motor.ko).\n");
	}
	return fd;
}

static void print_usage(const char *prog)
{
	printf("OpenIPC In-Kernel Motor Controller\n");
	printf("Usage:\n");
	printf("  %s move <pan_half_steps> <tilt_half_steps> [delay_us] [low_torque:0|1]\n", prog);
	printf("  %s status\n", prog);
	printf("  %s setpos <pan_pos> <tilt_pos>\n", prog);
	printf("  %s calibrate [tilt_span] [pan_span]\n", prog);
	printf("  %s config [pan_backlash] [tilt_backlash] [hold_ms]\n", prog);
	printf("  %s off\n", prog);
}

int main(int argc, char **argv)
{
	int fd;

	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}

	fd = open_dev();
	if (fd < 0)
		return 1;

	if (strcmp(argv[1], "status") == 0) {
		struct openipc_motor_status st;
		if (ioctl(fd, OPENIPC_MOTOR_IOC_GET_STATUS, &st) < 0) {
			perror("ioctl GET_STATUS");
			close(fd);
			return 1;
		}
		printf("{\"pan_pos\":%d,\"tilt_pos\":%d,\"energised\":%d,\"last_pan_dir\":%d,\"last_tilt_dir\":%d,\"pan_backlash_hits\":%d,\"tilt_backlash_hits\":%d}\n",
		       st.pan_pos, st.tilt_pos, st.energised, st.last_pan_dir, st.last_tilt_dir,
		       st.pan_backlash_hits, st.tilt_backlash_hits);
	} else if (strcmp(argv[1], "move") == 0) {
		struct openipc_motor_move m;
		if (argc < 4) {
			fprintf(stderr, "Usage: %s move <pan> <tilt> [delay_us] [low_torque]\n", argv[0]);
			close(fd);
			return 1;
		}
		m.pan_steps = atoi(argv[2]);
		m.tilt_steps = atoi(argv[3]);
		m.delay_us = (argc >= 5) ? atoi(argv[4]) : 2500;
		m.low_torque = (argc >= 6) ? atoi(argv[5]) : 0;

		if (ioctl(fd, OPENIPC_MOTOR_IOC_MOVE, &m) < 0) {
			perror("ioctl MOVE");
			close(fd);
			return 1;
		}
		printf("{\"status\":\"ok\",\"action\":\"move\",\"pan\":%d,\"tilt\":%d}\n",
		       m.pan_steps, m.tilt_steps);
	} else if (strcmp(argv[1], "setpos") == 0) {
		struct openipc_motor_set_pos sp;
		if (argc < 4) {
			fprintf(stderr, "Usage: %s setpos <pan_pos> <tilt_pos>\n", argv[0]);
			close(fd);
			return 1;
		}
		sp.pan_pos = atoi(argv[2]);
		sp.tilt_pos = atoi(argv[3]);

		if (ioctl(fd, OPENIPC_MOTOR_IOC_SET_POS, &sp) < 0) {
			perror("ioctl SET_POS");
			close(fd);
			return 1;
		}
		printf("{\"status\":\"ok\",\"action\":\"setpos\",\"pan_pos\":%d,\"tilt_pos\":%d}\n",
		       sp.pan_pos, sp.tilt_pos);
	} else if (strcmp(argv[1], "off") == 0) {
		if (ioctl(fd, OPENIPC_MOTOR_IOC_COILS_OFF) < 0) {
			perror("ioctl COILS_OFF");
			close(fd);
			return 1;
		}
		printf("{\"status\":\"ok\",\"action\":\"coils_off\"}\n");
	} else if (strcmp(argv[1], "config") == 0) {
		struct openipc_motor_config cfg;
		if (argc >= 5) {
			cfg.pan_backlash = atoi(argv[2]);
			cfg.tilt_backlash = atoi(argv[3]);
			cfg.hold_ms = atoi(argv[4]);
			cfg.ramp_start_us = 8000;
			cfg.ramp_steps = 48;
			if (ioctl(fd, OPENIPC_MOTOR_IOC_SET_CONFIG, &cfg) < 0) {
				perror("ioctl SET_CONFIG");
				close(fd);
				return 1;
			}
		}
		if (ioctl(fd, OPENIPC_MOTOR_IOC_GET_CONFIG, &cfg) < 0) {
			perror("ioctl GET_CONFIG");
			close(fd);
			return 1;
		}
		printf("{\"pan_backlash\":%d,\"tilt_backlash\":%d,\"ramp_start_us\":%d,\"ramp_steps\":%d,\"hold_ms\":%d}\n",
		       cfg.pan_backlash, cfg.tilt_backlash, cfg.ramp_start_us, cfg.ramp_steps, cfg.hold_ms);
	} else if (strcmp(argv[1], "calibrate") == 0) {
		int tilt_span = (argc >= 3) ? atoi(argv[2]) : 2048;
		int pan_span  = (argc >= 4) ? atoi(argv[3]) : 3900;
		struct openipc_motor_move m;
		struct openipc_motor_set_pos sp;

		/* Phase 1: Soft-homing Tilt alone to bottom hard stop with low_torque=1 */
		memset(&m, 0, sizeof(m));
		m.tilt_steps = -(tilt_span + 250);
		m.delay_us = 4000;
		m.low_torque = 1;
		if (ioctl(fd, OPENIPC_MOTOR_IOC_MOVE, &m) < 0) {
			perror("ioctl MOVE Phase 1");
			close(fd);
			return 1;
		}

		/* Reset tilt coordinate to 0 */
		sp.pan_pos = 0;
		sp.tilt_pos = 0;
		ioctl(fd, OPENIPC_MOTOR_IOC_SET_POS, &sp);
		usleep(100000);

		/* Phase 2: Soft-homing Pan alone to left hard stop with low_torque=1 */
		memset(&m, 0, sizeof(m));
		m.pan_steps = -(pan_span + 250);
		m.delay_us = 3500;
		m.low_torque = 1;
		if (ioctl(fd, OPENIPC_MOTOR_IOC_MOVE, &m) < 0) {
			perror("ioctl MOVE Phase 2");
			close(fd);
			return 1;
		}

		/* Reset coordinates to (0, 0) */
		sp.pan_pos = 0;
		sp.tilt_pos = 0;
		ioctl(fd, OPENIPC_MOTOR_IOC_SET_POS, &sp);
		usleep(100000);

		/* Phase 3: Move smoothly to Level Horizon Center (Pan: span/2, Tilt: span/2) */
		memset(&m, 0, sizeof(m));
		m.pan_steps = pan_span / 2;
		m.tilt_steps = tilt_span / 2;
		m.delay_us = 2500;
		m.low_torque = 0;
		if (ioctl(fd, OPENIPC_MOTOR_IOC_MOVE, &m) < 0) {
			perror("ioctl MOVE Phase 3");
			close(fd);
			return 1;
		}

		printf("{\"status\":\"ok\",\"action\":\"calibrate\",\"pan_pos\":%d,\"tilt_pos\":%d,\"pan_span\":%d,\"tilt_span\":%d}\n",
		       pan_span / 2, tilt_span / 2, pan_span, tilt_span);
	} else {
		print_usage(argv[0]);
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
