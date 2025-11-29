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
#define MAX_CMD_LEN   1024
#define MAX_ARGS      64
#define MAX_CMDS      8

typedef struct {
    char sse_tail[MAX_VALUE_LEN];
    char sse_host[MAX_VALUE_LEN];
    int  sse_base_port;

    char rx_nics[MAX_VALUE_LEN];
    char tx_nics[MAX_VALUE_LEN];
    char master_node[MAX_VALUE_LEN];
    char link_id[MAX_VALUE_LEN];
    int  mcs;
    int  ldpc;
    int  stbc;
    char key_file[MAX_VALUE_LEN];
    int  log_interval;

    char init_cmds[MAX_CMDS][MAX_VALUE_LEN];
    int  init_cmd_count;
    char cleanup_cmds[MAX_CMDS][MAX_VALUE_LEN];
    int  cleanup_cmd_count;
} general_config_t;

typedef struct {
    char name[MAX_NAME_LEN];
    char cmd[MAX_VALUE_LEN];
    int  sse_enable;
    int  sse_port;       // 0 = auto
    char sse_name[MAX_VALUE_LEN];
    int  quiet;          // suppress stdout/stderr when not using SSE

    pid_t pid;
    int   exit_status;
    int   running;
} instance_t;

static general_config_t g_cfg;
static instance_t g_instances[MAX_INSTANCES];
static int g_instance_count = 0;
static volatile sig_atomic_t g_stop_requested = 0;

/* Utils */

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
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end-1))) *(--end) = '\0';
    return s;
}

static int parse_bool(const char *v, int *out) {
    if (!v) return -1;
    if (strcasecmp(v, "1") == 0 || strcasecmp(v, "yes") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "on") == 0) {
        *out = 1; return 0;
    }
    if (strcasecmp(v, "0") == 0 || strcasecmp(v, "no") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "off") == 0) {
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

/* Config */

static void init_defaults(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.sse_base_port = 18080;
    strcpy(g_cfg.sse_host, "0.0.0.0");
    strcpy(g_cfg.rx_nics, "");
    strcpy(g_cfg.tx_nics, "");
    strcpy(g_cfg.master_node, "");
    strcpy(g_cfg.link_id, "");
    g_cfg.mcs = 0;
    g_cfg.ldpc = 0;
    g_cfg.stbc = 0;
    strcpy(g_cfg.key_file, "");
    g_cfg.log_interval = 0;
}

static instance_t *add_instance(const char *name, int line_no) {
    if (g_instance_count >= MAX_INSTANCES) {
        die("config:%d: too many instances (max %d)", line_no, MAX_INSTANCES);
    }
    instance_t *inst = &g_instances[g_instance_count++];
    memset(inst, 0, sizeof(*inst));
    strncpy(inst->name, name, sizeof(inst->name)-1);
    inst->sse_port = 0;
    return inst;
}

static void parse_general_kv(int line_no, const char *key, const char *val) {
    if (strcasecmp(key, "sse_tail") == 0) {
        strncpy(g_cfg.sse_tail, val, sizeof(g_cfg.sse_tail)-1);
    } else if (strcasecmp(key, "sse_host") == 0) {
        strncpy(g_cfg.sse_host, val, sizeof(g_cfg.sse_host)-1);
    } else if (strcasecmp(key, "sse_base_port") == 0) {
        if (parse_int(val, &g_cfg.sse_base_port)) die("config:%d: invalid sse_base_port", line_no);
    } else if (strcasecmp(key, "init_cmd") == 0) {
        if (g_cfg.init_cmd_count >= MAX_CMDS) die("config:%d: too many init_cmd entries (max %d)", line_no, MAX_CMDS);
        strncpy(g_cfg.init_cmds[g_cfg.init_cmd_count], val, sizeof(g_cfg.init_cmds[g_cfg.init_cmd_count])-1);
        g_cfg.init_cmds[g_cfg.init_cmd_count][sizeof(g_cfg.init_cmds[g_cfg.init_cmd_count])-1] = '\0';
        g_cfg.init_cmd_count++;
    } else if (strcasecmp(key, "cleanup_cmd") == 0) {
        if (g_cfg.cleanup_cmd_count >= MAX_CMDS) die("config:%d: too many cleanup_cmd entries (max %d)", line_no, MAX_CMDS);
        strncpy(g_cfg.cleanup_cmds[g_cfg.cleanup_cmd_count], val, sizeof(g_cfg.cleanup_cmds[g_cfg.cleanup_cmd_count])-1);
        g_cfg.cleanup_cmds[g_cfg.cleanup_cmd_count][sizeof(g_cfg.cleanup_cmds[g_cfg.cleanup_cmd_count])-1] = '\0';
        g_cfg.cleanup_cmd_count++;
    } else if (strcasecmp(key, "rx_nics") == 0) {
        strncpy(g_cfg.rx_nics, val, sizeof(g_cfg.rx_nics)-1);
    } else if (strcasecmp(key, "tx_nics") == 0) {
        strncpy(g_cfg.tx_nics, val, sizeof(g_cfg.tx_nics)-1);
    } else if (strcasecmp(key, "master_node") == 0) {
        strncpy(g_cfg.master_node, val, sizeof(g_cfg.master_node)-1);
    } else if (strcasecmp(key, "link_id") == 0) {
        strncpy(g_cfg.link_id, val, sizeof(g_cfg.link_id)-1);
    } else if (strcasecmp(key, "mcs") == 0) {
        if (parse_int(val, &g_cfg.mcs)) die("config:%d: invalid mcs", line_no);
    } else if (strcasecmp(key, "ldpc") == 0) {
        if (parse_int(val, &g_cfg.ldpc)) die("config:%d: invalid ldpc", line_no);
    } else if (strcasecmp(key, "stbc") == 0) {
        if (parse_int(val, &g_cfg.stbc)) die("config:%d: invalid stbc", line_no);
    } else if (strcasecmp(key, "key_file") == 0) {
        strncpy(g_cfg.key_file, val, sizeof(g_cfg.key_file)-1);
    } else if (strcasecmp(key, "log_interval") == 0) {
        if (parse_int(val, &g_cfg.log_interval)) die("config:%d: invalid log_interval", line_no);
    } else {
        die("config:%d: unknown general key '%s'", line_no, key);
    }
}

static void parse_instance_kv(instance_t *inst, int line_no, const char *key, const char *val) {
    if (strcasecmp(key, "cmd") == 0) {
        strncpy(inst->cmd, val, sizeof(inst->cmd)-1);
    } else if (strcasecmp(key, "sse") == 0) {
        if (parse_bool(val, &inst->sse_enable)) die("config:%d: invalid sse value '%s'", line_no, val);
    } else if (strcasecmp(key, "sse_port") == 0) {
        if (parse_int(val, &inst->sse_port)) die("config:%d: invalid sse_port", line_no);
    } else if (strcasecmp(key, "sse_name") == 0) {
        strncpy(inst->sse_name, val, sizeof(inst->sse_name)-1);
    } else if (strcasecmp(key, "quiet") == 0) {
        if (parse_bool(val, &inst->quiet)) die("config:%d: invalid quiet value '%s'", line_no, val);
    } else {
        die("config:%d: unknown key '%s' in instance '%s'", line_no, key, inst->name);
    }
}

static void load_config(const char *path) {
    init_defaults();
    g_instance_count = 0;
    memset(g_instances, 0, sizeof(g_instances));

    FILE *f = fopen(path, "r");
    if (!f) die("cannot open config '%s': %s", path, strerror(errno));

    char linebuf[512];
    int line_no = 0;
    enum { SEC_NONE, SEC_GENERAL, SEC_INSTANCE } section = SEC_NONE;
    instance_t *current_inst = NULL;

    while (fgets(linebuf, sizeof(linebuf), f)) {
        line_no++;
        char *line = linebuf;
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
            if (strcasecmp(secname, "general") == 0) {
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

    if (g_instance_count == 0) die("no instances defined in config");

    for (int i = 0; i < g_instance_count; i++) {
        if (!g_instances[i].cmd[0]) {
            die("instance '%s': cmd is required", g_instances[i].name);
        }
        if (g_instances[i].sse_enable) {
            if (!g_cfg.sse_tail[0]) die("instance '%s': sse enabled but sse_tail not set in [general]", g_instances[i].name);
            if (g_instances[i].sse_port == 0) g_instances[i].sse_port = g_cfg.sse_base_port++;
            if (!g_instances[i].sse_name[0]) {
                snprintf(g_instances[i].sse_name, sizeof(g_instances[i].sse_name), "%.255s", g_instances[i].name);
            }
        }
    }
}

/* Hooks */

static void expand_placeholders(const char *in, char *out, size_t out_len) {
    char mcs_buf[16], ldpc_buf[16], stbc_buf[16], log_buf[32];
    snprintf(mcs_buf, sizeof(mcs_buf), "%d", g_cfg.mcs);
    snprintf(ldpc_buf, sizeof(ldpc_buf), "%d", g_cfg.ldpc);
    snprintf(stbc_buf, sizeof(stbc_buf), "%d", g_cfg.stbc);
    snprintf(log_buf, sizeof(log_buf), "%d", g_cfg.log_interval);

    struct kv { const char *key; const char *val; } table[] = {
        {"$rx_nics", g_cfg.rx_nics},
        {"$tx_nics", g_cfg.tx_nics},
        {"$master_node", g_cfg.master_node},
        {"$link_id", g_cfg.link_id},
        {"$mcs", mcs_buf},
        {"$ldpc", ldpc_buf},
        {"$stbc", stbc_buf},
        {"$key_file", g_cfg.key_file},
        {"$log_interval", log_buf},
    };
    size_t pos = 0;
    size_t out_pos = 0;
    size_t in_len = strlen(in);
    while (pos < in_len) {
        int matched = 0;
        for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
            size_t klen = strlen(table[i].key);
            if (klen > 0 && strncmp(in + pos, table[i].key, klen) == 0) {
                size_t vlen = strlen(table[i].val);
                if (out_pos + vlen >= out_len) die("expanded command too long");
                memcpy(out + out_pos, table[i].val, vlen);
                out_pos += vlen;
                pos += klen;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            if (out_pos + 1 >= out_len) die("expanded command too long");
            out[out_pos++] = in[pos++];
        }
    }
    if (out_pos >= out_len) die("expanded command too long");
    out[out_pos] = '\0';
}

static void run_commands(char cmds[][MAX_VALUE_LEN], int count, const char *phase) {
    for (int i = 0; i < count; i++) {
        char expanded[MAX_CMD_LEN];
        expand_placeholders(cmds[i], expanded, sizeof(expanded));
        fprintf(stderr, "forker: running %s command: %s\n", phase, expanded);
        pid_t pid = fork();
        if (pid < 0) die("%s command fork failed: %s", phase, strerror(errno));
        if (pid == 0) {
            execl("/bin/sh", "sh", "-c", expanded, (char *)NULL);
            fprintf(stderr, "forker: exec failed for %s command '%s': %s\n", phase, expanded, strerror(errno));
            _exit(127);
        }
        int status;
        if (waitpid(pid, &status, 0) < 0) die("%s command waitpid failed: %s", phase, strerror(errno));
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            die("%s command '%s' failed (status %d)", phase, expanded,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        }
    }
}

/* Build commands */

static void build_command(const instance_t *inst, char **argv, int *argc, char *exec_path, size_t exec_len) {
    char expanded_cmd[MAX_CMD_LEN];
    expand_placeholders(inst->cmd, expanded_cmd, sizeof(expanded_cmd));

    if (inst->sse_enable) {
        snprintf(exec_path, exec_len, "%s", g_cfg.sse_tail);
        *argc = 0;
        argv[(*argc)++] = exec_path;

        static char portbuf[32];
        snprintf(portbuf, sizeof(portbuf), "%d", inst->sse_port);
        argv[(*argc)++] = "-p";
        argv[(*argc)++] = portbuf;

        if (g_cfg.sse_host[0]) {
            argv[(*argc)++] = "-h";
            argv[(*argc)++] = g_cfg.sse_host;
        }
        if (inst->sse_name[0]) {
            argv[(*argc)++] = "-n";
            argv[(*argc)++] = (char *)inst->sse_name;
        }
        argv[(*argc)++] = "--";
        argv[(*argc)++] = "/bin/sh";
        argv[(*argc)++] = "-c";
        argv[(*argc)++] = expanded_cmd;
        argv[*argc] = NULL;
    } else {
        snprintf(exec_path, exec_len, "%s", "/bin/sh");
        *argc = 0;
        argv[(*argc)++] = exec_path;
        argv[(*argc)++] = "-c";
        argv[(*argc)++] = expanded_cmd;
        argv[*argc] = NULL;
    }
}

/* Supervision */

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

    for (int i = 0; i < g_instance_count; i++) {
        if (g_instances[i].running && g_instances[i].pid > 0) {
            kill(g_instances[i].pid, SIGTERM);
        }
    }

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

static void signal_handler(int sig) {
    (void)sig;
    g_stop_requested = 1;
}

static void start_children(void) {
    for (int i = 0; i < g_instance_count; i++) {
        instance_t *inst = &g_instances[i];
        char exec_path[MAX_VALUE_LEN];
        char *argv[MAX_ARGS];
        int argc = 0;

        build_command(inst, argv, &argc, exec_path, sizeof(exec_path));

        fprintf(stderr, "forker: starting instance '%s':", inst->name);
        for (int k = 0; k < argc; k++) {
            fprintf(stderr, " %s", argv[k]);
        }
        fprintf(stderr, "\n");

        pid_t pid = fork();
        if (pid < 0) {
            die("fork failed for instance '%s': %s", inst->name, strerror(errno));
        } else if (pid == 0) {
            if (!inst->sse_enable && inst->quiet) {
                int nullfd = open("/dev/null", O_RDWR);
                if (nullfd >= 0) {
                    dup2(nullfd, STDOUT_FILENO);
                    dup2(nullfd, STDERR_FILENO);
                    if (nullfd > 2) close(nullfd);
                }
            }
            execvp(exec_path, argv);
            fprintf(stderr, "forker: execvp failed for '%s': %s\n", exec_path, strerror(errno));
            _exit(127);
        } else {
            inst->pid = pid;
            inst->running = 1;
        }
    }
}

int main(int argc, char **argv) {
    const char *config_path = "wfb.conf";
    if (argc > 1) config_path = argv[1];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    load_config(config_path);

    run_commands(g_cfg.init_cmds, g_cfg.init_cmd_count, "init");

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
        if (idx < 0) continue;

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
    run_commands(g_cfg.cleanup_cmds, g_cfg.cleanup_cmd_count, "cleanup");

    return (failed_idx >= 0) ? 2 : 0;
}
