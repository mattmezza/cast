/* SDL retains ownership of its Cocoa delegate. Forward all lifecycle messages
 * except panel reopen and Dock menu actions, and restore it before SDL_Quit. */
#import <AppKit/AppKit.h>
#include "panel_macos.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int identity_lock = -1, requests = -1;
static char request_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
@interface CastPanelDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) id original;
@property(nonatomic) void (*show)(void *);
@property(nonatomic) void *context;
- (void)showPanel:(id)sender;
@end
@implementation CastPanelDelegate
- (BOOL)respondsToSelector:(SEL)selector
{ return [super respondsToSelector:selector] || [self.original respondsToSelector:selector]; }
- (id)forwardingTargetForSelector:(SEL)selector
{ return [self.original respondsToSelector:selector] ? self.original : [super forwardingTargetForSelector:selector]; }
- (void)showPanel:(id)sender
{
    (void)sender;
    [NSApp unhide:nil];
    if (self.show) self.show(self.context);
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)visible
{
    (void)app; (void)visible;
    [self showPanel:nil];
    return NO;
}
- (NSMenu *)applicationDockMenu:(NSApplication *)app
{
    NSMenu *menu = [self.original respondsToSelector:_cmd]
        ? [[self.original applicationDockMenu:app] copy] : nil;
    if (!menu) menu = [NSMenu new];
    NSMenuItem *show = [[NSMenuItem alloc] initWithTitle:@"Show Panel"
        action:@selector(showPanel:) keyEquivalent:@""];
    show.target = self;
    [menu insertItem:show atIndex:0];
    return menu;
}
@end
static CastPanelDelegate *panel_delegate;

int mac_panel_claim(const char *socket_path, char *error, size_t size)
{
    char lock_path[1024], parent[1024];
    struct stat st;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (!socket_path || socket_path[0] != '/' || strlen(socket_path) >= sizeof parent) {
        errno = EINVAL;
        goto failed;
    }
    snprintf(parent, sizeof parent, "%s", socket_path);
    char *slash = strrchr(parent, '/');
    if (slash == parent) slash[1] = 0;
    else *slash = 0;
    if (lstat(parent, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077)) {
        errno = EACCES;
        goto failed;
    }
    if (snprintf(request_path, sizeof request_path, "%s.panel", socket_path) >= (int)sizeof request_path ||
        snprintf(lock_path, sizeof lock_path, "%s.panel.lock", socket_path) >= (int)sizeof lock_path)
        goto failed;
    identity_lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (identity_lock < 0 || fstat(identity_lock, &st) || !S_ISREG(st.st_mode) ||
        st.st_uid != getuid() || (st.st_mode & 0077) || st.st_nlink != 1) goto failed;
    memcpy(address.sun_path, request_path, strlen(request_path) + 1);
    if (flock(identity_lock, LOCK_EX | LOCK_NB)) {
        if (errno != EWOULDBLOCK) goto failed;
        int sender = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (sender < 0) goto failed;
        if (fcntl(sender, F_SETFL, O_NONBLOCK)) { close(sender); goto failed; }
        bool delivered = false;
        /* The first launch binds immediately after taking its lock. */
        for (int attempt = 0; attempt < 20; attempt++) {
            if (sendto(sender, "S", 1, 0, (void *)&address, sizeof address) == 1) {
                delivered = true;
                break;
            }
            if (errno != ENOENT && errno != ECONNREFUSED) break;
            usleep(10000);
        }
        close(sender);
        close(identity_lock);
        identity_lock = -1;
        if (!delivered) goto failed;
        return 1;
    }
    requests = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (requests < 0 || fcntl(requests, F_SETFD, FD_CLOEXEC) ||
        fcntl(requests, F_SETFL, O_NONBLOCK)) goto failed;
    /* Only the lock owner can replace a stale endpoint. */
    unlink(request_path);
    if (bind(requests, (void *)&address, sizeof address) || chmod(request_path, 0600)) goto failed;
    return 0;
failed:
    snprintf(error, size, "cannot acquire or reopen control panel: %s", strerror(errno));
    mac_panel_release();
    return -1;
}
void mac_panel_attach(void (*show)(void *), void *context)
{
    panel_delegate = [CastPanelDelegate new];
    panel_delegate.original = NSApp.delegate;
    panel_delegate.show = show;
    panel_delegate.context = context;
    NSApp.delegate = panel_delegate;
}
void mac_panel_poll(void)
{
    char message[32];
    bool show = false;
    for (int i = 0; i < 32 && recv(requests, message, sizeof message, 0) > 0; i++) show = true;
    if (show) [panel_delegate showPanel:nil];
}
void mac_panel_release(void)
{
    if (panel_delegate && NSApp.delegate == panel_delegate) NSApp.delegate = panel_delegate.original;
    panel_delegate = nil;
    if (requests >= 0) { close(requests); requests = -1; unlink(request_path); }
    if (identity_lock >= 0) { close(identity_lock); identity_lock = -1; }
}
