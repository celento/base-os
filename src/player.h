#ifndef PLAYER_APP_H
#define PLAYER_APP_H
#define PLAYER_W 560
#define PLAYER_H 504
#define PLAYER_MIN_W 420
#define PLAYER_MIN_H 414

/* Singleton client-area module. All drawing/input coordinates include the
 * caller-supplied client origin; the desktop owns the surrounding window. */
void player_init(void);
int player_open_file(int id);
void player_draw(int x, int y, int w, int h);
int player_click(int x, int y, int w, int h, int mouse_x, int mouse_y);
int player_key(int scancode, char character);
/* Returns nonzero when client pixels changed. Does not service the audio
 * engine; audio_poll must also run when the player window is not visible. */
int player_tick(void);
void player_refresh(void);
const char *player_title(void);
#endif
