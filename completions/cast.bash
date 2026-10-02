# Bash completion for cast. Source this file, or use cast completions bash.
# Settings keys come from the local defaults command; no daemon/device is opened.
_cast_complete()
{
    local -a words=() matches=()
    local index token current=0
    # Readline splits aspect ratios at ':'. Rejoin for matching, then return the tail.
    for ((index = 0; index <= COMP_CWORD; index++)); do
        token=${COMP_WORDS[index]}
        if [[ $token == : && ${#words[@]} -gt 0 ]]; then
            words[${#words[@]}-1]+=':'
        elif ((index > 0)) && [[ ${COMP_WORDS[index-1]} == : ]]; then
            words[${#words[@]}-1]+=$token
        else
            words+=("$token")
        fi
        current=$((${#words[@]} - 1))
    done
    local cur=${words[current]} raw=${COMP_WORDS[COMP_CWORD]} offer='' mode=''
    local trim=$((${#cur} - ${#raw})) command_index=1 command='' argument=0
    local commands='layout split camera screen capture zoom cursor clicks keys annotations pause resume live record audio preset preview status doctor config settings panel reset quit completions setup update help'
    local flags='--config --socket --backend --output-device --camera-device --width --height --fps --no-live --no-camera --mic-source --desktop-source --record-dir --container --video-codec --audio-codec --countdown --help --version'
    while ((command_index < current)); do
        token=${words[command_index]}
        case $token in
            --config|--socket|--backend|--output-device|--camera-device|--width|--height|--fps|--mic-source|--desktop-source|--record-dir|--container|--video-codec|--audio-codec|--countdown)
                if ((current == command_index + 1)); then
                    case $token in
                        --backend) offer='xorg wayland synthetic' ;;
                        --config|--socket|--output-device|--camera-device) mode=file ;;
                        --record-dir) mode=directory ;;
                    esac
                    break
                fi
                ((command_index += 2)) ;;
            --no-live|--no-camera) ((command_index++)) ;;
            *) command=${words[command_index]}; break ;;
        esac
    done
    if [[ -z $command && $command_index -eq $current ]]; then
        offer="$commands $flags"
    elif [[ -n $command ]]; then
        argument=$((current - command_index))
        local first=${words[command_index+1]-} second=${words[command_index+2]-}
        case "$command:$argument" in
            layout:1) offer='overlay split screen camera next' ;;
            split:1) offer='side ratio' ;;
            split:2) [[ $first == side ]] && offer='left right' ;;
            camera:1) offer='show hide toggle size move position anchor shape aspect crop mirror list device' ;;
            camera:2)
                case $first in
                    anchor) offer='top-left top-right bottom-left bottom-right top bottom left right next' ;;
                    shape) offer='rectangle rounded circle next' ;;
                    aspect) offer='native 16:9 4:3 1:1' ;;
                    mirror) offer='on off toggle' ;;
                    crop) offer='move' ;;
                    device) mode=file ;;
                esac ;;
            screen:1) offer='list next select' ;;
            capture:1) offer='monitor region window fit' ;;
            capture:2)
                case $first in
                    region) offer=select ;;
                    window) offer='select active' ;;
                    fit) offer='contain cover' ;;
                esac ;;
            zoom:1) offer='toggle in out reset set follow' ;;
            zoom:2) [[ $first == follow ]] && offer='on off' ;;
            cursor:1) offer='on off toggle highlight' ;;
            cursor:2) [[ $first == highlight ]] && offer='on off toggle' ;;
            clicks:1) offer='on off toggle' ;;
            keys:1) offer='on off toggle mode clear' ;;
            keys:2) [[ $first == mode ]] && offer='shortcuts all' ;;
            annotations:1) offer='live record' ;;
            annotations:2) offer='keys clicks' ;;
            annotations:3) offer='on off' ;;
            live:1) offer='pause resume toggle freeze unfreeze blur unblur message title subtitle footer' ;;
            live:2) [[ $first == blur ]] && offer='on off toggle' ;;
            record:1) offer='start stop pause resume toggle freeze unfreeze blur unblur cut cancel title subtitle footer' ;;
            record:2)
                if [[ $first == start ]]; then mode=file
                elif [[ $first == blur ]]; then offer='on off toggle'
                fi ;;
            audio:1) offer='list mic desktop virtual' ;;
            audio:2)
                if [[ $first == virtual ]]; then offer='on off toggle'
                elif [[ $first == mic || $first == desktop ]]; then offer='on off toggle source gain'
                fi ;;
            preset:1) offer=next ;;
            preview:1) offer='on off toggle target' ;;
            preview:2) [[ $first == target ]] && offer='live record' ;;
            status:1) offer=--json ;;
            config:1) offer='check defaults reload' ;;
            config:2) [[ $first == check ]] && mode=file ;;
            completions:1) offer='bash zsh fish --script' ;;
            completions:2) [[ $first == --script ]] && offer='bash zsh fish' ;;
            update:*)
                if [[ ${words[current-1]} == --download-only ]]; then mode=directory
                else offer=--download-only
                fi ;;
            settings:*)
                if ((argument % 2)); then
                    offer=$(command cast config defaults 2>/dev/null | awk '
                        /^\[/ { section=$0; sub(/^\[/,"",section); sub(/\]$/,"",section) }
                        section !~ /^preset[.]/ && /^[a-z_]+[[:space:]]*=/ { key=$1; print section "." key }')
                else
                    case ${words[current-1]} in
                        output.backend) offer='xorg wayland synthetic' ;;
                        composition.layout) offer='overlay split screen camera' ;;
                        composition.split_side) offer='left right' ;;
                        composition.fit) offer='contain cover' ;;
                        capture.kind) offer='monitor region window' ;;
                        camera.shape) offer='rectangle rounded circle' ;;
                        camera.background) offer='blurred gradient solid' ;;
                        camera.anchor) offer='top-left top-right bottom-left bottom-right top bottom left right free' ;;
                        camera.aspect) offer='native 16:9 4:3 1:1' ;;
                        keys.mode) offer='shortcuts all' ;;
                        keys.position) offer='top-left top-right bottom-left bottom-right' ;;
                        preview.target) offer='live record' ;;
                        output.enabled|camera.enabled|camera.visible|camera.mirror|zoom.follow|cursor.enabled|cursor.highlight|clicks.enabled|clicks.middle|keys.enabled|annotations.live_keys|annotations.live_clicks|annotations.record_keys|annotations.record_clicks|audio.mic|audio.desktop|audio.virtual|preview.enabled) offer='true false' ;;
                    esac
                fi ;;
        esac
        [[ $cur == --* ]] && offer="$offer --help"
    fi
    COMPREPLY=()
    if [[ $mode == file ]]; then
        mapfile -t matches < <(compgen -f -- "$cur")
        compopt -o filenames 2>/dev/null || :
    elif [[ $mode == directory ]]; then
        mapfile -t matches < <(compgen -d -- "$cur")
        compopt -o filenames 2>/dev/null || :
    elif [[ -n $offer ]]; then
        mapfile -t matches < <(compgen -W "$offer" -- "$cur")
    fi
    for token in "${matches[@]}"; do COMPREPLY+=("${token:trim}"); done
    return 0
}
complete -F _cast_complete cast
