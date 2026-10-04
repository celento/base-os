/* Compile only: symbol sizes give actual freestanding i386 layout measurements. */
#include "app_storage.h"
char measured_terminal_text[sizeof(TerminalText)];
char measured_app_canvas[sizeof(AppCanvas)];
char measured_view_metadata[offsetof(AppView,canvas)];
char measured_app_view[sizeof(AppView)];
char measured_app_storage[sizeof(AppStorage)];
char measured_view_offset[offsetof(AppStorage,views)];
char measured_published_pixels[PROCESS_TASKS*APP_CANVAS_PIXELS];
_Static_assert(sizeof(void *)==4,"must measure the production i386 ABI");
_Static_assert(_Alignof(AppStorage)==4,"unexpected app arena alignment");
_Static_assert(sizeof(AppStorage)==732288,"update measured i386 app arena budget");
_Static_assert(TERM_MEMORY==APPS_BASE+0x300000,"must reuse original Terminal container");
_Static_assert(TERM_MEMORY+APP_STORAGE_CAPACITY<=APPS_BASE+APPS_CAPACITY,"app arena outside reservation");
