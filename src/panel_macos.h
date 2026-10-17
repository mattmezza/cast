#ifndef CAST_PANEL_MACOS_H
#define CAST_PANEL_MACOS_H
#include <stddef.h>
/* Acquire the per-socket panel identity before initializing AppKit/SDL.
 * 0 owns it, 1 requested the existing panel, -1 reports an error. */
int mac_panel_claim(const char *socket_path, char *error, size_t size);
/* Show through SDL so its visibility/minimization state stays synchronized. */
void mac_panel_attach(void (*show)(void *), void *context);
void mac_panel_poll(void);
void mac_panel_release(void);
#endif
