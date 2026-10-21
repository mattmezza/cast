#ifndef CAST_UI_FILE_DIALOG_H
#define CAST_UI_FILE_DIALOG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define FILE_DIALOG_ARGS 8
#define FILE_DIALOG_PATH 4096
#define FILE_DIALOG_FILTERS 8
typedef struct { char name[64], extensions[128]; } FileDialogFilter;
typedef struct {
    unsigned argc, path_index;
    char args[FILE_DIALOG_ARGS][FILE_DIALOG_PATH];
    bool save, folder;
    uint64_t generation;
    char title[128], default_path[FILE_DIALOG_PATH];
    unsigned filter_count;
    /* SDL extension lists, e.g. "mp4;mkv;webm" or "*". No glob syntax. */
    FileDialogFilter filters[FILE_DIALOG_FILTERS];
} FileDialogRequest;
typedef enum {
    FILE_DIALOG_SELECTED, FILE_DIALOG_CANCELLED, FILE_DIALOG_ERROR, FILE_DIALOG_STALE
} FileDialogStatus;
typedef struct {
    FileDialogStatus status;
    uint64_t generation;
    unsigned argc;
    char args[FILE_DIALOG_ARGS][FILE_DIALOG_PATH];
    char error[256];
} FileDialogResult;
/* Portable validation; no SDL, filesystem or daemon access. Path placeholder
 * may be empty. No command is executed or allowlisted by this component. */
bool file_dialog_request_valid(const FileDialogRequest *, char *error, size_t size);
typedef struct FileDialogHost FileDialogHost;
typedef struct SDL_Window SDL_Window;
/* Main thread only, except the private native completion callback. NULL parent
 * is supported. Explicit start only; one request including its unpolled result
 * at a time. Failed validation/start returns false, with file_dialog_error(). */
FileDialogHost *file_dialog_create(void);
bool file_dialog_start(FileDialogHost *, const FileDialogRequest *, SDL_Window *parent);
bool file_dialog_busy(FileDialogHost *);
const char *file_dialog_error(FileDialogHost *);
/* Main thread takes a completion once. Only SELECTED is actionable; generation
 * mismatch or disconnection retires it as STALE with no command arguments.
 * Caller queues the selected template through its normal command validator. */
bool file_dialog_poll(FileDialogHost *, uint64_t generation, bool connected, FileDialogResult *);
/* Releases host ownership immediately. A late callback releases its own ref;
 * it never touches the app/window. Do not use host after close. Native dialogs
 * cannot be programmatically cancelled portably by SDL. */
void file_dialog_close(FileDialogHost *);
#endif
