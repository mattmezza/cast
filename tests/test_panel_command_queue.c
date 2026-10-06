/* Exercise the production acknowledgement watermark without a desktop/socket.
 * Privacy requests can finish before the model preparation they interrupt. */
#include "../src/panel_transport.c"
#include <assert.h>

int main(void)
{
    PanelClient client = {0};
    assert(!pthread_mutex_init(&client.mutex, NULL));
    for (unsigned round = 0; round < 5; ++round) {
        uint64_t first = client.completed + 1;
        for (unsigned i = 1; i < PANEL_QUEUE; ++i) {
            QueuedCommand command = {.id = first + i};
            client_acknowledge(&client, &command, 0, "privacy acknowledged");
            assert(client.completed == first - 1);
        }
        QueuedCommand prepared = {.id = first};
        client_acknowledge(&client, &prepared, -1, "preparation cancelled by privacy");
        assert(client.completed == first + PANEL_QUEUE - 1);
        assert(!client.acknowledged && client.snapshot.command_failed);
        assert(strstr(client.snapshot.last_reply, "cancelled"));
    }
    const char *pause[] = {"pause"};
    const char *off[] = {"transcription", "off"};
    const char *reveal[] = {"stream", "resume"};
    const char *load[] = {"notes", "load", "/test/file.md"};
    const char *status[] = {"status"};
    const char *model[] = {"settings", "transcription.model", "/test/model.bin"};
    const char *panel_off[] = {"settings", "transcription.enabled", "false", "subtitles.virtual",
                               "false",    "subtitles.sidecar",     "none"};
    const char *mixed[] = {"settings", "transcription.enabled", "false", "transcription.model",
                           "/test/model.bin"};
    assert(cast_command_privacy_priority(1, pause));
    assert(cast_command_privacy_priority(2, off));
    assert(!cast_command_privacy_priority(2, reveal));
    assert(!cast_command_privacy_priority(3, load));
    assert(cast_command_privacy_priority(7, panel_off));
    assert(!cast_command_privacy_priority(5, mixed));
    assert(cast_command_reply_timeout(7, panel_off, 5000) == 5000);
    assert(cast_command_reply_timeout(3, load, 5000) == 125000);
    assert(cast_command_reply_timeout(3, model, 5000) == 125000);
    assert(cast_command_reply_timeout(1, status, 5000) == 5000);
    assert(cast_command_reply_timeout(2, off, 5000) == 5000);
    client.completed = client.queued = 0;
    client.snapshot.connected = true;
    client.config.ipc_timeout_ms = 5000;
    client.wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    assert(client.wake >= 0);
    char error[CAST_ERR];
    for (unsigned i = 0; i < PANEL_QUEUE - 1; ++i) {
        assert(!panel_client_command(&client, 3, model, error, sizeof error));
    }
    assert(panel_client_command(&client, 3, model, error, sizeof error));
    assert(!panel_client_command(&client, 1, pause, error, sizeof error));
    assert(client.count == PANEL_QUEUE && client.queued == PANEL_QUEUE);
    assert(client.queue[PANEL_QUEUE - 1].privacy_priority);
    assert(panel_client_command(&client, 1, pause, error, sizeof error));
    close(client.wake);
    pthread_mutex_destroy(&client.mutex);
    puts("panel queue: privacy priorities, preparation budget and ordered acknowledgements PASS");
    return 0;
}
