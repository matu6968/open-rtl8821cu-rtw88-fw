/* SPDX-License-Identifier: GPL-2.0-only */
/* Source-owned cold startup. Preserve the tested opaque entry operation;
 * only BSS is cleared, never initialized data, handoff words, or the stack.
 * No new interrupt enables, ROM services, or firmware-ready writes.
 */
	.set noreorder
	.set mips16
	.set mips16e2
	.section .text.start, "ax"
	.globl _start
	.ent _start
_start:
	la $4, __stack_top
	move $sp, $4
	li $4, 0xff00
	.hword 0xb98c
	nop
	nop
	nop
	/* Stock 80030016: bit23 means a retained low-clock return. Do this
	 * before cold initialization can clear the suspended C runtime. */
	lui $4, 0xb860
	lw $4, 0x80($4)
	lui $5, 0x80
	and $4, $5
	beqz $4, 3f
	nop
	la $7, g_deep_resume
	lw $4, 0($7)
	la $5, 0x31534d52
	cmp $4, $5
	btnez 4f
	nop
	lw $4, 4($7)
	la $5, 0x198dcafe
	cmp $4, $5
	btnez 4f
	nop
	lw $4, 8($7)
	la $6, __stack_bottom+32
	sltu $4, $6
	btnez 4f
	nop
	la $6, __stack_top-32
	sltu $6, $4
	btnez 4f
	nop
	move $5, $4
	li $6, 7
	and $5, $6
	bnez $5, 4f
	nop
	lw $5, 12($7)
	la $6, core_deep_clock_resume
	cmp $5, $6
	btnez 4f
	nop
	lw $6, 16($7)
	addiu $6, 1
	sw $6, 16($7)
	move $sp, $4
	lui $6, 0xb860
	addiu $6, 0x10fc
	la $4, 0xd5ff0100
	sw $4, 0($6)
	jr $5
	nop
4:
	/* Invalid warm context is terminal. Never disguise it as a cold boot. */
	lui $4, 0xb860
	addiu $4, 0x10fc
	la $5, 0xd5ff0007
	sw $5, 0($4)
	b 4b
	nop
3:
	lui $5, 0xb860
	addiu $5, 0x10fc
	lui $2, 0xc001
	addiu $2, 1
	sw $2, 0($5)
	la $4, __bss_start
	la $7, __bss_end
	li $6, 0
1:
	sltu $4, $7
	bteqz 2f
	nop
	sw $6, 0($4)
	addiu $4, 4
	b 1b
	nop
2:
	la $4, diag_main
	jr $4
	nop
	.end _start

	.section .text.diag_read_sp, "ax"
	.balign 4
	.globl diag_read_sp
	.ent diag_read_sp
diag_read_sp:
	move $2, $sp
	jr $ra
	nop
	.end diag_read_sp
