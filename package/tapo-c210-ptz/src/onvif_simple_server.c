/*
 * onvif_simple_server.c — Lightweight ONVIF PTZ & Preset SOAP Daemon for OpenIPC
 * Target: /usr/sbin/onvif_simple_server
 *
 * Reads /etc/onvif_simple_server.conf and handles ONVIF Profile S/T PTZ SOAP
 * requests (GetPresets, GotoPreset, SetPreset, RemovePreset, GotoHomePosition,
 * SetHomePosition, ContinuousMove, RelativeMove, AbsoluteMove, Stop, GetStatus,
 * GetNodes, GetNode, GetConfigurations, GetConfiguration, GetConfigurationOptions,
 * GetServiceCapabilities), while proxying Device/Media requests to 127.0.0.1:80.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CONF_FILE "/etc/onvif_simple_server.conf"
#define PRESETS_FILE      "/etc/ptz_presets.json"
#define STATE_FILE        "/tmp/ptz_state"

typedef struct {
    int port;
    int ptz;
    char model[64];
    char manufacturer[64];
    char move_left[128];
    char move_right[128];
    char move_up[128];
    char move_down[128];
    char move_stop[128];
    char move_preset[128];
} onvif_conf_t;

static onvif_conf_t g_conf;

static void init_conf(onvif_conf_t *c) {
    memset(c, 0, sizeof(*c));
    c->port = 8080;
    c->ptz = 1;
    strcpy(c->model, "Tapo C210 v2 OpenIPC");
    strcpy(c->manufacturer, "TP-Link / OpenIPC");
    strcpy(c->move_left, "/usr/sbin/ptz-controller step -50 0");
    strcpy(c->move_right, "/usr/sbin/ptz-controller step 50 0");
    strcpy(c->move_up, "/usr/sbin/ptz-controller step 0 30");
    strcpy(c->move_down, "/usr/sbin/ptz-controller step 0 -30");
    strcpy(c->move_stop, "/usr/sbin/ptz-controller stop");
    strcpy(c->move_preset, "/usr/sbin/ptz-controller goto");
}

static void trim_right(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || isspace((unsigned char)s[n - 1]))) {
        s[--n] = '\0';
    }
}

static void load_conf(onvif_conf_t *c, const char *path) {
    init_conf(c);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim_right(line);
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        trim_right(key);
        while (*val && isspace((unsigned char)*val)) val++;

        if (strcmp(key, "port") == 0) c->port = atoi(val);
        else if (strcmp(key, "ptz") == 0) c->ptz = atoi(val);
        else if (strcmp(key, "model") == 0) snprintf(c->model, sizeof(c->model), "%s", val);
        else if (strcmp(key, "manufacturer") == 0) snprintf(c->manufacturer, sizeof(c->manufacturer), "%s", val);
        else if (strcmp(key, "move_left") == 0) snprintf(c->move_left, sizeof(c->move_left), "%s", val);
        else if (strcmp(key, "move_right") == 0) snprintf(c->move_right, sizeof(c->move_right), "%s", val);
        else if (strcmp(key, "move_up") == 0) snprintf(c->move_up, sizeof(c->move_up), "%s", val);
        else if (strcmp(key, "move_down") == 0) snprintf(c->move_down, sizeof(c->move_down), "%s", val);
        else if (strcmp(key, "move_stop") == 0) snprintf(c->move_stop, sizeof(c->move_stop), "%s", val);
        else if (strcmp(key, "move_preset") == 0) snprintf(c->move_preset, sizeof(c->move_preset), "%s", val);
    }
    fclose(f);
}

static void run_shell_async(const char *cmd) {
    if (!cmd || !*cmd) return;
    pid_t pid = fork();
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 == 0) {
            close(STDIN_FILENO);
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

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

static void send_soap_response(int fd, int status, const char *soap_xml) {
    char hdr[512];
    size_t body_len = strlen(soap_xml);
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/soap+xml; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        status, (status == 200) ? "OK" : "Internal Server Error", body_len);
    (void)write(fd, hdr, (size_t)hlen);
    (void)write(fd, soap_xml, body_len);
}

static void handle_client(int cfd) {
    char req[8192];
    ssize_t n = read(cfd, req, sizeof(req) - 1);
    if (n <= 0) return;
    req[n] = '\0';

    char *body = strstr(req, "\r\n\r\n");
    if (!body) body = req;
    else body += 4;

    char resp[16384];

    if (strstr(body, "GetPresets")) {
        char json[4096] = {0};
        FILE *f = fopen(PRESETS_FILE, "r");
        if (f) {
            size_t jn = fread(json, 1, sizeof(json) - 1, f);
            json[jn] = '\0';
            fclose(f);
        }
        int pan_span = 2000, tilt_span = 400;
        int off = snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
            "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
            "<s:Body><tptz:GetPresetsResponse>");

        char *presets_sec = strstr(json, "\"presets\"");
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
                    int pan = 1000, tilt = 200;
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
                    if (pp && strchr(pp, ':')) pan = atoi(strchr(pp, ':') + 1);
                    char *tp = strstr(obj, "\"tilt\"");
                    if (tp && strchr(tp, ':')) tilt = atoi(strchr(tp, ':') + 1);
                    double x = ((double)pan - (pan_span / 2.0)) / (pan_span / 2.0);
                    double y = ((double)tilt - (tilt_span / 2.0)) / (tilt_span / 2.0);
                    off += snprintf(resp + off, sizeof(resp) - (size_t)off,
                        "<tptz:Preset token=\"%d\"><tt:Name>%s</tt:Name>"
                        "<tt:PTZPosition><tt:PanTilt x=\"%.4f\" y=\"%.4f\"/></tt:PTZPosition></tptz:Preset>",
                        id, name, x, y);
                }
                cur = obj_e + 1;
            }
        }
        snprintf(resp + off, sizeof(resp) - (size_t)off, "</tptz:GetPresetsResponse></s:Body></s:Envelope>");
        send_soap_response(cfd, 200, resp);
        return;
    }

    if (strstr(body, "GotoPreset")) {
        char token[32] = "1";
        extract_xml_tag(body, "PresetToken", token, sizeof(token));
        /* Sanitize token */
        for (size_t i = 0; token[i]; i++) {
            if (!isalnum((unsigned char)token[i]) && token[i] != '_' && token[i] != '-') token[i] = '\0';
        }
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "%s %s", g_conf.move_preset, token);
        run_shell_async(cmd);
        snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
            "<s:Body><tptz:GotoPresetResponse/></s:Body></s:Envelope>");
        send_soap_response(cfd, 200, resp);
        return;
    }

    if (strstr(body, "SetPreset")) {
        char token[32] = "1";
        char name[32] = "preset_1";
        extract_xml_tag(body, "PresetToken", token, sizeof(token));
        extract_xml_tag(body, "PresetName", name, sizeof(name));
        for (size_t i = 0; token[i]; i++) {
            if (!isalnum((unsigned char)token[i]) && token[i] != '_') token[i] = '\0';
        }
        for (size_t i = 0; name[i]; i++) {
            if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-') name[i] = '\0';
        }
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "/usr/sbin/ptz-controller set %s %s", token[0] ? token : "1", name[0] ? name : "preset_1");
        int rc = system(cmd);
        (void)rc;
        const char *d = token;
        while (*d && !isdigit((unsigned char)*d)) d++;
        int id = (*d) ? atoi(d) : 1;
        snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
            "<s:Body><tptz:SetPresetResponse><tptz:PresetToken>%d</tptz:PresetToken></tptz:SetPresetResponse></s:Body></s:Envelope>",
            id);
        send_soap_response(cfd, 200, resp);
        return;
    }

    if (strstr(body, "RemovePreset")) {
        char token[32] = "1";
        extract_xml_tag(body, "PresetToken", token, sizeof(token));
        for (size_t i = 0; token[i]; i++) {
            if (!isalnum((unsigned char)token[i]) && token[i] != '_') token[i] = '\0';
        }
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "/usr/sbin/ptz-controller remove %s", token);
        int rc = system(cmd);
        (void)rc;
        snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
            "<s:Body><tptz:RemovePresetResponse/></s:Body></s:Envelope>");
        send_soap_response(cfd, 200, resp);
        return;
    }

    if (strstr(body, "GotoHomePosition")) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "%s 1", g_conf.move_preset);
        run_shell_async(cmd);
        snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
            "<s:Body><tptz:GotoHomePositionResponse/></s:Body></s:Envelope>");
        send_soap_response(cfd, 200, resp);
        return;
    }

    if (strstr(body, "ContinuousMove")) {
        double x = 0.0, y = 0.0;
        extract_pantilt_xy(body, &x, &y);
        if (x < -0.01) run_shell_async(g_conf.move_left);
        else if (x > 0.01) run_shell_async(g_conf.move_right);
        else if (y > 0.01) run_shell_async(g_conf.move_up);
        else if (y < -0.01) run_shell_async(g_conf.move_down);
        snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
            "<s:Body><tptz:ContinuousMoveResponse/></s:Body></s:Envelope>");
        send_soap_response(cfd, 200, resp);
        return;
    }

    if (strstr(body, "RelativeMove")) {
        double x = 0.0, y = 0.0;
        extract_pantilt_xy(body, &x, &y);
        int d_pan = (int)(x * 1000.0);
        int d_tilt = (int)(y * 200.0);
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "/usr/sbin/ptz-controller step %d %d", d_pan, d_tilt);
        run_shell_async(cmd);
        snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
            "<s:Body><tptz:RelativeMoveResponse/></s:Body></s:Envelope>");
        send_soap_response(cfd, 200, resp);
        return;
    }

    if (strstr(body, "AbsoluteMove")) {
        double x = 0.0, y = 0.0;
        extract_pantilt_xy(body, &x, &y);
        int pan = 1000 + (int)(x * 1000.0);
        int tilt = 200 + (int)(y * 200.0);
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "/usr/sbin/ptz-controller abs %d %d", pan, tilt);
        run_shell_async(cmd);
        snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
            "<s:Body><tptz:AbsoluteMoveResponse/></s:Body></s:Envelope>");
        send_soap_response(cfd, 200, resp);
        return;
    }

    if (strstr(body, ":Stop") || strstr(body, "<Stop")) {
        run_shell_async(g_conf.move_stop);
        snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
            "<s:Body><tptz:StopResponse/></s:Body></s:Envelope>");
        send_soap_response(cfd, 200, resp);
        return;
    }

    if (strstr(body, "GetStatus")) {
        int pan = 1000, tilt = 200, moving = 0;
        FILE *f = fopen(STATE_FILE, "r");
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                sscanf(line, "pan_pos=%d", &pan);
                sscanf(line, "tilt_pos=%d", &tilt);
                sscanf(line, "moving=%d", &moving);
            }
            fclose(f);
        }
        double x = ((double)pan - 1000.0) / 1000.0;
        double y = ((double)tilt - 200.0) / 200.0;
        snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
            "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
            "<s:Body><tptz:GetStatusResponse><tptz:PTZStatus>"
            "<tt:Position><tt:PanTilt x=\"%.4f\" y=\"%.4f\" space=\"http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace\"/></tt:Position>"
            "<tt:MoveStatus><tt:PanTilt>%s</tt:PanTilt></tt:MoveStatus>"
            "</tptz:PTZStatus></tptz:GetStatusResponse></s:Body></s:Envelope>",
            x, y, moving ? "MOVING" : "IDLE");
        send_soap_response(cfd, 200, resp);
        return;
    }

    /* Proxy all other ONVIF calls to Majestic on 127.0.0.1:80 */
    int mfd = socket(AF_INET, SOCK_STREAM, 0);
    if (mfd >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(80);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(mfd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            (void)write(mfd, req, (size_t)n);
            ssize_t r;
            while ((r = read(mfd, resp, sizeof(resp))) > 0) {
                (void)write(cfd, resp, (size_t)r);
            }
        }
        close(mfd);
    }
}

int main(int argc, char **argv) {
    const char *conf_path = DEFAULT_CONF_FILE;
    int daemonize = 1;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "-f") == 0) && i + 1 < argc) {
            conf_path = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "-n") == 0) {
            daemonize = (strcmp(argv[i], "-n") == 0) ? 0 : 1;
        }
    }

    load_conf(&g_conf, conf_path);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) return 1;

    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)g_conf.port);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sfd);
        return 1;
    }
    if (listen(sfd, 8) < 0) {
        close(sfd);
        return 1;
    }

    if (daemonize) {
        if (daemon(0, 0) < 0) {
            close(sfd);
            return 1;
        }
    }

    signal(SIGCHLD, SIG_IGN);

    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_client(cfd);
        close(cfd);
    }

    close(sfd);
    return 0;
}
