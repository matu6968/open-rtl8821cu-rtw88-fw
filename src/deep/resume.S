/* SPDX-License-Identifier: GPL-2.0-only
 * An ordinary o32 call boundary across the hardware's warm entry at _start.
 * Only the owned stack frame and this initialized data record are resumed.
 * The stock SRAM task/return slots are never borrowed.
 */
 .set noreorder
 .set mips16
 .set mips16e2
 .section .data.deep_resume,"aw"
 .balign 4
 .globl g_deep_resume
 .type g_deep_resume,@object
g_deep_resume:
 .word 0x31534d52,0,0,0,0,0
 .size g_deep_resume,.-g_deep_resume

 .section .text.core_deep_clock_gate,"ax"
 .balign 4
 .globl core_deep_clock_gate
 .ent core_deep_clock_gate
core_deep_clock_gate:
 addiu $sp,-32
 sw $16,20($sp)
 sw $17,24($sp)
 move $5,$ra
 sw $5,28($sp)
 .hword 0xb84c
 nop
 sw $2,16($sp)
 la $5,g_deep_resume
 move $6,$sp
 sw $6,8($5)
 la $6,core_deep_clock_resume
 sw $6,12($5)
 la $6,0x198dcafe
 sw $6,4($5)
 lui $5,0xb860
 addiu $5,0x82
 lbu $6,0($5)
 la $7,g_deep_resume
 sw $6,20($7)
 li $7,0x80
 or $6,$7
 sb $6,0($5)
 addiu $5,14
 sb $4,0($5)
 .rept 64
 nop
 .endr
 .end core_deep_clock_gate

 .globl core_deep_clock_resume
 .ent core_deep_clock_resume
core_deep_clock_resume:
 /* Keep this frame alive until the engine has actually returned. The
  * warm reset can occur after the initial store's pipeline delay. */
 lui $4,0xb860
 addiu $4,0x90
1:
 lbu $5,0($4)
 li $6,1
 and $5,$6
 bnez $5,1b
 nop
 lw $4,16($sp)
 .hword 0xb98c
 nop
 nop
 nop
 lw $16,20($sp)
 lw $17,24($sp)
 lw $4,28($sp)
 move $ra,$4
 addiu $sp,32
 jr $ra
 nop
 .end core_deep_clock_resume
