/*
 * pepstat — compact HUD for WalnutOS / Walnut Pi Zero 2W
 * Primary target: 480x320  (~60 cols x 20 rows at 8x16)
 * Scales to any tty via TIOCGWINSZ (including 1920x1080 terminals).
 *
 *   make && ./pepstat -w
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define WATCH_SEC     2
#define SAMPLE_MS     80
#define MAX_PROCS     512
#define MAX_CORES     64
#define HIST          64
#define PREV_SLOTS    512
#define FILTER_MAX    32

enum { PAGE_DASH = 0, PAGE_PROC = 1 };
enum { SORT_CPU = 0, SORT_RSS = 1, SORT_NAME = 2 };

typedef struct { int r, g, b; } Rgb;
static Rgb ACC, FOG, SLATE, AMBER, ROSE, WHITE, SEL, BOX;

static int use_color = 1, use_uni = 1, page = PAGE_DASH, sort_mode = SORT_CPU;
static int live_mode, cols, rows, sel, scroll, selected_pid;
static char filter[FILTER_MAX], status_msg[120];
static int view_idx[MAX_PROCS], nview;
static struct termios term_orig;
static int term_raw;

static void rgb(Rgb *c, int r, int g, int b) { c->r = r; c->g = g; c->b = b; }
static void theme(void)
{
    rgb(&ACC, 61, 204, 122);
    rgb(&FOG, 168, 190, 178);
    rgb(&SLATE, 110, 130, 122);
    rgb(&AMBER, 255, 196, 64);
    rgb(&ROSE, 255, 79, 107);
    rgb(&WHITE, 236, 250, 240);
    rgb(&SEL, 20, 70, 48);
    rgb(&BOX, 26, 160, 90);
}
static void rst(void) { if (use_color) fputs("\033[0m", stdout); }
static void fg(Rgb c) { if (use_color) printf("\033[38;2;%d;%d;%dm", c.r, c.g, c.b); }
static void bg(Rgb c) { if (use_color) printf("\033[48;2;%d;%d;%dm", c.r, c.g, c.b); }
static void bold(void) { if (use_color) fputs("\033[1m", stdout); }
static void nl(void) { if (live_mode) fputs("\033[K", stdout); fputc('\n', stdout); }

static void copy_str(char *d, size_t n, const char *s)
{
    size_t i = 0;
    if (!d || n == 0) return;
    if (!s) { d[0] = 0; return; }
    while (s[i] && i + 1 < n) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void term_size(void)
{
    struct winsize ws;
    cols = 60;
    rows = 20;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) cols = ws.ws_col;
        if (ws.ws_row > 0) rows = ws.ws_row;
    }
    /* Walnut 480x320 @ 8x16 is 60x20. Never assume more than we have. */
    if (cols < 36) cols = 36;
    if (rows < 12) rows = 12;
}

static int read_file(const char *path, char *buf, size_t n)
{
    FILE *f;
    if (!path || !buf || n < 2) return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)n, f)) { fclose(f); return -1; }
    fclose(f);
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}

static int read_key(const char *path, const char *key, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    char line[512];
    size_t klen;
    if (!f) return -1;
    klen = strlen(key);
    while (fgets(line, (int)sizeof line, f)) {
        if (!strncmp(line, key, klen) && (line[klen] == '=' || line[klen] == ':')) {
            char *v = line + klen + 1;
            while (*v == ' ' || *v == '\t' || *v == '"') v++;
            copy_str(out, n, v);
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
    static const char *u[] = {"B", "K", "M", "G", "T"};
    int i = 0;
    if (bytes < 0) bytes = 0;
    while (bytes >= 1024.0 && i < 4) { bytes /= 1024.0; i++; }
    if (i == 0 || bytes >= 10)
        snprintf(out, n, "%.0f%s", bytes, u[i]);
    else
        snprintf(out, n, "%.1f%s", bytes, u[i]);
}

static void human_secs(double secs, char *out, size_t n)
{
    unsigned long s = (unsigned long)secs, d, h, m;
    d = s / 86400UL; s %= 86400UL;
    h = s / 3600UL;  s %= 3600UL;
    m = s / 60UL;    s %= 60UL;
    if (d) snprintf(out, n, "%lud%02luh", d, h);
    else if (h) snprintf(out, n, "%luh%02lum", h, m);
    else snprintf(out, n, "%lum%02lus", m, s);
}

static int pct_lvl(double p)
{
    if (p >= 90) return 2;
    if (p >= 80) return 1;
    return 0;
}
static void fg_lvl(int l)
{
    if (l == 2) fg(ROSE);
    else if (l == 1) fg(AMBER);
    else fg(ACC);
}

static void bar(double pct, int width)
{
    int i, full, lvl;
    if (width < 4) width = 4;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lvl = pct_lvl(pct);
    fg_lvl(lvl);
    full = (int)((pct / 100.0) * width + 0.5);
    if (!use_uni) {
        fputc('[', stdout);
        for (i = 0; i < width; i++) fputc(i < full ? '#' : '-', stdout);
        fputc(']', stdout);
        rst();
        return;
    }
    for (i = 0; i < width; i++) {
        if (i < full) fputs("█", stdout);
        else { fg(SLATE); fputs("░", stdout); fg_lvl(lvl); }
    }
    rst();
}

static void spark(const double *h, int n, int width)
{
    static const char *ch[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    int i, start;
    if (width < 4) width = 4;
    if (n < 1) {
        fg(SLATE);
        for (i = 0; i < width; i++) fputc('.', stdout);
        rst();
        return;
    }
    start = n > width ? n - width : 0;
    for (i = start; i < n; i++) {
        double p = h[i];
        int idx;
        if (p < 0) p = 0;
        if (p > 100) p = 100;
        idx = (int)(p / 100.0 * 7.0 + 0.5);
        if (idx > 7) idx = 7;
        fg_lvl(pct_lvl(p));
        if (use_uni) fputs(ch[idx], stdout);
        else fputc(" .:-=+*#"[idx], stdout);
    }
    rst();
}

/* ---------- data ---------- */
typedef struct {
    char hostname[64], os[80], kernel[96], cpu_model[96];
    int cpu_cores, ncores, nprocs, nthreads;
    double load1, load5, load15, cpu_pct, uptime_sec, temp_c;
    unsigned long mem_total_kb, mem_avail_kb, swap_total_kb, swap_free_kb;
    unsigned long disk_total, disk_free;
    int have_bat, bat_pct, have_temp;
    char bat_status[24], iface[24], ipv4[48];
    double core_pct[MAX_CORES];
    unsigned long long cpu_total, cpu_idle;
    unsigned long long core_total[MAX_CORES], core_idle[MAX_CORES];
} Snap;

typedef struct {
    int pid, ppid;
    char name[32], user[12];
    char state;
    unsigned long rss_kb, ticks;
    double cpu_pct;
} Proc;

typedef struct { int pid; unsigned long ticks; } PrevProc;

static PrevProc prev_tab[PREV_SLOTS];
static int prev_n, prev_ncores, have_prev_cpu;
static unsigned long long prev_cpu_total, prev_cpu_idle;
static unsigned long long prev_core_total[MAX_CORES], prev_core_idle[MAX_CORES];
static struct timespec prev_mono;
static double cpu_hist[HIST];
static int cpu_hist_n;

static void snap_init(Snap *s)
{
    memset(s, 0, sizeof *s);
    copy_str(s->os, sizeof s->os, "Linux");
    copy_str(s->cpu_model, sizeof s->cpu_model, "cpu");
    copy_str(s->bat_status, sizeof s->bat_status, "-");
    copy_str(s->iface, sizeof s->iface, "-");
    copy_str(s->ipv4, sizeof s->ipv4, "-");
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
        copy_str(s->hostname, sizeof s->hostname, "walnut");
    s->hostname[sizeof s->hostname - 1] = 0;
    read_key("/etc/os-release", "PRETTY_NAME", s->os, sizeof s->os);
    if (uname(&u) == 0)
        snprintf(s->kernel, sizeof s->kernel, "%s", u.release);
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        while (fgets(line, (int)sizeof line, f)) {
            if (!strncmp(line, "model name", 10) && !strcmp(s->cpu_model, "cpu")) {
                char *p = strchr(line, ':');
                if (p) {
                    p++;
                    while (*p == ' ') p++;
                    copy_str(s->cpu_model, sizeof s->cpu_model, p);
                    s->cpu_model[strcspn(s->cpu_model, "\r\n")] = 0;
                }
            }
            if (!strncmp(line, "processor", 9)) cores++;
        }
        fclose(f);
    }
    if (cores == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        cores = n > 0 ? (int)n : 1;
    }
    s->cpu_cores = cores;
    if (!strcmp(s->cpu_model, "cpu")) {
        if (read_key("/proc/cpuinfo", "Hardware", s->cpu_model, sizeof s->cpu_model) != 0)
            read_key("/proc/cpuinfo", "Model", s->cpu_model, sizeof s->cpu_model);
    }
}

static void collect_load(Snap *s)
{
    char buf[128];
    if (!read_file("/proc/loadavg", buf, sizeof buf))
        sscanf(buf, "%lf %lf %lf", &s->load1, &s->load5, &s->load15);
    if (!read_file("/proc/uptime", buf, sizeof buf))
        s->uptime_sec = strtod(buf, NULL);
}

static void collect_cpu(Snap *s)
{
    FILE *f = fopen("/proc/stat", "r");
    char line[256], tag[16];
    unsigned long long user, nice, sys, id, iw, irq, sirq, st;
    int ncore = 0;
    if (!f) return;
    while (fgets(line, (int)sizeof line, f)) {
        unsigned long long idle, total;
        if (strncmp(line, "cpu", 3)) break;
        if (sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
                   tag, &user, &nice, &sys, &id, &iw, &irq, &sirq, &st) < 5)
            continue;
        idle = id + iw;
        total = user + nice + sys + id + iw + irq + sirq + st;
        if (!strcmp(tag, "cpu")) {
            s->cpu_idle = idle;
            s->cpu_total = total;
            if (have_prev_cpu && total > prev_cpu_total) {
                s->cpu_pct = 100.0 * (1.0 - (double)(idle - prev_cpu_idle) /
                                      (double)(total - prev_cpu_total));
                if (s->cpu_pct < 0) s->cpu_pct = 0;
                if (s->cpu_pct > 100) s->cpu_pct = 100;
            }
        } else if (ncore < MAX_CORES) {
            s->core_idle[ncore] = idle;
            s->core_total[ncore] = total;
            if (have_prev_cpu && ncore < prev_ncores && total > prev_core_total[ncore]) {
                s->core_pct[ncore] = 100.0 * (1.0 - (double)(idle - prev_core_idle[ncore]) /
                                              (double)(total - prev_core_total[ncore]));
                if (s->core_pct[ncore] < 0) s->core_pct[ncore] = 0;
                if (s->core_pct[ncore] > 100) s->core_pct[ncore] = 100;
            }
            ncore++;
        }
    }
    fclose(f);
    s->ncores = ncore;
    if (!have_prev_cpu) {
        struct timespec ts = {0, (long)SAMPLE_MS * 1000000L};
        nanosleep(&ts, NULL);
        have_prev_cpu = 1;
        prev_cpu_total = s->cpu_total;
        prev_cpu_idle = s->cpu_idle;
        prev_ncores = ncore;
        memcpy(prev_core_total, s->core_total, sizeof prev_core_total);
        memcpy(prev_core_idle, s->core_idle, sizeof prev_core_idle);
        collect_cpu(s);
        return;
    }
    if (s->cpu_pct >= 0) {
        if (cpu_hist_n < HIST) cpu_hist[cpu_hist_n++] = s->cpu_pct;
        else {
            memmove(cpu_hist, cpu_hist + 1, (HIST - 1) * sizeof *cpu_hist);
            cpu_hist[HIST - 1] = s->cpu_pct;
        }
    }
}

static void collect_mem(Snap *s)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char key[64], unit[16];
    unsigned long val;
    if (!f) return;
    while (fscanf(f, "%63s %lu %15s", key, &val, unit) == 3) {
        if (!strcmp(key, "MemTotal:")) s->mem_total_kb = val;
        else if (!strcmp(key, "MemAvailable:")) s->mem_avail_kb = val;
        else if (!strcmp(key, "SwapTotal:")) s->swap_total_kb = val;
        else if (!strcmp(key, "SwapFree:")) s->swap_free_kb = val;
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
    DIR *d = opendir("/sys/class/power_supply");
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        char path[320], buf[64];
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "/sys/class/power_supply/%.80s/type", e->d_name);
        if (read_file(path, buf, sizeof buf)) continue;
        if (strcasecmp(buf, "Battery")) continue;
        snprintf(path, sizeof path, "/sys/class/power_supply/%.80s/capacity", e->d_name);
        if (!read_file(path, buf, sizeof buf)) {
            s->have_bat = 1;
            s->bat_pct = atoi(buf);
        }
        snprintf(path, sizeof path, "/sys/class/power_supply/%.80s/status", e->d_name);
        if (read_file(path, s->bat_status, sizeof s->bat_status))
            copy_str(s->bat_status, sizeof s->bat_status, "?");
        break;
    }
    closedir(d);
}

static void collect_temp(Snap *s)
{
    DIR *d = opendir("/sys/class/thermal");
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        char typep[320], tempp[320], typ[64], tbuf[32];
        double c;
        if (strncmp(e->d_name, "thermal_zone", 12)) continue;
        snprintf(typep, sizeof typep, "/sys/class/thermal/%.80s/type", e->d_name);
        snprintf(tempp, sizeof tempp, "/sys/class/thermal/%.80s/temp", e->d_name);
        if (read_file(typep, typ, sizeof typ) || read_file(tempp, tbuf, sizeof tbuf))
            continue;
        c = atof(tbuf);
        if (c > 200) c /= 1000.0;
        if (c <= 0 || c > 150) continue;
        s->have_temp = 1;
        s->temp_c = c;
        if (strstr(typ, "cpu") || strstr(typ, "soc") || strstr(typ, "x86"))
            break;
    }
    closedir(d);
}

static void collect_net(Snap *s)
{
    FILE *f = fopen("/proc/net/route", "r");
    char line[256], ifn[32];
    unsigned dest;
    struct ifaddrs *ifa = NULL, *p;
    if (f) {
        if (fgets(line, (int)sizeof line, f)) {
            while (fgets(line, (int)sizeof line, f)) {
                if (sscanf(line, "%31s %X", ifn, &dest) == 2 && dest == 0) {
                    copy_str(s->iface, sizeof s->iface, ifn);
                    break;
                }
            }
        }
        fclose(f);
    }
    if (getifaddrs(&ifa) != 0) return;
    for (p = ifa; p; p = p->ifa_next) {
        struct sockaddr_in *in;
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!strcmp(p->ifa_name, "lo")) continue;
        if (s->iface[0] && s->iface[0] != '-' && strcmp(p->ifa_name, s->iface))
            continue;
        in = (struct sockaddr_in *)p->ifa_addr;
        inet_ntop(AF_INET, &in->sin_addr, s->ipv4, sizeof s->ipv4);
        if (s->iface[0] == '-' || !s->iface[0])
            copy_str(s->iface, sizeof s->iface, p->ifa_name);
        break;
    }
    freeifaddrs(ifa);
}

static unsigned long prev_lookup(int pid)
{
    int i;
    for (i = 0; i < prev_n; i++)
        if (prev_tab[i].pid == pid) return prev_tab[i].ticks;
    return 0;
}

static int collect_procs(Proc *list, int max, Snap *s, double elapsed)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int n = 0, threads = 0;
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    if (!d) return 0;
    while ((e = readdir(d))) {
        char path[96], buf[512], *lpar, *rpar, state;
        FILE *f;
        int pid, ppid = 0;
        unsigned long dummy, utime = 0, stime = 0, ticks, rss_kb = 0;
        Proc *p;
        struct passwd *pw;
        uid_t uid = (uid_t)-1;
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        pid = atoi(e->d_name);
        if (pid <= 0) continue;
        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(buf, (int)sizeof buf, f)) { fclose(f); continue; }
        fclose(f);
        lpar = strchr(buf, '(');
        rpar = strrchr(buf, ')');
        if (!lpar || !rpar || rpar <= lpar) continue;
        if (sscanf(rpar + 1, " %c %d %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                   &state, &ppid, &dummy, &dummy, &dummy, &dummy, &dummy,
                   &dummy, &dummy, &dummy, &dummy, &utime, &stime) < 13)
            continue;
        ticks = utime + stime;
        snprintf(path, sizeof path, "/proc/%d/status", pid);
        f = fopen(path, "r");
        if (f) {
            char line[256];
            while (fgets(line, (int)sizeof line, f)) {
                if (!strncmp(line, "VmRSS:", 6)) rss_kb = strtoul(line + 6, NULL, 10);
                else if (!strncmp(line, "Uid:", 4)) uid = (uid_t)strtoul(line + 4, NULL, 10);
                else if (!strncmp(line, "Threads:", 8)) threads += (int)strtol(line + 8, NULL, 10);
            }
            fclose(f);
        }
        if (n >= max) continue;
        p = &list[n];
        memset(p, 0, sizeof *p);
        p->pid = pid;
        p->ppid = ppid;
        p->state = state;
        p->rss_kb = rss_kb;
        p->ticks = ticks;
        {
            size_t clen = (size_t)(rpar - lpar - 1);
            if (clen >= sizeof p->name) clen = sizeof p->name - 1;
            memcpy(p->name, lpar + 1, clen);
            p->name[clen] = 0;
        }
        pw = uid != (uid_t)-1 ? getpwuid(uid) : NULL;
        if (pw) copy_str(p->user, sizeof p->user, pw->pw_name);
        else snprintf(p->user, sizeof p->user, "%d", (int)uid);
        if (elapsed > 0.05) {
            unsigned long old = prev_lookup(pid);
            if (old && ticks >= old)
                p->cpu_pct = 100.0 * (double)(ticks - old) / ((double)hz * elapsed);
        }
        if (p->cpu_pct > 999) p->cpu_pct = 999;
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
    prev_ncores = s->ncores;
    memcpy(prev_core_total, s->core_total, sizeof prev_core_total);
    memcpy(prev_core_idle, s->core_idle, sizeof prev_core_idle);
    have_prev_cpu = 1;
    clock_gettime(CLOCK_MONOTONIC, &prev_mono);
}

static int cmp_cpu(const void *a, const void *b)
{
    const Proc *pa = a, *pb = b;
    if (pb->cpu_pct > pa->cpu_pct) return 1;
    if (pb->cpu_pct < pa->cpu_pct) return -1;
    return (pb->rss_kb > pa->rss_kb) - (pb->rss_kb < pa->rss_kb);
}
static int cmp_rss(const void *a, const void *b)
{
    const Proc *pa = a, *pb = b;
    return (pb->rss_kb > pa->rss_kb) - (pb->rss_kb < pa->rss_kb);
}
static int cmp_name(const void *a, const void *b)
{
    const Proc *pa = a, *pb = b;
    int c = strcasecmp(pa->name, pb->name);
    return c ? c : (pa->pid - pb->pid);
}

static int ci_contains(const char *hay, const char *needle)
{
    size_t n, h, i, j;
    if (!needle || !needle[0]) return 1;
    if (!hay) return 0;
    n = strlen(needle);
    h = strlen(hay);
    if (n > h) return 0;
    for (i = 0; i + n <= h; i++) {
        for (j = 0; j < n; j++)
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j]))
                break;
        if (j == n) return 1;
    }
    return 0;
}

static int proc_match(const Proc *p)
{
    char pidbuf[16];
    if (!filter[0]) return 1;
    snprintf(pidbuf, sizeof pidbuf, "%d", p->pid);
    return ci_contains(p->name, filter) || ci_contains(p->user, filter) ||
           ci_contains(pidbuf, filter);
}

static int proc_view_h(void)
{
    /* leftover rows after header+footer+box chrome */
    return clampi(rows - 4, 4, rows);
}

static void rebuild_view(const Proc *procs, int nprocs)
{
    int i, found = -1, vh = proc_view_h();
    nview = 0;
    for (i = 0; i < nprocs; i++) {
        if (!proc_match(&procs[i])) continue;
        if (nview < MAX_PROCS) view_idx[nview++] = i;
    }
    if (selected_pid) {
        for (i = 0; i < nview; i++)
            if (procs[view_idx[i]].pid == selected_pid) { found = i; break; }
    }
    if (found >= 0) sel = found;
    if (sel >= nview) sel = nview ? nview - 1 : 0;
    if (sel < 0) sel = 0;
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + vh) scroll = sel - vh + 1;
    if (scroll < 0) scroll = 0;
    if (nview) selected_pid = procs[view_idx[sel]].pid;
}

static void collect(Snap *s, Proc *procs, int *nprocs)
{
    struct timespec now;
    double elapsed = 0;
    snap_init(s);
    collect_identity(s);
    collect_load(s);
    collect_cpu(s);
    collect_mem(s);
    collect_disk(s);
    collect_battery(s);
    collect_temp(s);
    collect_net(s);
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (prev_mono.tv_sec || prev_mono.tv_nsec)
        elapsed = (double)(now.tv_sec - prev_mono.tv_sec) +
                  (double)(now.tv_nsec - prev_mono.tv_nsec) / 1e9;
    *nprocs = collect_procs(procs, MAX_PROCS, s, elapsed);
    if (sort_mode == SORT_RSS) qsort(procs, (size_t)*nprocs, sizeof *procs, cmp_rss);
    else if (sort_mode == SORT_NAME) qsort(procs, (size_t)*nprocs, sizeof *procs, cmp_name);
    else qsort(procs, (size_t)*nprocs, sizeof *procs, cmp_cpu);
    store_prev(procs, *nprocs, s);
    rebuild_view(procs, *nprocs);
}

/* ---------- draw ---------- */
static const char *ch_tl(void) { return use_uni ? "╭" : "+"; }
static const char *ch_tr(void) { return use_uni ? "╮" : "+"; }
static const char *ch_bl(void) { return use_uni ? "╰" : "+"; }
static const char *ch_br(void) { return use_uni ? "╯" : "+"; }
static const char *ch_h(void) { return use_uni ? "─" : "-"; }
static const char *ch_v(void) { return use_uni ? "│" : "|"; }

static void box_top(const char *title)
{
    int i, tlen, inner;
    fg(BOX);
    fputs(ch_tl(), stdout);
    fputs(ch_h(), stdout);
    if (title && title[0]) {
        fg(ACC); bold();
        printf(" %s ", title);
        rst(); fg(BOX);
        tlen = (int)strlen(title) + 3;
    } else tlen = 1;
    inner = cols - 2 - tlen;
    if (inner < 0) inner = 0;
    for (i = 0; i < inner; i++) fputs(ch_h(), stdout);
    fputs(ch_tr(), stdout);
    rst();
    nl();
}
static void box_bot(void)
{
    int i;
    fg(BOX);
    fputs(ch_bl(), stdout);
    for (i = 0; i < cols - 2; i++) fputs(ch_h(), stdout);
    fputs(ch_br(), stdout);
    rst();
    nl();
}
static void box_begin(void)
{
    fg(BOX); fputs(ch_v(), stdout); rst(); fputc(' ', stdout);
}
static void box_end(void)
{
    if (isatty(STDOUT_FILENO)) printf("\033[%dG", cols);
    fg(BOX); fputs(ch_v(), stdout); rst();
    nl();
}

static const char *sort_lab(void)
{
    if (sort_mode == SORT_RSS) return "mem";
    if (sort_mode == SORT_NAME) return "name";
    return "cpu";
}

static void clip_print(const char *s, int max)
{
    int n = 0;
    if (max < 1) return;
    while (s[n] && n < max) { fputc(s[n], stdout); n++; }
}

static void render_header(const Snap *s)
{
    char clock[16], up[24];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm) strftime(clock, sizeof clock, "%H:%M:%S", tm);
    else copy_str(clock, sizeof clock, "--:--:--");
    human_secs(s->uptime_sec, up, sizeof up);
    fg(ACC); bold(); fputs(" pepstat", stdout); rst();
    fg(SLATE);
    printf(" %s", s->hostname);
    if (cols >= 50) printf("  %s", s->os);
    fg(WHITE); printf("  %s", clock);
    fg(SLATE); printf("  %s", up);
    rst();
    nl();
}

static void render_footer(int watch)
{
    fg(SLATE);
    if (status_msg[0]) clip_print(status_msg, cols - 2);
    else if (watch) clip_print("1 dash  2 proc  c/m/n  j/k  t term  q", cols - 2);
    else clip_print("./pepstat -w", cols - 2);
    rst();
    nl();
}

static void render_dash(const Snap *s, const Proc *procs, int nprocs)
{
    char a[24], b[24], title[40];
    double mem_pct = 0, disk_pct = 0, swap_pct = 0;
    int i, show, cores, per_row, barw, spark_w, used, room;
    int show_cores, show_swap;

    render_header(s);

    spark_w = clampi(cols - 18, 8, cols - 12);
    barw = clampi(cols - 20, 8, cols - 14);

    snprintf(title, sizeof title, "cpu %.0f%%", s->cpu_pct < 0 ? 0 : s->cpu_pct);
    box_top(title);
    box_begin();
    spark(cpu_hist, cpu_hist_n, spark_w);
    fputc(' ', stdout);
    fg(WHITE);
    printf("%.0f%%", s->cpu_pct < 0 ? 0 : s->cpu_pct);
    rst();
    box_end();
    box_begin();
    fg(SLATE);
    clip_print(s->cpu_model, cols >= 48 ? 22 : 10);
    printf("  %.2f %.2f", s->load1, s->load5);
    rst();
    box_end();

    cores = s->ncores > 0 ? s->ncores : s->cpu_cores;
    if (cores > MAX_CORES) cores = MAX_CORES;
    /* 480x320 (~20 rows): skip per-core grid. Bigger terms get it. */
    show_cores = rows >= 26 && cores > 1;
    per_row = clampi((cols - 4) / 14, 1, cores);
    if (show_cores) {
        for (i = 0; i < cores; ) {
            int k;
            box_begin();
            for (k = 0; k < per_row && i < cores; k++, i++) {
                fg(SLATE); printf("%d ", i);
                bar(s->core_pct[i], clampi((cols - 6) / per_row - 8, 4, 16));
                fg(FOG); printf(" %.0f ", s->core_pct[i]);
                rst();
            }
            box_end();
        }
    }
    box_bot();

    if (s->mem_total_kb)
        mem_pct = 100.0 * (double)(s->mem_total_kb - s->mem_avail_kb) / (double)s->mem_total_kb;
    if (s->swap_total_kb)
        swap_pct = 100.0 * (double)(s->swap_total_kb - s->swap_free_kb) / (double)s->swap_total_kb;
    if (s->disk_total)
        disk_pct = 100.0 * (double)(s->disk_total - s->disk_free) / (double)s->disk_total;
    show_swap = s->swap_total_kb && rows >= 22;

    snprintf(title, sizeof title, "mem %.0f%%", mem_pct);
    box_top(title);
    box_begin();
    human_bytes((double)(s->mem_total_kb - s->mem_avail_kb) * 1024.0, a, sizeof a);
    human_bytes((double)s->mem_total_kb * 1024.0, b, sizeof b);
    fg(FOG); fputs("RAM ", stdout);
    bar(mem_pct, barw);
    fg(WHITE); printf(" %s/%s", a, b);
    rst();
    box_end();
    if (show_swap) {
        box_begin();
        human_bytes((double)(s->swap_total_kb - s->swap_free_kb) * 1024.0, a, sizeof a);
        human_bytes((double)s->swap_total_kb * 1024.0, b, sizeof b);
        fg(FOG); fputs("SWP ", stdout);
        bar(swap_pct, barw);
        fg(WHITE); printf(" %s/%s", a, b);
        rst();
        box_end();
    }
    box_begin();
    human_bytes((double)(s->disk_total - s->disk_free), a, sizeof a);
    human_bytes((double)s->disk_total, b, sizeof b);
    fg(FOG); fputs("/   ", stdout);
    bar(disk_pct, barw);
    fg(WHITE); printf(" %s/%s", a, b);
    rst();
    box_end();
    box_bot();

    box_top("sys");
    box_begin();
    fg(FOG);
    printf("%s %s", s->iface, s->ipv4);
    if (s->have_temp) {
        fg(SLATE); fputs("  ", stdout);
        fg_lvl(s->temp_c >= 90 ? 2 : s->temp_c >= 75 ? 1 : 0);
        printf("%.0fC", s->temp_c);
    }
    if (s->have_bat) {
        fg(SLATE); fputs("  ", stdout);
        fg_lvl(s->bat_pct <= 10 ? 2 : s->bat_pct <= 20 ? 1 : 0);
        printf("%d%%", s->bat_pct);
    }
    rst();
    box_end();
    if (rows >= 18) {
        box_begin();
        fg(SLATE);
        printf("%d p  %s", s->nprocs, s->kernel);
        rst();
        box_end();
    }
    box_bot();

    /* leftover rows become the process peek */
    used = 1 /*hdr*/ + 4 /*cpu min*/ + (show_cores ? (cores + per_row - 1) / per_row : 0)
         + 5 /*mem min*/ + (show_swap ? 1 : 0) + 4 /*sys*/ + (rows >= 18 ? 1 : 0) + 1 /*foot*/;
    room = rows - used - 2; /* proc box chrome */
    if (room < 2) room = 2;
    show = nprocs < room ? nprocs : room;

    snprintf(title, sizeof title, "proc %s", sort_lab());
    box_top(title);
    for (i = 0; i < show; i++) {
        int namew;
        human_bytes((double)procs[i].rss_kb * 1024.0, a, sizeof a);
        box_begin();
        fg(SLATE); printf("%5d ", procs[i].pid);
        fg_lvl(pct_lvl(procs[i].cpu_pct));
        printf("%4.0f ", procs[i].cpu_pct);
        fg(FOG); printf("%4s ", a);
        fg(WHITE);
        namew = cols - 22;
        if (namew < 6) namew = 6;
        clip_print(procs[i].name, namew);
        rst();
        box_end();
    }
    box_bot();
    render_footer(live_mode);
}

static void render_proc(const Snap *s, const Proc *procs)
{
    char a[24], title[64];
    int i, vh, show, namew;
    render_header(s);
    vh = proc_view_h();
    snprintf(title, sizeof title, "proc %s %d", sort_lab(), nview);
    box_top(title);
    box_begin();
    fg(ACC); clip_print("PID   CPU  MEM  NAME", cols - 4); rst();
    box_end();
    show = nview < scroll + vh ? nview : scroll + vh;
    namew = clampi(cols - 22, 6, 40);
    for (i = scroll; i < show; i++) {
        const Proc *p = &procs[view_idx[i]];
        int on = (i == sel);
        human_bytes((double)p->rss_kb * 1024.0, a, sizeof a);
        box_begin();
        if (on) bg(SEL);
        fg(on ? WHITE : SLATE);
        printf("%5d ", p->pid);
        fg_lvl(pct_lvl(p->cpu_pct));
        printf("%4.0f ", p->cpu_pct);
        fg(FOG); printf("%4s ", a);
        fg(WHITE); clip_print(p->name, namew);
        rst();
        box_end();
    }
    box_bot();
    render_footer(live_mode);
}

static void frame_begin(void) { fputs("\033[H", stdout); }
static void frame_end(void) { fputs("\033[J", stdout); fflush(stdout); }

static void live_enter(void)
{
    if (!isatty(STDOUT_FILENO)) return;
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
    if (term_raw) tcsetattr(STDIN_FILENO, TCSANOW, &term_orig);
    term_raw = 0;
    if (isatty(STDOUT_FILENO))
        fputs("\033[0m\033[?25h\033[?1049l", stdout);
    live_mode = 0;
}

static int read_ch(void)
{
    unsigned char b;
    if (read(STDIN_FILENO, &b, 1) != 1) return 0;
    if (b == 0x1b) {
        unsigned char s[2];
        if (read(STDIN_FILENO, s, 2) == 2 && s[0] == '[') {
            if (s[1] == 'A') return 'k';
            if (s[1] == 'B') return 'j';
        }
        return 0;
    }
    return b;
}

static void usage(void)
{
    puts("pepstat — WalnutOS HUD  (480x320 native, scales up)");
    puts("  pepstat        one-shot dashboard");
    puts("  pepstat -w     live");
    puts("  pepstat -p     process page");
    puts("  1 dash  2 proc  c/m/n sort  j/k move  t TERM  q");
}

int main(int argc, char **argv)
{
    Snap snap;
    Proc procs[MAX_PROCS];
    int nprocs = 0, watch = 0, i, want_proc = 0;
    struct timespec tick = {WATCH_SEC, 0};

    theme();
    if (!isatty(STDOUT_FILENO)) use_color = 0;
    if (getenv("NO_COLOR")) use_color = 0;
    if (getenv("PEPSTAT_ASCII")) use_uni = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--watch")) watch = 1;
        else if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--procs")) want_proc = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--ascii")) use_uni = 0;
        else if (!strcmp(argv[i], "--plain")) use_color = 0;
    }
    if (want_proc) page = PAGE_PROC;

    term_size();
    collect(&snap, procs, &nprocs);

    if (!watch) {
        if (page == PAGE_PROC) render_proc(&snap, procs);
        else render_dash(&snap, procs, nprocs);
        return 0;
    }

    live_enter();
    atexit(live_leave);
    for (;;) {
        int k, dirty = 0;
        term_size();
        frame_begin();
        if (page == PAGE_PROC) render_proc(&snap, procs);
        else render_dash(&snap, procs, nprocs);
        frame_end();

        /* poll keys for watch period */
        {
            struct timespec left = tick, sl = {0, 50 * 1000000L};
            while (left.tv_sec > 0 || left.tv_nsec > 0) {
                k = read_ch();
                if (k == 'q' || k == 'Q') return 0;
                if (k == '1') { page = PAGE_DASH; dirty = 1; break; }
                if (k == '2') { page = PAGE_PROC; dirty = 1; break; }
                if (k == 'c') { sort_mode = SORT_CPU; dirty = 1; break; }
                if (k == 'm') { sort_mode = SORT_RSS; dirty = 1; break; }
                if (k == 'n') { sort_mode = SORT_NAME; dirty = 1; break; }
                if (k == 'j') {
                    if (sel + 1 < nview) sel++;
                    selected_pid = nview ? procs[view_idx[sel]].pid : 0;
                    dirty = 1; break;
                }
                if (k == 'k') {
                    if (sel > 0) sel--;
                    selected_pid = nview ? procs[view_idx[sel]].pid : 0;
                    dirty = 1; break;
                }
                if (k == 't' && selected_pid) {
                    kill(selected_pid, SIGTERM);
                    snprintf(status_msg, sizeof status_msg, "TERM %d", selected_pid);
                    dirty = 1; break;
                }
                if (k == 'r') { dirty = 1; break; }
                nanosleep(&sl, NULL);
                if (left.tv_nsec >= sl.tv_nsec) left.tv_nsec -= sl.tv_nsec;
                else { left.tv_sec--; left.tv_nsec += 1000000000L - sl.tv_nsec; }
            }
        }
        collect(&snap, procs, &nprocs);
        status_msg[0] = 0;
        (void)dirty;
    }
}
