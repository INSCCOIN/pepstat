/*
 * pepstat — one-screen system card for PeppermintOS (and any Linux)
 *
 *   gcc -O2 -Wall -Wextra -o pepstat pepstat.c
 *   ./pepstat              # print once and exit
 *   ./pepstat -w           # refresh every 2 seconds
 *   ./pepstat -w -n 1      # watch, 1 second interval
 *   ./pepstat --json       # machine-readable
 *   ./pepstat --no-color
 *
 * Reads only /proc, /sys, libc. No ncurses, GTK, or root.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

/* ---------- config ---------- */
#define WATCH_DEFAULT_SEC 2
#define CPU_SAMPLE_MS     120
#define WARN_PCT          80.0
#define CRIT_PCT          90.0
#define WARN_TEMP_C       75.0
#define CRIT_TEMP_C       90.0
#define WARN_BAT_PCT      20
#define CRIT_BAT_PCT      10

/* ---------- tiny helpers ---------- */
static int use_color = 1;

static int is_tty(void) { return isatty(STDOUT_FILENO); }

static void color(const char *code)
{
    if (use_color) fputs(code, stdout);
}

#define C_RESET  "\033[0m"
#define C_DIM    "\033[2m"
#define C_BOLD   "\033[1m"
#define C_GREEN  "\033[32m"
#define C_YEL    "\033[33m"
#define C_RED    "\033[31m"
#define C_CYAN   "\033[36m"
#define C_BLUE   "\033[34m"
#define C_WHITE  "\033[37m"

static const char *lvl_color(int lvl) /* 0 ok, 1 warn, 2 crit, -1 none */
{
    if (lvl == 2) return C_RED;
    if (lvl == 1) return C_YEL;
    if (lvl == 0) return C_GREEN;
    return C_RESET;
}

static int pct_level(double pct)
{
    if (pct >= CRIT_PCT) return 2;
    if (pct >= WARN_PCT) return 1;
    return 0;
}

static int read_file(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)n, f)) { fclose(f); return -1; }
    fclose(f);
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}

static int read_key_file(const char *path, const char *key, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    char line[512];
    size_t klen = strlen(key);
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) == 0 && (line[klen] == '=' || line[klen] == ':')) {
            char *v = line + klen + 1;
            while (*v == ' ' || *v == '\t' || *v == '"') v++;
            strncpy(out, v, n - 1);
            out[n - 1] = 0;
            out[strcspn(out, "\r\n\"")] = 0;
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static void human_bytes(double bytes, char *out, size_t n)
{
    const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int i = 0;
    while (bytes >= 1024.0 && i < 4) { bytes /= 1024.0; i++; }
    if (i == 0) snprintf(out, n, "%.0f %s", bytes, u[i]);
    else snprintf(out, n, "%.1f %s", bytes, u[i]);
}

static void human_secs(double secs, char *out, size_t n)
{
    unsigned long s = (unsigned long)secs;
    unsigned long d = s / 86400; s %= 86400;
    unsigned long h = s / 3600;  s %= 3600;
    unsigned long m = s / 60;    s %= 60;
    if (d) snprintf(out, n, "%lud %luh %lum", d, h, m);
    else if (h) snprintf(out, n, "%luh %lum", h, m);
    else snprintf(out, n, "%lum %lus", m, s);
}

static void bar(double pct, int width)
{
    if (width < 4) width = 4;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int fill = (int)((pct / 100.0) * width + 0.5);
    int lvl = pct_level(pct);
    color(lvl_color(lvl));
    fputc('[', stdout);
    for (int i = 0; i < width; i++) fputc(i < fill ? '#' : '-', stdout);
    fputc(']', stdout);
    color(C_RESET);
}

/* ---------- snapshot ---------- */
typedef struct {
    char hostname[128];
    char pretty_os[128];
    char kernel[128];
    char cpu_model[128];
    int  cpu_cores;
    double load1, load5, load15;
    double cpu_pct;          /* -1 if unknown */
    double uptime_sec;
    unsigned long mem_total_kb, mem_avail_kb;
    unsigned long swap_total_kb, swap_free_kb;
    unsigned long disk_total, disk_free; /* bytes, root */
    int  have_bat;
    int  bat_pct;
    char bat_status[32];
    int  have_temp;
    double temp_c;
    char iface[32];
    char ipv4[64];
    char init[32];
    char session[32];
    char display[32];
} Snap;

static void snap_init(Snap *s)
{
    memset(s, 0, sizeof *s);
    strcpy(s->pretty_os, "Linux");
    strcpy(s->cpu_model, "unknown");
    strcpy(s->bat_status, "n/a");
    strcpy(s->iface, "-");
    strcpy(s->ipv4, "-");
    strcpy(s->init, "unknown");
    strcpy(s->session, "-");
    strcpy(s->display, "-");
    s->cpu_pct = -1;
    s->cpu_cores = 1;
}

static void collect_identity(Snap *s)
{
    gethostname(s->hostname, sizeof s->hostname - 1);
    read_key_file("/etc/os-release", "PRETTY_NAME", s->pretty_os, sizeof s->pretty_os);

    struct utsname u;
    if (uname(&u) == 0)
        snprintf(s->kernel, sizeof s->kernel, "%s %s", u.sysname, u.release);

    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];
    int cores = 0;
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "model name", 10) == 0 && s->cpu_model[0] == 'u') {
                char *p = strchr(line, ':');
                if (p) {
                    p++;
                    while (*p == ' ') p++;
                    strncpy(s->cpu_model, p, sizeof s->cpu_model - 1);
                    s->cpu_model[strcspn(s->cpu_model, "\r\n")] = 0;
                }
            }
            /* ARM boards use "Processor" or "CPU part"; count processors */
            if (strncmp(line, "processor", 9) == 0) cores++;
        }
        fclose(f);
    }
    if (cores == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        cores = n > 0 ? (int)n : 1;
    }
    s->cpu_cores = cores;
    if (s->cpu_model[0] == 'u') {
        /* Raspberry / Allwinner often only have Hardware / Model */
        if (read_key_file("/proc/cpuinfo", "Hardware", s->cpu_model, sizeof s->cpu_model) != 0)
            read_key_file("/proc/cpuinfo", "Model", s->cpu_model, sizeof s->cpu_model);
    }
}

static void collect_load_uptime(Snap *s)
{
    char buf[128];
    if (read_file("/proc/loadavg", buf, sizeof buf) == 0)
        sscanf(buf, "%lf %lf %lf", &s->load1, &s->load5, &s->load15);
    if (read_file("/proc/uptime", buf, sizeof buf) == 0)
        s->uptime_sec = strtod(buf, NULL);
}

static int read_cpu_times(unsigned long long *idle, unsigned long long *total)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char cpu[8];
    unsigned long long u, n, sys, id, iw, irq, sirq, st, g = 0, gn = 0;
    int ok = fscanf(f, "%7s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                    cpu, &u, &n, &sys, &id, &iw, &irq, &sirq, &st, &g, &gn);
    fclose(f);
    if (ok < 5) return -1;
    *idle = id + iw;
    *total = u + n + sys + id + iw + irq + sirq + st;
    return 0;
}

static void collect_cpu_pct(Snap *s)
{
    unsigned long long i1, t1, i2, t2;
    if (read_cpu_times(&i1, &t1) != 0) return;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = CPU_SAMPLE_MS * 1000000L };
    nanosleep(&ts, NULL);
    if (read_cpu_times(&i2, &t2) != 0) return;
    unsigned long long dt = t2 - t1, di = i2 - i1;
    if (dt == 0) return;
    s->cpu_pct = 100.0 * (1.0 - (double)di / (double)dt);
    if (s->cpu_pct < 0) s->cpu_pct = 0;
}

static void collect_mem(Snap *s)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char key[64];
    unsigned long val;
    char unit[16];
    if (!f) return;
    while (fscanf(f, "%63s %lu %15s", key, &val, unit) == 3) {
        if (strcmp(key, "MemTotal:") == 0) s->mem_total_kb = val;
        else if (strcmp(key, "MemAvailable:") == 0) s->mem_avail_kb = val;
        else if (strcmp(key, "SwapTotal:") == 0) s->swap_total_kb = val;
        else if (strcmp(key, "SwapFree:") == 0) s->swap_free_kb = val;
    }
    fclose(f);
}

static void collect_disk(Snap *s)
{
    struct statvfs v;
    if (statvfs("/", &v) == 0) {
        s->disk_total = (unsigned long)v.f_blocks * v.f_frsize;
        s->disk_free  = (unsigned long)v.f_bavail * v.f_frsize;
    }
}

static void collect_battery(Snap *s)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[256], buf[64];
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/type", e->d_name);
        if (read_file(path, buf, sizeof buf) != 0) continue;
        if (strcasecmp(buf, "Battery") != 0) continue;
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", e->d_name);
        if (read_file(path, buf, sizeof buf) == 0) {
            s->have_bat = 1;
            s->bat_pct = atoi(buf);
        }
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/status", e->d_name);
        if (read_file(path, s->bat_status, sizeof s->bat_status) != 0)
            strcpy(s->bat_status, "Unknown");
        break;
    }
    closedir(d);
}

static void collect_temp(Snap *s)
{
    /* Prefer thermal zones named x86_pkg_id / cpu-thermal / soc */
    const char *prefer[] = {"x86_pkg_id", "cpu-thermal", "soc-thermal", "acpitz", NULL};
    DIR *d = opendir("/sys/class/thermal");
    if (!d) return;
    double best = 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
        char typep[256], tempp[256], typ[64], tbuf[32];
        snprintf(typep, sizeof typep, "/sys/class/thermal/%s/type", e->d_name);
        snprintf(tempp, sizeof tempp, "/sys/class/thermal/%s/temp", e->d_name);
        if (read_file(typep, typ, sizeof typ) != 0) continue;
        if (read_file(tempp, tbuf, sizeof tbuf) != 0) continue;
        double c = atof(tbuf) / 1000.0;
        if (c <= 0 || c > 150) continue;
        int pref = 0;
        for (int i = 0; prefer[i]; i++)
            if (strcmp(typ, prefer[i]) == 0) pref = 1;
        if (!found || pref) {
            found = 1;
            best = c;
            if (pref) break;
        }
    }
    closedir(d);
    if (found) {
        s->have_temp = 1;
        s->temp_c = best;
    }
}

static void collect_net(Snap *s)
{
    /* default route iface from /proc/net/route (dest 00000000) */
    FILE *f = fopen("/proc/net/route", "r");
    char line[256], ifn[32];
    unsigned dest;
    if (f) {
        fgets(line, sizeof line, f); /* header */
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "%31s %X", ifn, &dest) == 2 && dest == 0) {
                strncpy(s->iface, ifn, sizeof s->iface - 1);
                break;
            }
        }
        fclose(f);
    }
    struct ifaddrs *ifa = NULL, *p;
    if (getifaddrs(&ifa) != 0) return;
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (s->iface[0] && s->iface[0] != '-' && strcmp(p->ifa_name, s->iface) != 0)
            continue;
        if (strcmp(p->ifa_name, "lo") == 0) continue;
        struct sockaddr_in *in = (struct sockaddr_in *)p->ifa_addr;
        inet_ntop(AF_INET, &in->sin_addr, s->ipv4, sizeof s->ipv4);
        if (s->iface[0] == '-' || !s->iface[0])
            strncpy(s->iface, p->ifa_name, sizeof s->iface - 1);
        if (s->iface[0] != '-') break;
    }
    freeifaddrs(ifa);
}

static void collect_session(Snap *s)
{
    char comm[64] = {0};
    if (read_file("/proc/1/comm", comm, sizeof comm) == 0) {
        if (strcmp(comm, "systemd") == 0) strcpy(s->init, "systemd");
        else if (strcmp(comm, "init") == 0) {
            /* Devuan: could be sysvinit. Check a few tells. */
            if (access("/etc/runit", F_OK) == 0) strcpy(s->init, "runit");
            else if (access("/run/openrc", F_OK) == 0 || access("/etc/init.d/openrc", F_OK) == 0)
                strcpy(s->init, "OpenRC");
            else strcpy(s->init, "sysvinit");
        } else {
            strncpy(s->init, comm, sizeof s->init - 1);
        }
    }
    if (access("/run/systemd/system", F_OK) == 0) strcpy(s->init, "systemd");

    const char *st = getenv("XDG_SESSION_TYPE");
    if (st) strncpy(s->session, st, sizeof s->session - 1);
    else if (getenv("WAYLAND_DISPLAY")) strcpy(s->session, "wayland");
    else if (getenv("DISPLAY")) strcpy(s->session, "x11");

    if (getenv("DISPLAY")) strncpy(s->display, getenv("DISPLAY"), sizeof s->display - 1);
    else if (getenv("WAYLAND_DISPLAY"))
        strncpy(s->display, getenv("WAYLAND_DISPLAY"), sizeof s->display - 1);
}

static void collect(Snap *s)
{
    snap_init(s);
    collect_identity(s);
    collect_load_uptime(s);
    collect_cpu_pct(s);
    collect_mem(s);
    collect_disk(s);
    collect_battery(s);
    collect_temp(s);
    collect_net(s);
    collect_session(s);
}

/* ---------- render ---------- */
static void rule(int w)
{
    color(C_DIM);
    for (int i = 0; i < w; i++) fputc('-', stdout);
    color(C_RESET);
    fputc('\n', stdout);
}

static void kv(const char *k, const char *v)
{
    color(C_DIM); printf("  %-10s", k); color(C_RESET);
    printf("%s\n", v);
}

static void render_text(const Snap *s)
{
    char buf[256], a[64], b[64];
    int cols = 72;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 40)
        cols = ws.ws_col;
    if (cols > 88) cols = 88;

    color(C_BOLD); color(C_CYAN);
    printf(" pepstat");
    color(C_RESET); color(C_DIM);
    printf("  %s\n", s->hostname);
    color(C_RESET);
    rule(cols);

    kv("os", s->pretty_os);
    kv("kernel", s->kernel);
    snprintf(buf, sizeof buf, "%s  (%s %s)", s->init, s->session, s->display);
    kv("session", buf);
    human_secs(s->uptime_sec, a, sizeof a);
    kv("uptime", a);

    rule(cols);

    snprintf(buf, sizeof buf, "%s  (%d cores)", s->cpu_model, s->cpu_cores);
    kv("cpu", buf);
    if (s->cpu_pct >= 0) {
        color(C_DIM); printf("  %-10s", "busy"); color(C_RESET);
        bar(s->cpu_pct, 28);
        printf("  %.0f%%\n", s->cpu_pct);
    }
    {
        double per = s->cpu_cores > 0 ? s->load1 / s->cpu_cores : s->load1;
        int lvl = per >= 1.5 ? 2 : per >= 1.0 ? 1 : 0;
        color(C_DIM); printf("  %-10s", "load"); color(C_RESET);
        color(lvl_color(lvl));
        printf("%.2f  %.2f  %.2f", s->load1, s->load5, s->load15);
        color(C_RESET);
        color(C_DIM); printf("   (1 / 5 / 15 min)\n"); color(C_RESET);
    }

    if (s->have_temp) {
        int lvl = s->temp_c >= CRIT_TEMP_C ? 2 : s->temp_c >= WARN_TEMP_C ? 1 : 0;
        color(C_DIM); printf("  %-10s", "temp"); color(C_RESET);
        color(lvl_color(lvl));
        printf("%.1f C\n", s->temp_c);
        color(C_RESET);
    }

    rule(cols);

    if (s->mem_total_kb) {
        double used = (double)(s->mem_total_kb - s->mem_avail_kb);
        double pct = 100.0 * used / (double)s->mem_total_kb;
        human_bytes(used * 1024.0, a, sizeof a);
        human_bytes((double)s->mem_total_kb * 1024.0, b, sizeof b);
        color(C_DIM); printf("  %-10s", "memory"); color(C_RESET);
        bar(pct, 28);
        printf("  %s / %s  (%.0f%%)\n", a, b, pct);
    }
    if (s->swap_total_kb) {
        double used = (double)(s->swap_total_kb - s->swap_free_kb);
        double pct = 100.0 * used / (double)s->swap_total_kb;
        human_bytes(used * 1024.0, a, sizeof a);
        human_bytes((double)s->swap_total_kb * 1024.0, b, sizeof b);
        color(C_DIM); printf("  %-10s", "swap"); color(C_RESET);
        bar(pct, 28);
        printf("  %s / %s  (%.0f%%)\n", a, b, pct);
    } else {
        kv("swap", "none");
    }

    if (s->disk_total) {
        double used = (double)(s->disk_total - s->disk_free);
        double pct = 100.0 * used / (double)s->disk_total;
        human_bytes(used, a, sizeof a);
        human_bytes((double)s->disk_total, b, sizeof b);
        color(C_DIM); printf("  %-10s", "disk /"); color(C_RESET);
        bar(pct, 28);
        printf("  %s / %s  (%.0f%%)\n", a, b, pct);
    }

    rule(cols);

    snprintf(buf, sizeof buf, "%s  %s", s->iface, s->ipv4);
    kv("net", buf);

    if (s->have_bat) {
        int lvl = 0;
        if (strcasecmp(s->bat_status, "Discharging") == 0) {
            if (s->bat_pct <= CRIT_BAT_PCT) lvl = 2;
            else if (s->bat_pct <= WARN_BAT_PCT) lvl = 1;
        }
        color(C_DIM); printf("  %-10s", "battery"); color(C_RESET);
        bar((double)s->bat_pct, 28);
        color(lvl_color(lvl));
        printf("  %d%%  %s\n", s->bat_pct, s->bat_status);
        color(C_RESET);
    } else {
        kv("battery", "none (desktop / no sysfs)");
    }

    rule(cols);
    color(C_DIM);
    printf("  q quit   r refresh   flags: -w --json --no-color -n SEC\n");
    color(C_RESET);
}

static void render_json(const Snap *s)
{
    printf("{\n");
    printf("  \"hostname\": \"%s\",\n", s->hostname);
    printf("  \"os\": \"%s\",\n", s->pretty_os);
    printf("  \"kernel\": \"%s\",\n", s->kernel);
    printf("  \"init\": \"%s\",\n", s->init);
    printf("  \"session\": \"%s\",\n", s->session);
    printf("  \"display\": \"%s\",\n", s->display);
    printf("  \"uptime_sec\": %.0f,\n", s->uptime_sec);
    printf("  \"cpu_model\": \"%s\",\n", s->cpu_model);
    printf("  \"cpu_cores\": %d,\n", s->cpu_cores);
    printf("  \"cpu_pct\": %.1f,\n", s->cpu_pct);
    printf("  \"load\": [%.2f, %.2f, %.2f],\n", s->load1, s->load5, s->load15);
    printf("  \"mem_total_kb\": %lu,\n", s->mem_total_kb);
    printf("  \"mem_avail_kb\": %lu,\n", s->mem_avail_kb);
    printf("  \"swap_total_kb\": %lu,\n", s->swap_total_kb);
    printf("  \"swap_free_kb\": %lu,\n", s->swap_free_kb);
    printf("  \"disk_total\": %lu,\n", s->disk_total);
    printf("  \"disk_free\": %lu,\n", s->disk_free);
    printf("  \"iface\": \"%s\",\n", s->iface);
    printf("  \"ipv4\": \"%s\",\n", s->ipv4);
    if (s->have_temp) printf("  \"temp_c\": %.1f,\n", s->temp_c);
    else printf("  \"temp_c\": null,\n");
    if (s->have_bat) {
        printf("  \"battery_pct\": %d,\n", s->bat_pct);
        printf("  \"battery_status\": \"%s\"\n", s->bat_status);
    } else {
        printf("  \"battery_pct\": null,\n");
        printf("  \"battery_status\": null\n");
    }
    printf("}\n");
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-w] [-n SEC] [--json] [--no-color] [-h]\n"
        "  -w, --watch     refresh until q / Ctrl-C\n"
        "  -n, --interval  seconds between refreshes (default %d)\n"
        "  --json          print one JSON object\n"
        "  --no-color      disable ANSI colors\n",
        argv0, WATCH_DEFAULT_SEC);
}

int main(int argc, char **argv)
{
    int watch = 0, json = 0;
    int interval = WATCH_DEFAULT_SEC;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--watch")) watch = 1;
        else if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--no-color")) use_color = 0;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]); return 0;
        } else if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "--interval")) && i + 1 < argc) {
            interval = atoi(argv[++i]);
            if (interval < 1) interval = 1;
        } else {
            usage(argv[0]); return 2;
        }
    }
    if (!is_tty() || json) use_color = 0;

    if (json) {
        Snap s;
        collect(&s);
        render_json(&s);
        return 0;
    }

    if (!watch) {
        Snap s;
        collect(&s);
        render_text(&s);
        return 0;
    }

    /* Non-blocking stdin so 'q' works without Enter. */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    for (;;) {
        Snap s;
        collect(&s);
        if (is_tty()) fputs("\033[H\033[J", stdout); /* home + erase */
        render_text(&s);
        fflush(stdout);

        /* wait `interval` seconds, abort early on q */
        time_t end = time(NULL) + interval;
        while (time(NULL) < end) {
            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n == 1 && (c == 'q' || c == 'Q' || c == 3)) goto done;
            if (n == 1 && (c == 'r' || c == 'R')) break;
            struct timespec sl = { .tv_sec = 0, .tv_nsec = 80 * 1000000L };
            nanosleep(&sl, NULL);
        }
    }
done:
    if (is_tty()) fputs("\033[0m\n", stdout);
    return 0;
}
