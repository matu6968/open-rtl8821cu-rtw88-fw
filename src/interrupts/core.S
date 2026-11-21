/* SPDX-License-Identifier: GPL-2.0-only */
/* Verified core operations are kept in named assembly boundaries. No delay
 * slots. ROM vector 80/100 saves original v0/v1 before its dispatch clobbers.
 * Interrupts stay nonnested. C runs below the foreground frame and its 16-byte
 * outgoing-argument area; no scheduler, ROM ISR, or stock SRAM is borrowed.
 */
 .set noreorder
 .set mips16
 .set mips16e2
 .macro FUNC name
 .section .text.\name,"ax"
 .balign 4
 .globl \name
 .ent \name
\name:
 .endm
 .macro END name
 .end \name
 .endm
 FUNC core_status
 .hword 0xb84c
 nop
 jr $ra
 nop
 END core_status
 FUNC core_irq_save
 .hword 0xb84c
 nop
 .hword 0xba40
 nop
 nop
 nop
 jr $ra
 nop
 END core_irq_save
 FUNC core_irq_restore
 .hword 0xb98c
 nop
 nop
 nop
 jr $ra
 nop
 END core_irq_restore
 FUNC core_irq_enable
 .hword 0xba60
 nop
 nop
 nop
 jr $ra
 nop
 END core_irq_enable
 FUNC core_irq_disable
 .hword 0xba40
 nop
 nop
 nop
 jr $ra
 nop
 END core_irq_disable
 FUNC core_raise_software
 li $2,0x100
 .hword 0xb9aa
 nop
 nop
 nop
 jr $ra
 nop
 END core_raise_software
 FUNC core_clear_software
 li $2,0
 .hword 0xb9aa
 nop
 nop
 nop
 jr $ra
 nop
 END core_clear_software

 FUNC core_irq_entry
 addiu $sp,-80
 sw $4,24($sp)
 sw $5,28($sp)
 sw $6,32($sp)
 sw $7,36($sp)
 sw $16,40($sp)
 sw $17,44($sp)
 move $4,$24
 sw $4,48($sp)
 move $4,$25
 /* Register 25 is excluded from compiled C: on this core these MOVE forms
  * read zero after a nonzero write. Keep its observed slot, not a GPR claim.
  */
 sw $4,52($sp)
 move $4,$ra
 sw $4,56($sp)
 move $4,$sp
 addiu $4,80
 sw $4,60($sp)
 mflo $4
 mfhi $5
 sw $4,8($sp)
 sw $5,12($sp)
 .hword 0xb85a
 nop
 sw $2,16($sp)
 .hword 0xb87b
 nop
 sw $3,20($sp)
 .hword 0xb84c
 nop
 sw $2,0($sp)
 .hword 0xb84e
 nop
 sw $2,4($sp)
 .hword 0xb86d
 nop
 sw $3,64($sp)
 move $4,$sp
 addiu $sp,-16
 jal foundation_irq_dispatch
 nop
 addiu $sp,16
 /* Deliberately clobber the non-SP context after the C call. The litmus
  * checks the epilogue restores these, including HI/LO and condition state.
  * These poison values never escape a correctly restored interrupt.
  */
 li $4,0x51
 li $5,0x62
 .hword 0xec32,0xed30
 li $2,0x71
 li $3,0x72
 li $6,0x73
 li $7,0x74
 li $16,0x75
 li $17,0x76
 move $24,$2
 move $25,$3
 move $ra,$6
 lw $3,4($sp)
 .hword 0xb9cb
 nop
 nop
 nop
 lw $4,0($sp)
 .hword 0xb98c
 nop
 nop
 nop
 lw $4,16($sp)
 .hword 0xb95c
 nop
 nop
 nop
 lw $4,20($sp)
 .hword 0xb97c
 nop
 nop
 nop
 lw $4,8($sp)
 lw $5,12($sp)
 .hword 0xec32,0xed30
 lw $4,56($sp)
 move $ra,$4
 lw $4,52($sp)
 move $25,$4
 lw $4,48($sp)
 move $24,$4
 lw $17,44($sp)
 lw $16,40($sp)
 lw $7,36($sp)
 lw $6,32($sp)
 lw $5,28($sp)
 lw $4,24($sp)
 addiu $sp,80
 .hword 0xb85a
 nop
 .hword 0xb87b
 nop
 .hword 0xba00
 nop
 END core_irq_entry

 .macro CONTEXT name,timed
 FUNC \name
 addiu $sp,-96
 sw $16,64($sp)
 sw $17,68($sp)
 move $4,$ra
 sw $4,72($sp)
 la $4,0x11223344
 la $5,0x55667788
 .hword 0xec32,0xed30
 li $2,0x100
 la $3,0x22334455
 la $4,0x33445566
 la $5,0x44556677
 la $6,0x55667788
 la $7,0x66778899
 la $16,0x778899aa
 la $17,0x8899aabb
 move $24,$6
 move $25,$7
 move $ra,$5
 .if \timed
 li $2,60000
 li $3,0
2:
 addiu $2,-1
 addu $3,$2
 bnez $2,2b
 nop
 .else
 .hword 0xb9aa
 .endif
 nop
 nop
 nop
 sw $2,0($sp)
 sw $3,4($sp)
 sw $4,8($sp)
 sw $5,12($sp)
 sw $6,16($sp)
 sw $7,20($sp)
 sw $16,24($sp)
 sw $17,28($sp)
 move $2,$24
 sw $2,32($sp)
 move $2,$25
 sw $2,36($sp)
 mflo $2
 sw $2,40($sp)
 mfhi $2
 sw $2,44($sp)
 move $2,$ra
 sw $2,48($sp)
 move $2,$sp
 sw $2,52($sp)
 .hword 0xb84c
 nop
 sw $2,56($sp)
 .hword 0xb86d
 nop
 sw $3,60($sp)
 la $4,g_context_snapshot
 move $5,$sp
 li $6,16
1:
 lw $2,0($5)
 sw $2,0($4)
 addiu $4,4
 addiu $5,4
 addiu $6,-1
 bnez $6,1b
 nop
 lw $16,64($sp)
 lw $17,68($sp)
 lw $4,72($sp)
 move $ra,$4
 addiu $sp,96
 li $2,0
 jr $ra
 nop
 END \name
 .endm
 CONTEXT core_context_test,0
 CONTEXT core_context_timed_test,1

 FUNC core_trap_test
 nop
 .globl core_trap_point
core_trap_point:
 .hword 0xfeba
 li $2,0x5a
 jr $ra
 nop
 END core_trap_test
