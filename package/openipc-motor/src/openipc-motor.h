/* SPDX-License-Identifier: GPL-2.0 */
/*
 * openipc-motor.h - Shared ABI between the openipc-motor kernel driver and userspace.
 *
 * Implements fine half-step resolution, gear backlash compensation,
 * coil thermal protection (auto-off), and soft-stall homing.
 */
#ifndef _OPENIPC_MOTOR_H
#define _OPENIPC_MOTOR_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define OPENIPC_MOTOR_DEV_NAME "openipc-motor"

/* Movement request in individual half-steps (microsteps) */
struct openipc_motor_move {
	int pan_steps;      /* Positive = right, Negative = left */
	int tilt_steps;     /* Positive = up (ceiling), Negative = down (desk) */
	int delay_us;       /* Cruise delay per half-step in microseconds (e.g. 2500) */
	int low_torque;     /* 1 = calibration soft-impact mode (elongated pulses, gentle stall) */
};

/* Status query */
struct openipc_motor_status {
	int pan_pos;            /* Current tracked position in half-steps */
	int tilt_pos;           /* Current tracked position in half-steps */
	int moving;             /* 1 if motion in progress */
	int energised;          /* 1 if coils are currently powered */
	int last_pan_dir;       /* -1, 0, 1 */
	int last_tilt_dir;      /* -1, 0, 1 */
	int pan_backlash_hits;  /* Total backlash bursts applied */
	int tilt_backlash_hits; /* Total backlash bursts applied */
};

/* Runtime configuration */
struct openipc_motor_config {
	int pan_backlash;   /* Half-steps to inject on pan reversal (e.g. 16) */
	int tilt_backlash;  /* Half-steps to inject on tilt reversal (e.g. 24) */
	int ramp_start_us;  /* Starting pulse delay in us (e.g. 8000) */
	int ramp_steps;     /* Steps over which to ramp acceleration (e.g. 48) */
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
