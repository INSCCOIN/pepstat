/*
 * pepstat v3 — PeppermintOS system HUD + process viewer
 *
 *   sudo apt install build-essential
 *   make
 *   ./pepstat                 dashboard once
 *   ./pepstat -w              live HUD (no flicker)
 *   ./pepstat -p              process table once
 *   ./pepstat --plain         one line for XFCE Generic Monitor
 *   ./pepstat --json
 *
 * Live keys
 *   1 dash   2 procs   c/m sort   / filter   j/k move
 *   t TERM   K KILL    r refresh  q quit
 *
 * No ncurses, no GTK, no root. /proc + /sys + libc only.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <pwd.h>
#include <signal.h>
#include <termios.h>
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
#define CPU_SAMPLE_MS     80
#define WARN_PCT          80.0
#define CRIT_PCT          90.0
#define WARN_TEMP_C       75.0
#define CRIT_TEMP_C       90.0
#define WARN_BAT_PCT      20
#define CRIT_BAT_PCT      10
#define MAX_PROCS         512
#define VIEW_PROCS        16
#define PREV_SLOTS        512
#define FILTER_MAX        40

enum { PAGE_DASH = 0, PAGE_PROC = 1 };
enum { SORT_CPU = 0, SORT_RSS = 1 };

#define RGB_MINT    61, 204, 122
#define RGB_MINT2  150, 237, 103
#define RGB_LEAF    26, 160,  90
#define RGB_ICE    109, 227, 248
#define RGB_ICE2   142, 247, 255
#define RGB_FOG    168, 190, 178
#define RGB_SLATE  110, 130, 122
#define RGB_AMBER  255, 196,  64
#define RGB_ROSE   255,  79, 107
#define RGB_WHITE  236, 250, 240
#define RGB_SELBG   20,  70,  48

static int use_color = 1;
static int use_unicode = 1;
static int page = PAGE_DASH;
static int sort_mode = SORT_CPU;
static int live_mode = 0;

static char filter[FILTER_MAX];
static int filter_edit;
static int sel;
static int scroll;
static int selected_pid;
static int confirm_sig;
static char status_msg[160];
static int view_idx[MAX_PROCS];
static int nview;

static struct termios term_orig;
static int term_raw;

static int is_tty(void) { return isatty(STDOUT_FILENO); }

static void reset_col(void)
{
    if (use_color)
        fputs("\033[0m", stdout);
}

static void fg(int r, int g, int b)
{
    if (use_color)
        printf("\033[38;2;%d;%d;%dm", r, g, b);
}

static void bg(int r, int g, int b)
{
    if (use_color)
        printf("\033[48;2;%d;%d;%dm", r, g, b);
}

static void bold(void)
{
    if (use_color)
        fputs("\033[1m", stdout);
}

static void nl(void)
{
    if (live_mode)
        fputs("\033[K", stdout);
    fputc('\n', stdout);
}

static int pct_level(double pct)
{
    if (pct >= CRIT_PCT) return 2;
    if (pct >= WARN_PCT) return 1;
    return 0;
}

static void fg_level(int lvl)
{
    if (lvl == 2) fg(RGB_ROSE);
    else if (lvl == 1) fg(RGB_AMBER);
    else fg(RGB_MINT);
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

static int ci_contains(const char *hay, const char *needle)
{
    size_t n, h, i, j;
    if (!needle || !needle[0])
        return 1;
    if (!hay)
        return 0;
    n = strlen(needle);
    h = strlen(hay);
    if (n > h)
        return 0;
    for (i = 0; i + n <= h; i++) {
        for (j = 0; j < n; j++) {
            if (tolower((unsigned char)hay[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        }
        if (j == n)
            return 1;
    }
    return 0;
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
    static const char *u[] = {"B", "K", "M", "G", "T"};
    int i = 0;
    if (bytes < 0)
        bytes = 0;
    while (bytes >= 1024.0 && i < 4) {
        bytes /= 1024.0;
        i++;
    }
    if (i == 0)
        snprintf(out, n, "%.0f%s", bytes, u[i]);
    else if (bytes >= 10)
        snprintf(out, n, "%.0f%s", bytes, u[i]);
    else
        snprintf(out, n, "%.1f%s", bytes, u[i]);
}

static void human_secs(double secs, char *out, size_t n)
{
    unsigned long s = (unsigned long)secs;
    unsigned long d, h, m;
    d = s / 86400UL; s %= 86400UL;
    h = s / 3600UL;  s %= 3600UL;
    m = s / 60UL;    s %= 60UL;
    if (d)
        snprintf(out, n, "%lud %02luh %02lum", d, h, m);
    else if (h)
        snprintf(out, n, "%02luh %02lum %02lus", h, m, s);
    else
        snprintf(out, n, "%02lum %02lus", m, s);
}

static void bar(double pct, int width)
{
    int i, lvl;
    const char *blocks[] = {" ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"};

    if (width < 6)
        width = 6;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lvl = pct_level(pct);
    fg_level(lvl);

    if (!use_unicode) {
        int f = (int)((pct / 100.0) * width + 0.5);
        fputc('[', stdout);
        for (i = 0; i < width; i++)
            fputc(i < f ? '#' : '-', stdout);
        fputc(']', stdout);
        reset_col();
        return;
    }

    fputc('[', stdout);
    {
        double cells = (pct / 100.0) * width;
        int full = (int)cells;
        int frac = (int)((cells - full) * 8.0 + 0.5);
        if (frac > 8) frac = 8;
        for (i = 0; i < width; i++) {
            if (i < full)
                fputs("█", stdout);
            else if (i == full && frac > 0)
                fputs(blocks[frac], stdout);
            else {
                fg(RGB_SLATE);
                fputs("░", stdout);
                fg_level(lvl);
            }
        }
    }
    fputc(']', stdout);
    reset_col();
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
    unsigned long home_total, home_free;
    int have_home;
    int have_bat, bat_pct;
    char bat_status[32];
    int have_temp;
    double temp_c;
    char iface[32];
    char ipv4[64];
    char init[32];
    char session[32];
    char display[32];
    int nprocs;
    int nthreads;
    unsigned long long cpu_total;
    unsigned long long cpu_idle;
} Snap;

typedef struct {
    int pid;
    char name[40];
    char user[16];
    char state;
    unsigned long rss_kb;
    unsigned long ticks;
    double cpu_pct;
} Proc;

typedef struct {
    int pid;
    unsigned long ticks;
} PrevProc;

static PrevProc prev_tab[PREV_SLOTS];
static int prev_n;
static unsigned long long prev_cpu_total;
static unsigned long long prev_cpu_idle;
static int have_prev_cpu;
static struct timespec prev_mono;

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
                    while (*p == ' ') p++;
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
    FILE *f = fopen("/proc/stat", "r");
    char cpu[8];
    unsigned long long user, nice, sys, id, iw, irq, sirq, st;
    int ok;
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

static void collect_cpu_pct2(Snap *s)
{
    unsigned long long idle, total;
    if (read_cpu_times(&idle, &total) != 0)
        return;
    s->cpu_idle = idle;
    s->cpu_total = total;
    if (have_prev_cpu && total > prev_cpu_total) {
        unsigned long long dt = total - prev_cpu_total;
        unsigned long long di = idle - prev_cpu_idle;
        s->cpu_pct = 100.0 * (1.0 - (double)di / (double)dt);
        if (s->cpu_pct < 0) s->cpu_pct = 0;
        if (s->cpu_pct > 100) s->cpu_pct = 100;
    } else {
        unsigned long long i2, t2;
        struct timespec ts = {0, (long)CPU_SAMPLE_MS * 1000000L};
        nanosleep(&ts, NULL);
        if (read_cpu_times(&i2, &t2) == 0 && t2 > total) {
            s->cpu_pct = 100.0 * (1.0 - (double)(i2 - idle) / (double)(t2 - total));
            s->cpu_total = t2;
            s->cpu_idle = i2;
            if (s->cpu_pct < 0) s->cpu_pct = 0;
        }
    }
}

static void collect_mem(Snap *s)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char key[64], unit[16];
    unsigned long val;
    if (!f)
        return;
    while (fscanf(f, "%63s %lu %15s", key, &val, unit) == 3) {
        if (strcmp(key, "MemTotal:") == 0) s->mem_total_kb = val;
        else if (strcmp(key, "MemAvailable:") == 0) s->mem_avail_kb = val;
        else if (strcmp(key, "SwapTotal:") == 0) s->swap_total_kb = val;
        else if (strcmp(key, "SwapFree:") == 0) s->swap_free_kb = val;
    }
    fclose(f);
}

static int fill_statvfs(const char *path, unsigned long *total, unsigned long *freeb)
{
    struct statvfs v;
    if (!path || statvfs(path, &v) != 0)
        return -1;
    *total = (unsigned long)v.f_blocks * (unsigned long)v.f_frsize;
    *freeb = (unsigned long)v.f_bavail * (unsigned long)v.f_frsize;
    return 0;
}

static void collect_disk(Snap *s)
{
    const char *home;
    fill_statvfs("/", &s->disk_total, &s->disk_free);
    home = getenv("HOME");
    if (home && fill_statvfs(home, &s->home_total, &s->home_free) == 0)
        s->have_home = 1;
}

static void collect_battery(Snap *s)
{
    DIR *d = opendir("/sys/class/power_supply");
    struct dirent *e;
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
    static const char *prefer[] = {"x86_pkg_id", "cpu-thermal", "soc-thermal", "acpitz", NULL};
    DIR *d = opendir("/sys/class/thermal");
    struct dirent *e;
    double best = 0;
    int found = 0;
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        char typep[320], tempp[320], typ[64], tbuf[32];
        double c;
        int pref = 0, i;
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
        for (i = 0; prefer[i]; i++)
            if (strcmp(typ, prefer[i]) == 0)
                pref = 1;
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
    FILE *f = fopen("/proc/net/route", "r");
    char line[256], ifn[32];
    unsigned dest;
    struct ifaddrs *ifa = NULL, *p;

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
    for (p = ifa; p; p = p->ifa_next) {
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
    const char *st, *disp;

    if (read_file("/proc/1/comm", comm, sizeof comm) == 0) {
        if (strcmp(comm, "systemd") == 0)
            copy_str(s->init, sizeof s->init, "systemd");
        else if (strcmp(comm, "init") == 0) {
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

static unsigned long prev_lookup(int pid)
{
    int i;
    for (i = 0; i < prev_n; i++)
        if (prev_tab[i].pid == pid)
            return prev_tab[i].ticks;
    return 0;
}

static int collect_procs(Proc *list, int max, Snap *s, double elapsed)
{
    DIR *d;
    struct dirent *e;
    int n = 0, threads = 0;
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0)
        hz = 100;

    d = opendir("/proc");
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        char path[96], buf[512];
        FILE *f;
        char *lpar, *rpar;
        int pid;
        char state;
        unsigned long utime = 0, stime = 0, dummy;
        unsigned long ticks, rss_kb = 0;
        Proc *p;
        struct passwd *pw;
        uid_t uid = (uid_t)-1;

        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        pid = atoi(e->d_name);
        if (pid <= 0)
            continue;

        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        f = fopen(path, "r");
        if (!f)
            continue;
        if (!fgets(buf, (int)sizeof buf, f)) {
            fclose(f);
            continue;
        }
        fclose(f);
        lpar = strchr(buf, '(');
        rpar = strrchr(buf, ')');
        if (!lpar || !rpar || rpar <= lpar)
            continue;
        if (sscanf(rpar + 1,
                   " %c %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                   &state, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy,
                   &dummy, &dummy, &dummy, &dummy, &utime, &stime) < 13)
            continue;
        ticks = utime + stime;

        snprintf(path, sizeof path, "/proc/%d/status", pid);
        f = fopen(path, "r");
        if (f) {
            char line[256];
            while (fgets(line, (int)sizeof line, f)) {
                if (strncmp(line, "VmRSS:", 6) == 0)
                    rss_kb = strtoul(line + 6, NULL, 10);
                else if (strncmp(line, "Uid:", 4) == 0)
                    uid = (uid_t)strtoul(line + 4, NULL, 10);
                else if (strncmp(line, "Threads:", 8) == 0)
                    threads += (int)strtol(line + 8, NULL, 10);
            }
            fclose(f);
        }

        if (n >= max)
            continue;
        p = &list[n];
        memset(p, 0, sizeof *p);
        p->pid = pid;
        p->state = state;
        p->rss_kb = rss_kb;
        p->ticks = ticks;
        {
            size_t clen = (size_t)(rpar - lpar - 1);
            if (clen >= sizeof p->name)
                clen = sizeof p->name - 1;
            memcpy(p->name, lpar + 1, clen);
            p->name[clen] = '\0';
        }
        pw = (uid != (uid_t)-1) ? getpwuid(uid) : NULL;
        if (pw)
            copy_str(p->user, sizeof p->user, pw->pw_name);
        else
            snprintf(p->user, sizeof p->user, "%d", (int)uid);

        if (elapsed > 0.05) {
            unsigned long old = prev_lookup(pid);
            if (old && ticks >= old)
                p->cpu_pct = 100.0 * (double)(ticks - old) / ((double)hz * elapsed);
            else
                p->cpu_pct = 0;
        } else {
            p->cpu_pct = 0;
        }
        if (p->cpu_pct > 999)
            p->cpu_pct = 999;
        n++;
    }
    closedir(d);
    s->nprocs = n;
    s->nthreads = threads;
    return n;
}

static void store_prev(const Proc *list, int n, const Snap *s)
{
    int i, m = n < PREV_SLOTS ? n : PREV_SLOTS;
    for (i = 0; i < m; i++) {
        prev_tab[i].pid = list[i].pid;
        prev_tab[i].ticks = list[i].ticks;
    }
    prev_n = m;
    prev_cpu_total = s->cpu_total;
    prev_cpu_idle = s->cpu_idle;
    have_prev_cpu = 1;
    clock_gettime(CLOCK_MONOTONIC, &prev_mono);
}

static int cmp_cpu(const void *a, const void *b)
{
    const Proc *pa = a, *pb = b;
    if (pb->cpu_pct > pa->cpu_pct) return 1;
    if (pb->cpu_pct < pa->cpu_pct) return -1;
    if (pb->rss_kb > pa->rss_kb) return 1;
    if (pb->rss_kb < pa->rss_kb) return -1;
    return pa->pid - pb->pid;
}

static int cmp_rss(const void *a, const void *b)
{
    const Proc *pa = a, *pb = b;
    if (pb->rss_kb > pa->rss_kb) return 1;
    if (pb->rss_kb < pa->rss_kb) return -1;
    if (pb->cpu_pct > pa->cpu_pct) return 1;
    if (pb->cpu_pct < pa->cpu_pct) return -1;
    return pa->pid - pb->pid;
}

static int proc_match(const Proc *p)
{
    char pidbuf[16];
    if (!filter[0])
        return 1;
    snprintf(pidbuf, sizeof pidbuf, "%d", p->pid);
    return ci_contains(p->name, filter) ||
           ci_contains(p->user, filter) ||
           ci_contains(pidbuf, filter);
}

static void rebuild_view(const Proc *procs, int nprocs)
{
    int i, found = -1;
    nview = 0;
    for (i = 0; i < nprocs; i++) {
        if (!proc_match(&procs[i]))
            continue;
        if (nview < MAX_PROCS)
            view_idx[nview++] = i;
    }
    if (selected_pid) {
        for (i = 0; i < nview; i++) {
            if (procs[view_idx[i]].pid == selected_pid) {
                found = i;
                break;
            }
        }
    }
    if (found >= 0)
        sel = found;
    if (sel >= nview)
        sel = nview ? nview - 1 : 0;
    if (sel < 0)
        sel = 0;
    if (sel < scroll)
        scroll = sel;
    if (sel >= scroll + VIEW_PROCS)
        scroll = sel - VIEW_PROCS + 1;
    if (scroll < 0)
        scroll = 0;
    if (nview)
        selected_pid = procs[view_idx[sel]].pid;
}

static void collect(Snap *s, Proc *procs, int *nprocs)
{
    struct timespec now_mono;
    double elapsed = 0;

    snap_init(s);
    collect_identity(s);
    collect_load_uptime(s);
    collect_cpu_pct2(s);
    collect_mem(s);
    collect_disk(s);
    collect_battery(s);
    collect_temp(s);
    collect_net(s);
    collect_session(s);

    clock_gettime(CLOCK_MONOTONIC, &now_mono);
    if (prev_mono.tv_sec || prev_mono.tv_nsec) {
        elapsed = (double)(now_mono.tv_sec - prev_mono.tv_sec) +
                  (double)(now_mono.tv_nsec - prev_mono.tv_nsec) / 1e9;
    }
    *nprocs = collect_procs(procs, MAX_PROCS, s, elapsed);
    if (sort_mode == SORT_RSS)
        qsort(procs, (size_t)*nprocs, sizeof *procs, cmp_rss);
    else
        qsort(procs, (size_t)*nprocs, sizeof *procs, cmp_cpu);
    store_prev(procs, *nprocs, s);
    rebuild_view(procs, *nprocs);
}

static int term_cols(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 60)
        return ws.ws_col > 120 ? 120 : ws.ws_col;
    return 80;
}

static void frame_begin(void)
{
    fputs("\033[H", stdout);
}

static void frame_end(void)
{
    fputs("\033[J", stdout);
    fflush(stdout);
}

static void live_enter(void)
{
    if (!is_tty())
        return;
    if (tcgetattr(STDIN_FILENO, &term_orig) == 0) {
        struct termios raw = term_orig;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        term_raw = 1;
    }
    fputs("\033[?1049h\033[?25l\033[H\033[2J", stdout);
    fflush(stdout);
    live_mode = 1;
}

static void live_leave(void)
{
    if (term_raw)
        tcsetattr(STDIN_FILENO, TCSANOW, &term_orig);
    term_raw = 0;
    if (is_tty())
        fputs("\033[0m\033[?25h\033[?1049l", stdout);
    live_mode = 0;
}

static void hrule(int w)
{
    int i;
    fg(RGB_LEAF);
    fputs("  ", stdout);
    if (use_unicode) {
        for (i = 0; i < w - 2; i++)
            fputs("─", stdout);
    } else {
        for (i = 0; i < w - 2; i++)
            fputc('-', stdout);
    }
    reset_col();
    nl();
}

static void label(const char *tag)
{
    fg(RGB_ICE);
    bold();
    printf("  %-8s", tag);
    reset_col();
    fputs("  ", stdout);
}

static void render_header(const Snap *s, int w, int watch)
{
    char clock[16], up[32];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    const char *pg = (page == PAGE_PROC) ? "PROC.VIEW" : "SYS.CARD";

    if (tm)
        strftime(clock, sizeof clock, "%H:%M:%S", tm);
    else
        copy_str(clock, sizeof clock, "--:--:--");
    human_secs(s->uptime_sec, up, sizeof up);

    fg(RGB_MINT);
    bold();
    if (use_unicode)
        printf("  ░▒▓");
    else
        printf("  ===");
    fg(RGB_MINT2);
    printf("  PEPSTAT");
    fg(RGB_FOG);
    printf("  //  %s  v3", pg);
    reset_col();
    fg(RGB_ICE);
    printf("    %s", clock);
    if (watch) {
        fg(RGB_MINT);
        printf("  [LIVE]");
    }
    reset_col();
    nl();

    fg(RGB_SLATE);
    printf("  host ");
    fg(RGB_WHITE);
    printf("%s", s->hostname);
    fg(RGB_SLATE);
    printf("   ·   ");
    fg(RGB_FOG);
    printf("%s", s->pretty_os);
    reset_col();
    nl();
    fg(RGB_SLATE);
    printf("  link ");
    fg(RGB_ICE);
    printf("%s", s->session);
    fg(RGB_SLATE);
    printf(" %s   ·   init ", s->display);
    fg(RGB_MINT2);
    printf("%s", s->init);
    fg(RGB_SLATE);
    printf("   ·   up %s", up);
    reset_col();
    nl();
    hrule(w);
}

static void render_footer(int w, int watch)
{
    hrule(w);
    if (confirm_sig) {
        fg(RGB_ROSE);
        bold();
        printf("  CONFIRM %s pid %d ?   y yes   n / esc cancel",
               confirm_sig == SIGKILL ? "KILL" : "TERM", selected_pid);
        reset_col();
        nl();
        return;
    }
    if (filter_edit) {
        fg(RGB_AMBER);
        printf("  FILTER> %s█   esc abort   enter apply", filter);
        reset_col();
        nl();
        return;
    }
    if (status_msg[0]) {
        fg(RGB_AMBER);
        printf("  %s", status_msg);
        reset_col();
        nl();
        return;
    }
    fg(RGB_SLATE);
    if (watch) {
        printf("  ");
        fg(RGB_MINT2); printf("1"); fg(RGB_SLATE); printf(" dash  ");
        fg(RGB_MINT2); printf("2"); fg(RGB_SLATE); printf(" procs  ");
        fg(RGB_MINT2); printf("c"); fg(RGB_SLATE); printf("/");
        fg(RGB_MINT2); printf("m"); fg(RGB_SLATE); printf(" sort  ");
        fg(RGB_MINT2); printf("/"); fg(RGB_SLATE); printf(" find  ");
        fg(RGB_MINT2); printf("j"); fg(RGB_SLATE); printf("/");
        fg(RGB_MINT2); printf("k"); fg(RGB_SLATE); printf(" move  ");
        fg(RGB_MINT2); printf("t"); fg(RGB_SLATE); printf(" term  ");
        fg(RGB_ROSE);  printf("K"); fg(RGB_SLATE); printf(" kill  ");
        fg(RGB_ROSE);  printf("q"); fg(RGB_SLATE); printf(" quit");
    } else {
        printf("  flags: -w live  -p procs  --plain  --json  --no-color  --ascii");
    }
    reset_col();
    nl();
    (void)w;
}

static void metric_row(const char *tag, double pct, const char *rhs)
{
    label(tag);
    bar(pct, 26);
    printf("  ");
    fg(RGB_WHITE);
    printf("%s", rhs);
    reset_col();
    nl();
}

static void render_dash(const Snap *s, const Proc *procs, int nprocs, int w, int watch)
{
    char a[32], b[32], rhs[256];
    int show, i;

    render_header(s, w, watch);

    if (s->cpu_pct >= 0) {
        snprintf(rhs, sizeof rhs, "%5.1f%%   %d cores   %s",
                 s->cpu_pct, s->cpu_cores, s->cpu_model);
        metric_row("CPU", s->cpu_pct, rhs);
    }
    {
        double per = s->cpu_cores > 0 ? s->load1 / s->cpu_cores : s->load1;
        int lvl = per >= 1.5 ? 2 : per >= 1.0 ? 1 : 0;
        label("LOAD");
        fg_level(lvl);
        printf("%5.2f  %5.2f  %5.2f", s->load1, s->load5, s->load15);
        reset_col();
        fg(RGB_SLATE);
        printf("    1 / 5 / 15 min");
        reset_col();
        nl();
    }
    if (s->have_temp) {
        int lvl = s->temp_c >= CRIT_TEMP_C ? 2 : s->temp_c >= WARN_TEMP_C ? 1 : 0;
        label("TEMP");
        fg_level(lvl);
        printf("%5.1f C", s->temp_c);
        reset_col();
        nl();
    }

    if (s->mem_total_kb) {
        double used = (double)(s->mem_total_kb - s->mem_avail_kb);
        double pct = 100.0 * used / (double)s->mem_total_kb;
        human_bytes(used * 1024.0, a, sizeof a);
        human_bytes((double)s->mem_total_kb * 1024.0, b, sizeof b);
        snprintf(rhs, sizeof rhs, "%s / %s   %.0f%%", a, b, pct);
        metric_row("MEM", pct, rhs);
    }
    if (s->swap_total_kb) {
        double used = (double)(s->swap_total_kb - s->swap_free_kb);
        double pct = 100.0 * used / (double)s->swap_total_kb;
        human_bytes(used * 1024.0, a, sizeof a);
        human_bytes((double)s->swap_total_kb * 1024.0, b, sizeof b);
        snprintf(rhs, sizeof rhs, "%s / %s   %.0f%%", a, b, pct);
        metric_row("SWAP", pct, rhs);
    } else {
        label("SWAP");
        fg(RGB_SLATE);
        printf("none");
        reset_col();
        nl();
    }
    if (s->disk_total) {
        double used = (double)(s->disk_total - s->disk_free);
        double pct = 100.0 * used / (double)s->disk_total;
        human_bytes(used, a, sizeof a);
        human_bytes((double)s->disk_total, b, sizeof b);
        snprintf(rhs, sizeof rhs, "%s / %s   %.0f%%", a, b, pct);
        metric_row("DISK", pct, rhs);
    }
    if (s->have_home && s->home_total) {
        double used = (double)(s->home_total - s->home_free);
        double pct = 100.0 * used / (double)s->home_total;
        human_bytes(used, a, sizeof a);
        human_bytes((double)s->home_total, b, sizeof b);
        snprintf(rhs, sizeof rhs, "%s / %s   %.0f%%", a, b, pct);
        metric_row("HOME", pct, rhs);
    }

    label("NET");
    fg(RGB_ICE2);
    printf("%s", s->iface);
    reset_col();
    fg(RGB_WHITE);
    printf("   %s", s->ipv4);
    reset_col();
    nl();

    if (s->have_bat) {
        int lvl = 0;
        if (strcasecmp(s->bat_status, "Discharging") == 0) {
            if (s->bat_pct <= CRIT_BAT_PCT) lvl = 2;
            else if (s->bat_pct <= WARN_BAT_PCT) lvl = 1;
        }
        snprintf(rhs, sizeof rhs, "%d%%   %s", s->bat_pct, s->bat_status);
        label("BATT");
        bar((double)s->bat_pct, 26);
        printf("  ");
        fg_level(lvl);
        printf("%s", rhs);
        reset_col();
        nl();
    }

    label("TASKS");
    fg(RGB_MINT2);
    printf("%d", s->nprocs);
    fg(RGB_SLATE);
    printf(" procs   ");
    fg(RGB_ICE);
    printf("%d", s->nthreads);
    fg(RGB_SLATE);
    printf(" threads");
    reset_col();
    nl();

    hrule(w);
    fg(RGB_ICE);
    printf("  TOP.%s", sort_mode == SORT_RSS ? "RSS" : "CPU");
    reset_col();
    nl();
    show = nprocs < 6 ? nprocs : 6;
    for (i = 0; i < show; i++) {
        human_bytes((double)procs[i].rss_kb * 1024.0, a, sizeof a);
        fg(RGB_SLATE);
        printf("  %5d  ", procs[i].pid);
        fg(RGB_FOG);
        printf("%-8s ", procs[i].user);
        if (procs[i].cpu_pct >= 50)
            fg(RGB_ROSE);
        else if (procs[i].cpu_pct >= 15)
            fg(RGB_AMBER);
        else
            fg(RGB_MINT);
        printf("%5.1f%% ", procs[i].cpu_pct);
        fg(RGB_ICE);
        printf("%6s  ", a);
        fg(RGB_WHITE);
        printf("%s", procs[i].name);
        reset_col();
        nl();
    }

    render_footer(w, watch);
}

static void render_proc(const Snap *s, const Proc *procs, int nprocs, int w, int watch)
{
    char a[64];
    int i, show, cols, namew, shown;
    const char *sortname = sort_mode == SORT_RSS ? "RSS" : "CPU";

    (void)nprocs;
    render_header(s, w, watch);
    fg(RGB_ICE);
    printf("  PROCESS MATRIX");
    fg(RGB_SLATE);
    printf("   sort=%s", sortname);
    if (filter[0]) {
        fg(RGB_AMBER);
        printf("   /%s", filter);
    }
    fg(RGB_SLATE);
    shown = nview < VIEW_PROCS ? nview : VIEW_PROCS;
    printf("   %d/%d", shown, nview);
    reset_col();
    nl();
    fg(RGB_LEAF);
    printf("    PID    USER       CPU     RSS   ST  NAME");
    reset_col();
    nl();

    cols = term_cols();
    namew = cols - 42;
    if (namew < 8)
        namew = 8;

    show = nview < scroll + VIEW_PROCS ? nview : scroll + VIEW_PROCS;
    for (i = scroll; i < show; i++) {
        const Proc *p = &procs[view_idx[i]];
        int on = (i == sel);
        human_bytes((double)p->rss_kb * 1024.0, a, sizeof a);
        if (on) {
            bg(RGB_SELBG);
            fg(RGB_MINT2);
            printf(" ▸");
        } else {
            printf("  ");
        }
        if (on)
            fg(RGB_WHITE);
        else
            fg(RGB_SLATE);
        printf(" %-5d  ", p->pid);
        fg(RGB_FOG);
        printf("%-8s  ", p->user);
        if (p->cpu_pct >= 50)
            fg(RGB_ROSE);
        else if (p->cpu_pct >= 15)
            fg(RGB_AMBER);
        else
            fg(RGB_MINT2);
        printf("%5.1f%%  ", p->cpu_pct);
        fg(RGB_ICE);
        printf("%6s  ", a);
        if (p->state == 'R')
            fg(RGB_MINT2);
        else if (p->state == 'Z' || p->state == 'X')
            fg(RGB_ROSE);
        else
            fg(RGB_SLATE);
        printf("%c   ", p->state);
        fg(RGB_WHITE);
        printf("%.*s", namew, p->name);
        reset_col();
        nl();
    }
    render_footer(w, watch);
}

static void render_plain(const Snap *s)
{
    double mem = 0, disk = 0, home = 0;
    if (s->mem_total_kb)
        mem = 100.0 * (double)(s->mem_total_kb - s->mem_avail_kb) / (double)s->mem_total_kb;
    if (s->disk_total)
        disk = 100.0 * (double)(s->disk_total - s->disk_free) / (double)s->disk_total;
    if (s->have_home && s->home_total)
        home = 100.0 * (double)(s->home_total - s->home_free) / (double)s->home_total;

    printf("cpu %.0f%%  mem %.0f%%  disk %.0f%%  home %.0f%%  load %.2f",
           s->cpu_pct < 0 ? 0 : s->cpu_pct, mem, disk, home, s->load1);
    if (s->have_bat)
        printf("  bat %d%%", s->bat_pct);
    if (s->have_temp)
        printf("  temp %.0fC", s->temp_c);
    printf("\n");
}

static void render_json(const Snap *s, const Proc *procs, int nprocs)
{
    int i, show = nprocs < 12 ? nprocs : 12;
    printf("{\n");
    printf("  \"hostname\": \"%s\",\n", s->hostname);
    printf("  \"os\": \"%s\",\n", s->pretty_os);
    printf("  \"kernel\": \"%s\",\n", s->kernel);
    printf("  \"init\": \"%s\",\n", s->init);
    printf("  \"session\": \"%s\",\n", s->session);
    printf("  \"uptime_sec\": %.0f,\n", s->uptime_sec);
    printf("  \"cpu_pct\": %.1f,\n", s->cpu_pct);
    printf("  \"cpu_cores\": %d,\n", s->cpu_cores);
    printf("  \"load\": [%.2f, %.2f, %.2f],\n", s->load1, s->load5, s->load15);
    printf("  \"mem_total_kb\": %lu,\n", s->mem_total_kb);
    printf("  \"mem_avail_kb\": %lu,\n", s->mem_avail_kb);
    printf("  \"disk_total\": %lu,\n", s->disk_total);
    printf("  \"disk_free\": %lu,\n", s->disk_free);
    printf("  \"home_total\": %lu,\n", s->home_total);
    printf("  \"home_free\": %lu,\n", s->home_free);
    printf("  \"nprocs\": %d,\n", s->nprocs);
    printf("  \"nthreads\": %d,\n", s->nthreads);
    printf("  \"top\": [\n");
    for (i = 0; i < show; i++) {
        printf("    {\"pid\": %d, \"user\": \"%s\", \"cpu\": %.1f, \"rss_kb\": %lu, \"name\": \"%s\"}%s\n",
               procs[i].pid, procs[i].user, procs[i].cpu_pct, procs[i].rss_kb,
               procs[i].name, i + 1 < show ? "," : "");
    }
    printf("  ]\n}\n");
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-w] [-p] [--plain] [-n SEC] [--json] [--no-color] [--ascii] [-h]\n"
            "  -w, --watch     live HUD (no flicker)\n"
            "  -p, --procs     process table once\n"
            "  --plain         one line for XFCE Generic Monitor\n"
            "  -n SEC          refresh interval (default %d)\n"
            " live: 1 dash  2 procs  / find  j/k move  t TERM  K KILL\n",
            argv0, WATCH_DEFAULT_SEC);
}

static volatile sig_atomic_t got_winch;
static volatile sig_atomic_t got_exit;

static void on_winch(int sig) { (void)sig; got_winch = 1; }
static void on_exit_sig(int sig) { (void)sig; got_exit = 1; }

static void move_sel(int delta, const Proc *procs)
{
    if (!nview)
        return;
    sel += delta;
    if (sel < 0) sel = 0;
    if (sel >= nview) sel = nview - 1;
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + VIEW_PROCS) scroll = sel - VIEW_PROCS + 1;
    selected_pid = procs[view_idx[sel]].pid;
    page = PAGE_PROC;
}

static void request_signal(int sig, const Proc *procs)
{
    if (page != PAGE_PROC) {
        page = PAGE_PROC;
        return;
    }
    if (!nview) {
        copy_str(status_msg, sizeof status_msg, "no process selected");
        return;
    }
    selected_pid = procs[view_idx[sel]].pid;
    if (selected_pid <= 1) {
        copy_str(status_msg, sizeof status_msg, "refused: will not signal pid 1");
        return;
    }
    confirm_sig = sig;
}

static void apply_signal(void)
{
    if (!confirm_sig || selected_pid <= 1) {
        confirm_sig = 0;
        return;
    }
    if (kill((pid_t)selected_pid, confirm_sig) == 0)
        snprintf(status_msg, sizeof status_msg, "sent %s to pid %d",
                 confirm_sig == SIGKILL ? "KILL" : "TERM", selected_pid);
    else
        snprintf(status_msg, sizeof status_msg, "signal failed: %s", strerror(errno));
    confirm_sig = 0;
}

static int read_key(char *out)
{
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n != 1)
        return 0;
    if (c != 0x1b) {
        *out = (char)c;
        return 1;
    }
    {
        unsigned char a, b;
        struct timespec sl = {0, 8L * 1000000L};
        nanosleep(&sl, NULL);
        if (read(STDIN_FILENO, &a, 1) != 1) {
            *out = 0x1b;
            return 1;
        }
        if (a == '[' && read(STDIN_FILENO, &b, 1) == 1) {
            if (b == 'A') { *out = 'k'; return 1; }
            if (b == 'B') { *out = 'j'; return 1; }
        }
        *out = 0x1b;
        return 1;
    }
}

int main(int argc, char **argv)
{
    int watch = 0, json = 0, once_proc = 0, plain = 0;
    int interval = WATCH_DEFAULT_SEC;
    int i;
    Snap s;
    Proc procs[MAX_PROCS];
    int nprocs = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--watch"))
            watch = 1;
        else if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--procs"))
            once_proc = 1;
        else if (!strcmp(argv[i], "--plain"))
            plain = 1;
        else if (!strcmp(argv[i], "--json"))
            json = 1;
        else if (!strcmp(argv[i], "--no-color"))
            use_color = 0;
        else if (!strcmp(argv[i], "--ascii"))
            use_unicode = 0;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "--interval")) && i + 1 < argc) {
            interval = atoi(argv[++i]);
            if (interval < 1)
                interval = 1;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!is_tty() || json || plain)
        use_color = 0;
    if (once_proc)
        page = PAGE_PROC;

    if (json || plain || !watch) {
        struct timespec gap = {0, 200L * 1000000L};
        collect(&s, procs, &nprocs);
        nanosleep(&gap, NULL);
        collect(&s, procs, &nprocs);
        if (json) {
            render_json(&s, procs, nprocs);
            return 0;
        }
        if (plain) {
            render_plain(&s);
            return 0;
        }
        if (page == PAGE_PROC)
            render_proc(&s, procs, nprocs, term_cols(), 0);
        else
            render_dash(&s, procs, nprocs, term_cols(), 0);
        return 0;
    }

    signal(SIGWINCH, on_winch);
    signal(SIGINT, on_exit_sig);
    signal(SIGTERM, on_exit_sig);
    live_enter();

    {
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (flags >= 0)
            fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

    collect(&s, procs, &nprocs);
    {
        struct timespec now, next;
        int dirty = 1;
        clock_gettime(CLOCK_MONOTONIC, &now);
        next = now;
        next.tv_sec += interval;

        for (;;) {
            char key;
            struct timespec sl = {0, 30L * 1000000L};

            if (got_exit)
                break;
            if (got_winch) {
                got_winch = 0;
                dirty = 1;
            }

            clock_gettime(CLOCK_MONOTONIC, &now);
            if (!filter_edit && !confirm_sig &&
                (now.tv_sec > next.tv_sec ||
                 (now.tv_sec == next.tv_sec && now.tv_nsec >= next.tv_nsec))) {
                collect(&s, procs, &nprocs);
                next = now;
                next.tv_sec += interval;
                dirty = 1;
            }

            if (dirty) {
                int w = term_cols();
                frame_begin();
                if (page == PAGE_PROC)
                    render_proc(&s, procs, nprocs, w, 1);
                else
                    render_dash(&s, procs, nprocs, w, 1);
                frame_end();
                dirty = 0;
            }

            if (!read_key(&key)) {
                nanosleep(&sl, NULL);
                continue;
            }

            if (confirm_sig) {
                if (key == 'y' || key == 'Y') {
                    apply_signal();
                    dirty = 1;
                } else if (key == 'n' || key == 'N' || key == 0x1b || key == 'q') {
                    confirm_sig = 0;
                    dirty = 1;
                }
                continue;
            }

            if (filter_edit) {
                size_t fl = strlen(filter);
                if (key == 0x1b) {
                    filter_edit = 0;
                    filter[0] = '\0';
                    rebuild_view(procs, nprocs);
                    dirty = 1;
                } else if (key == '\n' || key == '\r') {
                    filter_edit = 0;
                    rebuild_view(procs, nprocs);
                    dirty = 1;
                } else if (key == 0x7f || key == 0x08) {
                    if (fl) filter[fl - 1] = '\0';
                    rebuild_view(procs, nprocs);
                    dirty = 1;
                } else if ((unsigned char)key >= 32 && (unsigned char)key < 127 &&
                           fl + 1 < sizeof filter) {
                    filter[fl] = key;
                    filter[fl + 1] = '\0';
                    rebuild_view(procs, nprocs);
                    dirty = 1;
                }
                continue;
            }

            if (key == 'q' || key == 3)
                break;
            if (key == '1') { page = PAGE_DASH; dirty = 1; continue; }
            if (key == '2' || key == 'p') { page = PAGE_PROC; dirty = 1; continue; }
            if (key == 'c') { sort_mode = SORT_CPU; collect(&s, procs, &nprocs); dirty = 1; continue; }
            if (key == 'm') { sort_mode = SORT_RSS; collect(&s, procs, &nprocs); dirty = 1; continue; }
            if (key == '/') { page = PAGE_PROC; filter_edit = 1; status_msg[0] = 0; dirty = 1; continue; }
            if (key == 'j') { move_sel(1, procs); dirty = 1; continue; }
            if (key == 'k') { move_sel(-1, procs); dirty = 1; continue; }
            if (key == 't') { request_signal(SIGTERM, procs); dirty = 1; continue; }
            if (key == 'K') { request_signal(SIGKILL, procs); dirty = 1; continue; }
            if (key == 'r' || key == ' ') {
                status_msg[0] = 0;
                collect(&s, procs, &nprocs);
                dirty = 1;
                continue;
            }
        }
    }

    live_leave();
    return 0;
}
