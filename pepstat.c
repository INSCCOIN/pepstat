/*
 * pepstat — one-screen system card for PeppermintOS (and any Linux)
 *
 * Build:
 *   sudo apt install build-essential
 *   make
 *   ./pepstat
 *
 *   ./pepstat              print once and exit
 *   ./pepstat -w           refresh every 2 seconds (q quits)
 *   ./pepstat -w -n 1      watch, 1 second interval
 *   ./pepstat --json       machine-readable
 *   ./pepstat --no-color
 *
 * Reads only /proc, /sys and libc. No ncurses, GTK, or root.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#define WATCH_DEFAULT_SEC 2
#define CPU_SAMPLE_MS     120
#define WARN_PCT          80.0
#define CRIT_PCT          90.0
#define WARN_TEMP_C       75.0
#define CRIT_TEMP_C       90.0
#define WARN_BAT_PCT      20
#define CRIT_BAT_PCT      10

static int use_color = 1;

static int is_tty(void)
{
    return isatty(STDOUT_FILENO);
}

static void color(const char *code)
{
    if (use_color)
        fputs(code, stdout);
}

#define C_RESET "\033[0m"
#define C_DIM   "\033[2m"
#define C_BOLD  "\033[1m"
#define C_GREEN "\033[32m"
#define C_YEL   "\033[33m"
#define C_RED   "\033[31m"
#define C_CYAN  "\033[36m"

static const char *lvl_color(int lvl)
{
    if (lvl == 2)
        return C_RED;
    if (lvl == 1)
        return C_YEL;
    if (lvl == 0)
        return C_GREEN;
    return C_RESET;
}

static int pct_level(double pct)
{
    if (pct >= CRIT_PCT)
        return 2;
    if (pct >= WARN_PCT)
        return 1;
    return 0;
}

static void copy_str(char *dst, size_t dstsz, const char *src)
{
    size_t i;
    if (!dst || dstsz == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (i = 0; i + 1 < dstsz && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int read_file(const char *path, char *buf, size_t n)
{
    FILE *f;
    if (!path || !buf || n < 2)
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

static int read_key_file(const char *path, const char *key, char *out, size_t n)
{
    FILE *f;
    char line[512];
    size_t klen;

    if (!path || !key || !out || n < 2)
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;
    klen = strlen(key);
    while (fgets(line, (int)sizeof line, f)) {
        if (strncmp(line, key, klen) == 0 &&
            (line[klen] == '=' || line[klen] == ':')) {
            char *v = line + klen + 1;
            while (*v == ' ' || *v == '\t' || *v == '"')
                v++;
            copy_str(out, n, v);
            out[strcspn(out, "\r\n\"")] = '\0';
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static void human_bytes(double bytes, char *out, size_t n)
{
    static const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int i = 0;
    if (bytes < 0)
        bytes = 0;
    while (bytes >= 1024.0 && i < 4) {
        bytes /= 1024.0;
        i++;
    }
    if (i == 0)
        snprintf(out, n, "%.0f %s", bytes, u[i]);
    else
        snprintf(out, n, "%.1f %s", bytes, u[i]);
}

static void human_secs(double secs, char *out, size_t n)
{
    unsigned long s = (unsigned long)secs;
    unsigned long d, h, m;

    d = s / 86400UL;
    s %= 86400UL;
    h = s / 3600UL;
    s %= 3600UL;
    m = s / 60UL;
    s %= 60UL;
    if (d)
        snprintf(out, n, "%lud %luh %lum", d, h, m);
    else if (h)
        snprintf(out, n, "%luh %lum", h, m);
    else
        snprintf(out, n, "%lum %lus", m, s);
}

static void bar(double pct, int width)
{
    int fill, i, lvl;

    if (width < 4)
        width = 4;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    fill = (int)((pct / 100.0) * width + 0.5);
    lvl = pct_level(pct);
    color(lvl_color(lvl));
    fputc('[', stdout);
    for (i = 0; i < width; i++)
        fputc(i < fill ? '#' : '-', stdout);
    fputc(']', stdout);
    color(C_RESET);
}

typedef struct {
    char hostname[128];
    char pretty_os[128];
    char kernel[160];
    char cpu_model[160];
    int cpu_cores;
    double load1, load5, load15;
    double cpu_pct;
    double uptime_sec;
    unsigned long mem_total_kb, mem_avail_kb;
    unsigned long swap_total_kb, swap_free_kb;
    unsigned long disk_total, disk_free;
    int have_bat;
    int bat_pct;
    char bat_status[32];
    int have_temp;
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
    copy_str(s->pretty_os, sizeof s->pretty_os, "Linux");
    copy_str(s->cpu_model, sizeof s->cpu_model, "unknown");
    copy_str(s->bat_status, sizeof s->bat_status, "n/a");
    copy_str(s->iface, sizeof s->iface, "-");
    copy_str(s->ipv4, sizeof s->ipv4, "-");
    copy_str(s->init, sizeof s->init, "unknown");
    copy_str(s->session, sizeof s->session, "-");
    copy_str(s->display, sizeof s->display, "-");
    s->cpu_pct = -1;
    s->cpu_cores = 1;
}

static void collect_identity(Snap *s)
{
    struct utsname u;
    FILE *f;
    char line[256];
    int cores = 0;

    if (gethostname(s->hostname, sizeof s->hostname) != 0)
        copy_str(s->hostname, sizeof s->hostname, "unknown");
    s->hostname[sizeof s->hostname - 1] = '\0';

    read_key_file("/etc/os-release", "PRETTY_NAME", s->pretty_os, sizeof s->pretty_os);

    if (uname(&u) == 0)
        snprintf(s->kernel, sizeof s->kernel, "%s %s", u.sysname, u.release);

    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        while (fgets(line, (int)sizeof line, f)) {
            if (strncmp(line, "model name", 10) == 0 &&
                strcmp(s->cpu_model, "unknown") == 0) {
                char *p = strchr(line, ':');
                if (p) {
                    p++;
                    while (*p == ' ')
                        p++;
                    copy_str(s->cpu_model, sizeof s->cpu_model, p);
                    s->cpu_model[strcspn(s->cpu_model, "\r\n")] = '\0';
                }
            }
            if (strncmp(line, "processor", 9) == 0)
                cores++;
        }
        fclose(f);
    }
    if (cores == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        cores = (n > 0) ? (int)n : 1;
    }
    s->cpu_cores = cores;
    if (strcmp(s->cpu_model, "unknown") == 0) {
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
    FILE *f;
    char cpu[8];
    unsigned long long user, nice, sys, id, iw, irq, sirq, st;
    int ok;

    f = fopen("/proc/stat", "r");
    if (!f)
        return -1;
    ok = fscanf(f, "%7s %llu %llu %llu %llu %llu %llu %llu %llu",
                cpu, &user, &nice, &sys, &id, &iw, &irq, &sirq, &st);
    fclose(f);
    if (ok < 5)
        return -1;
    *idle = id + iw;
    *total = user + nice + sys + id + iw + irq + sirq + st;
    return 0;
}

static void collect_cpu_pct(Snap *s)
{
    unsigned long long i1, t1, i2, t2;
    struct timespec ts;
    unsigned long long dt, di;

    if (read_cpu_times(&i1, &t1) != 0)
        return;
    ts.tv_sec = 0;
    ts.tv_nsec = (long)CPU_SAMPLE_MS * 1000000L;
    nanosleep(&ts, NULL);
    if (read_cpu_times(&i2, &t2) != 0)
        return;
    dt = t2 - t1;
    di = i2 - i1;
    if (dt == 0)
        return;
    s->cpu_pct = 100.0 * (1.0 - (double)di / (double)dt);
    if (s->cpu_pct < 0)
        s->cpu_pct = 0;
}

static void collect_mem(Snap *s)
{
    FILE *f;
    char key[64];
    unsigned long val;
    char unit[16];

    f = fopen("/proc/meminfo", "r");
    if (!f)
        return;
    while (fscanf(f, "%63s %lu %15s", key, &val, unit) == 3) {
        if (strcmp(key, "MemTotal:") == 0)
            s->mem_total_kb = val;
        else if (strcmp(key, "MemAvailable:") == 0)
            s->mem_avail_kb = val;
        else if (strcmp(key, "SwapTotal:") == 0)
            s->swap_total_kb = val;
        else if (strcmp(key, "SwapFree:") == 0)
            s->swap_free_kb = val;
    }
    fclose(f);
}

static void collect_disk(Snap *s)
{
    struct statvfs v;
    if (statvfs("/", &v) == 0) {
        s->disk_total = (unsigned long)v.f_blocks * (unsigned long)v.f_frsize;
        s->disk_free = (unsigned long)v.f_bavail * (unsigned long)v.f_frsize;
    }
}

static void collect_battery(Snap *s)
{
    DIR *d;
    struct dirent *e;

    d = opendir("/sys/class/power_supply");
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        char path[320], buf[64];

        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "/sys/class/power_supply/%.80s/type", e->d_name);
        if (read_file(path, buf, sizeof buf) != 0)
            continue;
        if (strcasecmp(buf, "Battery") != 0)
            continue;
        snprintf(path, sizeof path, "/sys/class/power_supply/%.80s/capacity", e->d_name);
        if (read_file(path, buf, sizeof buf) == 0) {
            s->have_bat = 1;
            s->bat_pct = atoi(buf);
        }
        snprintf(path, sizeof path, "/sys/class/power_supply/%.80s/status", e->d_name);
        if (read_file(path, s->bat_status, sizeof s->bat_status) != 0)
            copy_str(s->bat_status, sizeof s->bat_status, "Unknown");
        break;
    }
    closedir(d);
}

static void collect_temp(Snap *s)
{
    static const char *prefer[] = {
        "x86_pkg_id", "cpu-thermal", "soc-thermal", "acpitz", NULL
    };
    DIR *d;
    struct dirent *e;
    double best = 0;
    int found = 0;

    d = opendir("/sys/class/thermal");
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        char typep[320], tempp[320], typ[64], tbuf[32];
        double c;
        int pref = 0;
        int i;

        if (strncmp(e->d_name, "thermal_zone", 12) != 0)
            continue;
        snprintf(typep, sizeof typep, "/sys/class/thermal/%.80s/type", e->d_name);
        snprintf(tempp, sizeof tempp, "/sys/class/thermal/%.80s/temp", e->d_name);
        if (read_file(typep, typ, sizeof typ) != 0)
            continue;
        if (read_file(tempp, tbuf, sizeof tbuf) != 0)
            continue;
        c = atof(tbuf) / 1000.0;
        if (c <= 0 || c > 150)
            continue;
        for (i = 0; prefer[i] != NULL; i++) {
            if (strcmp(typ, prefer[i]) == 0)
                pref = 1;
        }
        if (!found || pref) {
            found = 1;
            best = c;
            if (pref)
                break;
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
    FILE *f;
    char line[256], ifn[32];
    unsigned dest;
    struct ifaddrs *ifa = NULL, *p;

    f = fopen("/proc/net/route", "r");
    if (f) {
        if (fgets(line, (int)sizeof line, f) != NULL) {
            while (fgets(line, (int)sizeof line, f)) {
                if (sscanf(line, "%31s %X", ifn, &dest) == 2 && dest == 0) {
                    copy_str(s->iface, sizeof s->iface, ifn);
                    break;
                }
            }
        }
        fclose(f);
    }

    if (getifaddrs(&ifa) != 0)
        return;
    for (p = ifa; p != NULL; p = p->ifa_next) {
        struct sockaddr_in *in;

        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        if (s->iface[0] && s->iface[0] != '-' && strcmp(p->ifa_name, s->iface) != 0)
            continue;
        if (strcmp(p->ifa_name, "lo") == 0)
            continue;
        in = (struct sockaddr_in *)p->ifa_addr;
        inet_ntop(AF_INET, &in->sin_addr, s->ipv4, sizeof s->ipv4);
        if (s->iface[0] == '-' || s->iface[0] == '\0')
            copy_str(s->iface, sizeof s->iface, p->ifa_name);
        if (s->iface[0] != '-')
            break;
    }
    freeifaddrs(ifa);
}

static void collect_session(Snap *s)
{
    char comm[64];
    const char *st;
    const char *disp;

    if (read_file("/proc/1/comm", comm, sizeof comm) == 0) {
        if (strcmp(comm, "systemd") == 0) {
            copy_str(s->init, sizeof s->init, "systemd");
        } else if (strcmp(comm, "init") == 0) {
            if (access("/etc/runit", F_OK) == 0)
                copy_str(s->init, sizeof s->init, "runit");
            else if (access("/run/openrc", F_OK) == 0)
                copy_str(s->init, sizeof s->init, "OpenRC");
            else
                copy_str(s->init, sizeof s->init, "sysvinit");
        } else {
            copy_str(s->init, sizeof s->init, comm);
        }
    }
    if (access("/run/systemd/system", F_OK) == 0)
        copy_str(s->init, sizeof s->init, "systemd");

    st = getenv("XDG_SESSION_TYPE");
    if (st)
        copy_str(s->session, sizeof s->session, st);
    else if (getenv("WAYLAND_DISPLAY"))
        copy_str(s->session, sizeof s->session, "wayland");
    else if (getenv("DISPLAY"))
        copy_str(s->session, sizeof s->session, "x11");

    disp = getenv("DISPLAY");
    if (disp)
        copy_str(s->display, sizeof s->display, disp);
    else if ((disp = getenv("WAYLAND_DISPLAY")) != NULL)
        copy_str(s->display, sizeof s->display, disp);
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

static void rule(int w)
{
    int i;
    color(C_DIM);
    for (i = 0; i < w; i++)
        fputc('-', stdout);
    color(C_RESET);
    fputc('\n', stdout);
}

static void kv(const char *k, const char *v)
{
    color(C_DIM);
    printf("  %-10s", k);
    color(C_RESET);
    printf("%s\n", v);
}

static void render_text(const Snap *s)
{
    char buf[256], a[64], b[64];
    int cols = 72;
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 40)
        cols = ws.ws_col;
    if (cols > 88)
        cols = 88;

    color(C_BOLD);
    color(C_CYAN);
    printf(" pepstat");
    color(C_RESET);
    color(C_DIM);
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
        color(C_DIM);
        printf("  %-10s", "busy");
        color(C_RESET);
        bar(s->cpu_pct, 28);
        printf("  %.0f%%\n", s->cpu_pct);
    }
    {
        double per = (s->cpu_cores > 0) ? (s->load1 / s->cpu_cores) : s->load1;
        int lvl = (per >= 1.5) ? 2 : (per >= 1.0) ? 1 : 0;
        color(C_DIM);
        printf("  %-10s", "load");
        color(C_RESET);
        color(lvl_color(lvl));
        printf("%.2f  %.2f  %.2f", s->load1, s->load5, s->load15);
        color(C_RESET);
        color(C_DIM);
        printf("   (1 / 5 / 15 min)\n");
        color(C_RESET);
    }

    if (s->have_temp) {
        int lvl = (s->temp_c >= CRIT_TEMP_C) ? 2 : (s->temp_c >= WARN_TEMP_C) ? 1 : 0;
        color(C_DIM);
        printf("  %-10s", "temp");
        color(C_RESET);
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
        color(C_DIM);
        printf("  %-10s", "memory");
        color(C_RESET);
        bar(pct, 28);
        printf("  %s / %s  (%.0f%%)\n", a, b, pct);
    }
    if (s->swap_total_kb) {
        double used = (double)(s->swap_total_kb - s->swap_free_kb);
        double pct = 100.0 * used / (double)s->swap_total_kb;
        human_bytes(used * 1024.0, a, sizeof a);
        human_bytes((double)s->swap_total_kb * 1024.0, b, sizeof b);
        color(C_DIM);
        printf("  %-10s", "swap");
        color(C_RESET);
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
        color(C_DIM);
        printf("  %-10s", "disk /");
        color(C_RESET);
        bar(pct, 28);
        printf("  %s / %s  (%.0f%%)\n", a, b, pct);
    }

    rule(cols);

    snprintf(buf, sizeof buf, "%s  %s", s->iface, s->ipv4);
    kv("net", buf);

    if (s->have_bat) {
        int lvl = 0;
        if (strcasecmp(s->bat_status, "Discharging") == 0) {
            if (s->bat_pct <= CRIT_BAT_PCT)
                lvl = 2;
            else if (s->bat_pct <= WARN_BAT_PCT)
                lvl = 1;
        }
        color(C_DIM);
        printf("  %-10s", "battery");
        color(C_RESET);
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
    if (s->have_temp)
        printf("  \"temp_c\": %.1f,\n", s->temp_c);
    else
        printf("  \"temp_c\": null,\n");
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
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--watch") == 0) {
            watch = 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            json = 1;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            use_color = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--interval") == 0) &&
                   i + 1 < argc) {
            interval = atoi(argv[++i]);
            if (interval < 1)
                interval = 1;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!is_tty() || json)
        use_color = 0;

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

    {
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (flags >= 0)
            fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

    for (;;) {
        Snap s;
        time_t end;
        collect(&s);
        if (is_tty())
            fputs("\033[H\033[J", stdout);
        render_text(&s);
        fflush(stdout);

        end = time(NULL) + interval;
        while (time(NULL) < end) {
            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            struct timespec sl;
            if (n == 1 && (c == 'q' || c == 'Q' || c == 3))
                goto done;
            if (n == 1 && (c == 'r' || c == 'R'))
                break;
            sl.tv_sec = 0;
            sl.tv_nsec = 80L * 1000000L;
            nanosleep(&sl, NULL);
        }
    }
done:
    if (is_tty())
        fputs("\033[0m\n", stdout);
    return 0;
}
