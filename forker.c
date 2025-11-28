//gcc -O2 -std=c11 -Wall -Wextra -o forker forker.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_INSTANCES 16
#define MAX_NAME_LEN  64
#define MAX_VALUE_LEN 256
#define MAX_ARGS      64

typedef struct {
    char wfb_nics[MAX_VALUE_LEN];    // comma-separated
    char wfb_tx[MAX_VALUE_LEN];
    char master_node[MAX_VALUE_LEN];
    int  fec_n;
    int  fec_k;
    int  mcs;
    int  tx_rcv_buf_size;
    int  rx_snd_buf_size;
    int  tx_snd_buf_size;
    int  fec_delay;
    int  fec_timeout;
    char frame_type[MAX_VALUE_LEN]; // data or rts
    int  log_interval;
    int  wifi_channel;
    int  stbc;
    int  ldpc;
    int  bandwidth;
    char wifi_region[MAX_VALUE_LEN];
    int  wifi_txpower;
    char guard_interval[MAX_VALUE_LEN]; // "short" or "long"
    char key_file[MAX_VALUE_LEN];

    int  radio_port;                 // default radio_port for -p

    // SSE integration
    char sse_tail[MAX_VALUE_LEN];    // path to sse_tail (optional)
    char sse_host[MAX_VALUE_LEN];    // e.g. 0.0.0.0
    int  sse_base_port;             // starting port for auto-assign
} general_config_t;

typedef enum {
    INST_KIND_UNKNOWN = 0,
    INST_KIND_RX_AGGREGATOR,
    INST_KIND_RX_FORWARDER,
    INST_KIND_TX_LOCAL,
    INST_KIND_TUNNEL
} inst_kind_t;

typedef struct {
    char name[MAX_NAME_LEN];
    char type[MAX_VALUE_LEN];       // aggregator, forwarder, local, tunnel
    char direction[MAX_VALUE_LEN];  // rx or tx (empty for tunnel)
    char input[MAX_VALUE_LEN];      // host:port or empty
    char output[MAX_VALUE_LEN];     // host:port or url
    char linkid[MAX_VALUE_LEN];
    int  control_port;              // for TX

    // tunnel-specific
    char ifname[MAX_VALUE_LEN];
    char ifaddr[MAX_VALUE_LEN];
    int  input_port;
    int  output_port;
    char tx_name[MAX_VALUE_LEN];
    char rx_name[MAX_VALUE_LEN];

    int  radio_port;                // per-instance radio_port override

    // SSE
    int  sse_enable;
    int  sse_port;                  // 0 = auto
    char sse_name[MAX_VALUE_LEN];

    // TX modes
    int  distributor;               // wfb_tx -d
    int  inject_port;               // wfb_tx -I <port>

    // derived
    inst_kind_t kind;

    // runtime
    pid_t pid;
    int   exit_status;
    int   running;
} instance_t;

static general_config_t g_cfg;
static instance_t g_instances[MAX_INSTANCES];
static int g_instance_count = 0;
static volatile sig_atomic_t g_stop_requested = 0;

/* Utility helpers */

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "forker: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static char *trim(char *s) {
    if (!s) return s;
    // left
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    // right
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

static int ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int parse_bool(const char *v, int *out) {
    if (ieq(v, "1") || ieq(v, "yes") || ieq(v, "true") || ieq(v, "on")) {
        *out = 1; return 0;
    }
    if (ieq(v, "0") || ieq(v, "no") || ieq(v, "false") || ieq(v, "off")) {
        *out = 0; return 0;
    }
    return -1;
}

static int parse_int(const char *v, int *out) {
    char *end = NULL;
    errno = 0;
    long val = strtol(v, &end, 0);
    if (errno != 0 || end == v || *end != '\0') return -1;
    *out = (int)val;
    return 0;
}

/* host:port or udp://host:port → host+port */
static int parse_host_port(const char *in, char *host_out, size_t host_len, int *port_out) {
    if (!in || !*in) return -1;
    const char *s = in;
    if (strncasecmp(s, "udp://", 6) == 0) s += 6;
    if (strncasecmp(s, "unix:", 5) == 0) {
        // unix sockets not supported here for simplicity
        return -1;
    }
    const char *colon = strrchr(s, ':');
    if (!colon) return -1;
    size_t hlen = (size_t)(colon - s);
    if (hlen == 0 || hlen >= host_len) return -1;
    memcpy(host_out, s, hlen);
    host_out[hlen] = '\0';
    int port;
    if (parse_int(colon + 1, &port) != 0) return -1;
    *port_out = port;
    return 0;
}

/* output endpoint: host:port or unix:/path */
static int parse_output_endpoint(const char *in,
                                 char *host_out, size_t host_len, int *port_out,
                                 char *unix_out, size_t unix_len) {
    if (!in || !*in) return -1;
    if (strncasecmp(in, "unix:", 5) == 0) {
        const char *p = in + 5;
        size_t len = strlen(p);
        if (len == 0 || len >= unix_len) return -1;
        memcpy(unix_out, p, len + 1);
        return 1; // unix socket
    }
    if (parse_host_port(in, host_out, host_len, port_out) != 0) return -1;
    return 0; // host:port
}

/* General config parsing */

static void init_general_defaults(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    strcpy(g_cfg.wfb_nics, "wlan0");
    g_cfg.fec_n = 12;
    g_cfg.fec_k = 8;
    g_cfg.mcs = 2;
    g_cfg.tx_rcv_buf_size = 2097152;
    g_cfg.rx_snd_buf_size = 2097152;
    g_cfg.tx_snd_buf_size = 0;
    g_cfg.fec_delay = 0;
    g_cfg.fec_timeout = 0;
    strcpy(g_cfg.frame_type, "data");
    g_cfg.log_interval = 1000;
    g_cfg.bandwidth = 20;
    strcpy(g_cfg.wifi_region, "US");
    g_cfg.wifi_txpower = 1500;
    strcpy(g_cfg.guard_interval, "long");
    strcpy(g_cfg.key_file, "/etc/gs.key");

    g_cfg.radio_port = 0;              // default: no -p, let WFB default

    // SSE defaults
    g_cfg.sse_base_port = 18080;
    strcpy(g_cfg.sse_host, "0.0.0.0");
}

static void parse_general_kv(int line_no, const char *key, const char *val) {
    if (ieq(key, "wfb_nics")) {
        strncpy(g_cfg.wfb_nics, val, sizeof(g_cfg.wfb_nics)-1);
    } else if (ieq(key, "wfb_tx")) {
        strncpy(g_cfg.wfb_tx, val, sizeof(g_cfg.wfb_tx)-1);
    } else if (ieq(key, "master_node")) {
        strncpy(g_cfg.master_node, val, sizeof(g_cfg.master_node)-1);
    } else if (ieq(key, "fec_n")) {
        if (parse_int(val, &g_cfg.fec_n)) die("config:%d: invalid fec_n", line_no);
    } else if (ieq(key, "fec_k")) {
        if (parse_int(val, &g_cfg.fec_k)) die("config:%d: invalid fec_k", line_no);
    } else if (ieq(key, "mcs")) {
        if (parse_int(val, &g_cfg.mcs)) die("config:%d: invalid mcs", line_no);
    } else if (ieq(key, "tx_rcv_buf_size")) {
        if (parse_int(val, &g_cfg.tx_rcv_buf_size)) die("config:%d: invalid tx_rcv_buf_size", line_no);
    } else if (ieq(key, "rx_snd_buf_size")) {
        if (parse_int(val, &g_cfg.rx_snd_buf_size)) die("config:%d: invalid rx_snd_buf_size", line_no);
    } else if (ieq(key, "tx_snd_buf_size")) {
        if (parse_int(val, &g_cfg.tx_snd_buf_size)) die("config:%d: invalid tx_snd_buf_size", line_no);
    } else if (ieq(key, "fec_delay")) {
        if (parse_int(val, &g_cfg.fec_delay)) die("config:%d: invalid fec_delay", line_no);
    } else if (ieq(key, "fec_timeout")) {
        if (parse_int(val, &g_cfg.fec_timeout)) die("config:%d: invalid fec_timeout", line_no);
    } else if (ieq(key, "frame_type")) {
        if (!(ieq(val, "data") || ieq(val, "rts"))) {
            die("config:%d: frame_type must be 'data' or 'rts'", line_no);
        }
        strncpy(g_cfg.frame_type, val, sizeof(g_cfg.frame_type)-1);
    } else if (ieq(key, "log_interval")) {
        if (parse_int(val, &g_cfg.log_interval)) die("config:%d: invalid log_interval", line_no);
    } else if (ieq(key, "wifi_channel")) {
        if (parse_int(val, &g_cfg.wifi_channel)) die("config:%d: invalid wifi_channel", line_no);
    } else if (ieq(key, "stbc")) {
        if (parse_int(val, &g_cfg.stbc)) die("config:%d: invalid stbc", line_no);
    } else if (ieq(key, "ldpc")) {
        if (parse_int(val, &g_cfg.ldpc)) die("config:%d: invalid ldpc", line_no);
    } else if (ieq(key, "bandwidth")) {
        if (parse_int(val, &g_cfg.bandwidth)) die("config:%d: invalid bandwidth", line_no);
    } else if (ieq(key, "wifi_region")) {
        strncpy(g_cfg.wifi_region, val, sizeof(g_cfg.wifi_region)-1);
    } else if (ieq(key, "wifi_txpower")) {
        if (parse_int(val, &g_cfg.wifi_txpower)) die("config:%d: invalid wifi_txpower", line_no);
    } else if (ieq(key, "guard_interval")) {
        if (!(ieq(val, "short") || ieq(val, "long"))) {
            die("config:%d: guard_interval must be 'short' or 'long'", line_no);
        }
        strncpy(g_cfg.guard_interval, val, sizeof(g_cfg.guard_interval)-1);
    } else if (ieq(key, "key_file")) {
        strncpy(g_cfg.key_file, val, sizeof(g_cfg.key_file)-1);
    } else if (ieq(key, "radio_port")) {
        if (parse_int(val, &g_cfg.radio_port)) die("config:%d: invalid radio_port", line_no);
    } else if (ieq(key, "sse_tail")) {
        strncpy(g_cfg.sse_tail, val, sizeof(g_cfg.sse_tail)-1);
    } else if (ieq(key, "sse_host")) {
        strncpy(g_cfg.sse_host, val, sizeof(g_cfg.sse_host)-1);
    } else if (ieq(key, "sse_base_port")) {
        if (parse_int(val, &g_cfg.sse_base_port)) die("config:%d: invalid sse_base_port", line_no);
    } else {
        die("config:%d: unknown general key '%s'", line_no, key);
    }
}

/* Instance parsing */

static instance_t *add_instance(const char *name, int line_no) {
    if (g_instance_count >= MAX_INSTANCES) {
        die("config:%d: too many instances (max %d)", line_no, MAX_INSTANCES);
    }
    instance_t *inst = &g_instances[g_instance_count++];
    memset(inst, 0, sizeof(*inst));
    strncpy(inst->name, name, sizeof(inst->name)-1);
    inst->sse_port = 0;
    inst->sse_enable = 0;
    inst->kind = INST_KIND_UNKNOWN;
    inst->radio_port = -1; // inherit general default unless explicitly set
    inst->distributor = 0;
    inst->inject_port = 0;
    return inst;
}

static void parse_instance_kv(instance_t *inst, int line_no, const char *key, const char *val) {
    if (ieq(key, "type")) {
        strncpy(inst->type, val, sizeof(inst->type)-1);
    } else if (ieq(key, "direction")) {
        strncpy(inst->direction, val, sizeof(inst->direction)-1);
    } else if (ieq(key, "input")) {
        strncpy(inst->input, val, sizeof(inst->input)-1);
    } else if (ieq(key, "output")) {
        strncpy(inst->output, val, sizeof(inst->output)-1);
    } else if (ieq(key, "linkid")) {
        strncpy(inst->linkid, val, sizeof(inst->linkid)-1);
    } else if (ieq(key, "control_port")) {
        if (parse_int(val, &inst->control_port)) die("config:%d: invalid control_port", line_no);
    } else if (ieq(key, "ifname")) {
        strncpy(inst->ifname, val, sizeof(inst->ifname)-1);
    } else if (ieq(key, "ifaddr")) {
        strncpy(inst->ifaddr, val, sizeof(inst->ifaddr)-1);
    } else if (ieq(key, "input_port")) {
        if (parse_int(val, &inst->input_port)) die("config:%d: invalid input_port", line_no);
    } else if (ieq(key, "output_port")) {
        if (parse_int(val, &inst->output_port)) die("config:%d: invalid output_port", line_no);
    } else if (ieq(key, "tx_name")) {
        strncpy(inst->tx_name, val, sizeof(inst->tx_name)-1);
    } else if (ieq(key, "rx_name")) {
        strncpy(inst->rx_name, val, sizeof(inst->rx_name)-1);
    } else if (ieq(key, "radio_port")) {
        if (parse_int(val, &inst->radio_port)) die("config:%d: invalid radio_port", line_no);
    } else if (ieq(key, "sse")) {
        if (parse_bool(val, &inst->sse_enable)) die("config:%d: invalid sse value '%s'", line_no, val);
    } else if (ieq(key, "sse_port")) {
        if (parse_int(val, &inst->sse_port)) die("config:%d: invalid sse_port", line_no);
    } else if (ieq(key, "sse_name")) {
        strncpy(inst->sse_name, val, sizeof(inst->sse_name)-1);
    } else if (ieq(key, "distributor")) {
        if (parse_bool(val, &inst->distributor)) die("config:%d: invalid distributor value '%s'", line_no, val);
    } else if (ieq(key, "inject_port")) {
        if (parse_int(val, &inst->inject_port)) die("config:%d: invalid inject_port", line_no);
    } else {
        die("config:%d: unknown key '%s' in instance '%s'", line_no, key, inst->name);
    }
}

static inst_kind_t deduce_kind(const instance_t *inst) {
    if (ieq(inst->type, "tunnel")) {
        return INST_KIND_TUNNEL;
    }
    if (ieq(inst->type, "aggregator")) {
        if (ieq(inst->direction, "tx")) return INST_KIND_TX_LOCAL;
        return INST_KIND_RX_AGGREGATOR;
    }
    if (ieq(inst->type, "forwarder")) {
        if (ieq(inst->direction, "tx")) return INST_KIND_TX_LOCAL;
        return INST_KIND_RX_FORWARDER;
    }
    if (ieq(inst->type, "local")) {
        if (ieq(inst->direction, "tx")) return INST_KIND_TX_LOCAL;
        else return INST_KIND_RX_AGGREGATOR; // treat as local RX if needed
    }
    if (ieq(inst->type, "distributor")) {
        return INST_KIND_TX_LOCAL;
    }
    if (ieq(inst->type, "injector")) {
        return INST_KIND_TX_LOCAL;
    }
    return INST_KIND_UNKNOWN;
}

static void derive_missing_direction(instance_t *inst) {
    if (inst->direction[0]) return;
    if (ieq(inst->type, "distributor")) {
        strcpy(inst->direction, "tx");
        return;
    }
    if (ieq(inst->type, "injector")) {
        strcpy(inst->direction, "tx");
        return;
    }
    size_t len = strlen(inst->name);
    if (len > 3 && strcmp(inst->name + len - 3, "-rx") == 0) {
        strcpy(inst->direction, "rx");
    } else if (len > 3 && strcmp(inst->name + len - 3, "-tx") == 0) {
        strcpy(inst->direction, "tx");
    }
}

/* Config file loader */

static void load_config(const char *path) {
    init_general_defaults();

    FILE *f = fopen(path, "r");
    if (!f) die("cannot open config '%s': %s", path, strerror(errno));

    char linebuf[512];
    int line_no = 0;
    enum { SEC_NONE, SEC_GENERAL, SEC_INSTANCE } section = SEC_NONE;
    instance_t *current_inst = NULL;

    while (fgets(linebuf, sizeof(linebuf), f)) {
        line_no++;
        char *line = linebuf;
        // strip newline
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        line = trim(line);
        if (*line == '\0') continue;
        if (*line == '#' || *line == ';') continue;

        if (*line == '[') {
            char *rb = strchr(line, ']');
            if (!rb) die("config:%d: missing ']'", line_no);
            *rb = '\0';
            char *secname = trim(line + 1);
            if (ieq(secname, "general")) {
                section = SEC_GENERAL;
                current_inst = NULL;
            } else if (strncasecmp(secname, "instance", 8) == 0) {
                char *p = secname + 8;
                while (*p && isspace((unsigned char)*p)) p++;
                if (*p == '\0') die("config:%d: instance name missing", line_no);
                section = SEC_INSTANCE;
                current_inst = add_instance(p, line_no);
            } else {
                die("config:%d: unknown section '%s'", line_no, secname);
            }
            continue;
        }

        // key=value
        char *eq = strchr(line, '=');
        if (!eq) die("config:%d: expected key=value", line_no);
        *eq = '\0';
        char *key = trim(line);
        char *val = trim(eq + 1);
        if (*key == '\0') die("config:%d: empty key", line_no);

        if (section == SEC_GENERAL) {
            parse_general_kv(line_no, key, val);
        } else if (section == SEC_INSTANCE) {
            if (!current_inst) die("config:%d: internal error: no current instance", line_no);
            parse_instance_kv(current_inst, line_no, key, val);
        } else {
            die("config:%d: key/value outside any section", line_no);
        }
    }

    fclose(f);

    if (g_instance_count == 0) {
        die("no instances defined in config");
    }

    // Derive kind for each instance
    for (int i = 0; i < g_instance_count; i++) {
        instance_t *inst = &g_instances[i];
        derive_missing_direction(inst);
        inst->kind = deduce_kind(inst);
        if (inst->kind == INST_KIND_UNKNOWN) {
            die("instance '%s': unknown/unsupported type='%s' direction='%s'",
                inst->name, inst->type, inst->direction);
        }
        if (ieq(inst->type, "distributor")) {
            inst->distributor = 1;
            if (!ieq(inst->direction, "tx")) {
                die("instance '%s': distributor must use direction=tx", inst->name);
            }
        }
        if (ieq(inst->type, "injector") && inst->inject_port <= 0) {
            die("instance '%s': injector requires inject_port", inst->name);
        }
        if (inst->inject_port > 0 && inst->distributor) {
            die("instance '%s': injector mode (-I) cannot be combined with distributor (-d)", inst->name);
        }
        if (inst->inject_port > 0 && inst->kind != INST_KIND_TX_LOCAL) {
            die("instance '%s': injector mode (-I) requires a TX kind", inst->name);
        }
        if (inst->sse_enable && !g_cfg.sse_tail[0]) {
            die("instance '%s': sse enabled but sse_tail not set in [general]",
                inst->name);
        }
        if (inst->sse_enable && inst->sse_port == 0) {
            inst->sse_port = g_cfg.sse_base_port++;
        }
        if (inst->sse_enable && !inst->sse_name[0]) {
            strncpy(inst->sse_name, inst->name,
                    sizeof(inst->sse_name)-1);
        }
    }
}

/* Build argv for an instance (underlying wfb_* binary) */

static void add_arg(char **argv, int *argc, const char *a) {
    if (*argc >= MAX_ARGS-1) die("too many arguments");
    argv[(*argc)++] = (char *)a;
}

/*
 * We avoid heap allocation by using a small string storage pool per call.
 * 'arg_storage' holds numeric strings (ports, buf sizes, etc.). We must not
 * reuse the same char buffer for multiple argv entries.
 */

static void build_wfb_command(const instance_t *inst,
                              char *cmd_buf, size_t cmd_len,
                              char **argv, int *argc) {
    *argc = 0;
    cmd_buf[0] = '\0';

    char host[128];
    int port = 0;
    char unix_path[MAX_VALUE_LEN];

    static char arg_storage[MAX_ARGS][MAX_VALUE_LEN];
    int storage_idx = 0;

    #define NEXT_SLOT() do { \
        if (storage_idx >= MAX_ARGS) die("internal: arg_storage exhausted"); \
    } while (0)

    switch (inst->kind) {
    case INST_KIND_RX_AGGREGATOR:
        snprintf(cmd_buf, cmd_len, "wfb_rx");
        add_arg(argv, argc, cmd_buf);
        // -a <server_port> from input
        if (parse_host_port(inst->input, host, sizeof(host), &port) != 0) {
            die("instance '%s': invalid input '%s' (expected host:port)",
                inst->name, inst->input);
        }
        NEXT_SLOT();
        snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", port);
        add_arg(argv, argc, "-a");
        add_arg(argv, argc, arg_storage[storage_idx]);
        storage_idx++;

        // -K key_file
        if (g_cfg.key_file[0]) {
            add_arg(argv, argc, "-K");
            add_arg(argv, argc, g_cfg.key_file);
        }
        // output → client_addr/client_port
        int out_kind = parse_output_endpoint(inst->output, host, sizeof(host),
                                             &port, unix_path, sizeof(unix_path));
        if (out_kind == 1) {
            add_arg(argv, argc, "-U");
            add_arg(argv, argc, unix_path);
        } else if (out_kind == 0) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", port);
            add_arg(argv, argc, "-c");
            add_arg(argv, argc, host);
            add_arg(argv, argc, "-u");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        } else {
            die("instance '%s': invalid output '%s' (host:port or unix:/path)",
                inst->name, inst->output);
        }

        // buffers, log, linkid
        if (g_cfg.tx_rcv_buf_size > 0) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]),
                     "%d", g_cfg.tx_rcv_buf_size);
            add_arg(argv, argc, "-R");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }
        if (g_cfg.rx_snd_buf_size > 0) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]),
                     "%d", g_cfg.rx_snd_buf_size);
            add_arg(argv, argc, "-s");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }
        if (g_cfg.log_interval > 0) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]),
                     "%d", g_cfg.log_interval);
            add_arg(argv, argc, "-l");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }
        if (inst->linkid[0]) {
            add_arg(argv, argc, "-i");
            add_arg(argv, argc, inst->linkid);
        }

        // radio_port
        {
            int rp = (inst->radio_port >= 0) ? inst->radio_port : g_cfg.radio_port;
            if (rp > 0) {
                NEXT_SLOT();
                snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", rp);
                add_arg(argv, argc, "-p");
                add_arg(argv, argc, arg_storage[storage_idx]);
                storage_idx++;
            }
        }

        // interfaces from WFB_NICS
        {
            char nics_copy[MAX_VALUE_LEN];
            strncpy(nics_copy, g_cfg.wfb_nics, sizeof(nics_copy)-1);
            nics_copy[sizeof(nics_copy)-1] = '\0';
            char *tok = strtok(nics_copy, ",");
            if (!tok) die("general: wfb_nics is empty");
            while (tok) {
                tok = trim(tok);
                if (*tok) {
                    NEXT_SLOT();
                    strncpy(arg_storage[storage_idx], tok, sizeof(arg_storage[storage_idx])-1);
                    arg_storage[storage_idx][sizeof(arg_storage[storage_idx])-1] = '\0';
                    add_arg(argv, argc, arg_storage[storage_idx]);
                    storage_idx++;
                }
                tok = strtok(NULL, ",");
            }
        }
        break;

    case INST_KIND_RX_FORWARDER:
        snprintf(cmd_buf, cmd_len, "wfb_rx");
        add_arg(argv, argc, cmd_buf);
        add_arg(argv, argc, "-f");
        // output host:port → -c/-u
        if (parse_host_port(inst->output, host, sizeof(host), &port) != 0) {
            die("instance '%s': invalid output '%s'", inst->name, inst->output);
        }
        NEXT_SLOT();
        snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", port);
        add_arg(argv, argc, "-c");
        add_arg(argv, argc, host);
        add_arg(argv, argc, "-u");
        add_arg(argv, argc, arg_storage[storage_idx]);
        storage_idx++;

        if (inst->linkid[0]) {
            add_arg(argv, argc, "-i");
            add_arg(argv, argc, inst->linkid);
        }

        // radio_port
        {
            int rp = (inst->radio_port >= 0) ? inst->radio_port : g_cfg.radio_port;
            if (rp > 0) {
                NEXT_SLOT();
                snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", rp);
                add_arg(argv, argc, "-p");
                add_arg(argv, argc, arg_storage[storage_idx]);
                storage_idx++;
            }
        }

        // interfaces
        {
            char nics_copy[MAX_VALUE_LEN];
            strncpy(nics_copy, g_cfg.wfb_nics, sizeof(nics_copy)-1);
            nics_copy[sizeof(nics_copy)-1] = '\0';
            char *tok = strtok(nics_copy, ",");
            if (!tok) die("general: wfb_nics is empty");
            while (tok) {
                tok = trim(tok);
                if (*tok) {
                    NEXT_SLOT();
                    strncpy(arg_storage[storage_idx], tok, sizeof(arg_storage[storage_idx])-1);
                    arg_storage[storage_idx][sizeof(arg_storage[storage_idx])-1] = '\0';
                    add_arg(argv, argc, arg_storage[storage_idx]);
                    storage_idx++;
                }
                tok = strtok(NULL, ",");
            }
        }
        break;

    case INST_KIND_TX_LOCAL:
        snprintf(cmd_buf, cmd_len, "wfb_tx");
        add_arg(argv, argc, cmd_buf);

        int distributor_mode = inst->distributor;
        int injector_mode = (inst->inject_port > 0);
        if (distributor_mode) {
            add_arg(argv, argc, "-d");
        }

        // -K, -k, -n
        if (!injector_mode) {
            if (g_cfg.key_file[0]) {
                add_arg(argv, argc, "-K");
                add_arg(argv, argc, g_cfg.key_file);
            }
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", g_cfg.fec_k);
            add_arg(argv, argc, "-k");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;

            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", g_cfg.fec_n);
            add_arg(argv, argc, "-n");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }

        // input host:port → -u port
        if (!injector_mode && inst->input[0]) {
            if (parse_host_port(inst->input, host, sizeof(host), &port) != 0) {
                die("instance '%s': invalid input '%s'", inst->name, inst->input);
            }
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", port);
            add_arg(argv, argc, "-u");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }
        if (injector_mode) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d",
                     inst->inject_port);
            add_arg(argv, argc, "-I");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }

        if (!injector_mode && inst->control_port > 0) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d",
                     inst->control_port);
            add_arg(argv, argc, "-C");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }
        if (g_cfg.tx_rcv_buf_size > 0) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d",
                     g_cfg.tx_rcv_buf_size);
            add_arg(argv, argc, "-R");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }
        if (!injector_mode && g_cfg.tx_snd_buf_size > 0) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d",
                     g_cfg.tx_snd_buf_size);
            add_arg(argv, argc, "-s");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }
        if (g_cfg.log_interval > 0) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]),
                     "%d", g_cfg.log_interval);
            add_arg(argv, argc, "-l");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }
        // modem parameters (TX only, skip for injector which is a separate mode)
        if (!injector_mode && g_cfg.fec_delay > 0) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", g_cfg.fec_delay);
            add_arg(argv, argc, "-F");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }
        if (!injector_mode && g_cfg.bandwidth > 0) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", g_cfg.bandwidth);
            add_arg(argv, argc, "-B");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }
        if (!injector_mode && g_cfg.guard_interval[0]) {
            add_arg(argv, argc, "-G");
            add_arg(argv, argc, g_cfg.guard_interval);
        }
        if (!injector_mode && g_cfg.frame_type[0]) {
            add_arg(argv, argc, "-f");
            add_arg(argv, argc, g_cfg.frame_type);
        }
        if (!injector_mode && g_cfg.fec_timeout > 0) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", g_cfg.fec_timeout);
            add_arg(argv, argc, "-T");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }
        if (!injector_mode) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", g_cfg.mcs);
            add_arg(argv, argc, "-M");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;

            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", g_cfg.stbc);
            add_arg(argv, argc, "-S");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;

            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", g_cfg.ldpc);
            add_arg(argv, argc, "-L");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }

        if (!injector_mode && inst->linkid[0]) {
            add_arg(argv, argc, "-i");
            add_arg(argv, argc, inst->linkid);
        }

        // radio_port
        {
            int rp = (inst->radio_port >= 0) ? inst->radio_port : g_cfg.radio_port;
            if (!injector_mode && !distributor_mode && rp > 0) {
                NEXT_SLOT();
                snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]), "%d", rp);
                add_arg(argv, argc, "-p");
                add_arg(argv, argc, arg_storage[storage_idx]);
                storage_idx++;
            }
        }

        if (distributor_mode) {
            if (!inst->output[0]) {
                die("instance '%s': distributor requires output host list", inst->name);
            }
            add_arg(argv, argc, inst->output);
        } else {
            // Interfaces: prefer wfb_tx if set, else first wfb_nics
            if (g_cfg.wfb_tx[0]) {
                add_arg(argv, argc, g_cfg.wfb_tx);
            } else {
                char nics_copy[MAX_VALUE_LEN];
                strncpy(nics_copy, g_cfg.wfb_nics, sizeof(nics_copy)-1);
                nics_copy[sizeof(nics_copy)-1] = '\0';
                char *tok = strtok(nics_copy, ",");
                if (!tok) die("general: wfb_nics is empty");
                tok = trim(tok);
                NEXT_SLOT();
                strncpy(arg_storage[storage_idx], tok, sizeof(arg_storage[storage_idx])-1);
                arg_storage[storage_idx][sizeof(arg_storage[storage_idx])-1] = '\0';
                add_arg(argv, argc, arg_storage[storage_idx]);
                storage_idx++;
            }
        }
        break;

    case INST_KIND_TUNNEL:
        snprintf(cmd_buf, cmd_len, "wfb_tun");
        add_arg(argv, argc, cmd_buf);
        if (inst->ifname[0]) {
            add_arg(argv, argc, "-t");
            add_arg(argv, argc, inst->ifname);
        }
        if (inst->ifaddr[0]) {
            add_arg(argv, argc, "-a");
            add_arg(argv, argc, inst->ifaddr);
        }
        if (inst->input_port > 0) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]),
                     "%d", inst->input_port);
            add_arg(argv, argc, "-l");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }
        if (inst->output_port > 0) {
            NEXT_SLOT();
            snprintf(arg_storage[storage_idx], sizeof(arg_storage[storage_idx]),
                     "%d", inst->output_port);
            add_arg(argv, argc, "-u");
            add_arg(argv, argc, arg_storage[storage_idx]);
            storage_idx++;
        }
        // Peer addr: use master_node if set, else 127.0.0.1
        if (g_cfg.master_node[0]) {
            add_arg(argv, argc, "-c");
            add_arg(argv, argc, g_cfg.master_node);
        }
        break;

    default:
        die("instance '%s': unsupported kind", inst->name);
    }

    argv[*argc] = NULL;
}

/* Build final command: either sse_tail -- wfb_* or wfb_* directly */

static void build_final_command(const instance_t *inst,
                                char *cmd_buf, size_t cmd_len,
                                char **argv, int *argc) {
    static char inner_cmd_buf[MAX_VALUE_LEN];
    static char *inner_argv[MAX_ARGS];
    int inner_argc = 0;

    build_wfb_command(inst, inner_cmd_buf, sizeof(inner_cmd_buf),
                      inner_argv, &inner_argc);

    if (inst->sse_enable) {
        // sse_tail -p <port> -h host -- inner_cmd ...
        *argc = 0;
        snprintf(cmd_buf, cmd_len, "%s", g_cfg.sse_tail);
        add_arg(argv, argc, cmd_buf);

        static char portbuf[32];
        snprintf(portbuf, sizeof(portbuf), "%d", inst->sse_port);
        add_arg(argv, argc, "-p");
        add_arg(argv, argc, portbuf);

        if (g_cfg.sse_host[0]) {
            add_arg(argv, argc, "-h");
            add_arg(argv, argc, g_cfg.sse_host);
        }

        // Optional name: pass as -n if available
        if (inst->sse_name[0]) {
            add_arg(argv, argc, "-n");
            add_arg(argv, argc, inst->sse_name);
        }

        add_arg(argv, argc, "--");
        for (int i = 0; i < inner_argc; i++) {
            add_arg(argv, argc, inner_argv[i]);
        }
        argv[*argc] = NULL;
    } else {
        // direct wfb_* command
        *argc = inner_argc;
        for (int i = 0; i < inner_argc; i++) {
            argv[i] = inner_argv[i];
        }
        argv[*argc] = NULL;
        snprintf(cmd_buf, cmd_len, "%s", inner_argv[0]);
    }
}

/* Process supervision */

static void sigint_handler(int sig) {
    (void)sig;
    g_stop_requested = 1;
}

// Build all commands up front to catch config errors before forking children.
static void validate_all_commands(void) {
    for (int i = 0; i < g_instance_count; i++) {
        char cmd_buf[MAX_VALUE_LEN];
        char *argv[MAX_ARGS];
        int argc = 0;
        build_final_command(&g_instances[i], cmd_buf, sizeof(cmd_buf), argv, &argc);
    }
}

static void start_children(void) {
    for (int i = 0; i < g_instance_count; i++) {
        instance_t *inst = &g_instances[i];
        char cmd_buf[MAX_VALUE_LEN];
        char *argv[MAX_ARGS];
        int argc = 0;

        build_final_command(inst, cmd_buf, sizeof(cmd_buf), argv, &argc);

        fprintf(stderr, "forker: starting instance '%s':", inst->name);
        for (int k = 0; k < argc; k++) {
            fprintf(stderr, " %s", argv[k]);
        }
        fprintf(stderr, "\n");

        pid_t pid = fork();
        if (pid < 0) {
            die("fork failed for instance '%s': %s", inst->name, strerror(errno));
        } else if (pid == 0) {
            // child
            if (!inst->sse_enable) {
                int nullfd = open("/dev/null", O_RDWR);
                if (nullfd >= 0) {
                    dup2(nullfd, STDOUT_FILENO);
                    dup2(nullfd, STDERR_FILENO);
                    if (nullfd > 2) close(nullfd);
                }
            }
            execvp(cmd_buf, argv);
            fprintf(stderr, "forker: execvp failed for '%s': %s\n", cmd_buf, strerror(errno));
            _exit(127);
        } else {
            inst->pid = pid;
            inst->running = 1;
        }
    }
}

static void shutdown_all(int failed_idx, int failed_status) {
    if (failed_idx >= 0) {
        instance_t *inst = &g_instances[failed_idx];
        if (WIFEXITED(failed_status)) {
            fprintf(stderr, "forker: instance '%s' (pid %d) exited with status %d, shutting down all\n",
                    inst->name, inst->pid, WEXITSTATUS(failed_status));
        } else if (WIFSIGNALED(failed_status)) {
            fprintf(stderr, "forker: instance '%s' (pid %d) terminated by signal %d, shutting down all\n",
                    inst->name, inst->pid, WTERMSIG(failed_status));
        } else {
            fprintf(stderr, "forker: instance '%s' (pid %d) exited, shutting down all\n",
                    inst->name, inst->pid);
        }
    } else {
        fprintf(stderr, "forker: shutdown requested, terminating all children\n");
    }

    // Send SIGTERM to all running children
    for (int i = 0; i < g_instance_count; i++) {
        if (g_instances[i].running && g_instances[i].pid > 0) {
            kill(g_instances[i].pid, SIGTERM);
        }
    }

    // Reap remaining children
    int still = 1;
    while (still) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < g_instance_count; i++) {
            if (g_instances[i].pid == pid) {
                g_instances[i].exit_status = status;
                g_instances[i].running = 0;
                break;
            }
        }
    }

    // Summary
    fprintf(stderr, "forker: summary:\n");
    for (int i = 0; i < g_instance_count; i++) {
        instance_t *inst = &g_instances[i];
        if (WIFEXITED(inst->exit_status)) {
            fprintf(stderr, "  %s: exit(%d)\n", inst->name, WEXITSTATUS(inst->exit_status));
        } else if (WIFSIGNALED(inst->exit_status)) {
            fprintf(stderr, "  %s: signal(%d)\n", inst->name, WTERMSIG(inst->exit_status));
        } else {
            fprintf(stderr, "  %s: status(%d)\n", inst->name, inst->exit_status);
        }
    }
}

int main(int argc, char **argv) {
    const char *config_path = "wfb.conf";
    if (argc > 1) config_path = argv[1];

    load_config(config_path);
    validate_all_commands();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    start_children();

    int running = g_instance_count;
    int shutdown_initiated = 0;
    int failed_idx = -1;
    int failed_status = 0;

    while (running > 0) {
        if (g_stop_requested && !shutdown_initiated) {
            shutdown_initiated = 1;
            failed_idx = -1;
            failed_status = 0;
            break;
        }

        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            if (errno == EINTR) continue;
            break;
        }

        int idx = -1;
        for (int i = 0; i < g_instance_count; i++) {
            if (g_instances[i].pid == pid) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            fprintf(stderr, "forker: reaped unknown child pid %d\n", (int)pid);
            continue;
        }

        g_instances[idx].exit_status = status;
        g_instances[idx].running = 0;
        running--;

        if (!shutdown_initiated) {
            shutdown_initiated = 1;
            failed_idx = idx;
            failed_status = status;
            break;
        }
    }

    shutdown_all(failed_idx, failed_status);

    return (failed_idx >= 0) ? 2 : 0;
}
