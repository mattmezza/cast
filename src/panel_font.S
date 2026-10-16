/* The licensed font travels with the optional panel, including relocated packages. */
#ifdef __APPLE__
.section __TEXT,__const
#else
.section .rodata
#endif
.balign 16
#ifdef __APPLE__
.global _cast_panel_font_data
#else
.global cast_panel_font_data
#endif
#ifdef __APPLE__
.global _cast_panel_font_end
#else
.global cast_panel_font_end
#endif
#ifdef __APPLE__
_cast_panel_font_data:
#else
cast_panel_font_data:
#endif
.incbin "assets/fonts/Inter.ttf"
#ifdef __APPLE__
_cast_panel_font_end:
#else
cast_panel_font_end:
#endif
#ifndef __APPLE__
.section .note.GNU-stack,""
#endif
