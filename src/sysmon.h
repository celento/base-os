#ifndef SYSMON_APP_H
#define SYSMON_APP_H

#define SYSMON_W 520
#define SYSMON_H 440
#define SYSMON_MAX_WIN 8

typedef struct {
    unsigned uptime_sec;
    unsigned frames;
    int win_n;
    const char *win_name[SYSMON_MAX_WIN];
    int win_id[SYSMON_MAX_WIN];
    int win_min[SYSMON_MAX_WIN];
    int fs_nodes;
    int fs_max;
    int fs_bytes;
    int fs_cap;
    int fb_w, fb_h, fb_bpp;
    int mem_mb;
    unsigned redraws;
} SysInfo;

void sysmon_draw(int bx, int by, int bw, int bh, const SysInfo *si);
int sysmon_click(int bx, int by, int bw, int mx, int my, const SysInfo *si);

#endif
