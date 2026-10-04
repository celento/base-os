#ifndef PLAYER_APP_H
#define PLAYER_APP_H
#define PLAYER_W 640
#define PLAYER_H 614
#define PLAYER_MIN_W 420
#define PLAYER_MIN_H 414
enum { PLAYER_CHANGED=1, PLAYER_VIDEO_FRAME=2 };

/* Singleton client-area module. All drawing/input coordinates include the
 * caller-supplied client origin; the desktop owns the surrounding window. */
void player_init(void);
int player_open_file(int id);
void player_draw(int x, int y, int w, int h);
/* Optional compositor fast path for PLAYER_VIDEO_FRAME only. Caller must
 * guarantee an unobscured frontmost player and handle cursor save/restore. */
void player_draw_playback(int x, int y, int w, int h);
int player_click(int x, int y, int w, int h, int mouse_x, int mouse_y);
int player_key(int scancode, char character);
/* Services video. Returns 0, PLAYER_CHANGED (full client redraw), or
 * PLAYER_VIDEO_FRAME (viewport/progress-only redraw is safe).
 * Does not service the audio engine; audio_poll must also run when the player window is not visible. */
int player_tick(void);
void player_refresh(void);
/* Close stops both transports; minimizing does not. */
void player_close(void);
const char *player_title(void);
#endif
