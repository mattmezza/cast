#ifndef CAST_PANEL_COLOR_PICK_H
#define CAST_PANEL_COLOR_PICK_H

#include <stdint.h>

typedef struct PanelColorPick PanelColorPick;

typedef enum {
    PANEL_COLOR_PICK_IDLE,
    PANEL_COLOR_PICK_PENDING,
    PANEL_COLOR_PICK_SUCCESS,
    PANEL_COLOR_PICK_CANCELLED,
    PANEL_COLOR_PICK_ERROR
} PanelColorPickStatus;

/* All calls belong to the panel thread. No daemon connection is used. */
PanelColorPick *panel_color_pick_new(void);
void panel_color_pick_free(PanelColorPick *pick);
/* driver is SDL's current video driver. parent_window may be NULL/empty. */
int panel_color_pick_begin(PanelColorPick *pick, const char *driver, const char *parent_window);
/* Never waits for user input. Terminal results persist until begin/cancel.
 * X11 displays the hovered color beside its crosshair; left-click selects,
 * Escape/right-click returns CANCELLED. Wayland appearance belongs to the
 * desktop portal, which supplies only the final selected color. */
PanelColorPickStatus panel_color_pick_poll(PanelColorPick *pick, uint8_t rgb[3]);
/* Explicit cancellation releases resources and resets the status to IDLE. */
void panel_color_pick_cancel(PanelColorPick *pick);
const char *panel_color_pick_error(const PanelColorPick *pick);

#endif
