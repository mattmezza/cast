/* Keep setup tools available in relocated binaries, independent of install paths. */
#ifdef __APPLE__
.section __TEXT,__const
#else
.section .rodata
#endif
#ifdef __APPLE__
.global _cast_completion_bash_begin
#else
.global cast_completion_bash_begin
#endif
#ifdef __APPLE__
.global _cast_completion_bash_end
#else
.global cast_completion_bash_end
#endif
#ifdef __APPLE__
_cast_completion_bash_begin:
#else
cast_completion_bash_begin:
#endif
.incbin "completions/cast.bash"
#ifdef __APPLE__
_cast_completion_bash_end:
#else
cast_completion_bash_end:
#endif
#ifdef __APPLE__
.global _cast_completion_zsh_begin
#else
.global cast_completion_zsh_begin
#endif
#ifdef __APPLE__
.global _cast_completion_zsh_end
#else
.global cast_completion_zsh_end
#endif
#ifdef __APPLE__
_cast_completion_zsh_begin:
#else
cast_completion_zsh_begin:
#endif
.incbin "completions/_cast"
#ifdef __APPLE__
_cast_completion_zsh_end:
#else
cast_completion_zsh_end:
#endif
#ifdef __APPLE__
.global _cast_completion_fish_begin
#else
.global cast_completion_fish_begin
#endif
#ifdef __APPLE__
.global _cast_completion_fish_end
#else
.global cast_completion_fish_end
#endif
#ifdef __APPLE__
_cast_completion_fish_begin:
#else
cast_completion_fish_begin:
#endif
.incbin "completions/cast.fish"
#ifdef __APPLE__
_cast_completion_fish_end:
#else
cast_completion_fish_end:
#endif
#ifdef __APPLE__
.global _cast_install_script_start
#else
.global cast_install_script_start
#endif
#ifdef __APPLE__
.global _cast_install_script_end
#else
.global cast_install_script_end
#endif
#ifdef __APPLE__
_cast_install_script_start:
#else
cast_install_script_start:
#endif
.incbin "packaging/install.sh"
#ifdef __APPLE__
_cast_install_script_end:
#else
cast_install_script_end:
#endif
#ifndef __APPLE__
.section .note.GNU-stack,""
#endif
