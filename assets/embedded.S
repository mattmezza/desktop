.section .rodata
.macro embed name, file
.balign 16
.global \name\()_start, \name\()_end
\name\()_start:
.incbin "\file"
\name\()_end:
.endm
embed geist, "assets/Geist-Regular.ttf"
embed geist_medium, "assets/Geist-Medium.ttf"
embed geist_mono, "assets/GeistMono-Regular.ttf"
embed defaults, "config.ini"
embed icons, "assets/icons.alpha"
.section .note.GNU-stack,"",%progbits
