/**
* autod – Autod Personal Use License
 * Copyright (c) 2025 Joakim Snökvist
 * Licensed for personal, non-commercial use only.
 * Redistribution or commercial use requires prior written approval from Joakim Snökvist.
 * See LICENSE.md for full terms.
 */

//gcc -O2 -std=c11 -Wall -Wextra -o wfb_supervisor wfb_supervisor.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <sched.h>

#define MAX_INSTANCES 16
#define MAX_NAME_LEN  64
#define MAX_KEY_LEN   64
#define MAX_VALUE_LEN 256
#define MAX_CMD_LEN   1024
#define MAX_ARGS      64
#define MAX_CMDS      8
#define MAX_PARAM_ENTRIES 64
#define DEFAULT_RESTART_ENABLED 0
#define DEFAULT_RESTART_DELAY   3

typedef struct {
    char init_cmds[MAX_CMDS][MAX_VALUE_LEN];
    int  init_cmd_count;
    char cleanup_cmds[MAX_CMDS][MAX_VALUE_LEN];
    int  cleanup_cmd_count;

    int  restart_enabled;
    int  restart_delay;

    char param_keys[MAX_PARAM_ENTRIES][MAX_KEY_LEN];
    char param_placeholders[MAX_PARAM_ENTRIES][MAX_KEY_LEN + 2];
    char param_vals[MAX_PARAM_ENTRIES][MAX_VALUE_LEN];
    int  param_count;
} general_config_t;

typedef struct {
    char name[MAX_NAME_LEN];
    char cmd[MAX_VALUE_LEN];
    int  quiet;          // suppress stdout/stderr
    int  cpu_core;       // pin to a specific CPU core (-1 for no pin)

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
    fprintf(stderr, "wfb_supervisor: ");
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

static void run_commands(char cmds[][MAX_VALUE_LEN], int count, const char *phase);
static void store_param_kv(int line_no, const char *key, const char *val);
static const char *get_param_value(const char *key);
static int get_param_bool(const char *key, int default_val);
static int get_param_int(const char *key, int default_val);
static void apply_runtime_settings(void);

/* Config */

static void init_defaults(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.restart_enabled = DEFAULT_RESTART_ENABLED;
    g_cfg.restart_delay = DEFAULT_RESTART_DELAY;
}

static void store_param_kv(int line_no, const char *key, const char *val) {
    int idx = -1;
    for (int i = 0; i < g_cfg.param_count; i++) {
        if (strcasecmp(g_cfg.param_keys[i], key) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        if (g_cfg.param_count >= MAX_PARAM_ENTRIES) {
            die("config:%d: too many general/parameter entries (max %d)", line_no, MAX_PARAM_ENTRIES);
        }

        idx = g_cfg.param_count++;

        strncpy(g_cfg.param_keys[idx], key, sizeof(g_cfg.param_keys[0]) - 1);
        g_cfg.param_keys[idx][sizeof(g_cfg.param_keys[0]) - 1] = '\0';

        char placeholder[MAX_KEY_LEN + 2];
        int n = snprintf(placeholder, sizeof(placeholder), "$%s",
                         g_cfg.param_keys[idx]);
        if (n < 0 || (size_t)n >= sizeof(placeholder)) {
            die("config:%d: general key '%s' too long to substitute", line_no, key);
        }

        size_t placeholder_len = strlen(placeholder);
        if (placeholder_len >= sizeof(g_cfg.param_placeholders[0])) {
            die("config:%d: general key '%s' too long to substitute", line_no, key);
        }
        memcpy(g_cfg.param_placeholders[idx], placeholder, placeholder_len + 1);
    }

    strncpy(g_cfg.param_vals[idx], val, sizeof(g_cfg.param_vals[0]) - 1);
    g_cfg.param_vals[idx][sizeof(g_cfg.param_vals[0]) - 1] = '\0';
}

static int parse_parameter_kv(int line_no, const char *key, const char *val) {
    if (strcasecmp(key, "init_cmd") == 0 || strcasecmp(key, "cleanup_cmd") == 0) {
        die("config:%d: %s must be placed in [general]", line_no, key);
    }

    store_param_kv(line_no, key, val);

    return 1;
}

static instance_t *add_instance(const char *name, int line_no) {
    if (g_instance_count >= MAX_INSTANCES) {
        die("config:%d: too many instances (max %d)", line_no, MAX_INSTANCES);
    }

    for (int i = 0; i < g_instance_count; i++) {
        if (strcasecmp(g_instances[i].name, name) == 0) {
            die("config:%d: duplicate instance name '%s'", line_no, name);
        }
    }

    instance_t *inst = &g_instances[g_instance_count++];
    memset(inst, 0, sizeof(*inst));
    strncpy(inst->name, name, sizeof(inst->name)-1);
    inst->cpu_core = -1;
    return inst;
}

static int parse_general_kv(int line_no, const char *key, const char *val) {
    if (strcasecmp(key, "init_cmd") == 0) {
        if (g_cfg.init_cmd_count >= MAX_CMDS) die("config:%d: too many init_cmd entries (max %d)", line_no, MAX_CMDS);
        strncpy(g_cfg.init_cmds[g_cfg.init_cmd_count], val, sizeof(g_cfg.init_cmds[g_cfg.init_cmd_count])-1);
        g_cfg.init_cmds[g_cfg.init_cmd_count][sizeof(g_cfg.init_cmds[g_cfg.init_cmd_count])-1] = '\0';
        g_cfg.init_cmd_count++;
    } else if (strcasecmp(key, "cleanup_cmd") == 0) {
        if (g_cfg.cleanup_cmd_count >= MAX_CMDS) die("config:%d: too many cleanup_cmd entries (max %d)", line_no, MAX_CMDS);
        strncpy(g_cfg.cleanup_cmds[g_cfg.cleanup_cmd_count], val, sizeof(g_cfg.cleanup_cmds[g_cfg.cleanup_cmd_count])-1);
        g_cfg.cleanup_cmds[g_cfg.cleanup_cmd_count][sizeof(g_cfg.cleanup_cmds[g_cfg.cleanup_cmd_count])-1] = '\0';
        g_cfg.cleanup_cmd_count++;
    } else {
        return 0;
    }

    return 1;
}

static void parse_instance_kv(instance_t *inst, int line_no, const char *key, const char *val) {
    if (strcasecmp(key, "cmd") == 0) {
        strncpy(inst->cmd, val, sizeof(inst->cmd)-1);
    } else if (strcasecmp(key, "quiet") == 0) {
        if (parse_bool(val, &inst->quiet)) die("config:%d: invalid quiet value '%s'", line_no, val);
    } else if (strcasecmp(key, "cpu") == 0) {
        if (parse_int(val, &inst->cpu_core)) die("config:%d: invalid cpu value '%s'", line_no, val);
        if (inst->cpu_core < 0) die("config:%d: cpu core must be non-negative", line_no);
        if (inst->cpu_core >= CPU_SETSIZE) die("config:%d: cpu core %d exceeds maximum %d", line_no, inst->cpu_core, CPU_SETSIZE - 1);
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
    enum { SEC_NONE, SEC_GENERAL, SEC_PARAMETERS, SEC_INSTANCE } section = SEC_NONE;
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
            } else if (strcasecmp(secname, "parameters") == 0) {
                section = SEC_PARAMETERS;
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
            if (!parse_general_kv(line_no, key, val)) {
                parse_parameter_kv(line_no, key, val);
            }
        } else if (section == SEC_PARAMETERS) {
            parse_parameter_kv(line_no, key, val);
        } else if (section == SEC_INSTANCE) {
            if (!current_inst) die("config:%d: internal error: no current instance", line_no);
            parse_instance_kv(current_inst, line_no, key, val);
        } else {
            die("config:%d: key/value outside any section", line_no);
        }
    }

    fclose(f);

    apply_runtime_settings();

    if (g_instance_count == 0) die("no instances defined in config");

    for (int i = 0; i < g_instance_count; i++) {
        if (!g_instances[i].cmd[0]) {
            die("instance '%s': cmd is required", g_instances[i].name);
        }
    }
}

static void reload_config_and_init(const char *config_path) {
    load_config(config_path);
    run_commands(g_cfg.init_cmds, g_cfg.init_cmd_count, "init");
}

/* Hooks */

static void expand_placeholders(const char *in, char *out, size_t out_len) {
    size_t pos = 0;
    size_t out_pos = 0;
    size_t in_len = strlen(in);
    while (pos < in_len) {
        int matched = 0;
        if (in[pos] == '$') {
            for (int i = 0; i < g_cfg.param_count; i++) {
                size_t klen = strlen(g_cfg.param_placeholders[i]);
                if (klen == 0) continue;

                if (strncasecmp(in + pos, g_cfg.param_placeholders[i], klen) == 0) {
                    size_t vlen = strlen(g_cfg.param_vals[i]);
                    if (out_pos + vlen >= out_len) die("expanded command too long");
                    memcpy(out + out_pos, g_cfg.param_vals[i], vlen);
                    out_pos += vlen;
                    pos += klen;
                    matched = 1;
                    break;
                }
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

static const char *get_param_value(const char *key) {
    for (int i = 0; i < g_cfg.param_count; i++) {
        if (strcasecmp(g_cfg.param_keys[i], key) == 0) {
            return g_cfg.param_vals[i];
        }
    }
    return NULL;
}

static int get_param_bool(const char *key, int default_val) {
    const char *val = get_param_value(key);
    if (!val) return default_val;

    int parsed = 0;
    if (parse_bool(val, &parsed)) {
        die("config: invalid boolean value '%s' for %s", val, key);
    }
    return parsed;
}

static int get_param_int(const char *key, int default_val) {
    const char *val = get_param_value(key);
    if (!val) return default_val;

    int parsed = 0;
    if (parse_int(val, &parsed)) {
        die("config: invalid integer value '%s' for %s", val, key);
    }
    return parsed;
}

static void apply_runtime_settings(void) {
    g_cfg.restart_enabled = get_param_bool("restart", DEFAULT_RESTART_ENABLED);
    g_cfg.restart_delay = get_param_int("restart_delay", DEFAULT_RESTART_DELAY);
    if (g_cfg.restart_delay < 0) {
        die("config: restart_delay must be non-negative (got %d)", g_cfg.restart_delay);
    }
}

static const char *wrap_exec(const char *cmd, char *buf, size_t buf_len) {
    if (strncmp(cmd, "exec ", 5) == 0) return cmd;

    int n = snprintf(buf, buf_len, "exec %s", cmd);
    if (n < 0 || (size_t)n >= buf_len) die("expanded command too long");
    return buf;
}

static void run_commands(char cmds[][MAX_VALUE_LEN], int count, const char *phase) {
    for (int i = 0; i < count; i++) {
        char expanded[MAX_CMD_LEN];
        char exec_wrapped[MAX_CMD_LEN];
        expand_placeholders(cmds[i], expanded, sizeof(expanded));
        const char *cmd = wrap_exec(expanded, exec_wrapped, sizeof(exec_wrapped));
        fprintf(stderr, "wfb_supervisor: running %s command: %s\n", phase, cmd);
        pid_t pid = fork();
        if (pid < 0) die("%s command fork failed: %s", phase, strerror(errno));
        if (pid == 0) {
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            fprintf(stderr, "wfb_supervisor: exec failed for %s command '%s': %s\n", phase, cmd, strerror(errno));
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
    char exec_wrapped[MAX_CMD_LEN];
    expand_placeholders(inst->cmd, expanded_cmd, sizeof(expanded_cmd));
    const char *cmd = wrap_exec(expanded_cmd, exec_wrapped, sizeof(exec_wrapped));

    snprintf(exec_path, exec_len, "%s", "/bin/sh");
    *argc = 0;
    argv[(*argc)++] = exec_path;
    argv[(*argc)++] = "-c";
    argv[(*argc)++] = (char *)cmd;
    argv[*argc] = NULL;
}

/* Supervision */

static void shutdown_all(int failed_idx, int failed_status) {
    if (failed_idx >= 0) {
        instance_t *inst = &g_instances[failed_idx];
        if (WIFEXITED(failed_status)) {
            fprintf(stderr, "wfb_supervisor: instance '%s' (pid %d) exited with status %d, shutting down all\n",
                    inst->name, inst->pid, WEXITSTATUS(failed_status));
        } else if (WIFSIGNALED(failed_status)) {
            fprintf(stderr, "wfb_supervisor: instance '%s' (pid %d) terminated by signal %d, shutting down all\n",
                    inst->name, inst->pid, WTERMSIG(failed_status));
        } else {
            fprintf(stderr, "wfb_supervisor: instance '%s' (pid %d) exited, shutting down all\n",
                    inst->name, inst->pid);
        }
    } else {
        fprintf(stderr, "wfb_supervisor: shutdown requested, terminating all children\n");
    }

    for (int i = 0; i < g_instance_count; i++) {
        if (g_instances[i].running && g_instances[i].pid > 0) {
            kill(g_instances[i].pid, SIGTERM);
        }
    }

    int escalated = 0;
    time_t start = time(NULL);

    while (1) {
        int running = 0;
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            for (int i = 0; i < g_instance_count; i++) {
                if (g_instances[i].pid == pid) {
                    g_instances[i].exit_status = status;
                    g_instances[i].running = 0;
                    break;
                }
            }
            continue;
        }

        for (int i = 0; i < g_instance_count; i++) {
            if (g_instances[i].running) running++;
        }
        if (running == 0) break;

        if (!escalated && difftime(time(NULL), start) >= 5) {
            fprintf(stderr, "wfb_supervisor: escalating shutdown with SIGKILL for %d remaining children\n", running);
            for (int i = 0; i < g_instance_count; i++) {
                if (g_instances[i].running && g_instances[i].pid > 0) {
                    kill(g_instances[i].pid, SIGKILL);
                }
            }
            escalated = 1;
        }

        if (pid < 0 && errno != EINTR) break;
        sleep(1);
    }

    fprintf(stderr, "wfb_supervisor: summary:\n");
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

static int start_children(int *failed_idx, int *failed_status) {
    if (failed_idx) *failed_idx = -1;
    if (failed_status) *failed_status = 0;

    for (int i = 0; i < g_instance_count; i++) {
        instance_t *inst = &g_instances[i];
        char exec_path[MAX_VALUE_LEN];
        char *argv[MAX_ARGS];
        int argc = 0;

        build_command(inst, argv, &argc, exec_path, sizeof(exec_path));

        fprintf(stderr, "wfb_supervisor: starting instance '%s':", inst->name);
        for (int k = 0; k < argc; k++) {
            fprintf(stderr, " %s", argv[k]);
        }
        fprintf(stderr, "\n");
        if (inst->cpu_core >= 0) {
            fprintf(stderr, "wfb_supervisor: pinning '%s' to CPU %d\n", inst->name, inst->cpu_core);
        }

        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "wfb_supervisor: fork failed for instance '%s': %s\n", inst->name, strerror(errno));
            if (failed_idx) *failed_idx = i;
            if (failed_status) *failed_status = (127 << 8);
            inst->exit_status = failed_status ? *failed_status : 0;
            inst->running = 0;
            return -1;
        } else if (pid == 0) {
            if (inst->cpu_core >= 0) {
                cpu_set_t set;
                CPU_ZERO(&set);
                CPU_SET(inst->cpu_core, &set);
                if (sched_setaffinity(0, sizeof(set), &set) != 0) {
                    fprintf(stderr, "wfb_supervisor: failed to set CPU %d affinity for '%s': %s\n",
                            inst->cpu_core, inst->name, strerror(errno));
                }
            }
            if (inst->quiet) {
                int nullfd = open("/dev/null", O_RDWR);
                if (nullfd >= 0) {
                    dup2(nullfd, STDOUT_FILENO);
                    dup2(nullfd, STDERR_FILENO);
                    if (nullfd > 2) close(nullfd);
                }
            }
            execvp(exec_path, argv);
            fprintf(stderr, "wfb_supervisor: execvp failed for '%s': %s\n", exec_path, strerror(errno));
            _exit(127);
        } else {
            inst->pid = pid;
            inst->running = 1;
        }
    }

    return 0;
}

static int supervise_once(void) {
    int failed_idx = -1;
    int failed_status = 0;

    if (start_children(&failed_idx, &failed_status) != 0) {
        shutdown_all(failed_idx, failed_status);
        run_commands(g_cfg.cleanup_cmds, g_cfg.cleanup_cmd_count, "cleanup");
        return 1;
    }

    int running = g_instance_count;
    int shutdown_initiated = 0;
    failed_idx = -1;
    failed_status = 0;

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

int main(int argc, char **argv) {
    const char *config_path = "/etc/wfb.conf";
    int restart = -1;
    int restart_delay = -1;
    int restart_delay_set = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--restart") == 0) {
            restart = 1;
        } else if (strcmp(arg, "--restart-delay") == 0) {
            if (i + 1 >= argc) die("missing value for --restart-delay");
            if (parse_int(argv[++i], &restart_delay)) die("invalid restart delay '%s'", argv[i]);
            restart_delay_set = 1;
        } else if (strncmp(arg, "--restart-delay=", 17) == 0) {
            if (parse_int(arg + 17, &restart_delay)) die("invalid restart delay '%s'", arg + 17);
            restart_delay_set = 1;
        } else if (arg[0] == '-') {
            die("unknown option '%s'", arg);
        } else {
            config_path = arg;
        }
    }

    if (restart_delay_set && restart_delay < 0) die("restart delay must be non-negative");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int exit_code = 0;

    do {
        if (g_stop_requested) break;
        g_stop_requested = 0;
        reload_config_and_init(config_path);
        exit_code = supervise_once();
        if (g_stop_requested) break;
        int restart_enabled = (restart >= 0) ? restart : g_cfg.restart_enabled;
        int effective_delay = restart_delay_set ? restart_delay : g_cfg.restart_delay;
        if (!restart_enabled) break;
        fprintf(stderr, "wfb_supervisor: restart requested, sleeping %d seconds before relaunch\n", effective_delay);
        for (int slept = 0; slept < effective_delay && !g_stop_requested; slept++) {
            sleep(1);
        }
    } while (1);

    return exit_code;
}
