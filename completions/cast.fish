# Native fish completion for cast. No daemon/device discovery occurs here.
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
                case --config --socket --backend --output-device --camera-device --width --height --fps --mic-source --desktop-source --record-dir --container --video-codec --audio-codec --countdown
                    set skip_value 1
                    continue
                case --no-live --no-camera
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
function __cast_command
    set -l positional (__cast_arguments)
    test (count $positional) -gt 0; and test "$positional[1]" = "$argv[1]"
end
function __cast_settings_key
    set -l positional (__cast_arguments)
    test "$positional[1]" = settings; or return 1
    test (math (count $positional) % 2) -eq 1
end
function __cast_setting_keys
    command cast config defaults 2>/dev/null | awk '
        /^\[/ { section=$0; sub(/^\[/,"",section); sub(/\]$/,"",section) }
        /^[a-z_]+[[:space:]]*=/ { key=$1; print section "." key }'
end

complete -c cast -f
complete -c cast -n __cast_at -a 'layout split camera screen capture zoom cursor clicks keys annotations pause resume live record audio preset preview status doctor config settings panel reset quit completions setup update'
complete -c cast -l help -d 'Show command reference'
complete -c cast -n __cast_at -l version -d 'Print cast version'
complete -c cast -n __cast_at -l config -r -F -d 'Config file'
complete -c cast -n __cast_at -l socket -r -F -d 'Daemon socket path'
complete -c cast -n __cast_at -l backend -r -a 'xorg wayland synthetic' -d 'Capture backend'
complete -c cast -n __cast_at -l output-device -r -F -d 'Virtual camera device'
complete -c cast -n __cast_at -l camera-device -r -F -d 'Physical camera device'
complete -c cast -n __cast_at -l width -r -d 'Output width'
complete -c cast -n __cast_at -l height -r -d 'Output height'
complete -c cast -n __cast_at -l fps -r -d 'Output frame rate'
complete -c cast -n __cast_at -l no-live -d 'Disable virtual-camera output'
complete -c cast -n __cast_at -l no-camera -d 'Disable camera input'
complete -c cast -n __cast_at -l mic-source -r -d 'PipeWire microphone source'
complete -c cast -n __cast_at -l desktop-source -r -d 'PipeWire desktop source'
complete -c cast -n __cast_at -l record-dir -r -a '(__fish_complete_directories)' -d 'Recording directory'
complete -c cast -n __cast_at -l container -r -d 'Recording container'
complete -c cast -n __cast_at -l video-codec -r -d 'Video encoder'
complete -c cast -n __cast_at -l audio-codec -r -d 'Audio encoder'
complete -c cast -n __cast_at -l countdown -r -d 'Recording countdown seconds'
complete -c cast -n '__cast_at layout' -a 'overlay split screen camera next'
complete -c cast -n '__cast_at split' -a 'side ratio'
complete -c cast -n '__cast_at split side' -a 'left right'
complete -c cast -n '__cast_at camera' -a 'show hide toggle size move position anchor shape aspect crop mirror list device'
complete -c cast -n '__cast_at camera anchor' -a 'top-left top-right bottom-left bottom-right next'
complete -c cast -n '__cast_at camera shape' -a 'rectangle rounded circle next'
complete -c cast -n '__cast_at camera aspect' -a 'native 16:9 4:3 1:1'
complete -c cast -n '__cast_at camera mirror' -a 'on off toggle'
complete -c cast -n '__cast_at camera crop' -a move
complete -c cast -n '__cast_at camera device' -F
complete -c cast -n '__cast_at screen' -a 'list next select'
complete -c cast -n '__cast_at capture' -a 'monitor region window fit'
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
complete -c cast -n '__cast_at annotations' -a 'live record'
complete -c cast -n '__cast_at annotations live; or __cast_at annotations record' -a 'keys clicks'
complete -c cast -n '__cast_at annotations live keys; or __cast_at annotations live clicks; or __cast_at annotations record keys; or __cast_at annotations record clicks' -a 'on off'
complete -c cast -n '__cast_at live' -a 'pause resume toggle freeze unfreeze message'
complete -c cast -n '__cast_at record' -a 'start stop pause resume toggle'
complete -c cast -n '__cast_at record start' -F
complete -c cast -n '__cast_at audio' -a 'list mic desktop virtual'
complete -c cast -n '__cast_at audio mic; or __cast_at audio desktop' -a 'on off toggle source gain'
complete -c cast -n '__cast_at audio virtual' -a 'on off toggle'
complete -c cast -n '__cast_at preset' -a next
complete -c cast -n '__cast_at preview' -a 'on off toggle target'
complete -c cast -n '__cast_at preview target' -a 'live record'
complete -c cast -n '__cast_at status' -l json -d 'Print JSON status'
complete -c cast -n '__cast_at config' -a 'check defaults reload'
complete -c cast -n '__cast_at config check' -F
complete -c cast -n '__cast_at completions' -a 'bash zsh fish'
complete -c cast -n '__cast_at completions' -l script -r -a 'bash zsh fish' -d 'Print bundled completion script'
complete -c cast -n '__cast_command update' -l download-only -r -a '(__fish_complete_directories)' -d 'Download release files without installing'
complete -c cast -n __cast_settings_key -a '(__cast_setting_keys)'
