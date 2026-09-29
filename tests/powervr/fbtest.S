/* Guest-side check that the PowerVR emulation writes into the VGA card's memory (not RAM).
 * Builds a one-plane display list in conventional memory, renders one 32 pixel span into
 * VGA mode 13h memory at A0000 as 565 red (fog colour), then prints pixels 0, 31 and 32:
 * expected "F800 F800 0000". Build as tests/powervr/pvrtest.S. */
	.code16
	.org 0x100
	mov $0xB102, %ax
	mov $0x002A, %cx
	mov $0x1033, %dx
	xor %si, %si
	int $0x1A
	jc fail
	mov $0xB10D, %ax            /* BAR0 -> D0000 */
	mov $0x10, %di
	mov $0x000D0000, %ecx
	int $0x1A
	mov $0x0013, %ax            /* VGA mode 13h: A0000 is VGA memory, not RAM */
	int $0x10
	/* display list at 9000:0000 (object pointers) and 9100:0000 (one plane) */
	mov $0x9000, %ax
	mov %ax, %es
	movl $0x40000000, %es:0     /* tile x=0 y=0, 32 wide, 1 high */
	movl $0x80080000, %es:4     /* very last, 1 plane at word 0 */
	mov $0x9100, %ax
	mov %ax, %es
	movl $0x20000000, %es:0     /* forw_visib_fp, A=0 */
	movl $0x00800000, %es:4     /* tag 8, B=0 */
	movl $0, %es:8              /* C=0: far away, full fog */
	mov $0xD000, %ax
	mov %ax, %es
	movl $0x00090000, %es:0x1c  /* OBJECT_OFFSET */
	movl $0x91, %es:0x400       /* TLB: page frame numbers */
	movl $0x92, %es:0x404
	movl $0x000A0000, %es:0x4c  /* SOF_ADDR = VGA memory */
	movl $320, %es:0x48         /* stride */
	movl $2, %es:0x40           /* 565 */
	movl $0x00FF0000, %es:0x38  /* fog colour red */
	movl $0, %es:0x18
	movl $0, %es:0x14           /* START_RENDER */
	mov $0xA000, %ax
	mov %ax, %es
	mov %es:0, %bx
	mov %es:62, %cx
	mov %es:64, %dx
	push %dx
	push %cx
	push %bx
	mov $0x0003, %ax
	int $0x10
	pop %ax
	call hex16
	call space
	pop %ax
	call hex16
	call space
	pop %ax
	call hex16
	mov $0x4C00, %ax
	int $0x21
fail:	mov $0x4C01, %ax
	int $0x21
space:	mov $0x02, %ah
	mov $' ', %dl
	int $0x21
	ret
hex16:	push %ax
	mov %ah, %al
	call hex8
	pop %ax
hex8:	push %ax
	shr $4, %al
	call hex4
	pop %ax
hex4:	and $0x0F, %al
	add $'0', %al
	cmp $'9', %al
	jbe 1f
	add $7, %al
1:	mov %al, %dl
	mov $0x02, %ah
	int $0x21
	ret
