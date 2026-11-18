@ Stubs d'import (12 octets, code ARM), comme ceux des bibliothèques de stubs
@ du VitaSDK. Le chargeur les réécrit ; non corrigés, ils plantent (UDF).
    .syntax unified
    .arm
    .section .vita.stubs, "ax", %progbits
    .align 4

    .macro stub name
    .global \name
    .type \name, %function
\name:
    .word 0xE7F000F0, 0xE7F000F0, 0xE7F000F0
    .size \name, 12
    .endm

    stub sceKernelExitProcess
    stub vitaTestReport
    stub vitaTestUnimplemented

@ Zone remplie par make_vita_elf.py : sceModuleInfo et tables d'imports.
    .section .sce.reserved, "a", %progbits
    .align 4
    .global __sce_reserved
__sce_reserved:
    .space 1024
