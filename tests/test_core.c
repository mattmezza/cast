#include "cast.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void load_test(const char *text, bool pass, Config *c)
{
    char path[] = "/tmp/cast-config-XXXXXX", err[CAST_ERR];
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    fputs(text, f);
    fclose(f);
    Config before = *c;
    int rc = config_load(c, path, true, err, sizeof err);
    if (pass) {
        if (rc) {
            fprintf(stderr, "%s\n", err);
        }
        assert(!rc);
    } else {
        assert(rc);
        assert(!memcmp(c, &before, sizeof *c));
        assert(strstr(err, path));
    }
    unlink(path);
}
int main(void)
{
    Config c;
    config_defaults(&c);
    char e[CAST_ERR];
    assert(!config_validate(&c, e, sizeof e));
    Config settings = c;
    assert(!config_set_value(&settings, "camera.radius", "32", e, sizeof e));
    assert(settings.radius == 32 && c.radius == 24);
    Config unchanged = settings;
    assert(config_set_value(&settings, "camera.radius", "bad", e, sizeof e));
    assert(!memcmp(&settings, &unchanged, sizeof settings));
    assert(config_set_value(&settings, "camera.unknown", "1", e, sizeof e));
    assert(!memcmp(&settings, &unchanged, sizeof settings));
    assert(!config_set_value(&settings, "output.pause_text", "Back in five minutes", e, sizeof e));
    assert(!strcmp(settings.pause_text, "Back in five minutes"));
    load_test(
        "[camera]\nwidth_percent=25\n[preset.lecture]\nlayout=split\ncamera_width_percent=30\n",
        true, &c);
    assert(c.camera_width_percent == 25 && c.preset_count == 4);
    load_test("[keys]\nmode=garbage\n", false, &c);
    load_test("[unknown]\n", false, &c);
    load_test("[output]\nunknown=1\n", false, &c);
    load_test("[keys]\nenabled=true\nenabled=false\n", false, &c);
    load_test("[output]\nwidth=333\n", false, &c);
    load_test("[preset.bad]\nsecret=foo\n", false, &c);
    load_test("[zoom]\nmax=1\nfactor=2\n", false, &c);
    load_test("[audio]\ndesktop=true\n", false, &c);
    load_test("[composition]\nlayout_order=screen,screen\n", false, &c);
    load_test("[preset.a_name_that_exceeds_the_parser_section_name_limit_without_truncation]"
              "\nlayout=screen\n",
              false, &c);
    load_test("[keys]\nenabled=on\nmode: all\nfilter=Ctrl+Super+Shift+S\n", true, &c);
    assert(c.keys && !strcmp(c.keys_mode, "all"));
    Config before = c;
    assert(config_load(&c, "/does/not/exist/cast.conf", true, e, sizeof e));
    assert(!memcmp(&c, &before, sizeof c));
    assert(!config_load(&c, "/does/not/exist/cast.conf", false, e, sizeof e));
    assert(c.camera_width_percent == 22);
    /* Every initial output combination must survive idempotent group pause. */
    for (int live_paused = 0; live_paused <= 1; live_paused++) {
        for (int recording = 0; recording <= 1; recording++) {
            for (int record_paused = 0; record_paused <= 1; record_paused++) {
                State initial = {.live_paused = live_paused,
                                 .recording = recording,
                                 .record_paused = record_paused};
                State actual = initial;
                assert(!state_command(&actual, "pause", "", e, sizeof e));
                assert(actual.live_paused);
                if (recording) {
                    assert(actual.record_paused);
                }
                assert(!state_command(&actual, "pause", "", e, sizeof e));
                assert(!state_command(&actual, "resume", "", e, sizeof e));
                assert(actual.live_paused == initial.live_paused);
                assert(actual.recording == initial.recording);
                assert(actual.record_paused == initial.record_paused);
                assert(!actual.group_paused && !actual.group_live_restore &&
                       !actual.group_record_restore);
            }
        }
    }
    State s = {.live_paused = true};
    assert(state_command(&s, "record", "toggle", e, sizeof e));
    s.live_paused = false;
    s.recording = true;
    assert(!state_command(&s, "pause", "", e, sizeof e));
    assert(s.live_paused && s.record_paused);
    assert(!state_command(&s, "pause", "", e, sizeof e));
    assert(!state_command(&s, "resume", "", e, sizeof e));
    assert(!s.live_paused && !s.record_paused);
    s.record_paused = true;
    state_command(&s, "pause", "", e, sizeof e);
    state_command(&s, "resume", "", e, sizeof e);
    assert(!s.live_paused && s.record_paused);
    s.record_paused = false;
    state_command(&s, "pause", "", e, sizeof e);
    state_command(&s, "record", "pause", e, sizeof e);
    state_command(&s, "live", "resume", e, sizeof e);
    state_command(&s, "resume", "", e, sizeof e);
    assert(!s.live_paused && s.record_paused);
    state_command(&s, "live", "pause", e, sizeof e);
    state_command(&s, "live", "freeze", e, sizeof e);
    assert(s.live_paused && s.live_frozen);
    state_command(&s, "live", "unfreeze", e, sizeof e);
    assert(s.live_paused && !s.live_frozen);
    puts("core configuration and privacy-state tests passed");
    return 0;
}
