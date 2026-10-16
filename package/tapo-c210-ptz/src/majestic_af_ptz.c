/*
 * majestic_af_ptz.c — OpenIPC Majestic PTZ Plugin & Native Port-80 ONVIF Preset Extension
 * Target: /usr/lib/majestic-af.so
 *
 * Implements the Majestic AF/PTZ plugin ABI (`af_plugin_call`, `af_plugin_exit`)
 * so Majestic natively enables its Port-80 ONVIF PTZ service, AND extends Majestic's
 * Port-80 `/onvif/ptz_service` SOAP dispatcher with full ONVIF Preset & Move actions:
 *   - GetPresets
 *   - GotoPreset
 *   - SetPreset
 *   - RemovePreset
 *   - GotoHomePosition
 *   - SetHomePosition
 *   - RelativeMove
 *   - AbsoluteMove
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PRESETS_FILE "/etc/ptz_presets.json"
#define STATE_FILE   "/tmp/ptz_state"

typedef int (*mxml_save_string_fn)(void *node, void *cb, char *buf, int bufsize);

static uintptr_t g_majestic_base = 0;
static mxml_save_string_fn g_mxml_save_string = NULL;

static void run_ptz_cmd_async(const char *arg1, const char *arg2, const char *arg3) {
    pid_t pid = fork();
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 == 0) {
            close(STDIN_FILENO);
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            execl("/usr/sbin/ptz-controller", "ptz-controller", arg1, arg2, arg3, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

static void run_ptz_cmd_sync(const char *arg1, const char *arg2, const char *arg3) {
    pid_t pid = fork();
    if (pid == 0) {
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        execl("/usr/sbin/ptz-controller", "ptz-controller", arg1, arg2, arg3, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

/* Extract XML element text, ignoring namespace prefix (e.g. <tptz:PresetToken>1</tptz:PresetToken>) */
static int extract_xml_tag(const char *xml, const char *tag, char *out, size_t out_sz) {
    if (!xml || !tag || !out || out_sz == 0) return 0;
    out[0] = '\0';
    size_t tag_len = strlen(tag);
    const char *p = xml;
    while ((p = strstr(p, tag)) != NULL) {
        if (p > xml && (p[-1] == '<' || p[-1] == ':')) {
            const char *gt = strchr(p + tag_len, '>');
            if (gt && gt[-1] != '/') {
                const char *val_start = gt + 1;
                const char *lt = strchr(val_start, '<');
                if (lt && lt >= val_start) {
                    size_t len = (size_t)(lt - val_start);
                    while (len > 0 && isspace((unsigned char)*val_start)) {
                        val_start++;
                        len--;
                    }
                    while (len > 0 && isspace((unsigned char)val_start[len - 1])) {
                        len--;
                    }
                    if (len >= out_sz) len = out_sz - 1;
                    memcpy(out, val_start, len);
                    out[len] = '\0';
                    return 1;
                }
            }
        }
        p += tag_len;
    }
    return 0;
}

/* Extract x="..." and y="..." from <PanTilt x="..." y="..."/> */
static int extract_pantilt_xy(const char *xml, double *x_out, double *y_out) {
    if (!xml) return 0;
    const char *p = strstr(xml, "PanTilt");
    if (!p) return 0;
    const char *gt = strchr(p, '>');
    if (!gt) return 0;

    char tag_buf[256];
    size_t len = (size_t)(gt - p);
    if (len >= sizeof(tag_buf)) len = sizeof(tag_buf) - 1;
    memcpy(tag_buf, p, len);
    tag_buf[len] = '\0';

    const char *xp = strstr(tag_buf, "x=\"");
    const char *yp = strstr(tag_buf, "y=\"");
    if (!xp && !yp) return 0;
    if (xp && x_out) *x_out = atof(xp + 3);
    if (yp && y_out) *y_out = atof(yp + 3);
    return 1;
}

static int build_get_presets_soap(char *resp_buf, size_t resp_sz) {
    char json[4096] = {0};
    FILE *f = fopen(PRESETS_FILE, "r");
    if (f) {
        size_t n = fread(json, 1, sizeof(json) - 1, f);
        json[n] = '\0';
        fclose(f);
    }

    int pan_span = 2000, tilt_span = 400;
    char *ps = strstr(json, "\"pan_span\"");
    if (ps) {
        char *c = strchr(ps, ':');
        if (c && atoi(c + 1) > 0) pan_span = atoi(c + 1);
    }
    char *ts = strstr(json, "\"tilt_span\"");
    if (ts) {
        char *c = strchr(ts, ':');
        if (c && atoi(c + 1) > 0) tilt_span = atoi(c + 1);
    }

    int off = snprintf(resp_buf, resp_sz,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
        "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<s:Body><tptz:GetPresetsResponse>");

    char *presets_sec = strstr(json, "\"presets\"");
    int count = 0;
    if (presets_sec) {
        char *cur = strchr(presets_sec, '{');
        if (cur) cur++;
        while (cur && *cur) {
            char *q1 = strchr(cur, '"');
            if (!q1) break;
            char *q2 = strchr(q1 + 1, '"');
            if (!q2) break;
            int id = atoi(q1 + 1);
            char *obj_s = strchr(q2, '{');
            if (!obj_s) break;
            char *obj_e = strchr(obj_s, '}');
            if (!obj_e) break;

            if (id >= 1 && id <= 16) {
                char obj[256];
                size_t olen = (size_t)(obj_e - obj_s + 1);
                if (olen >= sizeof(obj)) olen = sizeof(obj) - 1;
                memcpy(obj, obj_s, olen);
                obj[olen] = '\0';

                char name[32];
                snprintf(name, sizeof(name), "preset_%d", id);
                int pan = pan_span / 2;
                int tilt = tilt_span / 2;

                char *np = strstr(obj, "\"name\"");
                if (np) {
                    char *nq1 = strchr(np + 6, '"');
                    if (nq1) {
                        char *nq2 = strchr(nq1 + 1, '"');
                        if (nq2) {
                            size_t nlen = (size_t)(nq2 - (nq1 + 1));
                            if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
                            memcpy(name, nq1 + 1, nlen);
                            name[nlen] = '\0';
                        }
                    }
                }
                char *pp = strstr(obj, "\"pan\"");
                if (pp) {
                    char *c = strchr(pp, ':');
                    if (c) pan = atoi(c + 1);
                }
                char *tp = strstr(obj, "\"tilt\"");
                if (tp) {
                    char *c = strchr(tp, ':');
                    if (c) tilt = atoi(c + 1);
                }

                double x = ((double)pan - (pan_span / 2.0)) / (pan_span / 2.0);
                double y = ((double)tilt - (tilt_span / 2.0)) / (tilt_span / 2.0);

                off += snprintf(resp_buf + off, resp_sz - (size_t)off,
                    "<tptz:Preset token=\"%d\">"
                    "<tt:Name>%s</tt:Name>"
                    "<tt:PTZPosition>"
                    "<tt:PanTilt x=\"%.4f\" y=\"%.4f\" space=\"http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace\"/>"
                    "</tt:PTZPosition>"
                    "</tptz:Preset>",
                    id, name, x, y);
                count++;
            }
            cur = obj_e + 1;
        }
    }

    if (count == 0) {
        off += snprintf(resp_buf + off, resp_sz - (size_t)off,
            "<tptz:Preset token=\"1\"><tt:Name>home</tt:Name>"
            "<tt:PTZPosition><tt:PanTilt x=\"0.0000\" y=\"0.0000\"/></tt:PTZPosition></tptz:Preset>");
    }

    snprintf(resp_buf + off, resp_sz - (size_t)off, "</tptz:GetPresetsResponse></s:Body></s:Envelope>");
    return 1;
}

/*
 * Hook invoked when Majestic's Port-80 ONVIF PTZ service receives GetPresets,
 * GotoPreset, SetPreset, RemovePreset, GotoHomePosition, SetHomePosition,
 * RelativeMove, AbsoluteMove, or an unknown action.
 */
static int on_ptz_soap_action(char *resp_buf, int *http_status, char *req_ctx) {
    const char *action = req_ctx ? req_ctx : "";

    /* Note: req_ctx is only a full context struct when called from ptz_dispatch (0x5bf2c).
       Check if action matches one of our extended PTZ SOAP actions. */
    if (strcmp(action, "GetPresets") == 0) {
        *http_status = 200;
        return build_get_presets_soap(resp_buf, 65536);
    }

    char xml_buf[2048] = {0};
    if (strcmp(action, "GotoPreset") == 0 ||
        strcmp(action, "SetPreset") == 0 ||
        strcmp(action, "RemovePreset") == 0 ||
        strcmp(action, "GotoHomePosition") == 0 ||
        strcmp(action, "SetHomePosition") == 0 ||
        strcmp(action, "RelativeMove") == 0 ||
        strcmp(action, "AbsoluteMove") == 0) {

        void *req_xml = *(void **)(req_ctx + 388);
        if (req_xml && g_mxml_save_string) {
            g_mxml_save_string(req_xml, NULL, xml_buf, sizeof(xml_buf) - 1);
        }

        if (strcmp(action, "GotoPreset") == 0) {
            char token[64] = "1";
            extract_xml_tag(xml_buf, "PresetToken", token, sizeof(token));
            run_ptz_cmd_async("goto", token, NULL);
            *http_status = 200;
            snprintf(resp_buf, 65536,
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
                "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
                "<s:Body><tptz:GotoPresetResponse/></s:Body></s:Envelope>");
            return 1;
        }

        if (strcmp(action, "SetPreset") == 0) {
            char token[64] = "";
            char name[64] = "";
            extract_xml_tag(xml_buf, "PresetToken", token, sizeof(token));
            extract_xml_tag(xml_buf, "PresetName", name, sizeof(name));
            if (!token[0]) {
                if (name[0] && isdigit((unsigned char)name[0])) {
                    snprintf(token, sizeof(token), "%d", atoi(name));
                } else {
                    strcpy(token, "1");
                }
            }
            if (!name[0]) {
                snprintf(name, sizeof(name), "preset_%.50s", token);
            }
            run_ptz_cmd_sync("set", token, name);
            const char *digits = token;
            while (*digits && !isdigit((unsigned char)*digits)) digits++;
            int id = (*digits) ? atoi(digits) : 1;
            *http_status = 200;
            snprintf(resp_buf, 65536,
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
                "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
                "<s:Body><tptz:SetPresetResponse><tptz:PresetToken>%d</tptz:PresetToken></tptz:SetPresetResponse></s:Body></s:Envelope>",
                id);
            return 1;
        }

        if (strcmp(action, "RemovePreset") == 0) {
            char token[64] = "1";
            extract_xml_tag(xml_buf, "PresetToken", token, sizeof(token));
            run_ptz_cmd_sync("remove", token, NULL);
            *http_status = 200;
            snprintf(resp_buf, 65536,
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
                "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
                "<s:Body><tptz:RemovePresetResponse/></s:Body></s:Envelope>");
            return 1;
        }

        if (strcmp(action, "GotoHomePosition") == 0) {
            run_ptz_cmd_async("goto", "1", NULL);
            *http_status = 200;
            snprintf(resp_buf, 65536,
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
                "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
                "<s:Body><tptz:GotoHomePositionResponse/></s:Body></s:Envelope>");
            return 1;
        }

        if (strcmp(action, "SetHomePosition") == 0) {
            run_ptz_cmd_sync("set", "1", "home");
            *http_status = 200;
            snprintf(resp_buf, 65536,
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
                "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
                "<s:Body><tptz:SetHomePositionResponse/></s:Body></s:Envelope>");
            return 1;
        }

        if (strcmp(action, "RelativeMove") == 0) {
            double x = 0.0, y = 0.0;
            extract_pantilt_xy(xml_buf, &x, &y);
            int d_pan = (int)(x * 1000.0);
            int d_tilt = (int)(y * 200.0);
            char pan_s[32], tilt_s[32];
            snprintf(pan_s, sizeof(pan_s), "%d", d_pan);
            snprintf(tilt_s, sizeof(tilt_s), "%d", d_tilt);
            run_ptz_cmd_async("step", pan_s, tilt_s);
            *http_status = 200;
            snprintf(resp_buf, 65536,
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
                "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
                "<s:Body><tptz:RelativeMoveResponse/></s:Body></s:Envelope>");
            return 1;
        }

        if (strcmp(action, "AbsoluteMove") == 0) {
            double x = 0.0, y = 0.0;
            extract_pantilt_xy(xml_buf, &x, &y);
            int target_pan = 1000 + (int)(x * 1000.0);
            int target_tilt = 200 + (int)(y * 200.0);
            char pan_s[32], tilt_s[32];
            snprintf(pan_s, sizeof(pan_s), "%d", target_pan);
            snprintf(tilt_s, sizeof(tilt_s), "%d", target_tilt);
            run_ptz_cmd_async("abs", pan_s, tilt_s);
            *http_status = 200;
            snprintf(resp_buf, 65536,
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
                "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
                "<s:Body><tptz:AbsoluteMoveResponse/></s:Body></s:Envelope>");
            return 1;
        }
    }

    /* Fallback for unknown SOAP action */
    if (http_status) *http_status = 500;
    snprintf(resp_buf, 65536,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:ter=\"http://www.onvif.org/ver10/error\">"
        "<s:Body><s:Fault>"
        "<s:Code><s:Value>s:Sender</s:Value><s:Subcode><s:Value>ter:ActionNotSupported</s:Value></s:Subcode></s:Code>"
        "<s:Reason><s:Text xml:lang=\"en\">Unknown action: %s</s:Text></s:Reason>"
        "</s:Fault></s:Body></s:Envelope>",
        action);
    return 0;
}

static int find_main_base_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    if (info->dlpi_name == NULL || info->dlpi_name[0] == '\0') {
        *(uintptr_t *)data = (uintptr_t)info->dlpi_addr;
        return 1;
    }
    return 0;
}

__attribute__((constructor))
static void init_majestic_ptz_hooks(void) {
    uintptr_t base = 0;
    dl_iterate_phdr(find_main_base_cb, &base);
    if (!base) return;

    /* Verify exact bytes of /usr/bin/majestic ONVIF PTZ section before patching */
    uint8_t *p_5bde0 = (uint8_t *)(base + 0x5bde0);
    uint8_t *p_5c864 = (uint8_t *)(base + 0x5c864);
    if (p_5bde0[0] != 0x70 || p_5bde0[1] != 0xb5 ||
        p_5c864[0] != 0x7f || p_5c864[1] != 0xf4) {
        return;
    }

    g_majestic_base = base;
    g_mxml_save_string = (mxml_save_string_fn)(base + 0x1dd970 + 1);

    uintptr_t page_start = (base + 0x5a000) & ~((uintptr_t)0xfff);
    size_t len = 0x4000;
    if (mprotect((void *)page_start, len, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        /* 1. Patch PTZNode MaximumNumberOfPresets from "0" to "16" (0x2377d8) */
        *(uint32_t *)(base + 0x5ad38) = (uint32_t)(0x2377d8 - (0x5acea + 4));
        /* 2. Patch PTZNode HomeSupported from "false" to "true" (0x21aac1) */
        *(uint32_t *)(base + 0x5ad40) = (uint32_t)(0x21aac1 - (0x5ad00 + 4));
        /* 3. Route GetPresets (0x5c85e) to our SOAP handler via 0x5bf40 -> 0x5bde0 */
        uint8_t mov_r0_1[4] = {0x4f, 0xf0, 0x01, 0x00}; /* mov.w r0, #1 */
        memcpy((void *)(base + 0x5c85e), mov_r0_1, 4);
        /* 4. Redirect 0x5bde0 to on_ptz_soap_action */
        uint8_t ldr_pc[4] = {0xdf, 0xf8, 0x00, 0xf0};   /* ldr.w pc, [pc, #0] */
        uint32_t target = (uint32_t)((uintptr_t)on_ptz_soap_action | 1);
        memcpy(p_5bde0, ldr_pc, 4);
        memcpy(p_5bde0 + 4, &target, 4);

        __builtin___clear_cache((char *)page_start, (char *)(page_start + len));
        mprotect((void *)page_start, len, PROT_READ | PROT_EXEC);
    }
}

/* Exported Majestic AF/PTZ Plugin ABI */
__attribute__((visibility("default")))
const char *af_plugin_call(const char *cmd, const char *val) {
    if (!cmd) return NULL;
    if (strcmp(cmd, "ptz") != 0) return NULL;

    /* Capability query: af_plugin_call("ptz", "") */
    if (!val || val[0] == '\0') {
        return "verbs=left,right,up,down,upleft,upright,downleft,downright,stop\nspeeds=1..100\n";
    }

    char move_verb[32] = {0};
    int speed = 80;

    const char *m = strstr(val, "move=");
    if (m) {
        m += 5;
        size_t i = 0;
        while (m[i] && m[i] != '&' && !isspace((unsigned char)m[i]) && i < sizeof(move_verb) - 1) {
            move_verb[i] = m[i];
            i++;
        }
        move_verb[i] = '\0';
    } else {
        snprintf(move_verb, sizeof(move_verb), "%s", val);
    }

    const char *s = strstr(val, "speed=");
    if (s) {
        speed = atoi(s + 6);
        if (speed < 1) speed = 1;
        if (speed > 100) speed = 100;
    }

    if (strcmp(move_verb, "stop") == 0) {
        run_ptz_cmd_sync("stop", NULL, NULL);
        return "ok";
    }
    if (strcmp(move_verb, "home") == 0 || strcmp(move_verb, "center") == 0) {
        run_ptz_cmd_async("goto", "1", NULL);
        return "ok";
    }
    if (strcmp(move_verb, "calibrate") == 0) {
        run_ptz_cmd_async("calibrate", NULL, NULL);
        return "ok";
    }
    if (move_verb[0] != '\0') {
        char speed_s[16];
        snprintf(speed_s, sizeof(speed_s), "%d", speed);
        run_ptz_cmd_sync("move", move_verb, speed_s);
        return "ok";
    }

    return "ok";
}

__attribute__((visibility("default")))
void af_plugin_exit(void) {
    run_ptz_cmd_sync("stop", NULL, NULL);
}
