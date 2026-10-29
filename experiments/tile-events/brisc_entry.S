.section .text
.global _start
_start:
    li sp, 0xFFB02000
    call brisc_main
1:  j 1b
