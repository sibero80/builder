/*
 * ptz-controller.c — Unified PTZ Motion, 94-Step S-Curve Ramping & Preset Engine
 * Target: TP-Link Tapo C210 v2 / C220 (SigmaStar SSC333) on OpenIPC
 *
 * Features:
 *  - Automatic delegation to /dev/openipc-motor kernel hrtimer driver when loaded
 *  - Real-time SCHED_FIFO user-space fallback with 94-entry S-curve ramp & backlash slack
 *  - Bounded slack-hysteresis backlash compensation (Pan: 16, Tilt: 24 half-steps)
 *  - Sequential single-axis stepping to prevent 5V rail voltage sag
 *  - Soft-touch homing calibration (8000 us final 15% touchdown)
 *  - Persistent phase & coordinate tracking (/tmp/ptz_state & /etc/ptz_presets.json)
 *  - Silent boot coordinate restoration (no noisy calibration sweep if already homed)
 *  - Named & numbered presets for ONVIF (Frigate) and HTTP/WebUI
 *  - Drop-in compatibility when invoked as `gpio-motors`
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef STATE_FILE
#define STATE_FILE       "/tmp/ptz_state"
#endif
#define LOCK_FILE        "/tmp/ptz.lock"
#ifndef PRESETS_FILE
#define PRESETS_FILE     "/etc/ptz_presets.json"
#endif
#ifndef PRESETS_TMP_FILE
#define PRESETS_TMP_FILE "/etc/ptz_presets.json.tmp"
#endif

#define KERNEL_MOTOR_DEV "/dev/openipc-motor"

/* Kernel driver ioctl ABI (matches openipc-motor.h) */
struct openipc_motor_move {
    int pan_steps;
    int tilt_steps;
    int delay_us;
    int low_torque;
};

struct openipc_motor_status {
    int pan_pos;
    int tilt_pos;
    int moving;
    int energised;
    int last_pan_dir;
    int last_tilt_dir;
    int pan_backlash_hits;
    int tilt_backlash_hits;
};

struct openipc_motor_set_pos {
    int pan_pos;
    int tilt_pos;
};

#define OPENIPC_MOTOR_IOC_MAGIC       'M'
#define OPENIPC_MOTOR_IOC_MOVE        _IOW(OPENIPC_MOTOR_IOC_MAGIC, 1, struct openipc_motor_move)
#define OPENIPC_MOTOR_IOC_GET_STATUS  _IOR(OPENIPC_MOTOR_IOC_MAGIC, 2, struct openipc_motor_status)
#define OPENIPC_MOTOR_IOC_SET_POS     _IOW(OPENIPC_MOTOR_IOC_MAGIC, 5, struct openipc_motor_set_pos)
#define OPENIPC_MOTOR_IOC_COILS_OFF   _IO(OPENIPC_MOTOR_IOC_MAGIC,  6)
#define OPENIPC_MOTOR_IOC_STOP        _IO(OPENIPC_MOTOR_IOC_MAGIC,  7)

/* Tapo C210 v2 Hardware Travel & Timing Constants (in half-steps) */
#define DEFAULT_PAN_SPAN   3900   /* ~343 deg travel */
#define DEFAULT_TILT_SPAN  2048   /* ~114 deg travel (physical desk to ceiling limits matching stock Tapo) */
#define DEFAULT_PAN_HOME   1950   /* Center */
#define DEFAULT_TILT_HOME  1024   /* Level horizon */

#define PAN_BACKLASH       16     /* 16 half-steps (~1.4 deg) pan gear slack */
#define TILT_BACKLASH      24     /* 24 half-steps (~1.3 deg) tilt gear slack */

#define T_START_US         8000   /* 8.0 ms (125 Hz) high-torque S-curve start */
#define T_CRUISE_US        2500   /* 2.5 ms (400 Hz) smooth cruise */
#define T_SOFT_STOP_US     8000   /* 8.0 ms low-torque soft mechanical touchdown */
#define SCURVE_LEN         94     /* 94-step S-curve ramp matching stock motor_driver.ko */

#define MAX_PRESETS        16

/* 94-entry Sigmoid S-curve frequency percentage table (10% .. 99%) from stock motor_driver.ko */
static const unsigned char STOCK_SCURVE_PCT[SCURVE_LEN] = {
    10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 12, 12, 12, 12,
    13, 13, 14, 14, 14, 15, 15, 16, 16, 17, 18, 19, 19, 20, 21, 22,
    23, 24, 25, 26, 28, 29, 31, 32, 34, 36, 38, 40, 42, 44, 46, 48,
    50, 53, 56, 59, 62, 65, 67, 69, 71, 73, 75, 77, 79, 81, 83, 85,
    86, 87, 88, 89, 90, 90, 91, 92, 93, 93, 94, 94, 95, 95, 96, 96,
    96, 97, 97, 97, 98, 98, 98, 98, 99, 99, 99, 99, 99, 99
};

typedef struct {
    int id;
    int used;
    char name[32];
    int pan;
    int tilt;
} ptz_preset_t;

typedef struct {
    int pan_pos;
    int tilt_pos;
    int pan_span;
    int tilt_span;
    int calibrated;
    int moving;
    int pan_phase;
    int tilt_phase;
    int pan_slack;
    int tilt_slack;
    int pid;
    int pan_pins[4];
    int tilt_pins[4];
    ptz_preset_t presets[MAX_PRESETS];
} ptz_ctx_t;

/* 8-phase half-step excitation sequence for 24BYJ-48 / 4-phase unipolar stepper */
static const int PHASE_SEQ[8][4] = {
    {1, 0, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 0},
    {0, 1, 1, 0},
    {0, 0, 1, 0},
    {0, 0, 1, 1},
    {0, 0, 0, 1},
    {1, 0, 0, 1}
};

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_sig(int sig) {
    (void)sig;
    g_stop_requested = 1;
}

static void sleep_us(int us) {
    if (us <= 0) return;
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (long)(us % 1000000) * 1000L;
    nanosleep(&ts, NULL);
}

/* Compute step delay (in us) using the 94-entry S-curve table for accel and decel */
static int compute_step_delay(int step_idx, int total_steps, int t_start, int t_cruise, int n_ramp) {
    if (total_steps <= 1 || n_ramp <= 0 || t_start <= t_cruise) {
        return t_cruise;
    }
    int dist_from_start = step_idx;
    int dist_from_end = total_steps - 1 - step_idx;
    int k = (dist_from_start < dist_from_end) ? dist_from_start : dist_from_end;

    if (k >= n_ramp) {
        return t_cruise;
    }
    int tbl_idx = (k * (SCURVE_LEN - 1)) / n_ramp;
    int pct = (int)STOCK_SCURVE_PCT[tbl_idx];
    int start_freq = 1000000 / t_start;
    int cruise_freq = 1000000 / t_cruise;
    int freq = start_freq + ((cruise_freq - start_freq) * (pct - 10)) / 90;
    return (freq > 0) ? (1000000 / freq) : t_cruise;
}

static void init_default_ctx(ptz_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->pan_pos = DEFAULT_PAN_HOME;
    ctx->tilt_pos = DEFAULT_TILT_HOME;
    ctx->pan_span = DEFAULT_PAN_SPAN;
    ctx->tilt_span = DEFAULT_TILT_SPAN;
    ctx->calibrated = 1;
    ctx->moving = 0;
    ctx->pan_phase = 0;
    ctx->tilt_phase = 0;
    ctx->pan_slack = PAN_BACKLASH;
    ctx->tilt_slack = TILT_BACKLASH;
    ctx->pid = 0;

    /* Default Tapo C210 v2 GPIOs: Pan 44 45 46 47, Tilt 80 79 76 77 */
    ctx->pan_pins[0] = 44;
    ctx->pan_pins[1] = 45;
    ctx->pan_pins[2] = 46;
    ctx->pan_pins[3] = 47;
    ctx->tilt_pins[0] = 80;
    ctx->tilt_pins[1] = 79;
    ctx->tilt_pins[2] = 76;
    ctx->tilt_pins[3] = 77;

    /* Default presets matching full hardware physical range */
    ctx->presets[0].id = 1;
    ctx->presets[0].used = 1;
    strcpy(ctx->presets[0].name, "home");
    ctx->presets[0].pan = DEFAULT_PAN_HOME;
    ctx->presets[0].tilt = DEFAULT_TILT_HOME;

    ctx->presets[1].id = 2;
    ctx->presets[1].used = 1;
    strcpy(ctx->presets[1].name, "door");
    ctx->presets[1].pan = 2800;
    ctx->presets[1].tilt = 900;

    ctx->presets[2].id = 3;
    ctx->presets[2].used = 1;
    strcpy(ctx->presets[2].name, "window");
    ctx->presets[2].pan = 800;
    ctx->presets[2].tilt = 1200;
}

static void load_gpio_pins_from_env(ptz_ctx_t *ctx) {
    FILE *fp = popen("fw_printenv -n ptz_gpio 2>/dev/null", "r");
    if (!fp) return;
    char buf[128] = {0};
    if (fgets(buf, sizeof(buf), fp)) {
        int p[8];
        if (sscanf(buf, "%d %d %d %d %d %d %d %d",
                   &p[0], &p[1], &p[2], &p[3],
                   &p[4], &p[5], &p[6], &p[7]) == 8) {
            for (int i = 0; i < 4; i++) {
                ctx->pan_pins[i] = p[i];
                ctx->tilt_pins[i] = p[i + 4];
            }
        }
    }
    pclose(fp);
}

/* Parse /etc/ptz_presets.json without external library dependencies */
static int load_presets_json(ptz_ctx_t *ctx) {
    FILE *f = fopen(PRESETS_FILE, "r");
    if (!f) return -1;

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    char *p;
    if ((p = strstr(buf, "\"homed\"")) != NULL) {
        char *colon = strchr(p, ':');
        if (colon) {
            while (*colon && (*colon == ':' || isspace((unsigned char)*colon))) colon++;
            ctx->calibrated = (strncmp(colon, "true", 4) == 0 || *colon == '1') ? 1 : 0;
        }
    }
    if ((p = strstr(buf, "\"pan_pos\"")) != NULL) {
        int v;
        if (sscanf(p, "\"pan_pos\" : %d", &v) == 1 || sscanf(p, "\"pan_pos\":%d", &v) == 1)
            ctx->pan_pos = v;
    }
    if ((p = strstr(buf, "\"tilt_pos\"")) != NULL) {
        int v;
        if (sscanf(p, "\"tilt_pos\" : %d", &v) == 1 || sscanf(p, "\"tilt_pos\":%d", &v) == 1)
            ctx->tilt_pos = v;
    }
    if ((p = strstr(buf, "\"pan_span\"")) != NULL) {
        int v;
        if (sscanf(p, "\"pan_span\" : %d", &v) == 1 || sscanf(p, "\"pan_span\":%d", &v) == 1)
            if (v > 0) ctx->pan_span = v;
    }
    if ((p = strstr(buf, "\"tilt_span\"")) != NULL) {
        int v;
        if (sscanf(p, "\"tilt_span\" : %d", &v) == 1 || sscanf(p, "\"tilt_span\":%d", &v) == 1)
            if (v > 0) ctx->tilt_span = v;
    }

    char *presets_sec = strstr(buf, "\"presets\"");
    if (presets_sec) {
        for (int i = 0; i < MAX_PRESETS; i++) {
            ctx->presets[i].used = 0;
            ctx->presets[i].id = i + 1;
        }
        char *cur = strchr(presets_sec, '{');
        if (cur) cur++;
        while (cur && *cur) {
            char *q1 = strchr(cur, '"');
            if (!q1) break;
            char *q2 = strchr(q1 + 1, '"');
            if (!q2) break;
            int id = atoi(q1 + 1);
            char *obj_start = strchr(q2, '{');
            if (!obj_start) break;
            char *obj_end = strchr(obj_start, '}');
            if (!obj_end) break;

            if (id >= 1 && id <= MAX_PRESETS) {
                int idx = id - 1;
                ctx->presets[idx].id = id;
                ctx->presets[idx].used = 1;
                snprintf(ctx->presets[idx].name, sizeof(ctx->presets[idx].name), "preset_%d", id);
                ctx->presets[idx].pan = DEFAULT_PAN_HOME;
                ctx->presets[idx].tilt = DEFAULT_TILT_HOME;

                /* Extract name, pan, tilt inside [obj_start, obj_end] */
                char obj_buf[256];
                size_t olen = (size_t)(obj_end - obj_start + 1);
                if (olen >= sizeof(obj_buf)) olen = sizeof(obj_buf) - 1;
                memcpy(obj_buf, obj_start, olen);
                obj_buf[olen] = '\0';

                char *np = strstr(obj_buf, "\"name\"");
                if (np) {
                    char *nq1 = strchr(np + 6, '"');
                    if (nq1) {
                        char *nq2 = strchr(nq1 + 1, '"');
                        if (nq2) {
                            size_t nlen = (size_t)(nq2 - (nq1 + 1));
                            if (nlen >= sizeof(ctx->presets[idx].name))
                                nlen = sizeof(ctx->presets[idx].name) - 1;
                            memcpy(ctx->presets[idx].name, nq1 + 1, nlen);
                            ctx->presets[idx].name[nlen] = '\0';
                        }
                    }
                }
                char *pp = strstr(obj_buf, "\"pan\"");
                if (pp) {
                    char *c = strchr(pp, ':');
                    if (c) ctx->presets[idx].pan = atoi(c + 1);
                }
                char *tp = strstr(obj_buf, "\"tilt\"");
                if (tp) {
                    char *c = strchr(tp, ':');
                    if (c) ctx->presets[idx].tilt = atoi(c + 1);
                }
            }
            cur = obj_end + 1;
        }
    }
    return 0;
}

static int save_presets_json(const ptz_ctx_t *ctx) {
    int used_tmp = 1;
    FILE *f = fopen(PRESETS_TMP_FILE, "w");
    if (!f) {
        used_tmp = 0;
        f = fopen(PRESETS_FILE, "w");
    }
    if (!f) return -1;

    fprintf(f, "{\n");
    fprintf(f, "  \"homed\": %s,\n", ctx->calibrated ? "true" : "false");
    fprintf(f, "  \"pan_pos\": %d,\n", ctx->pan_pos);
    fprintf(f, "  \"tilt_pos\": %d,\n", ctx->tilt_pos);
    fprintf(f, "  \"pan_span\": %d,\n", ctx->pan_span);
    fprintf(f, "  \"tilt_span\": %d,\n", ctx->tilt_span);
    fprintf(f, "  \"presets\": {\n");

    int first = 1;
    for (int i = 0; i < MAX_PRESETS; i++) {
        if (!ctx->presets[i].used) continue;
        if (!first) fprintf(f, ",\n");
        fprintf(f, "    \"%d\": {\"name\": \"%s\", \"pan\": %d, \"tilt\": %d}",
                ctx->presets[i].id, ctx->presets[i].name,
                ctx->presets[i].pan, ctx->presets[i].tilt);
        first = 0;
    }
    fprintf(f, "\n  }\n}\n");
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (used_tmp && rename(PRESETS_TMP_FILE, PRESETS_FILE) != 0) {
        unlink(PRESETS_TMP_FILE);
    }
    return 0;
}

static void sync_from_kernel_driver(ptz_ctx_t *ctx) {
    int kfd = open(KERNEL_MOTOR_DEV, O_RDWR);
    if (kfd >= 0) {
        struct openipc_motor_status st;
        if (ioctl(kfd, OPENIPC_MOTOR_IOC_GET_STATUS, &st) == 0) {
            ctx->pan_pos = st.pan_pos;
            ctx->tilt_pos = st.tilt_pos;
            ctx->moving = st.moving;
        }
        close(kfd);
    }
}

static int load_state(ptz_ctx_t *ctx) {
    init_default_ctx(ctx);
    load_presets_json(ctx);

    FILE *f = fopen(STATE_FILE, "r");
    if (!f) {
        load_gpio_pins_from_env(ctx);
        return -1;
    }
    char line[128];
    int got_pins = 0;
    while (fgets(line, sizeof(line), f)) {
        int val;
        if (sscanf(line, "pan_pos=%d", &val) == 1) ctx->pan_pos = val;
        else if (sscanf(line, "tilt_pos=%d", &val) == 1) ctx->tilt_pos = val;
        else if (sscanf(line, "calibrated=%d", &val) == 1) ctx->calibrated = val;
        else if (sscanf(line, "moving=%d", &val) == 1) ctx->moving = val;
        else if (sscanf(line, "pan_phase=%d", &val) == 1) ctx->pan_phase = val & 7;
        else if (sscanf(line, "tilt_phase=%d", &val) == 1) ctx->tilt_phase = val & 7;
        else if (sscanf(line, "pan_slack=%d", &val) == 1) ctx->pan_slack = val;
        else if (sscanf(line, "tilt_slack=%d", &val) == 1) ctx->tilt_slack = val;
        else if (sscanf(line, "pid=%d", &val) == 1) ctx->pid = val;
        else if (strncmp(line, "pins=", 5) == 0) {
            int p[8];
            if (sscanf(line + 5, "%d %d %d %d %d %d %d %d",
                       &p[0], &p[1], &p[2], &p[3],
                       &p[4], &p[5], &p[6], &p[7]) == 8) {
                for (int i = 0; i < 4; i++) {
                    ctx->pan_pins[i] = p[i];
                    ctx->tilt_pins[i] = p[i + 4];
                }
                got_pins = 1;
            }
        }
    }
    fclose(f);
    if (!got_pins) {
        load_gpio_pins_from_env(ctx);
    }
    sync_from_kernel_driver(ctx);
    return 0;
}

static int save_state(const ptz_ctx_t *ctx) {
    FILE *f = fopen(STATE_FILE, "w");
    if (!f) return -1;
    fprintf(f, "pan_pos=%d\n", ctx->pan_pos);
    fprintf(f, "tilt_pos=%d\n", ctx->tilt_pos);
    fprintf(f, "calibrated=%d\n", ctx->calibrated);
    fprintf(f, "moving=%d\n", ctx->moving);
    fprintf(f, "pan_phase=%d\n", ctx->pan_phase & 7);
    fprintf(f, "tilt_phase=%d\n", ctx->tilt_phase & 7);
    fprintf(f, "pan_slack=%d\n", ctx->pan_slack);
    fprintf(f, "tilt_slack=%d\n", ctx->tilt_slack);
    fprintf(f, "pid=%d\n", ctx->pid);
    fprintf(f, "pins=%d %d %d %d %d %d %d %d\n",
            ctx->pan_pins[0], ctx->pan_pins[1], ctx->pan_pins[2], ctx->pan_pins[3],
            ctx->tilt_pins[0], ctx->tilt_pins[1], ctx->tilt_pins[2], ctx->tilt_pins[3]);
    fclose(f);
    return 0;
}

/* Sysfs GPIO helpers for user-space fallback mode */
static int gpio_open_pins(const int pins[4], int fds[4]) {
    char path[64];
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pins[i]);
        if (access(path, F_OK) != 0) {
            int efd = open("/sys/class/gpio/export", O_WRONLY);
            if (efd >= 0) {
                char num[16];
                int len = snprintf(num, sizeof(num), "%d", pins[i]);
                (void)write(efd, num, (size_t)len);
                close(efd);
            }
            snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pins[i]);
            int dfd = open(path, O_WRONLY);
            if (dfd >= 0) {
                (void)write(dfd, "out", 3);
                close(dfd);
            }
            snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pins[i]);
        }
        fds[i] = open(path, O_WRONLY);
    }
    return 0;
}

static void gpio_set_phase(const int fds[4], int phase) {
    const int *seq = PHASE_SEQ[phase & 7];
    for (int i = 0; i < 4; i++) {
        if (fds[i] >= 0) {
            (void)lseek(fds[i], 0, SEEK_SET);
            (void)write(fds[i], seq[i] ? "1" : "0", 1);
        }
    }
}

static void gpio_deenergize_and_close(int fds[4]) {
    for (int i = 0; i < 4; i++) {
        if (fds[i] >= 0) {
            (void)lseek(fds[i], 0, SEEK_SET);
            (void)write(fds[i], "0", 1);
            close(fds[i]);
            fds[i] = -1;
        }
    }
}

/* Stop any currently running ptz-controller motion or kernel hrtimer move */
static void stop_active_motion(ptz_ctx_t *ctx) {
    int kfd = open(KERNEL_MOTOR_DEV, O_RDWR);
    if (kfd >= 0) {
        (void)ioctl(kfd, OPENIPC_MOTOR_IOC_STOP);
        for (int i = 0; i < 30; i++) {
            struct openipc_motor_status st;
            if (ioctl(kfd, OPENIPC_MOTOR_IOC_GET_STATUS, &st) == 0 && !st.moving)
                break;
            sleep_us(5000);
        }
        close(kfd);
    }
    if (ctx->pid > 0 && ctx->pid != (int)getpid()) {
        /* Wait up to 100ms for child process to exit naturally */
        for (int i = 0; i < 20; i++) {
            if (kill(ctx->pid, 0) != 0) break;
            sleep_us(5000);
        }
        /* If still alive, signal SIGTERM */
        if (kill(ctx->pid, 0) == 0) {
            (void)kill(ctx->pid, SIGTERM);
            for (int i = 0; i < 20; i++) {
                if (kill(ctx->pid, 0) != 0) break;
                sleep_us(5000);
            }
        }
        /* Final escalation to guarantee no orphaned process remains */
        if (kill(ctx->pid, 0) == 0) {
            (void)kill(ctx->pid, SIGKILL);
        }
    }
    /* Reload updated state after previous process exited */
    load_state(ctx);
    ctx->moving = 0;
    ctx->pid = 0;
    save_state(ctx);
}

/*
 * Single-axis user-space fallback runner with S-curve ramp and bounded slack backlash.
 */
static void run_userspace_axis(const int pins[4], int *phase, int *pos, int *slack,
                               int steps, int backlash_max, int is_tilt,
                               int t_start, int t_cruise, int homing_mode) {
    if (steps == 0 || g_stop_requested) return;

    int logical_dir = (steps > 0) ? 1 : -1;
    int motor_dir = is_tilt ? -logical_dir : logical_dir;

    if (*slack < 0) *slack = 0;
    if (*slack > backlash_max) *slack = backlash_max;

    int backlash_to_inject = 0;
    if (!homing_mode && backlash_max > 0) {
        backlash_to_inject = (logical_dir > 0) ? (backlash_max - *slack) : *slack;
    }

    int total_steps = abs(steps) + backlash_to_inject;
    int soft_zone_start = (total_steps * 85) / 100;
    int fds[4] = {-1, -1, -1, -1};
    gpio_open_pins(pins, fds);

    for (int i = 0; i < total_steps; i++) {
        if (g_stop_requested) {
            int rem = total_steps - i;
            if (rem > 16) total_steps = i + 16;
        }

        *phase = (*phase + motor_dir + 8) & 7;
        gpio_set_phase(fds, *phase);

        if (!homing_mode && backlash_max > 0) {
            if (logical_dir > 0) {
                if (*slack < backlash_max) (*slack)++;
                else (*pos)++;
            } else {
                if (*slack > 0) (*slack)--;
                else (*pos)--;
            }
        } else {
            *pos += logical_dir;
            if (homing_mode) *slack = (logical_dir > 0) ? backlash_max : 0;
        }

        int delay_us = compute_step_delay(i, total_steps, t_start, t_cruise, SCURVE_LEN);
        if (homing_mode && i >= soft_zone_start) {
            int soft_len = total_steps - soft_zone_start;
            int soft_idx = i - soft_zone_start;
            int soft_delay = t_cruise + ((T_SOFT_STOP_US - t_cruise) * soft_idx) / (soft_len > 0 ? soft_len : 1);
            if (soft_delay > delay_us) delay_us = soft_delay;
        }
        sleep_us(delay_us);
    }

    sleep_us(20000); /* 20ms rotor settling hold before coil release */
    gpio_deenergize_and_close(fds);
}

/*
 * Execute a Pan/Tilt move.
 * Automatically delegates to /dev/openipc-motor when loaded; otherwise uses
 * SCHED_FIFO user-space sequential S-curve stepping.
 */
static int execute_ramped_move(ptz_ctx_t *ctx, int d_pan, int d_tilt,
                               int t_cruise, int clamp_bounds, int homing_mode) {
    if (clamp_bounds && ctx->calibrated) {
        int target_pan = ctx->pan_pos + d_pan;
        int target_tilt = ctx->tilt_pos + d_tilt;
        if (target_pan < 0) target_pan = 0;
        if (target_pan > ctx->pan_span) target_pan = ctx->pan_span;
        if (target_tilt < 0) target_tilt = 0;
        if (target_tilt > ctx->tilt_span) target_tilt = ctx->tilt_span;
        d_pan = target_pan - ctx->pan_pos;
        d_tilt = target_tilt - ctx->tilt_pos;
    }

    if (d_pan == 0 && d_tilt == 0) {
        return 0;
    }

    if (t_cruise < 1500) t_cruise = T_CRUISE_US;

    g_stop_requested = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sig;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    ctx->moving = 1;
    ctx->pid = (int)getpid();
    save_state(ctx);

    /* Path 1: Delegate to in-kernel /dev/openipc-motor hrtimer driver if loaded */
    int kfd = open(KERNEL_MOTOR_DEV, O_RDWR);
    if (kfd >= 0) {
        struct openipc_motor_move m;
        struct openipc_motor_status st;
        memset(&m, 0, sizeof(m));
        m.pan_steps = d_pan;
        m.tilt_steps = d_tilt;
        m.delay_us = t_cruise;
        m.low_torque = homing_mode ? 1 : 0;

        (void)ioctl(kfd, OPENIPC_MOTOR_IOC_MOVE, &m);
        if (ioctl(kfd, OPENIPC_MOTOR_IOC_GET_STATUS, &st) == 0) {
            ctx->pan_pos = st.pan_pos;
            ctx->tilt_pos = st.tilt_pos;
        }
        close(kfd);

        ctx->moving = 0;
        ctx->pid = 0;
        save_state(ctx);
        return 0;
    }

    /* Path 2: User-space fallback with real-time SCHED_FIFO priority */
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = 50;
    (void)sched_setscheduler(0, SCHED_FIFO, &sp);

    int t_start = (t_cruise > T_START_US) ? t_cruise : T_START_US;

    /* Move sequentially (Tilt then Pan) to avoid 5V rail sag */
    if (d_tilt != 0) {
        run_userspace_axis(ctx->tilt_pins, &ctx->tilt_phase, &ctx->tilt_pos, &ctx->tilt_slack,
                           d_tilt, TILT_BACKLASH, 1, t_start, t_cruise, homing_mode);
    }
    if (d_pan != 0 && !g_stop_requested) {
        run_userspace_axis(ctx->pan_pins, &ctx->pan_phase, &ctx->pan_pos, &ctx->pan_slack,
                           d_pan, PAN_BACKLASH, 0, t_start, t_cruise, homing_mode);
    }

    if (ctx->calibrated) {
        if (ctx->pan_pos < 0) ctx->pan_pos = 0;
        if (ctx->pan_pos > ctx->pan_span) ctx->pan_pos = ctx->pan_span;
        if (ctx->tilt_pos < 0) ctx->tilt_pos = 0;
        if (ctx->tilt_pos > ctx->tilt_span) ctx->tilt_pos = ctx->tilt_span;
    }

    ctx->moving = 0;
    ctx->pid = 0;
    save_state(ctx);
    return 0;
}

static void sync_kernel_pos(int pan_pos, int tilt_pos) {
    int kfd = open(KERNEL_MOTOR_DEV, O_RDWR);
    if (kfd >= 0) {
        struct openipc_motor_set_pos sp;
        sp.pan_pos = pan_pos;
        sp.tilt_pos = tilt_pos;
        (void)ioctl(kfd, OPENIPC_MOTOR_IOC_SET_POS, &sp);
        close(kfd);
    }
}

static int cmd_calibrate(ptz_ctx_t *ctx) {
    stop_active_motion(ctx);
    ctx->calibrated = 0;
    save_state(ctx);

    /*
     * Sequential Homing to Prevent Brownouts:
     * Phase 1A: Soft-homing sweep Tilt to negative mechanical stop alone.
     */
    int sweep_tilt = -(ctx->tilt_span + 350); /* -2398 half-steps */
    execute_ramped_move(ctx, 0, sweep_tilt, 4000, 0, 1);
    if (g_stop_requested) return 1;
    sleep_us(100000);
    if (g_stop_requested) return 1;

    /*
     * Phase 1B: Soft-homing sweep Pan to negative mechanical stop alone.
     */
    int sweep_pan = -(ctx->pan_span + 300);   /* -4200 half-steps */
    execute_ramped_move(ctx, sweep_pan, 0, 3500, 0, 1);
    if (g_stop_requested) return 1;
    sleep_us(100000);
    if (g_stop_requested) return 1;

    /* At mechanical stop origin, reset coordinates to (0, 0) in both kernel and userspace */
    ctx->pan_pos = 0;
    ctx->tilt_pos = 0;
    ctx->pan_slack = 0;
    ctx->tilt_slack = 0;
    ctx->calibrated = 1;
    sync_kernel_pos(0, 0);
    save_state(ctx);
    sleep_us(150000);
    if (g_stop_requested) return 1;

    /* Determine home target (Preset 1 if defined, otherwise center (1950, 1024)) */
    int home_pan = ctx->pan_span / 2;
    int home_tilt = ctx->tilt_span / 2;
    if (ctx->presets[0].used && ctx->presets[0].pan >= 0 && ctx->presets[0].tilt >= 0) {
        home_pan = ctx->presets[0].pan;
        home_tilt = ctx->presets[0].tilt;
    }

    /* Phase 2: Sequential smooth move from (0, 0) to Home/Center */
    execute_ramped_move(ctx, home_pan, 0, T_CRUISE_US, 1, 0);
    if (g_stop_requested) return 1;
    sleep_us(100000);
    if (g_stop_requested) return 1;

    execute_ramped_move(ctx, 0, home_tilt, T_CRUISE_US, 1, 0);
    if (g_stop_requested) return 1;

    ctx->pan_pos = home_pan;
    ctx->tilt_pos = home_tilt;
    ctx->calibrated = 1;
    save_state(ctx);
    save_presets_json(ctx);
    printf("{\"status\":\"ok\",\"action\":\"calibrate\",\"pan_pos\":%d,\"tilt_pos\":%d,\"homed\":true}\n",
           ctx->pan_pos, ctx->tilt_pos);
    return 0;
}

static const ptz_preset_t *find_preset(const ptz_ctx_t *ctx, const char *token_or_name) {
    if (!token_or_name || !*token_or_name) return NULL;

    /* Check if token has prefix like "PresetToken_1" or "preset_1" or numeric "1" */
    const char *digits = token_or_name;
    while (*digits && !isdigit((unsigned char)*digits)) digits++;
    if (*digits) {
        int id = atoi(digits);
        if (id >= 1 && id <= MAX_PRESETS && ctx->presets[id - 1].used) {
            return &ctx->presets[id - 1];
        }
    }
    /* Match by preset name (case-insensitive) */
    for (int i = 0; i < MAX_PRESETS; i++) {
        if (ctx->presets[i].used && strcasecmp(ctx->presets[i].name, token_or_name) == 0) {
            return &ctx->presets[i];
        }
    }
    return NULL;
}

static int cmd_goto(ptz_ctx_t *ctx, const char *target) {
    stop_active_motion(ctx);
    const ptz_preset_t *pr = find_preset(ctx, target);
    if (!pr) {
        if (strcasecmp(target, "home") == 0 || strcmp(target, "0") == 0 || strcmp(target, "1") == 0) {
            int d_pan = (ctx->pan_span / 2) - ctx->pan_pos;
            int d_tilt = (ctx->tilt_span / 2) - ctx->tilt_pos;
            execute_ramped_move(ctx, d_pan, d_tilt, T_CRUISE_US, 1, 0);
            printf("{\"status\":\"ok\",\"action\":\"goto\",\"preset\":\"home\",\"pan_pos\":%d,\"tilt_pos\":%d}\n",
                   ctx->pan_pos, ctx->tilt_pos);
            return 0;
        }
        fprintf(stderr, "Preset '%s' not found\n", target);
        return 1;
    }

    int d_pan = pr->pan - ctx->pan_pos;
    int d_tilt = pr->tilt - ctx->tilt_pos;
    execute_ramped_move(ctx, d_pan, d_tilt, T_CRUISE_US, 1, 0);
    printf("{\"status\":\"ok\",\"action\":\"goto\",\"preset_id\":%d,\"preset_name\":\"%s\",\"pan_pos\":%d,\"tilt_pos\":%d}\n",
           pr->id, pr->name, ctx->pan_pos, ctx->tilt_pos);
    return 0;
}

static int cmd_set_preset(ptz_ctx_t *ctx, const char *id_str, const char *name,
                          const char *pan_str, const char *tilt_str) {
    int id = 0;
    if (id_str && *id_str) {
        const char *d = id_str;
        while (*d && !isdigit((unsigned char)*d)) d++;
        if (*d) id = atoi(d);
    }
    if (id < 1 || id > MAX_PRESETS) {
        if (name && *name) {
            for (int i = 0; i < MAX_PRESETS; i++) {
                if (ctx->presets[i].used && strcasecmp(ctx->presets[i].name, name) == 0) {
                    id = i + 1;
                    break;
                }
            }
        }
        if (id < 1 || id > MAX_PRESETS) {
            for (int i = 0; i < MAX_PRESETS; i++) {
                if (!ctx->presets[i].used) {
                    id = i + 1;
                    break;
                }
            }
        }
        if (id < 1 || id > MAX_PRESETS) id = 1;
    }

    int idx = id - 1;
    ctx->presets[idx].id = id;
    ctx->presets[idx].used = 1;
    if (name && *name) {
        snprintf(ctx->presets[idx].name, sizeof(ctx->presets[idx].name), "%s", name);
    } else if (!ctx->presets[idx].name[0]) {
        snprintf(ctx->presets[idx].name, sizeof(ctx->presets[idx].name), "preset_%d", id);
    }
    ctx->presets[idx].pan = (pan_str && *pan_str) ? atoi(pan_str) : ctx->pan_pos;
    ctx->presets[idx].tilt = (tilt_str && *tilt_str) ? atoi(tilt_str) : ctx->tilt_pos;

    save_presets_json(ctx);
    printf("{\"status\":\"ok\",\"action\":\"set\",\"preset_id\":%d,\"preset_name\":\"%s\",\"pan\":%d,\"tilt\":%d}\n",
           id, ctx->presets[idx].name, ctx->presets[idx].pan, ctx->presets[idx].tilt);
    return 0;
}

static int cmd_remove_preset(ptz_ctx_t *ctx, const char *id_str) {
    if (!id_str || !*id_str) return 1;
    const char *d = id_str;
    while (*d && !isdigit((unsigned char)*d)) d++;
    int id = (*d) ? atoi(d) : 0;
    if (id >= 1 && id <= MAX_PRESETS) {
        ctx->presets[id - 1].used = 0;
        save_presets_json(ctx);
        printf("{\"status\":\"ok\",\"action\":\"remove\",\"preset_id\":%d}\n", id);
        return 0;
    }
    return 1;
}

static void print_status_json(const ptz_ctx_t *ctx) {
    double pan_norm = 0.0;
    double tilt_norm = 0.0;
    if (ctx->pan_span > 0) {
        pan_norm = ((double)ctx->pan_pos - (ctx->pan_span / 2.0)) / (ctx->pan_span / 2.0);
    }
    if (ctx->tilt_span > 0) {
        tilt_norm = ((double)ctx->tilt_pos - (ctx->tilt_span / 2.0)) / (ctx->tilt_span / 2.0);
    }
    printf("{\"homed\":%s,\"calibrated\":%d,\"moving\":%d,\"pan_pos\":%d,\"tilt_pos\":%d,"
           "\"pan_span\":%d,\"tilt_span\":%d,\"pan_norm\":%.4f,\"tilt_norm\":%.4f}\n",
           ctx->calibrated ? "true" : "false",
           ctx->calibrated, ctx->moving,
           ctx->pan_pos, ctx->tilt_pos,
           ctx->pan_span, ctx->tilt_span,
           pan_norm, tilt_norm);
}

static void print_presets_json(const ptz_ctx_t *ctx) {
    printf("{\"homed\":%s,\"pan_pos\":%d,\"tilt_pos\":%d,\"pan_span\":%d,\"tilt_span\":%d,\"presets\":{",
           ctx->calibrated ? "true" : "false",
           ctx->pan_pos, ctx->tilt_pos,
           ctx->pan_span, ctx->tilt_span);
    int first = 1;
    for (int i = 0; i < MAX_PRESETS; i++) {
        if (!ctx->presets[i].used) continue;
        if (!first) printf(",");
        printf("\"%d\":{\"name\":\"%s\",\"pan\":%d,\"tilt\":%d}",
               ctx->presets[i].id, ctx->presets[i].name,
               ctx->presets[i].pan, ctx->presets[i].tilt);
        first = 0;
    }
    printf("}}\n");
}

int main(int argc, char **argv) {
    ptz_ctx_t ctx;
    load_state(&ctx);

    const char *prog = strrchr(argv[0], '/');
    prog = prog ? (prog + 1) : argv[0];

    /* Drop-in compatibility when invoked as `gpio-motors` */
    if (strcmp(prog, "gpio-motors") == 0) {
        if (argc == 2 && strcmp(argv[1], "status") == 0) {
            print_status_json(&ctx);
            return 0;
        }
        if (argc < 3) {
            fprintf(stderr, "Usage: gpio-motors <pan_cycles> <tilt_cycles> [delay_ms]\n");
            return 1;
        }
        int pan_cycles = atoi(argv[1]);
        int tilt_cycles = atoi(argv[2]);
        /* 1 cycle in legacy gpio-motors = 8 half-steps */
        int d_pan = pan_cycles * 8;
        int d_tilt = tilt_cycles * 8;
        stop_active_motion(&ctx);
        return execute_ramped_move(&ctx, d_pan, d_tilt, T_CRUISE_US, 1, 0);
    }

    if (argc < 2) {
        fprintf(stderr,
                "Usage: ptz-controller <command> [args...]\n"
                "Commands:\n"
                "  step <pan_steps> <tilt_steps> [cruise_us]  Ramped relative step move\n"
                "  abs <pan_pos> <tilt_pos>                   Ramped absolute coordinate move\n"
                "  move <left|right|up|down> [speed_1_100]    Continuous move until stop/limit\n"
                "  stop                                       Smoothly stop active motion\n"
                "  goto <preset_id_or_name>                   Move to saved preset\n"
                "  set <preset_id> [name] [pan] [tilt]        Save current or given position as preset\n"
                "  remove <preset_id>                         Delete a preset\n"
                "  presets                                    Output JSON preset table\n"
                "  calibrate                                  Soft-homing calibration to center\n"
                "  boot                                       Silent boot coordinate restore\n"
                "  status                                     Output JSON PTZ coordinates & state\n");
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "status") == 0) {
        print_status_json(&ctx);
        return 0;
    }
    if (strcmp(cmd, "presets") == 0) {
        print_presets_json(&ctx);
        return 0;
    }
    if (strcmp(cmd, "stop") == 0) {
        stop_active_motion(&ctx);
        return 0;
    }
    if (strcmp(cmd, "boot") == 0) {
        /* Silent boot restore: only sweep if uncalibrated or presets file missing */
        if (access(PRESETS_FILE, F_OK) != 0) {
            save_presets_json(&ctx);
        }
        if (!ctx.calibrated) {
            return cmd_calibrate(&ctx);
        }
        sync_kernel_pos(ctx.pan_pos, ctx.tilt_pos);
        ctx.moving = 0;
        ctx.pid = 0;
        save_state(&ctx);
        return 0;
    }
    if (strcmp(cmd, "calibrate") == 0 || strcmp(cmd, "recalibrate") == 0) {
        return cmd_calibrate(&ctx);
    }
    if (strcmp(cmd, "home") == 0) {
        return cmd_goto(&ctx, "1");
    }
    if (strcmp(cmd, "goto") == 0) {
        const char *target = (argc >= 3) ? argv[2] : "1";
        return cmd_goto(&ctx, target);
    }
    if (strcmp(cmd, "set") == 0) {
        const char *id_str = (argc >= 3) ? argv[2] : "1";
        const char *name = (argc >= 4) ? argv[3] : NULL;
        const char *pan_s = (argc >= 5) ? argv[4] : NULL;
        const char *tilt_s = (argc >= 6) ? argv[5] : NULL;
        return cmd_set_preset(&ctx, id_str, name, pan_s, tilt_s);
    }
    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) return 1;
        return cmd_remove_preset(&ctx, argv[2]);
    }
    if (strcmp(cmd, "step") == 0) {
        if (argc < 4) return 1;
        int d_pan = atoi(argv[2]);
        int d_tilt = atoi(argv[3]);
        int cruise = (argc >= 5) ? atoi(argv[4]) : T_CRUISE_US;
        stop_active_motion(&ctx);
        int rc = execute_ramped_move(&ctx, d_pan, d_tilt, cruise, 1, 0);
        printf("{\"status\":\"ok\",\"action\":\"step\",\"pan_pos\":%d,\"tilt_pos\":%d}\n", ctx.pan_pos, ctx.tilt_pos);
        return rc;
    }
    if (strcmp(cmd, "abs") == 0) {
        if (argc < 4) return 1;
        int target_pan = (strcmp(argv[2], "c") == 0 || strcmp(argv[2], "-1") == 0) ? ctx.pan_pos : atoi(argv[2]);
        int target_tilt = (strcmp(argv[3], "c") == 0 || strcmp(argv[3], "-1") == 0) ? ctx.tilt_pos : atoi(argv[3]);
        int cruise = (argc >= 5) ? atoi(argv[4]) : T_CRUISE_US;
        stop_active_motion(&ctx);
        int rc = execute_ramped_move(&ctx, target_pan - ctx.pan_pos, target_tilt - ctx.tilt_pos, cruise, 1, 0);
        printf("{\"status\":\"ok\",\"action\":\"abs\",\"pan_pos\":%d,\"tilt_pos\":%d}\n", ctx.pan_pos, ctx.tilt_pos);
        return rc;
    }
    if (strcmp(cmd, "move") == 0) {
        if (argc < 3) return 1;
        const char *dir = argv[2];
        int speed = (argc >= 4) ? atoi(argv[3]) : 80;
        if (speed < 1) speed = 1;
        if (speed > 100) speed = 100;
        /* Map speed 1..100 to cruise delay 8000..2500 us */
        int cruise_us = 8000 - ((speed - 1) * (8000 - T_CRUISE_US)) / 99;

        int d_pan = 0, d_tilt = 0;
        if (strstr(dir, "left"))  d_pan = -ctx.pan_span;
        if (strstr(dir, "right")) d_pan =  ctx.pan_span;
        if (strstr(dir, "up"))    d_tilt =  ctx.tilt_span;
        if (strstr(dir, "down"))  d_tilt = -ctx.tilt_span;

        stop_active_motion(&ctx);
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > 2) close(devnull);
            }
            execute_ramped_move(&ctx, d_pan, d_tilt, cruise_us, 1, 0);
            _exit(0);
        }
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    return 1;
}
