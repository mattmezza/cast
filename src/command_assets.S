/* Keep setup tools available in relocated binaries, independent of install paths. */
.section .rodata
.global cast_completion_bash_begin
.global cast_completion_bash_end
cast_completion_bash_begin:
.incbin "completions/cast.bash"
cast_completion_bash_end:
.global cast_completion_zsh_begin
.global cast_completion_zsh_end
cast_completion_zsh_begin:
.incbin "completions/_cast"
cast_completion_zsh_end:
.global cast_completion_fish_begin
.global cast_completion_fish_end
cast_completion_fish_begin:
.incbin "completions/cast.fish"
cast_completion_fish_end:
.global cast_install_script_start
.global cast_install_script_end
cast_install_script_start:
.incbin "packaging/install.sh"
cast_install_script_end:
.section .note.GNU-stack,""
