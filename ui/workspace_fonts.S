/* Embed the licensed workspace fonts in the optional root executable. */
#ifdef __APPLE__
.section __TEXT,__const
#define FONT_SYMBOL(name) _##name
#else
.section .rodata
#define FONT_SYMBOL(name) name
#endif
.balign 16
.global FONT_SYMBOL(cast_ui_regular_start)
.global FONT_SYMBOL(cast_ui_regular_end)
FONT_SYMBOL(cast_ui_regular_start):
.incbin "ui/assets/fonts/Inter-Regular.otf"
FONT_SYMBOL(cast_ui_regular_end):
.balign 16
.global FONT_SYMBOL(cast_ui_bold_start)
.global FONT_SYMBOL(cast_ui_bold_end)
FONT_SYMBOL(cast_ui_bold_start):
.incbin "ui/assets/fonts/Inter-Bold.otf"
FONT_SYMBOL(cast_ui_bold_end):
.balign 16
.global FONT_SYMBOL(cast_ui_medium_start)
.global FONT_SYMBOL(cast_ui_medium_end)
FONT_SYMBOL(cast_ui_medium_start):
.incbin "ui/assets/fonts/Inter-Medium.ttf"
FONT_SYMBOL(cast_ui_medium_end):
.balign 16
.global FONT_SYMBOL(cast_ui_semibold_start)
.global FONT_SYMBOL(cast_ui_semibold_end)
FONT_SYMBOL(cast_ui_semibold_start):
.incbin "ui/assets/fonts/Inter-SemiBold.ttf"
FONT_SYMBOL(cast_ui_semibold_end):
.balign 16
.global FONT_SYMBOL(cast_ui_mono_regular_start)
.global FONT_SYMBOL(cast_ui_mono_regular_end)
FONT_SYMBOL(cast_ui_mono_regular_start):
.incbin "ui/assets/fonts/IBMPlexMono-Regular.ttf"
FONT_SYMBOL(cast_ui_mono_regular_end):
.balign 16
.global FONT_SYMBOL(cast_ui_mono_medium_start)
.global FONT_SYMBOL(cast_ui_mono_medium_end)
FONT_SYMBOL(cast_ui_mono_medium_start):
.incbin "ui/assets/fonts/IBMPlexMono-Medium.ttf"
FONT_SYMBOL(cast_ui_mono_medium_end):
.balign 16
.global FONT_SYMBOL(cast_ui_mono_semibold_start)
.global FONT_SYMBOL(cast_ui_mono_semibold_end)
FONT_SYMBOL(cast_ui_mono_semibold_start):
.incbin "ui/assets/fonts/IBMPlexMono-SemiBold.ttf"
FONT_SYMBOL(cast_ui_mono_semibold_end):
#ifndef __APPLE__
.section .note.GNU-stack,""
#endif
