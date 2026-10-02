/* The licensed font travels with the optional panel, including relocated packages. */
.section .rodata
.balign 16
.global cast_panel_font_data
.global cast_panel_font_end
cast_panel_font_data:
.incbin "assets/fonts/Inter.ttf"
cast_panel_font_end:
.section .note.GNU-stack,""
