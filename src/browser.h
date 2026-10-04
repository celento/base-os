#ifndef BASEOS_BROWSER_H
#define BASEOS_BROWSER_H

#define BROWSER_W 720
#define BROWSER_H 520
#define BROWSER_MIN_W 360
#define BROWSER_MIN_H 200
#define BROWSER_MOD_CTRL 1
#define BROWSER_MOD_SHIFT 2
#define BROWSER_MOD_ALT 4

/* One cooperative browser instance. x/y/w/h are the window's client area.
 * net_poll() must run in the desktop loop; browser_tick() never waits for I/O.
 * Input/tick methods return nonzero when the window should be repainted. */
void browser_init(void);
void browser_draw(int x, int y, int w, int h);
int browser_key(int sc, char ch, int modifiers);
int browser_click(int x, int y, int w, int h, int mx, int my);
int browser_scroll(int lines);
int browser_tick(void);
int browser_open(const char *url);
int browser_open_file(int fs_id);
void browser_close(void);
/* Save original completed page bytes to a new local path. Never overwrites.
 * Return the new file ID or -1; browser_status describes the result. */
int browser_save_page(int cwd, const char *path);
int browser_can_save(void);
const char *browser_title(void);
const char *browser_url(void);
const char *browser_status(void);
int browser_loading(void);

#endif
