#include "daemon_bridge.h"
#include "workspace_extension.h"
#include "../src/panel_transport.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct DaemonBridge {
    PanelClient *client;
    Frame frame;
    int target;
    uint64_t generation, privacy_epoch, revision;
    bool pending;
    uint64_t edit_generation, edit_ack;
    char edit_error[256];
    AppDeviceKind discovery;
    AppDevices devices, screens, cameras, microphones, desktops;
    uint64_t devices_generation;
    bool metadata_valid;
    unsigned char metadata[offsetof(AppLive, devices)];
};

static void clear_frame(DaemonBridge *bridge)
{
    frame_free(&bridge->frame);
}

DaemonBridge *bridge_open(const Config *config, uint64_t window, char *error, size_t size)
{
    if (!config) {
        if (error && size) snprintf(error, size, "workspace configuration unavailable");
        return NULL;
    }
    DaemonBridge *bridge = calloc(1, sizeof *bridge);
    if (!bridge) {
        if (error && size) snprintf(error, size, "cannot allocate workspace connection");
        return NULL;
    }
    bridge->target = -1;
    bridge->client = panel_client_open(config, window, error, size);
    if (!bridge->client) {
        free(bridge);
        return NULL;
    }
    return bridge;
}

void bridge_close(DaemonBridge *bridge)
{
    if (!bridge) return;
    panel_client_close(bridge->client);
    clear_frame(bridge);
    free(bridge);
}

static int refuse_edit(DaemonBridge *bridge, char *error, size_t size, const char *message)
{
    if (bridge) snprintf(bridge->edit_error, sizeof bridge->edit_error, "%s", message);
    if (error && size) snprintf(error, size, "%s", message);
    return -1;
}

static bool listed(const char *value, const char *const *allowed, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(value, allowed[i])) return true;
    return false;
}

/* Decimal input only: reject signs, whitespace, exponents and partial parses. */
static bool decimal_range(const char *value, double low, double high)
{
    bool digit = false, dot = false;
    for (const char *p = value; *p; ++p) {
        if (*p >= '0' && *p <= '9') digit = true;
        else if (*p == '.' && !dot) dot = true;
        else return false;
    }
    double number = strtod(value, NULL);
    return digit && isfinite(number) && number >= low && number <= high;
}

static bool color_value(const char *value)
{
    if (strlen(value) != 7 || value[0] != '#') return false;
    for (unsigned i = 1; i < 7; ++i)
        if (!isxdigit((unsigned char)value[i])) return false;
    return true;
}

int bridge_edit(DaemonBridge *bridge, AppEdit edit, const char *value, char *error, size_t size)
{
    static const char *const layouts[] = {"overlay", "split", "screen", "camera", "stage"};
    static const char *const fits[] = {"contain", "cover"};
    static const char *const anchors[] = {"top-left", "top", "top-right", "left", "right",
                                         "bottom-left", "bottom", "bottom-right"};
    static const char *const backgrounds[] = {"blurred", "gradient", "solid"};
    const char *key = NULL;
    bool valid = false;
    if (!bridge || !value || strlen(value) > (edit == APP_EDIT_RECORD_DIRECTORY ? sizeof(((Config *)0)->record_dir)-1 : 31))
        return refuse_edit(bridge, error, size, "Invalid composition edit");
    if (bridge->pending)
        return refuse_edit(bridge, error, size, "Wait for the pending composition edit");
    switch (edit) {
    case APP_EDIT_LAYOUT:
        key = "composition.layout";
        valid = listed(value, layouts, sizeof layouts / sizeof *layouts);
        break;
    case APP_EDIT_FIT:
        key = "composition.fit";
        valid = listed(value, fits, sizeof fits / sizeof *fits);
        break;
    case APP_EDIT_ANCHOR:
        key = "camera.anchor";
        valid = listed(value, anchors, sizeof anchors / sizeof *anchors);
        break;
    case APP_EDIT_SIZE: key = "camera.width_percent"; valid = decimal_range(value, 1, 100); break;
    case APP_EDIT_BACKGROUND_SOURCE:
        key = "background.source"; valid = !strcmp(value, "screen") || !strcmp(value, "camera"); break;
    case APP_EDIT_SCREEN_BACKGROUND:
    case APP_EDIT_CAMERA_BACKGROUND:
        key = edit == APP_EDIT_SCREEN_BACKGROUND ? "screen.background" : "camera.background";
        valid = listed(value, backgrounds, sizeof backgrounds / sizeof *backgrounds); break;
    case APP_EDIT_SCREEN_BACKGROUND_COLOR: key = "screen.background_color"; valid = color_value(value); break;
    case APP_EDIT_CAMERA_BACKGROUND_COLOR: key = "camera.background_color"; valid = color_value(value); break;
    case APP_EDIT_GRADIENT_FROM: key = "background.gradient_from"; valid = color_value(value); break;
    case APP_EDIT_GRADIENT_VIA: key = "background.gradient_via"; valid = color_value(value); break;
    case APP_EDIT_GRADIENT_TO: key = "background.gradient_to"; valid = color_value(value); break;
    case APP_EDIT_GRADIENT_VIA_ENABLED:
        key = "background.gradient_via_enabled"; valid = !strcmp(value, "true") || !strcmp(value, "false"); break;
    case APP_EDIT_GRADIENT_ANGLE: key = "background.gradient_angle"; valid = decimal_range(value, 0, 360); break;
    case APP_EDIT_GRADIENT_WAYPOINT: key = "background.gradient_waypoint"; valid = decimal_range(value, 1, 99); break;
    case APP_EDIT_ZOOM_FACTOR: key = "zoom.factor"; valid = decimal_range(value, 1, 20); break;
    case APP_EDIT_ZOOM_MIN: key = "zoom.min"; valid = decimal_range(value, 1, 20); break;
    case APP_EDIT_ZOOM_MAX: key = "zoom.max"; valid = decimal_range(value, 1, 20); break;
    case APP_EDIT_ZOOM_FOLLOW:
        key = "zoom.follow"; valid = !strcmp(value, "true") || !strcmp(value, "false"); break;
    case APP_EDIT_RECORD_ACTION:
        key = "record"; valid = !strcmp(value,"start") || !strcmp(value,"stop") || !strcmp(value,"pause") || !strcmp(value,"resume") || !strcmp(value,"cut") || !strcmp(value,"cancel"); break;
    case APP_EDIT_RECORD_CONTAINER: key="record.container"; valid=!strcmp(value,"matroska") || !strcmp(value,"mp4") || !strcmp(value,"mov") || !strcmp(value,"webm"); break;
    case APP_EDIT_RECORD_VIDEO_CODEC: key="record.video_codec"; valid=!strcmp(value,"auto") || !strcmp(value,"libx264") || !strcmp(value,"libx265") || !strcmp(value,"libvpx-vp9") || !strcmp(value,"libaom-av1"); break;
    case APP_EDIT_RECORD_AUDIO_CODEC: key="record.audio_codec"; valid=!strcmp(value,"aac") || !strcmp(value,"libopus") || !strcmp(value,"pcm_s16le") || !strcmp(value,"flac"); break;
    case APP_EDIT_RECORD_CRF: key="record.crf"; valid=decimal_range(value,0,51) && !strchr(value,'.'); break;
    case APP_EDIT_RECORD_DIRECTORY:
        key="record.directory"; valid=*value!=0;
        for(const char *p=value;*p;++p) if((unsigned char)*p<32 || (unsigned char)*p==127)valid=false;
        break;
    case APP_EDIT_RECORD_COUNTDOWN: key="record.countdown"; valid=decimal_range(value,0,60) && !strchr(value,'.'); break;
    case APP_EDIT_AUDIO_MIC: case APP_EDIT_AUDIO_DESKTOP: case APP_EDIT_AUDIO_VIRTUAL:
        key=edit==APP_EDIT_AUDIO_MIC ? "mic" : edit==APP_EDIT_AUDIO_DESKTOP ? "desktop" : "virtual";
        valid=!strcmp(value,"true") || !strcmp(value,"false"); break;
    case APP_EDIT_AUDIO_MIC_GAIN: case APP_EDIT_AUDIO_DESKTOP_GAIN:
        key=edit==APP_EDIT_AUDIO_MIC_GAIN ? "mic" : "desktop"; valid=decimal_range(value,0,400); break;
    case APP_EDIT_NOTES_ACTION: key="notes"; valid=!strcmp(value,"open") || !strcmp(value,"close"); break;
    default: break;
    }
    if (!valid) return refuse_edit(bridge, error, size, "Unsupported composition value");
    PanelSnapshot snapshot;
    if (!panel_client_snapshot(bridge->client, &snapshot))
        return refuse_edit(bridge, error, size, "Daemon is busy; try the edit again");
    if (!snapshot.connected)
        return refuse_edit(bridge, error, size, "Daemon disconnected; edit was not queued");
    if (snapshot.command_queued != snapshot.command_completed || snapshot.command_queued == UINT64_MAX)
        return refuse_edit(bridge, error, size, "Daemon command queue is busy");
    double number = strtod(value, NULL);
    const Config *config = &snapshot.config;
    if ((edit == APP_EDIT_ZOOM_FACTOR && (number < config->zoom_min || number > config->zoom_max)) ||
        (edit == APP_EDIT_ZOOM_MIN && (number > config->zoom_max || number > config->zoom_factor)) ||
        (edit == APP_EDIT_ZOOM_MAX && (number < config->zoom_min || number < config->zoom_factor)))
        return refuse_edit(bridge, error, size, "Zoom factor must remain between minimum and maximum");
    if (edit == APP_EDIT_ZOOM_FOLLOW && !strcmp(value, "true") && !snapshot.capabilities.cursor_metadata)
        return refuse_edit(bridge, error, size, "Cursor follow requires cursor metadata from the capture backend");
    if (edit == APP_EDIT_RECORD_ACTION) {
        bool start = !strcmp(value,"start"), stop = !strcmp(value,"stop"), cancel = !strcmp(value,"cancel");
        if ((start && (snapshot.state.recording || snapshot.countdown || snapshot.finalizing)) ||
            (stop && !snapshot.state.recording && !snapshot.countdown) ||
            (cancel && !snapshot.countdown) || (!start && !stop && !cancel && (!snapshot.state.recording || snapshot.countdown || snapshot.finalizing)))
            return refuse_edit(bridge,error,size,"Recording action is unavailable in the current state");
    }
    if (((edit >= APP_EDIT_RECORD_CONTAINER && edit <= APP_EDIT_RECORD_CRF) || edit == APP_EDIT_RECORD_DIRECTORY) &&
        (snapshot.state.recording || snapshot.countdown || snapshot.finalizing))
        return refuse_edit(bridge,error,size,"Stop the recording before changing recording settings");
    if (edit == APP_EDIT_AUDIO_VIRTUAL && !strcmp(value,"true") && !strcmp(config->backend,"macos"))
        return refuse_edit(bridge,error,size,"Virtual microphone is unavailable on macOS");
    if (edit == APP_EDIT_NOTES_ACTION) {
        bool entitled = false;
        for (unsigned i=0;i<CAST_FEATURE_COUNT;++i)
            if (!strcmp(snapshot.edition.features[i].id,"speech_teleprompter"))
                entitled = snapshot.edition.features[i].entitled;
        if (!entitled) return refuse_edit(bridge,error,size,"Prompter requires an active Pro teleprompter entitlement");
    }
    char transport_error[CAST_ERR];
    int result;
    if (edit == APP_EDIT_ZOOM_FOLLOW || edit == APP_EDIT_ZOOM_FACTOR) {
        const char *args[] = {"zoom", edit == APP_EDIT_ZOOM_FACTOR ? "set" : "follow",
            edit == APP_EDIT_ZOOM_FACTOR ? value : !strcmp(value, "true") ? "on" : "off"};
        result = panel_client_command(bridge->client, 3, args, transport_error, sizeof transport_error);
    } else if (edit == APP_EDIT_RECORD_ACTION || edit == APP_EDIT_NOTES_ACTION) {
        const char *args[]={edit == APP_EDIT_NOTES_ACTION ? "notes" : "record",value};
        result=panel_client_command(bridge->client,2,args,transport_error,sizeof transport_error);
    } else if (edit >= APP_EDIT_AUDIO_MIC && edit <= APP_EDIT_AUDIO_DESKTOP_GAIN) {
        bool gain=edit==APP_EDIT_AUDIO_MIC_GAIN || edit==APP_EDIT_AUDIO_DESKTOP_GAIN;
        char percent[40];snprintf(percent,sizeof percent,"%s%%",value);
        const char *args[]={"audio",key,gain ? "gain" : !strcmp(value,"true") ? "on" : "off",percent};
        result=panel_client_command(bridge->client,gain ? 4 : 3,args,transport_error,sizeof transport_error);
    } else result = panel_client_setting(bridge->client, key, value, transport_error, sizeof transport_error);
    if (result)
        return refuse_edit(bridge, error, size, "Composition edit could not be queued");
    bridge->edit_generation = snapshot.daemon_generation;
    bridge->edit_ack = snapshot.command_queued + 1;
    bridge->pending = true;
    bridge->edit_error[0] = 0;
    if (error && size) error[0] = 0;
    return 0;
}

static bool monitor_backend(const Config *config)
{
    return !strcmp(config->backend, "xorg") || !strcmp(config->backend, "x11") ||
           !strcmp(config->backend, "macos");
}

static int source_snapshot(DaemonBridge *bridge, PanelSnapshot *snapshot, char *error, size_t size)
{
    if (!bridge) return refuse_edit(NULL, error, size, "Workspace connection unavailable");
    if (bridge->pending) return refuse_edit(bridge, error, size, "Wait for the pending request");
    if (!panel_client_snapshot(bridge->client, snapshot))
        return refuse_edit(bridge, error, size, "Daemon is busy; try again");
    if (!snapshot->connected) return refuse_edit(bridge, error, size, "Daemon disconnected; request was not queued");
    if (snapshot->command_queued != snapshot->command_completed || snapshot->command_queued == UINT64_MAX)
        return refuse_edit(bridge, error, size, "Daemon command queue is busy");
    return 0;
}

static void source_queued(DaemonBridge *bridge, const PanelSnapshot *snapshot)
{
    bridge->pending = true;
    bridge->edit_generation = snapshot->daemon_generation;
    bridge->edit_ack = snapshot->command_queued + 1;
    bridge->edit_error[0] = 0;
}

static AppDevices *device_cache(DaemonBridge *bridge, AppDeviceKind kind)
{
    return kind == APP_DEVICE_SCREEN ? &bridge->screens : kind == APP_DEVICE_CAMERA ? &bridge->cameras :
        kind == APP_DEVICE_MIC ? &bridge->microphones : &bridge->desktops;
}

int bridge_devices(DaemonBridge *bridge, AppDeviceKind kind, char *error, size_t size)
{
    if (kind < APP_DEVICE_SCREEN || kind > APP_DEVICE_DESKTOP)
        return refuse_edit(bridge, error, size, "Unsupported device kind");
    PanelSnapshot snapshot;
    if (source_snapshot(bridge, &snapshot, error, size)) return -1;
    if (kind == APP_DEVICE_SCREEN && !monitor_backend(&snapshot.config))
        return refuse_edit(bridge, error, size, "This capture backend uses desktop consent instead of a display list");
    const char *args[] = {kind == APP_DEVICE_SCREEN ? "screen" : kind == APP_DEVICE_CAMERA ? "camera" : "audio", "list"};
    char detail[CAST_ERR];
    if (panel_client_command(bridge->client, 2, args, detail, sizeof detail))
        return refuse_edit(bridge, error, size, "Device discovery could not be queued");
    source_queued(bridge, &snapshot);
    bridge->discovery = kind;
    bridge->devices = *device_cache(bridge, kind);
    bridge->devices.kind = kind;
    bridge->devices.generation = snapshot.daemon_generation;
    bridge->devices.loading = true;
    bridge->devices.error[0] = 0;
    if (error && size) error[0] = 0;
    return 0;
}

int bridge_source(DaemonBridge *bridge, AppDeviceKind kind, const char *id, char *error, size_t size)
{
    if (!id || (kind < APP_DEVICE_SCREEN || kind > APP_DEVICE_DESKTOP))
        return refuse_edit(bridge, error, size, "Invalid source selection");
    PanelSnapshot snapshot;
    if (source_snapshot(bridge, &snapshot, error, size)) return -1;
    const AppDevices *cache = device_cache(bridge, kind);
    bool found = false;
    for (unsigned i = 0; i < cache->count; ++i)
        if (!strcmp(id, cache->choices[i].id)) found = true;
    size_t capacity = kind == APP_DEVICE_SCREEN ? sizeof snapshot.config.monitor : kind == APP_DEVICE_CAMERA ? sizeof snapshot.config.camera_device : sizeof snapshot.config.mic_source;
    if (*id && (!found || cache->generation != snapshot.daemon_generation || strlen(id) >= capacity))
        return refuse_edit(bridge, error, size, "Choose a device from the current discovered list");
    if (kind == APP_DEVICE_SCREEN && ((*id && !monitor_backend(&snapshot.config)) ||
        (!*id && strcmp(snapshot.config.backend, "wayland"))))
        return refuse_edit(bridge, error, size, "This display selection is unavailable for the capture backend");
    if (kind == APP_DEVICE_MIC || kind == APP_DEVICE_DESKTOP) {
        if (!*id || (!strcmp(snapshot.config.backend,"macos") && strcmp(id,"default")))
            return refuse_edit(bridge,error,size,"Choose a supported audio source");
        const char *audio_args[]={"audio",kind==APP_DEVICE_MIC ? "mic" : "desktop","source",id};
        char detail[CAST_ERR];
        if (panel_client_command(bridge->client,4,audio_args,detail,sizeof detail))
            return refuse_edit(bridge,error,size,"Audio source selection could not be queued");
        source_queued(bridge,&snapshot);
        if (error && size) error[0]=0;
        return 0;
    }
    const char *args[] = {kind == APP_DEVICE_SCREEN ? "screen" : "camera", "select", id};
    int count = 3;
    if (kind == APP_DEVICE_CAMERA) args[1] = "device";
    if (kind == APP_DEVICE_SCREEN && !*id) { args[0] = "capture"; args[1] = "monitor"; count = 2; }
    char detail[CAST_ERR];
    int result = kind == APP_DEVICE_CAMERA && !*id
        ? panel_client_setting(bridge->client, "camera.enabled", "false", detail, sizeof detail)
        : panel_client_command(bridge->client, count, args, detail, sizeof detail);
    if (result) return refuse_edit(bridge, error, size, "Source selection could not be queued");
    source_queued(bridge, &snapshot);
    if (error && size) error[0] = 0;
    return 0;
}

static bool parse_audio_devices(AppDevices *devices, const PanelSnapshot *snapshot, AppDeviceKind kind)
{
    const char *reply=snapshot->device_reply;
    const char *end=memchr(reply,0,sizeof snapshot->device_reply);
    if (!end || (end!=reply && end[-1]!='\n')) return false;
    bool mac=!strcmp(snapshot->config.backend,"macos");
    devices->kind=kind; devices->generation=snapshot->daemon_generation;
    for (const char *line=reply; line<end;) {
        const char *next=memchr(line,'\n',(size_t)(end-line));
        if (!next) return false;
        for (const char *p=line;p<next;++p) if ((unsigned char)*p<32 || (unsigned char)*p==127) return false;
        const char *split=strstr(line,mac ? " [" : " [id=");
        if (!split || split>=next || split==line || split-line>=256) return false;
        const char *suffix=memchr(split,']',(size_t)(next-split));
        if (!suffix || suffix+1>=next || suffix[1]!=' ') return false;
        bool include;
        if (mac) include=kind==APP_DEVICE_MIC ? !strncmp(split," [microphone] ",14) : !strncmp(split," [desktop] ",11);
        else {
            const char *cl=strstr(split," class=");
            if (!cl || cl>=suffix) return false;
            bool source=(size_t)(suffix-cl)>=19 && !strncmp(cl," class=Audio/Source",19);
            include=kind==APP_DEVICE_MIC ? source : source ||
                !strncmp(cl," class=Audio/Sink]",18) || !strncmp(cl," class=Stream/Output/Audio]",26);
            size_t id_length=(size_t)(split-line);
            if ((id_length==strlen(snapshot->config.virtual_name) && !memcmp(line,snapshot->config.virtual_name,id_length)) ||
                (id_length>=13 && !strncmp(line,"cast.capture.",13))) include=false;
        }
        if (include) {
            if (devices->count==APP_DEVICE_MAX) return false;
            AppDeviceChoice *choice=&devices->choices[devices->count];
            size_t len=(size_t)(split-line); memcpy(choice->id,line,len);choice->id[len]=0;
            for(unsigned i=0;i<devices->count;++i) if(!strcmp(choice->id,devices->choices[i].id)) return false;
            size_t label=(size_t)(next-(suffix+2));
            if (!label || label>=sizeof choice->label) return false;
            memcpy(choice->label,suffix+2,label);choice->label[label]=0;++devices->count;
        }
        line=next+1;
    }
    return true;
}

static bool parse_devices(AppDevices *devices, const PanelSnapshot *snapshot, AppDeviceKind kind)
{
    if (kind == APP_DEVICE_MIC || kind == APP_DEVICE_DESKTOP) return parse_audio_devices(devices,snapshot,kind);
    const char *reply = snapshot->device_reply;
    const char *end = memchr(reply, 0, sizeof snapshot->device_reply);
    devices->kind = kind;
    devices->generation = snapshot->daemon_generation;
    if (!end || (end != reply && end[-1] != '\n')) return false;
    if (kind == APP_DEVICE_CAMERA && !strcmp(reply,
        "No accessible V4L2 capture devices. Check device ACLs with getfacl /dev/videoN.\n")) return true;
    bool mac = !strcmp(snapshot->config.backend, "macos");
    for (const char *line = reply; line < end;) {
        const char *next = memchr(line, '\n', (size_t)(end - line));
        if (!next) return false;
        const char *start = line;
        if (kind == APP_DEVICE_SCREEN && !mac) {
            if (next - line < 3 || (line[0] != '*' && line[0] != ' ') || line[1] != ' ') return false;
            start += 2;
        }
        const char *split = start;
        while (split < next && *split != ' ' && !(mac && *split == ':')) ++split;
        /* Linux camera paths and macOS camera UIDs may contain colons. */
        if (kind == APP_DEVICE_CAMERA) { split = start; while (split < next && *split != ' ') ++split; }
        size_t id_size = (size_t)(split - start);
        size_t capacity = kind == APP_DEVICE_SCREEN ? sizeof snapshot->config.monitor : sizeof snapshot->config.camera_device;
        if (!id_size || id_size >= capacity || id_size >= APP_DEVICE_ID_MAX || split == next || devices->count == APP_DEVICE_MAX)
            return false;
        for (const char *p = start; p < next; ++p)
            if ((unsigned char)*p < 32 || (unsigned char)*p == 127) return false;
        if (kind == APP_DEVICE_SCREEN && mac) {
            for (const char *p = start; p < split; ++p) if (!isdigit((unsigned char)*p)) return false;
        }
        if (kind == APP_DEVICE_CAMERA && !mac) {
            if (id_size < 11 || strncmp(start, "/dev/video", 10)) return false;
            for (const char *p = start + 10; p < split; ++p) if (!isdigit((unsigned char)*p)) return false;
        }
        if (kind == APP_DEVICE_SCREEN) {
            int width = 0, height = 0, x = 0, y = 0, consumed = 0;
            char geometry[128];
            size_t bytes = (size_t)(next - split);
            if (bytes >= sizeof geometry) return false;
            memcpy(geometry, split, bytes); geometry[bytes] = 0;
            if (mac) {
                const char *dimensions = !strncmp(geometry, " (main): ", 9) ? geometry + 9
                    : !strncmp(geometry, ": ", 2) ? geometry + 2 : NULL;
                if (!dimensions || sscanf(dimensions, "%dx%d%n", &width, &height, &consumed) != 2 ||
                    dimensions[consumed]) return false;
            } else {
                if (sscanf(geometry, " %dx%d%d%d%n", &width, &height, &x, &y, &consumed) != 4 ||
                    (geometry[consumed] && strcmp(geometry + consumed, " primary"))) return false;
            }
            if (width < 1 || height < 1) return false;
        }
        AppDeviceChoice *choice = &devices->choices[devices->count];
        memcpy(choice->id, start, id_size);
        choice->id[id_size] = 0;
        for (unsigned i = 0; i < devices->count; ++i)
            if (!strcmp(choice->id, devices->choices[i].id)) return false;
        const char *label = kind == APP_DEVICE_SCREEN ? start : split + 1;
        const char *label_end = next;
        if (kind == APP_DEVICE_CAMERA && !mac) {
            const char *formats = strstr(label, " formats:");
            if (!formats || formats > next) return false;
            label_end = formats;
        }
        size_t label_size = (size_t)(label_end - label);
        if (!label_size || label_size >= sizeof choice->label) return false;
        memcpy(choice->label, label, label_size);
        choice->label[label_size] = 0;
        ++devices->count;
        line = next + 1;
    }
    return true;
}

static void edit_snapshot(DaemonBridge *bridge, const PanelSnapshot *snapshot, AppLive *out)
{
    if (snapshot && (!snapshot->connected ||
        (bridge->devices_generation && bridge->devices_generation != snapshot->daemon_generation))) {
        memset(&bridge->devices, 0, sizeof bridge->devices);
        memset(&bridge->screens, 0, sizeof bridge->screens);
        memset(&bridge->cameras, 0, sizeof bridge->cameras);
        memset(&bridge->microphones, 0, sizeof bridge->microphones);
        memset(&bridge->desktops, 0, sizeof bridge->desktops);
    }
    if (snapshot && snapshot->connected) bridge->devices_generation = snapshot->daemon_generation;
    if (snapshot && bridge->pending) {
        if (!snapshot->connected || snapshot->daemon_generation != bridge->edit_generation) {
            bridge->pending = false;
            snprintf(bridge->edit_error, sizeof bridge->edit_error,
                     "Daemon connection changed; request canceled. Inspect the current settings.");
        } else if (snapshot->command_completed >= bridge->edit_ack) {
            bridge->pending = false;
            if (bridge->discovery != APP_DEVICE_NONE) {
                AppDevices *parsed = calloc(1, sizeof *parsed);
                bool valid = parsed && snapshot->device_reply_id == bridge->edit_ack &&
                    !snapshot->device_reply_failed && parse_devices(parsed, snapshot, bridge->discovery);
                if (valid) {
                    bridge->devices = *parsed;
                    *device_cache(bridge,bridge->discovery) = *parsed;
                } else {
                    memset(device_cache(bridge,bridge->discovery),0,sizeof(AppDevices));
                    bridge->devices.count = 0;
                    snprintf(bridge->devices.error, sizeof bridge->devices.error, "Device discovery failed or returned an incomplete list. Try again.");
                }
                free(parsed);
            }
            if (snapshot->command_failed)
                snprintf(bridge->edit_error, sizeof bridge->edit_error,
                         "Daemon rejected the requested change. Current settings remain authoritative.");
            else bridge->edit_error[0] = 0;
        }
    }
    if (!bridge->pending) {
        bridge->devices.loading = false;
        bridge->discovery = APP_DEVICE_NONE;
    }
    out->devices = bridge->devices;
    out->pending = bridge->pending;
    snprintf(out->edit_error, sizeof out->edit_error, "%s", bridge->edit_error);
}

static void source_label(char *out, size_t size, const char *text)
{
    if (!size) return;
    size_t i = 0;
    for (; text[i] && i + 1 < size; ++i)
        out[i] = (unsigned char)text[i] < 32 || (unsigned char)text[i] == 127 ? '?' : text[i];
    out[i] = 0;
}

static void copy_config(AppLive *out, const PanelSnapshot *snapshot, const DaemonBridge *bridge)
{
    const Config *config = &snapshot->config;
    out->project=snapshot->project;
    const State *state = &snapshot->state;
    out->setting_count=config->extension_setting_count;
    if(out->setting_count>APP_SETTING_MAX)out->setting_count=APP_SETTING_MAX;
    for(unsigned i=0;i<out->setting_count;++i) {
        snprintf(out->settings[i].section,sizeof out->settings[i].section,"%s",config->extension_settings[i].section);
        snprintf(out->settings[i].key,sizeof out->settings[i].key,"%s",config->extension_settings[i].key);
        snprintf(out->settings[i].value,sizeof out->settings[i].value,"%s",config->extension_settings[i].value);
    }
#if defined(WITH_PRO) && defined(WITH_CUI)
    cast_workspace_snapshot(out,snapshot);
#endif
    snprintf(out->background_source, sizeof out->background_source, "%.*s", 15, config->background_source);
    snprintf(out->screen_background, sizeof out->screen_background, "%.*s", 15, config->screen_background);
    snprintf(out->camera_background, sizeof out->camera_background, "%.*s", 15, config->camera_background);
    out->screen_background_color = config->screen_background_color;
    out->camera_background_color = config->camera_background_color;
    out->gradient_from = config->gradient_from;
    out->gradient_via = config->gradient_via;
    out->gradient_to = config->gradient_to;
    out->gradient_via_enabled = config->gradient_via_enabled;
    out->gradient_angle = (float)config->gradient_angle;
    out->gradient_waypoint = (float)config->gradient_waypoint;
    out->zoom_factor = (float)config->zoom_factor;
    out->zoom_min = (float)config->zoom_min;
    out->zoom_max = (float)config->zoom_max;
    out->zoom_follow = config->zoom_follow;
    out->can_zoom_follow = snapshot->capabilities.cursor_metadata;
    out->cinematic_zoom_active = snapshot->cinematic_active;
    for (unsigned i = 0; i < CAST_FEATURE_COUNT; ++i)
        if (!strncmp(snapshot->edition.features[i].id, "cinematic_zoom", sizeof snapshot->edition.features[i].id))
            out->cinematic_zoom_available = snapshot->edition.features[i].entitled;
    out->can_select_monitor = monitor_backend(config);
    out->can_reselect_portal = !strcmp(config->backend, "wayland");
    out->can_select_camera = true;
    snprintf(out->screen_id, sizeof out->screen_id, "%.*s", (int)sizeof config->monitor - 1, config->monitor);
    snprintf(out->camera_id, sizeof out->camera_id, "%.*s", (int)sizeof config->camera_device - 1, config->camera_device);
    out->stream = (AppStreamSnapshot){.connected = snapshot->connected,
        .generation = snapshot->daemon_generation, .phase = (AppStreamPhase)snapshot->stream.state,
        .active = snapshot->stream.active, .paused = state->stream_paused,
        .frozen = state->stream_frozen, .blurred = state->stream_blurred,
        .video_bitrate_kbps = config->stream.video_bitrate_kbps,
        .audio_bitrate_kbps = config->stream.audio_bitrate_kbps,
        .reconnect_attempts = config->stream.reconnect_attempts,
        .reconnect_initial_ms = config->stream.reconnect_initial_ms,
        .reconnect_max_ms = config->stream.reconnect_max_ms,
        .outgoing_bitrate_kbps = snapshot->stream.outgoing_bitrate_kbps,
        .retry_attempt = snapshot->stream.retry_attempt, .queue_depth = snapshot->stream.queue_depth,
        .dropped_frames = snapshot->stream.dropped_frames, .bytes_written = snapshot->stream.bytes_written};
    snprintf(out->stream.service, sizeof out->stream.service, "%s", config->stream.service);
    snprintf(out->stream.server_url, sizeof out->stream.server_url, "%s", config->stream.server_url);
    snprintf(out->stream.key_file, sizeof out->stream.key_file, "%s", config->stream.key_file);
    snprintf(out->stream.video_encoder, sizeof out->stream.video_encoder, "%s", config->stream.video_encoder);
    snprintf(out->stream.encoder_preset, sizeof out->stream.encoder_preset, "%s", config->stream.encoder_preset);
    if (snapshot->stream.error[0])
        snprintf(out->stream.error, sizeof out->stream.error, "Stream failed. Check the destination, key file and connection, then retry.");
    out->audio_levels_supported = snapshot->audio_levels.supported;
    out->mic_level = (AppAudioLevel){.peak = snapshot->audio_levels.mic.peak,
        .rms = snapshot->audio_levels.mic.rms, .fresh = snapshot->audio_levels.mic.fresh,
        .clipped = snapshot->audio_levels.mic.clipped, .available = snapshot->audio_levels.mic.available};
    out->desktop_level = (AppAudioLevel){.peak = snapshot->audio_levels.desktop.peak,
        .rms = snapshot->audio_levels.desktop.rms, .fresh = snapshot->audio_levels.desktop.fresh,
        .clipped = snapshot->audio_levels.desktop.clipped, .available = snapshot->audio_levels.desktop.available};
    out->pro = snapshot->edition.identity.pro;
    out->license_valid = snapshot->edition.license.state == CAST_LICENSE_VALID;
    snprintf(out->license_id, sizeof out->license_id, "%s", snapshot->edition.license.masked_id);
    out->notes_window_open = snapshot->workflow.notes_window_open;
    for (unsigned i=0;i<CAST_FEATURE_COUNT;++i)
        if (!strcmp(snapshot->edition.features[i].id,"speech_teleprompter"))
            out->notes_available = snapshot->edition.features[i].entitled;
    out->generation = snapshot->daemon_generation;
    out->privacy_epoch = snapshot->privacy_epoch;
    out->width = config->width;
    out->height = config->height;
    out->fps = config->fps;
    snprintf(out->backend, sizeof out->backend, "%.*s", (int)sizeof out->backend - 1, config->backend);
    snprintf(out->record_container, sizeof out->record_container, "%.*s", (int)sizeof config->record_container, config->record_container);
    snprintf(out->record_codec, sizeof out->record_codec, "%.*s", (int)sizeof config->video_codec, config->video_codec);
    snprintf(out->stream_service, sizeof out->stream_service, "%.*s", (int)sizeof config->stream.service, config->stream.service);
    out->camera_width_percent = (float)config->camera_width_percent;
    /* These are validated configuration enums, never endpoints or credentials. */
    snprintf(out->layout, sizeof out->layout, "%.*s", (int)sizeof config->layout, config->layout);
    snprintf(out->fit, sizeof out->fit, "%.*s", (int)sizeof config->fit, config->fit);
    snprintf(out->anchor, sizeof out->anchor, "%.*s", (int)sizeof config->anchor, config->anchor);
    out->virtual_enabled = config->virtual_enabled;
    out->virtual_paused = state->virtual_paused;
    out->virtual_frozen = state->virtual_frozen;
    out->virtual_blurred = state->virtual_blurred;
    out->recording = state->recording;
    out->record_paused = state->record_paused;
    out->record_frozen = state->record_frozen;
    out->record_blurred = state->record_blurred;
    out->stream_active = state->stream_active;
    out->stream_paused = state->stream_paused;
    out->stream_frozen = state->stream_frozen;
    out->stream_blurred = state->stream_blurred;
    out->record_cut = state->record_cut;
    out->record_countdown_active = snapshot->countdown;
    out->record_finalizing = snapshot->finalizing;
    out->record_duration_ns = snapshot->duration_ns;
    out->record_countdown_remaining_ns = snapshot->countdown_remaining_ns;
    source_label(out->record_path,sizeof out->record_path,state->record_path);
    source_label(out->record_directory,sizeof out->record_directory,config->record_dir);
    source_label(out->record_audio_codec,sizeof out->record_audio_codec,config->audio_codec);
    source_label(out->record_encoder,sizeof out->record_encoder,snapshot->record_encoder);
    out->record_crf=config->record_crf;out->record_countdown=config->record_countdown;
    out->mic_gain=(float)(config->mic_gain*100);out->desktop_gain=(float)(config->desktop_gain*100);
    out->virtual_audio=config->virtual_audio;out->can_virtual_audio=!strcmp(config->backend,"xorg") || !strcmp(config->backend,"x11") || !strcmp(config->backend,"wayland");
    out->can_select_audio=!strcmp(config->backend,"macos") || !strcmp(config->backend,"xorg") ||
        !strcmp(config->backend,"x11") || !strcmp(config->backend,"wayland");
    source_label(out->mic_source,sizeof out->mic_source,config->mic_source);
    source_label(out->desktop_source,sizeof out->desktop_source,config->desktop_source);
    snprintf(out->mic_id,sizeof out->mic_id,"%s",out->mic_source);
    snprintf(out->desktop_id,sizeof out->desktop_id,"%s",out->desktop_source);
    source_label(out->audio_status,sizeof out->audio_status,snapshot->audio_status);
    out->audio_available=strstr(out->audio_status,"\"available\":true")!=NULL;
    out->mic_ready=strstr(out->audio_status,"\"mic_ready\":true")!=NULL;
    out->desktop_ready=strstr(out->audio_status,"\"desktop_ready\":true")!=NULL;
    out->virtual_ready=strstr(out->audio_status,"\"virtual_ready\":true")!=NULL;
    out->virtual_silent=strstr(out->audio_status,"\"virtual_silent\":true")!=NULL;
    out->mic = config->mic;
    out->desktop = config->desktop;
    out->camera_enabled = config->camera_enabled;
    out->camera_visible = config->camera_visible;
    out->mirror = config->mirror;
    bool default_display = !out->screen_id[0] || !strcmp(out->screen_id, "main");
    bool default_camera = !out->camera_id[0] || !strcmp(out->camera_id, "default");
    source_label(out->screen_name, sizeof out->screen_name,
                 default_display ? "Default display" : out->screen_id);
    source_label(out->camera_name, sizeof out->camera_name,
                 !config->camera_enabled ? "No camera" : default_camera ? "Default camera" : out->camera_id);
    snprintf(out->mic_name, sizeof out->mic_name, "%s", config->mic ? "Microphone" : "Microphone off");
    snprintf(out->desktop_name, sizeof out->desktop_name, "%s", config->desktop ? "Desktop audio" : "Desktop audio off");
    const AppDevices *caches[] = {&bridge->screens, &bridge->cameras};
    for (unsigned kind = 0; kind < 2; ++kind) {
        const AppDevices *cache = caches[kind];
        const char *id = kind ? out->camera_id : out->screen_id;
        bool default_source = kind ? default_camera : default_display;
        if (cache->generation != snapshot->daemon_generation || (kind && !config->camera_enabled)) continue;
        bool found = default_source && cache->count;
        for (unsigned i = 0; i < cache->count; ++i)
            if (!strcmp(id, cache->choices[i].id)) {
                char *label = kind ? out->camera_name : out->screen_name;
                source_label(label, 128, cache->choices[i].label);
                found = true;
                break;
            }
        if (!found) {
            char *label = kind ? out->camera_name : out->screen_name;
            char selected[128];
            source_label(selected, sizeof selected, default_source ?
                (kind ? "Default camera" : "Default display") : id);
            snprintf(label, 128, "%.113s (unavailable)", selected);
        }
    }
    if (snapshot->composition_error[0])
        snprintf(out->error, sizeof out->error, "%s", snapshot->composition_error);
    else if (snapshot->error[0] || snapshot->stream.error[0])
        snprintf(out->error, sizeof out->error, "Daemon reported an error. Inspect cast status for details.");
}

static void remember_metadata(DaemonBridge *bridge, const AppLive *out)
{
    memcpy(bridge->metadata, out, sizeof bridge->metadata);
    /* No borrowed pixel pointer or drawable state survives in this cache. */
    memset(bridge->metadata + offsetof(AppLive, frame), 0, sizeof out->frame);
    memset(bridge->metadata + offsetof(AppLive, has_frame), 0, sizeof out->has_frame);
    bridge->metadata_valid = true;
}

static void busy_metadata(DaemonBridge *bridge, int target, AppLive *out)
{
    if (bridge->metadata_valid) memcpy(out, bridge->metadata, sizeof bridge->metadata);
    out->frame = (cui_bitmap){0};
    out->has_frame = false;
    out->target = target;
    out->snapshot_busy = true;
    edit_snapshot(bridge, NULL, out);
}

void bridge_poll(DaemonBridge *bridge, int target, AppLive *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->enabled = true;
    out->target = target;
    snprintf(out->status, sizeof out->status, "Waiting for daemon");
    if (!bridge) {
        snprintf(out->error, sizeof out->error, "Workspace connection unavailable");
        return;
    }
    edit_snapshot(bridge, NULL, out);
    if (target < 0 || target > 2) {
        clear_frame(bridge);
        bridge->target = -1;
        snprintf(out->error, sizeof out->error, "Invalid preview target");
        return;
    }
    PanelSnapshot before, after;
    /* A missed snapshot cannot prove that retained pixels remain private. */
    bool sampled = panel_client_snapshot(bridge->client, &before);
    edit_snapshot(bridge, sampled ? &before : NULL, out);
    if (!sampled || !before.connected) {
        clear_frame(bridge);
        if (!sampled) {
            busy_metadata(bridge, target, out);
            return;
        }
        bridge->generation = 0;
        bridge->metadata_valid = false;
        if (before.error[0]) {
            snprintf(out->error, sizeof out->error, "%.*s", (int)sizeof out->error-1, before.error);
            snprintf(out->status, sizeof out->status, "Daemon not connected");
        }
        return;
    }
    if (before.daemon_generation != bridge->generation) bridge->metadata_valid = false;
    if (target != bridge->target || before.daemon_generation != bridge->generation ||
        before.privacy_epoch != bridge->privacy_epoch) clear_frame(bridge);
    bridge->target = target;
    bridge->generation = before.daemon_generation;
    bridge->privacy_epoch = before.privacy_epoch;
    char error[CAST_ERR] = {0};
    int copied = panel_client_frame(bridge->client, PANEL_PREVIEW_COMPOSITION, &bridge->frame, error, sizeof error);
    bool observed = panel_client_snapshot(bridge->client, &after);
    edit_snapshot(bridge, observed ? &after : NULL, out);
    if (copied < 0 || !observed || !after.connected ||
        after.daemon_generation != before.daemon_generation ||
        after.privacy_epoch != before.privacy_epoch) {
        clear_frame(bridge);
        if (observed && (!after.connected || after.daemon_generation != before.daemon_generation))
            bridge->metadata_valid = false;
        if (!observed) {
            busy_metadata(bridge, target, out);
            return;
        }
        if (after.connected && after.daemon_generation == before.daemon_generation) {
            copy_config(out, &after, bridge);
            out->connected = true;
            snprintf(out->status, sizeof out->status, "Connected");
            remember_metadata(bridge, out);
        }
        if (copied < 0 && error[0])
            snprintf(out->error, sizeof out->error, "%.*s", (int)sizeof out->error-1, error);
        else if (observed && !after.connected && after.error[0])
            snprintf(out->error, sizeof out->error, "%.*s", (int)sizeof out->error-1, after.error);
        return;
    }
    copy_config(out, &after, bridge);
    out->connected = true;
    snprintf(out->status, sizeof out->status, "Connected");
    if (copied > 0) ++bridge->revision;
    if (bridge->frame.data) {
        out->has_frame = true;
        out->frame = (cui_bitmap){bridge->frame.data, (unsigned)bridge->frame.width,
            (unsigned)bridge->frame.height, (size_t)bridge->frame.stride, bridge->revision};
    }
    remember_metadata(bridge, out);
}

int bridge_session(DaemonBridge *b,bool command,uint64_t generation,unsigned argc,const char *const *argv,char *error,size_t size) {
    if(!b || b->pending || !argc || argc>8 || (!command && argc!=2))return refuse_edit(b,error,size,"Session edit unavailable");
    size_t bytes=64;
    for(unsigned i=0;i<argc;++i){if(!argv[i] || strlen(argv[i])>=4096)return refuse_edit(b,error,size,"Session edit is too long");bytes+=strlen(argv[i])+1;}
    if(bytes>=CAST_IPC_MAX)return refuse_edit(b,error,size,"Session edit exceeds transport limit");
    PanelSnapshot snapshot;
    if(!panel_client_snapshot(b->client,&snapshot) || !snapshot.connected || snapshot.daemon_generation!=generation || snapshot.command_queued!=snapshot.command_completed || snapshot.command_queued==UINT64_MAX)
        return refuse_edit(b,error,size,"Daemon changed or is busy; edit was not queued");
    char detail[CAST_ERR];
    int result=command ? panel_client_command(b->client,(int)argc,argv,detail,sizeof detail) : panel_client_setting(b->client,argv[0],argv[1],detail,sizeof detail);
    if(result)return refuse_edit(b,error,size,"Session edit could not be queued");
    b->edit_generation=snapshot.daemon_generation;b->edit_ack=snapshot.command_queued+1;b->pending=true;b->edit_error[0]=0;
    if(error&&size)error[0]=0;
    return 0;
}
