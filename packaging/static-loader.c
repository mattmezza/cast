/* Static delivery admits only this linked-in Xlib cursor extension. */
#include <stddef.h>
#include <string.h>

/* Addresses only: the caller supplies the upstream Xcursor function ABI. */
extern void XcursorNoticeCreateBitmap(void);
extern void XcursorNoticePutBitmap(void);
extern void XcursorTryShapeCursor(void);
extern void XcursorTryShapeBitmapCursor(void);

static char cursor_handle;
static _Thread_local const char *pending_error;
void *__wrap_dlopen(const char *name, int flags);
void *__wrap_dlsym(void *handle, const char *name);
int __wrap_dlclose(void *handle);
char *__wrap_dlerror(void);

void *__wrap_dlopen(const char *name, int flags)
{
    (void)flags;
    pending_error = NULL;
    if (name && (!strcmp(name, "libXcursor.so.1") || !strcmp(name, "libXcursor.so") ||
                 !strcmp(name, "libXcursor"))) return &cursor_handle;
    pending_error = "external shared modules are disabled in Cast static delivery";
    return NULL;
}
void *__wrap_dlsym(void *handle, const char *name)
{
    pending_error = NULL;
    if (handle == &cursor_handle && name) {
        if (*name == '_') name++;
#define ENTRY(symbol) if (!strcmp(name, #symbol)) return (void *)symbol
        ENTRY(XcursorNoticeCreateBitmap);
        ENTRY(XcursorNoticePutBitmap);
        ENTRY(XcursorTryShapeCursor);
        ENTRY(XcursorTryShapeBitmapCursor);
#undef ENTRY
    }
    pending_error = "symbol is not exported by a Cast static module";
    return NULL;
}
int __wrap_dlclose(void *handle)
{
    pending_error = NULL;
    if (handle == &cursor_handle) return 0;
    pending_error = "invalid Cast static module handle";
    return -1;
}
char *__wrap_dlerror(void)
{
    const char *error = pending_error;
    pending_error = NULL;
    return (char *)error;
}
