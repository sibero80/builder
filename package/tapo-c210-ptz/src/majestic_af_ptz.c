/*
 * majestic_af_ptz.c — OpenIPC Majestic AF/PTZ Plugin for Tapo C210 v2 / C220
 * Target: /usr/lib/majestic-af.so
 *
 * Implements the official OpenIPC/majestic-af plugin ABI (include/majestic/af_plugin_abi.h):
 *   - const char *af_plugin_call(const char *cmd, const char *val);
 *   - void af_plugin_exit(void);
 *
 * Delegates all physical stepper motion, trapezoidal speed ramping, soft-limit
 * clamping, and persistent coordinate tracking to /usr/sbin/ptz-controller.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char g_ptz_reply[64];

static void run_ptz_cmd_async(const char *arg1, const char *arg2, const char *arg3, const char *arg4) {
    pid_t pid = fork();
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 == 0) {
            close(STDIN_FILENO);
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            execl("/usr/sbin/ptz-controller", "ptz-controller", arg1, arg2, arg3, arg4, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

static void run_ptz_cmd_sync(const char *arg1, const char *arg2, const char *arg3, const char *arg4) {
    pid_t pid = fork();
    if (pid == 0) {
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        execl("/usr/sbin/ptz-controller", "ptz-controller", arg1, arg2, arg3, arg4, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

static int is_valid_pantilt_verb(const char *v) {
    return strcmp(v, "up") == 0 ||
           strcmp(v, "down") == 0 ||
           strcmp(v, "left") == 0 ||
           strcmp(v, "right") == 0 ||
           strcmp(v, "upleft") == 0 ||
           strcmp(v, "upright") == 0 ||
           strcmp(v, "downleft") == 0 ||
           strcmp(v, "downright") == 0;
}

/*
 * Parse "<verb>[:<ms>[:<speed>]]" or query-string style "move=<verb>&speed=<speed>"
 */
static int parse_ptz_val(const char *val, char *verb_out, size_t verb_sz, int *ms_out, int *speed_out) {
    if (!val || !*val) return 0;
    *ms_out = 500;
    *speed_out = 80;

    const char *m = strstr(val, "move=");
    if (m) {
        m += 5;
        size_t i = 0;
        while (m[i] && m[i] != '&' && !isspace((unsigned char)m[i]) && i < verb_sz - 1) {
            verb_out[i] = m[i];
            i++;
        }
        verb_out[i] = '\0';
        const char *s = strstr(val, "speed=");
        if (s) {
            int sp = atoi(s + 6);
            if (sp >= 1 && sp <= 100) *speed_out = sp;
        }
        return verb_out[0] != '\0';
    }

    const char *colon1 = strchr(val, ':');
    if (!colon1) {
        snprintf(verb_out, verb_sz, "%s", val);
        return verb_out[0] != '\0';
    }

    size_t vlen = (size_t)(colon1 - val);
    if (vlen == 0 || vlen >= verb_sz) return 0;
    memcpy(verb_out, val, vlen);
    verb_out[vlen] = '\0';

    const char *p = colon1 + 1;
    char *endp = NULL;
    long ms = strtol(p, &endp, 10);
    if (endp == p || ms < 0) return 0;
    if (ms > 0) *ms_out = (int)ms;

    if (*endp == ':') {
        const char *p2 = endp + 1;
        char *endp2 = NULL;
        long sp = strtol(p2, &endp2, 10);
        if (endp2 == p2 || *endp2 != '\0' || sp < 1 || sp > 100) return 0;
        *speed_out = (int)sp;
    } else if (*endp != '\0') {
        return 0;
    }

    return 1;
}

/*
 * Parse "<verb>:<steps>[:<speed>]" for cmd = "ptzstep"
 */
static int parse_ptzstep_val(const char *val, char *verb_out, size_t verb_sz, int *steps_out, int *speed_out) {
    if (!val || !*val) return 0;
    *steps_out = 0;
    *speed_out = 80;

    const char *colon1 = strchr(val, ':');
    if (!colon1) return 0;

    size_t vlen = (size_t)(colon1 - val);
    if (vlen == 0 || vlen >= verb_sz) return 0;
    memcpy(verb_out, val, vlen);
    verb_out[vlen] = '\0';

    const char *p = colon1 + 1;
    char *endp = NULL;
    long st = strtol(p, &endp, 10);
    if (endp == p || st <= 0 || st > 2000) return 0;
    *steps_out = (int)st;

    if (*endp == ':') {
        const char *p2 = endp + 1;
        char *endp2 = NULL;
        long sp = strtol(p2, &endp2, 10);
        if (endp2 == p2 || *endp2 != '\0' || sp < 1 || sp > 100) return 0;
        *speed_out = (int)sp;
    } else if (*endp != '\0') {
        return 0;
    }

    return is_valid_pantilt_verb(verb_out);
}

__attribute__((visibility("default")))
const char *af_plugin_call(const char *cmd, const char *val) {
    if (!cmd || !val) {
        return NULL;
    }

    /* Config schema query: returning NULL keeps Majestic's built-in schema */
    if (strcmp(cmd, "schema") == 0) {
        return NULL;
    }

    /* Fixed-focus lens on Tapo C210 v2 / C220: report autofocus unavailable */
    if (strcmp(cmd, "autofocus") == 0) {
        if (strcmp(val, "status") == 0 || strcmp(val, "run") == 0 || strcmp(val, "settle") == 0) {
            return "unavailable";
        }
        return NULL;
    }

    /* Fixed-zoom lens: report zoom unavailable except stop */
    if (strcmp(cmd, "zoom") == 0) {
        if (strcmp(val, "stop") == 0) {
            return "stopped";
        }
        if (strcmp(val, "tele") == 0 || strcmp(val, "wide") == 0) {
            return "unavailable";
        }
        return NULL;
    }

    /* Counted relative step move: "<verb>:<steps>[:<speed>]" */
    if (strcmp(cmd, "ptzstep") == 0) {
        char verb[32] = {0};
        int steps = 0, speed = 80;
        if (!parse_ptzstep_val(val, verb, sizeof(verb), &steps, &speed)) {
            return NULL;
        }
        int half_steps = steps * 8;
        int d_pan = 0, d_tilt = 0;
        if (strstr(verb, "left"))  d_pan = -half_steps;
        if (strstr(verb, "right")) d_pan =  half_steps;
        if (strstr(verb, "up"))    d_tilt =  half_steps;
        if (strstr(verb, "down"))  d_tilt = -half_steps;

        int cruise_us = 8000 - ((speed - 1) * (8000 - 2500)) / 99;
        char pan_s[16], tilt_s[16], cruise_s[16];
        snprintf(pan_s, sizeof(pan_s), "%d", d_pan);
        snprintf(tilt_s, sizeof(tilt_s), "%d", d_tilt);
        snprintf(cruise_s, sizeof(cruise_s), "%d", cruise_us);

        run_ptz_cmd_async("step", pan_s, tilt_s, cruise_s);
        snprintf(g_ptz_reply, sizeof(g_ptz_reply), "moving %s", verb);
        return g_ptz_reply;
    }

    /* Continuous/pulsed PTZ command or capability line */
    if (strcmp(cmd, "ptz") == 0) {
        if (val[0] == '\0') {
            return "actuator=gpiostep pulse=500 state=ready "
                   "verbs=up,down,left,right,upleft,upright,downleft,downright,stop "
                   "speeds=1-100 steps=1";
        }

        char verb[32] = {0};
        int ms = 500, speed = 80;
        if (!parse_ptz_val(val, verb, sizeof(verb), &ms, &speed)) {
            return NULL;
        }

        if (strcmp(verb, "stop") == 0) {
            run_ptz_cmd_sync("stop", NULL, NULL, NULL);
            return "stopped";
        }
        if (strcmp(verb, "home") == 0 || strcmp(verb, "center") == 0) {
            run_ptz_cmd_async("goto", "1", NULL, NULL);
            return "moving home";
        }
        if (strcmp(verb, "calibrate") == 0) {
            run_ptz_cmd_async("calibrate", NULL, NULL, NULL);
            return "moving calibrate";
        }
        if (is_valid_pantilt_verb(verb)) {
            char speed_s[16];
            snprintf(speed_s, sizeof(speed_s), "%d", speed);
            run_ptz_cmd_sync("move", verb, speed_s, NULL);
            snprintf(g_ptz_reply, sizeof(g_ptz_reply), "moving %s", verb);
            return g_ptz_reply;
        }

        return NULL;
    }

    return NULL;
}

__attribute__((visibility("default")))
void af_plugin_exit(void) {
    run_ptz_cmd_sync("stop", NULL, NULL, NULL);
}
