# Native fish completion for cast. No daemon/device discovery occurs here.
function __cast_pro
    set -l executable (commandline -opc)[1]
    command "$executable" edition --json 2>/dev/null | string match -qr '"edition"[[:space:]]*:[[:space:]]*"pro"'
end
function __cast_arguments
    set -l tokens (commandline -opc)
    set -e tokens[1]
    set -l positional
    set -l skip_value 0
    for token in $tokens
        if test $skip_value -eq 1
            set skip_value 0
            continue
        end
        if test (count $positional) -eq 0
            switch $token
                case --config --socket --backend --output-device --camera-device --width --height --fps --mic-source --desktop-source --record-dir --container --video-codec --audio-codec --countdown --record-bitrate --record-rate-control --stream-video-encoder
                    set skip_value 1
                    continue
                case --headless --no-virtual --no-camera
                    continue
            end
        end
        set -a positional $token
    end
    if test $skip_value -eq 1
        printf '%s\n' __cast_option_value
    else if test (count $positional) -gt 0
        printf '%s\n' $positional
    end
end
function __cast_at
    set -l positional (__cast_arguments)
    test (count $positional) -eq (count $argv); or return 1
    for i in (seq (count $argv))
        test "$positional[$i]" = "$argv[$i]"; or return 1
    end
    return 0
end
# Fish rechecks option conditions while completing their required values.
function __cast_global_option
    __cast_at; or __cast_at __cast_option_value
end
function __cast_command
    set -l positional (__cast_arguments)
    test (count $positional) -gt 0; and test "$positional[1]" = "$argv[1]"
end
function __cast_subcommand
    set -l positional (__cast_arguments)
    test (count $positional) -ge 2; and test "$positional[1]" = "$argv[1]"; and test "$positional[2]" = "$argv[2]"
end
function __cast_settings_key
    set -l positional (__cast_arguments)
    test "$positional[1]" = settings; or return 1
    test (math (count $positional) % 2) -eq 1
end
function __cast_setting_keys
    set -l executable (commandline -opc)[1]
    command "$executable" config defaults 2>/dev/null | awk '
        /^\[/ { section=$0; sub(/^\[/,"",section); sub(/\]$/,"",section) }
        section !~ /^preset[.]/ && /^[a-z_]+[[:space:]]*=/ { key=$1; print section "." key }'
end
function __cast_setting_value
    set -l positional (__cast_arguments)
    test (count $positional) -ge 2; or return 1
    test "$positional[1]" = settings; or return 1
    test (math (count $positional) % 2) -eq 0; or return 1
    contains -- "$positional[-1]" $argv
end

complete -c cast -f
complete -c cast -n __cast_at -a 'edition features license layout split camera screen logo text capture zoom cursor clicks keys annotations pause resume virtual stream record audio preset preview status doctor config settings panel reset quit completions setup update help'
complete -c cast -l help -d 'Show command reference'
complete -c cast -n __cast_at -l headless -d 'Run the daemon without opening the app'
complete -c cast -n __cast_at -l version -d 'Print cast version'
complete -c cast -n __cast_global_option -l config -r -F -d 'Config file'
complete -c cast -n __cast_global_option -l socket -r -F -d 'Daemon socket path'
complete -c cast -n __cast_global_option -l backend -r -a 'xorg wayland synthetic' -d 'Capture backend'
complete -c cast -n __cast_global_option -l output-device -r -F -d 'Virtual camera device'
complete -c cast -n __cast_global_option -l camera-device -r -F -d 'Physical camera device'
complete -c cast -n __cast_global_option -l width -r -d 'Output width'
complete -c cast -n __cast_global_option -l height -r -d 'Output height'
complete -c cast -n __cast_global_option -l fps -r -d 'Output frame rate'
complete -c cast -n __cast_at -l no-virtual -d 'Disable virtual-camera output'
complete -c cast -n __cast_at -l no-camera -d 'Disable camera input'
complete -c cast -n __cast_global_option -l mic-source -r -d 'PipeWire microphone source'
complete -c cast -n __cast_global_option -l desktop-source -r -d 'PipeWire desktop source'
complete -c cast -n __cast_global_option -l record-dir -r -a '(__fish_complete_directories)' -d 'Recording directory'
complete -c cast -n __cast_global_option -l container -r -d 'Recording container'
complete -c cast -n __cast_global_option -l video-codec -r -a 'auto libx264 libopenh264' -d 'Video encoder'
complete -c cast -n __cast_global_option -l audio-codec -r -d 'Audio encoder'
complete -c cast -n __cast_global_option -l countdown -r -d 'Recording countdown seconds'
complete -c cast -n '__cast_at layout' -a 'overlay stage split screen camera next prev'
complete -c cast -n '__cast_at split' -a 'side ratio'
complete -c cast -n '__cast_at split side' -a 'left right'
complete -c cast -n '__cast_at camera' -a 'show hide toggle size move position anchor shape aspect crop mirror list device'
complete -c cast -n '__cast_at camera anchor' -a 'top-left top-right bottom-left bottom-right top bottom left right next prev'
complete -c cast -n '__cast_at camera shape' -a 'rectangle rounded circle next prev'
complete -c cast -n '__cast_at camera aspect' -a 'native 16:9 4:3 1:1 next prev'
complete -c cast -n '__cast_at camera mirror' -a 'on off toggle'
complete -c cast -n '__cast_at camera crop' -a move
complete -c cast -n '__cast_at camera device' -F
complete -c cast -n '__cast_at screen' -a 'list next prev select size margin radius border background'
complete -c cast -n '__cast_at screen border' -a 'width color'
complete -c cast -n '__cast_at screen background' -a 'blurred gradient solid'
complete -c cast -n '__cast_at logo' -a 'on off toggle path size anchor margin opacity'
complete -c cast -n '__cast_at logo path' -F
complete -c cast -n '__cast_at logo anchor; or __cast_at text anchor' -a 'top-left top-right bottom-left bottom-right top bottom left right'
complete -c cast -n '__cast_at text' -a 'on off toggle set font size color anchor margin opacity'
complete -c cast -n '__cast_at capture' -a 'monitor region window fit mask-color'
complete -c cast -n '__cast_at capture region' -a select
complete -c cast -n '__cast_at capture window' -a 'select active'
complete -c cast -n '__cast_at capture fit' -a 'contain cover'
complete -c cast -n '__cast_at zoom' -a 'toggle in out reset set follow'
complete -c cast -n '__cast_at zoom follow' -a 'on off'
complete -c cast -n '__cast_at cursor' -a 'on off toggle highlight'
complete -c cast -n '__cast_at cursor highlight' -a 'on off toggle'
complete -c cast -n '__cast_at clicks' -a 'on off toggle'
complete -c cast -n '__cast_at keys' -a 'on off toggle mode clear'
complete -c cast -n '__cast_at keys mode' -a 'shortcuts all'
complete -c cast -n '__cast_at annotations' -a 'virtual record stream'
complete -c cast -n '__cast_at annotations virtual; or __cast_at annotations record; or __cast_at annotations stream' -a 'keys clicks'
complete -c cast -n '__cast_at annotations virtual keys; or __cast_at annotations virtual clicks; or __cast_at annotations record keys; or __cast_at annotations record clicks' -a 'on off'
complete -c cast -n '__cast_at virtual' -a 'start stop pause resume toggle freeze unfreeze blur unblur message title subtitle footer'
complete -c cast -n '__cast_at record' -a 'start stop pause resume toggle freeze unfreeze blur unblur cut cancel title subtitle footer'
complete -c cast -n '__cast_at virtual blur; or __cast_at record blur' -a 'on off toggle'
complete -c cast -n '__cast_at record start' -F
complete -c cast -n '__cast_at audio' -a 'list mic desktop virtual'
complete -c cast -n '__cast_at audio mic; or __cast_at audio desktop' -a 'on off toggle source gain'
complete -c cast -n '__cast_at audio virtual' -a 'on off toggle'
complete -c cast -n '__cast_at preset' -a 'next prev'
complete -c cast -n '__cast_at preview' -a 'on off toggle target'
complete -c cast -n '__cast_at preview target' -a 'virtual record stream'
complete -c cast -n '__cast_at status' -l json -d 'Print JSON status'
complete -c cast -n '__cast_at config' -a 'check defaults reload migrate'
complete -c cast -n '__cast_at config check' -F
complete -c cast -n '__cast_at completions' -a 'bash zsh fish'
complete -c cast -n '__cast_at completions' -l script -r -a 'bash zsh fish' -d 'Print bundled completion script'
complete -c cast -n '__cast_command update; and not __cast_pro' -l download-only -r -a '(__fish_complete_directories)' -d 'Download release files without installing'
complete -c cast -n __cast_settings_key -a '(__cast_setting_keys)'
complete -c cast -n '__cast_setting_value output.backend' -a 'xorg wayland synthetic'
complete -c cast -n '__cast_setting_value composition.layout' -a 'overlay stage split screen camera'
complete -c cast -n '__cast_setting_value composition.split_side' -a 'left right'
complete -c cast -n '__cast_setting_value composition.fit' -a 'contain cover'
complete -c cast -n '__cast_setting_value capture.kind' -a 'monitor region window'
complete -c cast -n '__cast_setting_value camera.shape' -a 'rectangle rounded circle'
complete -c cast -n '__cast_setting_value camera.background screen.background' -a 'blurred gradient solid'
complete -c cast -n '__cast_setting_value background.source' -a 'screen camera'
complete -c cast -n '__cast_setting_value logo.anchor text.anchor' -a 'top-left top-right bottom-left bottom-right top bottom left right'
complete -c cast -n '__cast_setting_value camera.anchor' -a 'top-left top-right bottom-left bottom-right top bottom left right free'
complete -c cast -n '__cast_setting_value camera.aspect' -a 'native 16:9 4:3 1:1'
complete -c cast -n '__cast_setting_value keys.mode' -a 'shortcuts all'
complete -c cast -n '__cast_setting_value keys.position' -a 'top-left top-right bottom-left bottom-right'
complete -c cast -n '__cast_setting_value preview.target' -a 'virtual record stream'
complete -c cast -n '__cast_setting_value logo.enabled text.enabled background.gradient_via_enabled output.enabled camera.enabled camera.visible camera.mirror zoom.follow cursor.enabled cursor.highlight clicks.enabled clicks.middle keys.enabled annotations.virtual_keys annotations.virtual_clicks annotations.record_keys annotations.record_clicks annotations.stream_keys annotations.stream_clicks audio.mic audio.desktop audio.virtual preview.enabled' -a 'true false'

complete -c cast -n '__cast_at stream' -a 'start stop pause resume toggle freeze unfreeze blur unblur status'
complete -c cast -n '__cast_at stream blur' -a 'on off toggle'
complete -c cast -n '__cast_at stream status' -l json
complete -c cast -n '__cast_at annotations stream keys; or __cast_at annotations stream clicks' -a 'on off'
complete -c cast -n '__cast_setting_value stream.service' -a 'custom twitch youtube'
complete -c cast -n '__cast_setting_value stream.encoder_preset' -a 'ultrafast superfast veryfast faster fast medium slow slower veryslow'
complete -c cast -n '__cast_setting_value stream.key_file stream.tls_ca_file logo.path camera.device output.device' -F

complete -c cast -n '__cast_at edition; or __cast_at features; or __cast_at license status' -l json
complete -c cast -n '__cast_at license' -a 'status inspect import reload remove'
complete -c cast -n '__cast_at license inspect; or __cast_at license import' -F
complete -c cast -n '__cast_subcommand config check' -l availability
complete -c cast -n '__cast_subcommand config migrate' -l edition -xa 'community pro'
complete -c cast -n '__cast_subcommand config migrate' -l write -r -F
complete -c cast -n '__cast_subcommand config migrate' -l backup -r -F
complete -c cast -n __cast_global_option -l record-bitrate -x
complete -c cast -n __cast_global_option -l record-rate-control -xa 'auto bitrate crf'
complete -c cast -n __cast_global_option -l stream-video-encoder -xa 'auto libx264 libopenh264'

# Both edition executables share this grammar.
complete -c cast-pro -f
complete -c cast-pro -n __cast_at -a 'edition features license layout split camera screen logo text capture zoom cursor clicks keys annotations pause resume virtual stream record audio preset preview status doctor config settings panel reset quit completions setup update help'
complete -c cast-pro -l help -d 'Show command reference'
complete -c cast-pro -n __cast_at -l headless -d 'Run the daemon without opening the app'
complete -c cast-pro -n __cast_at -l version -d 'Print cast version'
complete -c cast-pro -n __cast_global_option -l config -r -F -d 'Config file'
complete -c cast-pro -n __cast_global_option -l socket -r -F -d 'Daemon socket path'
complete -c cast-pro -n __cast_global_option -l backend -r -a 'xorg wayland synthetic' -d 'Capture backend'
complete -c cast-pro -n __cast_global_option -l output-device -r -F -d 'Virtual camera device'
complete -c cast-pro -n __cast_global_option -l camera-device -r -F -d 'Physical camera device'
complete -c cast-pro -n __cast_global_option -l width -r -d 'Output width'
complete -c cast-pro -n __cast_global_option -l height -r -d 'Output height'
complete -c cast-pro -n __cast_global_option -l fps -r -d 'Output frame rate'
complete -c cast-pro -n __cast_at -l no-virtual -d 'Disable virtual-camera output'
complete -c cast-pro -n __cast_at -l no-camera -d 'Disable camera input'
complete -c cast-pro -n __cast_global_option -l mic-source -r -d 'PipeWire microphone source'
complete -c cast-pro -n __cast_global_option -l desktop-source -r -d 'PipeWire desktop source'
complete -c cast-pro -n __cast_global_option -l record-dir -r -a '(__fish_complete_directories)' -d 'Recording directory'
complete -c cast-pro -n __cast_global_option -l container -r -d 'Recording container'
complete -c cast-pro -n __cast_global_option -l video-codec -r -a 'auto libx264 libopenh264' -d 'Video encoder'
complete -c cast-pro -n __cast_global_option -l audio-codec -r -d 'Audio encoder'
complete -c cast-pro -n __cast_global_option -l countdown -r -d 'Recording countdown seconds'
complete -c cast-pro -n '__cast_at layout' -a 'overlay stage split screen camera next prev'
complete -c cast-pro -n '__cast_at split' -a 'side ratio'
complete -c cast-pro -n '__cast_at split side' -a 'left right'
complete -c cast-pro -n '__cast_at camera' -a 'show hide toggle size move position anchor shape aspect crop mirror list device'
complete -c cast-pro -n '__cast_at camera anchor' -a 'top-left top-right bottom-left bottom-right top bottom left right next prev'
complete -c cast-pro -n '__cast_at camera shape' -a 'rectangle rounded circle next prev'
complete -c cast-pro -n '__cast_at camera aspect' -a 'native 16:9 4:3 1:1 next prev'
complete -c cast-pro -n '__cast_at camera mirror' -a 'on off toggle'
complete -c cast-pro -n '__cast_at camera crop' -a move
complete -c cast-pro -n '__cast_at camera device' -F
complete -c cast-pro -n '__cast_at screen' -a 'list next prev select size margin radius border background'
complete -c cast-pro -n '__cast_at screen border' -a 'width color'
complete -c cast-pro -n '__cast_at screen background' -a 'blurred gradient solid'
complete -c cast-pro -n '__cast_at logo' -a 'on off toggle path size anchor margin opacity'
complete -c cast-pro -n '__cast_at logo path' -F
complete -c cast-pro -n '__cast_at logo anchor; or __cast_at text anchor' -a 'top-left top-right bottom-left bottom-right top bottom left right'
complete -c cast-pro -n '__cast_at text' -a 'on off toggle set font size color anchor margin opacity'
complete -c cast-pro -n '__cast_at capture' -a 'monitor region window fit mask-color'
complete -c cast-pro -n '__cast_at capture region' -a select
complete -c cast-pro -n '__cast_at capture window' -a 'select active'
complete -c cast-pro -n '__cast_at capture fit' -a 'contain cover'
complete -c cast-pro -n '__cast_at zoom' -a 'toggle in out reset set follow'
complete -c cast-pro -n '__cast_at zoom follow' -a 'on off'
complete -c cast-pro -n '__cast_at cursor' -a 'on off toggle highlight'
complete -c cast-pro -n '__cast_at cursor highlight' -a 'on off toggle'
complete -c cast-pro -n '__cast_at clicks' -a 'on off toggle'
complete -c cast-pro -n '__cast_at keys' -a 'on off toggle mode clear'
complete -c cast-pro -n '__cast_at keys mode' -a 'shortcuts all'
complete -c cast-pro -n '__cast_at annotations' -a 'virtual record stream'
complete -c cast-pro -n '__cast_at annotations virtual; or __cast_at annotations record; or __cast_at annotations stream' -a 'keys clicks'
complete -c cast-pro -n '__cast_at annotations virtual keys; or __cast_at annotations virtual clicks; or __cast_at annotations record keys; or __cast_at annotations record clicks' -a 'on off'
complete -c cast-pro -n '__cast_at virtual' -a 'start stop pause resume toggle freeze unfreeze blur unblur message title subtitle footer'
complete -c cast-pro -n '__cast_at record' -a 'start stop pause resume toggle freeze unfreeze blur unblur cut cancel title subtitle footer'
complete -c cast-pro -n '__cast_at virtual blur; or __cast_at record blur' -a 'on off toggle'
complete -c cast-pro -n '__cast_at record start' -F
complete -c cast-pro -n '__cast_at audio' -a 'list mic desktop virtual'
complete -c cast-pro -n '__cast_at audio mic; or __cast_at audio desktop' -a 'on off toggle source gain'
complete -c cast-pro -n '__cast_at audio virtual' -a 'on off toggle'
complete -c cast-pro -n '__cast_at preset' -a 'next prev'
complete -c cast-pro -n '__cast_at preview' -a 'on off toggle target'
complete -c cast-pro -n '__cast_at preview target' -a 'virtual record stream'
complete -c cast-pro -n '__cast_at status' -l json -d 'Print JSON status'
complete -c cast-pro -n '__cast_at config' -a 'check defaults reload migrate'
complete -c cast-pro -n '__cast_at config check' -F
complete -c cast-pro -n '__cast_at completions' -a 'bash zsh fish'
complete -c cast-pro -n '__cast_at completions' -l script -r -a 'bash zsh fish' -d 'Print bundled completion script'
complete -c cast-pro -n '__cast_command update; and not __cast_pro' -l download-only -r -a '(__fish_complete_directories)' -d 'Download release files without installing'
complete -c cast-pro -n __cast_settings_key -a '(__cast_setting_keys)'
complete -c cast-pro -n '__cast_setting_value output.backend' -a 'xorg wayland synthetic'
complete -c cast-pro -n '__cast_setting_value composition.layout' -a 'overlay stage split screen camera'
complete -c cast-pro -n '__cast_setting_value composition.split_side' -a 'left right'
complete -c cast-pro -n '__cast_setting_value composition.fit' -a 'contain cover'
complete -c cast-pro -n '__cast_setting_value capture.kind' -a 'monitor region window'
complete -c cast-pro -n '__cast_setting_value camera.shape' -a 'rectangle rounded circle'
complete -c cast-pro -n '__cast_setting_value camera.background screen.background' -a 'blurred gradient solid'
complete -c cast-pro -n '__cast_setting_value background.source' -a 'screen camera'
complete -c cast-pro -n '__cast_setting_value logo.anchor text.anchor' -a 'top-left top-right bottom-left bottom-right top bottom left right'
complete -c cast-pro -n '__cast_setting_value camera.anchor' -a 'top-left top-right bottom-left bottom-right top bottom left right free'
complete -c cast-pro -n '__cast_setting_value camera.aspect' -a 'native 16:9 4:3 1:1'
complete -c cast-pro -n '__cast_setting_value keys.mode' -a 'shortcuts all'
complete -c cast-pro -n '__cast_setting_value keys.position' -a 'top-left top-right bottom-left bottom-right'
complete -c cast-pro -n '__cast_setting_value preview.target' -a 'virtual record stream'
complete -c cast-pro -n '__cast_setting_value logo.enabled text.enabled background.gradient_via_enabled output.enabled camera.enabled camera.visible camera.mirror zoom.follow cursor.enabled cursor.highlight clicks.enabled clicks.middle keys.enabled annotations.virtual_keys annotations.virtual_clicks annotations.record_keys annotations.record_clicks annotations.stream_keys annotations.stream_clicks audio.mic audio.desktop audio.virtual preview.enabled' -a 'true false'
complete -c cast-pro -n '__cast_at stream' -a 'start stop pause resume toggle freeze unfreeze blur unblur status'
complete -c cast-pro -n '__cast_at stream blur' -a 'on off toggle'
complete -c cast-pro -n '__cast_at stream status' -l json
complete -c cast-pro -n '__cast_at annotations stream keys; or __cast_at annotations stream clicks' -a 'on off'
complete -c cast-pro -n '__cast_setting_value stream.service' -a 'custom twitch youtube'
complete -c cast-pro -n '__cast_setting_value stream.encoder_preset' -a 'ultrafast superfast veryfast faster fast medium slow slower veryslow'
complete -c cast-pro -n '__cast_setting_value stream.key_file stream.tls_ca_file logo.path camera.device output.device' -F
complete -c cast-pro -n '__cast_at edition; or __cast_at features; or __cast_at license status' -l json
complete -c cast-pro -n '__cast_at license' -a 'status inspect import reload remove'
complete -c cast-pro -n '__cast_at license inspect; or __cast_at license import' -F
complete -c cast-pro -n '__cast_subcommand config check' -l availability
complete -c cast-pro -n '__cast_subcommand config migrate' -l edition -xa 'community pro'
complete -c cast-pro -n '__cast_subcommand config migrate' -l write -r -F
complete -c cast-pro -n '__cast_subcommand config migrate' -l backup -r -F
complete -c cast-pro -n __cast_global_option -l record-bitrate -x
complete -c cast-pro -n __cast_global_option -l record-rate-control -xa 'auto bitrate crf'
complete -c cast-pro -n __cast_global_option -l stream-video-encoder -xa 'auto libx264 libopenh264'
complete -c cast -n '__cast_subcommand config migrate' -F
complete -c cast -n '__cast_subcommand license inspect' -l json
complete -c cast-pro -n '__cast_subcommand config migrate' -F
complete -c cast-pro -n '__cast_subcommand license inspect' -l json
complete -c cast -n '__cast_setting_value record.rate_control' -a 'auto bitrate crf'
complete -c cast -n '__cast_setting_value record.video_codec stream.video_encoder' -a 'auto libx264 libopenh264'
complete -c cast -n '__cast_setting_value licensing.file' -F
complete -c cast-pro -n '__cast_setting_value record.rate_control' -a 'auto bitrate crf'
complete -c cast-pro -n '__cast_setting_value record.video_codec stream.video_encoder' -a 'auto libx264 libopenh264'
complete -c cast-pro -n '__cast_setting_value licensing.file' -F

# Pro identity is read locally; completion never starts a workflow or queries a daemon.
for cast_completion_command in cast cast-pro
    complete -c $cast_completion_command -n '__cast_at; and __cast_pro' -a 'transcription transcribe subtitles notes'
    complete -c $cast_completion_command -n '__cast_at zoom; and __cast_pro' -a 'motion focus auto status cinematic'
    complete -c $cast_completion_command -n '__cast_at zoom motion; and __cast_pro' -a 'legacy cinematic'
    complete -c $cast_completion_command -n '__cast_at zoom auto; and __cast_pro' -a 'off click'
    complete -c $cast_completion_command -n '__cast_at zoom status; and __cast_pro' -l json
    complete -c $cast_completion_command -n '__cast_subcommand zoom focus; and __cast_pro; and test (count (__cast_arguments)) -ge 4' -l factor -r
    complete -c $cast_completion_command -n '__cast_at transcription; and __cast_pro' -a 'on off status model language source models transcribe job'
    complete -c $cast_completion_command -n '__cast_at transcription status; and __cast_pro' -l json
    complete -c $cast_completion_command -n '__cast_at transcription model; and __cast_pro' -F
    complete -c $cast_completion_command -n '__cast_at transcription language; and __cast_pro' -a auto
    complete -c $cast_completion_command -n '__cast_at transcription source; and __cast_pro' -a 'mic desktop mix'
    complete -c $cast_completion_command -n '__cast_at transcription job; and __cast_pro' -a 'status cancel'
    complete -c $cast_completion_command -n '__cast_at transcription transcribe; and __cast_pro' -F
    complete -c $cast_completion_command -n '__cast_at transcribe; and __cast_pro' -F
    complete -c $cast_completion_command -n '__cast_subcommand transcription transcribe; or __cast_command transcribe' -l output -r -F
    complete -c $cast_completion_command -n '__cast_subcommand transcription transcribe; or __cast_command transcribe' -l format -xa 'srt vtt both'
    complete -c $cast_completion_command -n '__cast_subcommand transcription transcribe; or __cast_command transcribe' -l overwrite
    complete -c $cast_completion_command -n '__cast_at subtitles; and __cast_pro' -a 'virtual record stream sidecar'
    complete -c $cast_completion_command -n '__cast_at subtitles virtual; or __cast_at subtitles record; or __cast_at subtitles stream' -a 'on off'
    complete -c $cast_completion_command -n '__cast_at subtitles sidecar; and __cast_pro' -a 'none srt vtt both'
    complete -c $cast_completion_command -n '__cast_at notes; and __cast_pro' -a 'open close load reload start pause toggle next prev center restart mode speed goto status'
    complete -c $cast_completion_command -n '__cast_at notes open; or __cast_at notes load' -F
    complete -c $cast_completion_command -n '__cast_at notes mode; and __cast_pro' -a 'timed speech'
    complete -c $cast_completion_command -n '__cast_at notes goto; and __cast_pro' -l line -r
    complete -c $cast_completion_command -n '__cast_at notes status; and __cast_pro' -l json
    complete -c $cast_completion_command -n '__cast_command update; and __cast_pro' -l bundle -r -a '(__fish_complete_directories)'
    complete -c $cast_completion_command -n '__cast_command update; and __cast_pro' -l install -r -F
    complete -c $cast_completion_command -n '__cast_command update; and __cast_pro' -l download-only -r -a '(__fish_complete_directories)'
    complete -c $cast_completion_command -n '__cast_command update; and __cast_pro' -l rollback
    complete -c $cast_completion_command -n '__cast_setting_value zoom.motion' -a 'legacy cinematic'
    complete -c $cast_completion_command -n '__cast_setting_value zoom.auto' -a 'off click'
    complete -c $cast_completion_command -n '__cast_setting_value zoom.filter' -a 'bilinear bicubic'
    complete -c $cast_completion_command -n '__cast_setting_value transcription.backend' -a whisper
    complete -c $cast_completion_command -n '__cast_setting_value transcription.device' -a 'cpu auto gpu'
    complete -c $cast_completion_command -n '__cast_setting_value transcription.language' -a auto
    complete -c $cast_completion_command -n '__cast_setting_value transcription.source' -a 'mic desktop mix'
    complete -c $cast_completion_command -n '__cast_setting_value transcription.task' -a 'transcribe translate'
    complete -c $cast_completion_command -n '__cast_setting_value transcription.vad_backend' -a 'energy silero'
    complete -c $cast_completion_command -n '__cast_setting_value subtitles.sidecar' -a 'none srt vtt both'
    complete -c $cast_completion_command -n '__cast_setting_value subtitles.anchor' -a 'top bottom'
    complete -c $cast_completion_command -n '__cast_setting_value subtitles.align' -a 'left center right'
    complete -c $cast_completion_command -n '__cast_setting_value notes.mode' -a 'timed speech'
    complete -c $cast_completion_command -n '__cast_setting_value notes.format' -a 'auto plain markdown'
    complete -c $cast_completion_command -n '__cast_setting_value notes.align' -a 'left center'
    complete -c $cast_completion_command -n '__cast_setting_value transcription.model_path notes.file' -F
    complete -c $cast_completion_command -n '__cast_setting_value zoom.avoid_camera zoom.motion_blur cursor.smooth transcription.enabled transcription.show_partial transcription.auto_finalize subtitles.virtual subtitles.record subtitles.stream subtitles.background notes.always_on_top notes.allow_backward_reacquire notes.exclude_from_capture notes.match_code_blocks' -a 'true false'
end
