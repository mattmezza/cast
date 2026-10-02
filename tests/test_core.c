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
static void test_presentation_config(void)
{
    Config c;
    config_defaults(&c);
    char error[CAST_ERR];
    assert(c.pause_foreground == 0xffffff && c.blur_radius == 32 && c.blur_opacity == .60);
    assert(!strcmp(c.pause_font, "Noto Sans") && !strcmp(c.blur_font, "Noto Sans"));
    assert(!c.pause_footer[0] && !c.blur_footer[0]);
    assert(c.pause_footer_size == 18 && c.blur_footer_size == 18);
    assert(c.pause_text_gap == 12 && c.blur_text_gap == 12);
    load_test("[output]\npause_footer=Réunion {date} {{private}}\nblur_footer={time}\n"
              "pause_footer_size=20\nblur_footer_size=22\npause_text_gap=0\nblur_text_gap=512\n",
              true, &c);
    assert(!strcmp(c.pause_footer, "Réunion {date} {{private}}") &&
           !strcmp(c.blur_footer, "{time}"));
    assert(c.pause_footer_size == 20 && c.blur_footer_size == 22 && c.pause_text_gap == 0 &&
           c.blur_text_gap == 512);
    load_test("[output]\npause_footer={unknown}\n", false, &c);
    load_test("[output]\nblur_footer={time:%1000H}{date:%1000Y}\n", false, &c);
    load_test("[output]\npause_footer_size=7\n", false, &c);
    load_test("[output]\nblur_footer_size=257\n", false, &c);
    load_test("[output]\npause_text_gap=-1\n", false, &c);
    load_test("[output]\nblur_text_gap=513\n", false, &c);
    load_test("[output]\npause_text=Back at {time:%H:%M}\npause_color=#123456\n", true, &c);
    assert(!strcmp(c.pause_text, "Back at {time:%H:%M}") && c.pause_color == 0x123456);
    load_test("[output]\npause_title=Réunion {{private}}\npause_subtitle={date}\n"
              "pause_background=#234567\npause_foreground=#000000\n"
              "pause_title_size=56\npause_subtitle_size=28\nblur_title=Break\n"
              "blur_subtitle={datetime:%Y-%m-%d %H:%M}\nblur_color=#345678\n"
              "blur_foreground=#fedcba\nblur_radius=48\nblur_opacity=0.75\n",
              true, &c);
    assert(c.pause_color == 0x234567 && c.pause_foreground == 0 && c.pause_title_size == 56);
    assert(!strcmp(c.pause_subtitle, "{date}") && c.blur_radius == 48 && c.blur_opacity == .75);
    load_test("[output]\npause_title=One\npause_text=Two\n", false, &c);
    load_test("[output]\npause_background=#111111\npause_color=#222222\n", false, &c);
    load_test("[output]\npause_title={password}\n", false, &c);
    load_test("[output]\nblur_subtitle={date\n", false, &c);
    load_test("[output]\npause_subtitle={time:%Q}\n", false, &c);
    load_test("[output]\npause_subtitle={time:%99999999999Y}\n", false, &c);
    load_test("[output]\npause_title={date:%1000Y}{date:%1000Y}\n", false, &c);
    char oversized[300];
    memset(oversized, 'x', 200);
    snprintf(oversized + 200, sizeof oversized - 200, "{date:%%1400Y}");
    assert(config_set_value(&c, "output.blur_subtitle", oversized, error, sizeof error) < 0);
    load_test("[output]\nblur_radius=0\n", false, &c);
    load_test("[output]\nblur_radius=129\n", false, &c);
    load_test("[output]\nblur_opacity=nan\n", false, &c);
    load_test("[output]\npause_font=\n", false, &c);
    load_test("[output]\npause_title=\npause_subtitle=\npause_footer=\n"
              "blur_title=\nblur_subtitle=\nblur_footer=\n",
              true, &c);
    const char *anchors[] = {"top",        "bottom",        "left",        "right",
                             "top-center", "bottom-center", "center-left", "center-right"};
    for (size_t i = 0; i < sizeof anchors / sizeof *anchors; i++) {
        assert(!config_set_value(&c, "camera.anchor", anchors[i], error, sizeof error));
        assert(!config_validate(&c, error, sizeof error));
    }
    load_test("[camera]\nanchor=top\ncorner_order=top,right,bottom,left\n"
              "[preset.edges]\ncamera_anchor=center-left\n",
              true, &c);
    assert(!strcmp(c.anchor, "top"));
    load_test("[camera]\nbackground=gradient\nbackground_color=#223344\n"
              "background_blur_radius=100\nbackground_brightness=0.30\n",
              true, &c);
    assert(!strcmp(c.camera_background, "gradient") && c.camera_background_color == 0x223344);
    assert(c.camera_background_blur_radius == 100 && c.camera_background_brightness == .30);
    load_test("[camera]\nbackground=unknown\n", false, &c);
    load_test("[camera]\nbackground_brightness=-1\n", false, &c);
    load_test("[camera]\nbackground_blur_radius=129\n", false, &c);
}
int main(void)
{
    test_presentation_config();
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
    /* Group controls restore solid flags and preserve every orthogonal mode. */
    for (unsigned modes = 0; modes < 128; modes++) {
        State initial = {.recording = true,
                         .live_paused = modes & 1,
                         .live_frozen = modes & 2,
                         .live_blurred = modes & 4,
                         .record_paused = modes & 8,
                         .record_frozen = modes & 16,
                         .record_blurred = modes & 32,
                         .record_cut = modes & 64};
        State actual = initial;
        assert(!state_command(&actual, "pause", "", e, sizeof e));
        assert(!state_command(&actual, "resume", "", e, sizeof e));
        assert(!memcmp(&actual, &initial, sizeof initial));
    }
    s = (State){.recording = true, .record_cut = true};
    assert(!state_command(&s, "pause", "", e, sizeof e));
    assert(!state_command(&s, "live", "blur", e, sizeof e));
    assert(!state_command(&s, "record", "freeze", e, sizeof e));
    assert(!state_command(&s, "resume", "", e, sizeof e));
    assert(!s.live_paused && !s.record_paused && s.live_blurred && s.record_frozen && s.record_cut);
    assert(!state_command(&s, "record", "toggle", e, sizeof e));
    assert(s.record_paused && s.record_cut);
    assert(!state_command(&s, "record", "resume", e, sizeof e));
    assert(!s.record_cut && s.record_paused && s.record_frozen);
    puts("core configuration and privacy-state tests passed");
    return 0;
}
