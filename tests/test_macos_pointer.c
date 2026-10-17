#include "macos_pointer.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    Cursor p = mac_pointer_map(-720, 450, -1440, 0, 1440, 900, 2880, 1800, 42);
    assert(p.valid && p.x == 1440 && p.y == 900 && p.ts_ns == 42);
    assert(!mac_pointer_map(0, 0, -1440, 0, 1440, 900, 2880, 1800, 1).valid);
    assert(!mac_pointer_map(NAN, 0, 0, 0, 100, 100, 200, 200, 1).valid);
    assert(!mac_pointer_map(0, 0, 0, 0, 0, 100, 200, 200, 1).valid);
    MacPointerQueue queue = {0};
    MacPointerClick clicks[64];
    for (unsigned i = 0; i < 100; ++i)
        mac_pointer_push(&queue, (MacPointerClick){i, i + 1, 1, i + 2});
    assert(queue.count == 64);
    assert(mac_pointer_drain(&queue, false, clicks) == 64 && queue.count == 0);
    assert(clicks[0].x == 0 && clicks[63].at == 65);
    assert(mac_pointer_drain(&queue, false, clicks) == 0);
    mac_pointer_push(&queue, (MacPointerClick){100, 200, 3, 300});
    assert(mac_pointer_drain(&queue, true, clicks) == 0 && queue.count == 0);
    for (unsigned i = 0; i < 64; ++i) assert(!queue.clicks[i].at);
    assert(mac_pointer_drain(&queue, false, clicks) == 0);
    puts("macOS pointer: Retina/negative origin/bounds, overflow ordering, privacy retirement PASS");
    return 0;
}
