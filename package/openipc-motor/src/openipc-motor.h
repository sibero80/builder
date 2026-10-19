/* SPDX-License-Identifier: GPL-2.0 */
/*
 * openipc-motor.h - Shared ABI between the openipc-motor kernel driver and userspace.
 *
 * Implements fine half-step resolution, bounded slack-hysteresis backlash
 * compensation, 94-step S-curve acceleration/deceleration profiling,
 * coil thermal protection (auto-off), and soft-stall homing.
 */
#ifndef _OPENIPC_MOTOR_H
#define _OPENIPC_MOTOR_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define OPENIPC_MOTOR_DEV_NAME "openipc-motor"

/* Default physical travel spans (in half-steps) for Tapo C210 / C220 */
#define OPENIPC_MOTOR_DEFAULT_PAN_MAX   3900
#define OPENIPC_MOTOR_DEFAULT_TILT_MAX  2048

/* Movement request in individual half-steps (microsteps) */
struct openipc_motor_move {
	int pan_steps;      /* Positive = right, Negative = left */
	int tilt_steps;     /* Positive = up (ceiling), Negative = down (desk) */
	int delay_us;       /* Cruise delay per half-step in microseconds (e.g. 2500) */
	int low_torque;     /* 1 = calibration soft-impact mode (bypass limits, soft touchdown) */
};

/* Status query */
struct openipc_motor_status {
	int pan_pos;            /* Current tracked position in half-steps [0..pan_max] */
	int tilt_pos;           /* Current tracked position in half-steps [0..tilt_max] */
	int moving;             /* 1 if motion in progress */
	int energised;          /* Bitmask: 1=pan coils active, 2=tilt coils active */
	int last_pan_dir;       /* -1, 0, 1 */
	int last_tilt_dir;      /* -1, 0, 1 */
	int pan_backlash_hits;  /* Total backlash reversals compensated */
	int tilt_backlash_hits; /* Total backlash reversals compensated */
};

/* Runtime configuration */
struct openipc_motor_config {
	int pan_backlash;   /* Half-steps of gear slack on pan axis (e.g. 16) */
	int tilt_backlash;  /* Half-steps of gear slack on tilt axis (e.g. 24) */
	int ramp_start_us;  /* Starting pulse delay in us (e.g. 8000) */
	int ramp_steps;     /* Steps over which to ramp S-curve accel/decel (e.g. 94) */
	int hold_ms;        /* Coils stay energized after move in ms (e.g. 20), then auto-off */
};

/* Position override / reset */
struct openipc_motor_set_pos {
	int pan_pos;
	int tilt_pos;
};

#define OPENIPC_MOTOR_IOC_MAGIC       'M'
#define OPENIPC_MOTOR_IOC_MOVE        _IOW(OPENIPC_MOTOR_IOC_MAGIC, 1, struct openipc_motor_move)
#define OPENIPC_MOTOR_IOC_GET_STATUS  _IOR(OPENIPC_MOTOR_IOC_MAGIC, 2, struct openipc_motor_status)
#define OPENIPC_MOTOR_IOC_SET_CONFIG  _IOW(OPENIPC_MOTOR_IOC_MAGIC, 3, struct openipc_motor_config)
#define OPENIPC_MOTOR_IOC_GET_CONFIG  _IOR(OPENIPC_MOTOR_IOC_MAGIC, 4, struct openipc_motor_config)
#define OPENIPC_MOTOR_IOC_SET_POS     _IOW(OPENIPC_MOTOR_IOC_MAGIC, 5, struct openipc_motor_set_pos)
#define OPENIPC_MOTOR_IOC_COILS_OFF   _IO(OPENIPC_MOTOR_IOC_MAGIC,  6)
#define OPENIPC_MOTOR_IOC_STOP        _IO(OPENIPC_MOTOR_IOC_MAGIC,  7)

#endif /* _OPENIPC_MOTOR_H */
