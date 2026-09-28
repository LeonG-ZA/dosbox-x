/* Guest-side smoke test for the PowerVR PCX1 emulation (DOS .COM).
 * Finds the card via the PCI BIOS, moves BAR0 to D000:0000, reads/writes registers,
 * starts a render and checks the end-of-render status and its read-acknowledge.
 * Build: as --32 pvrtest.S -o pvrtest.o && ld -m elf_i386 -Ttext=0 --oformat binary pvrtest.o -o full.bin
 *        && dd if=full.bin of=PVRTEST.COM bs=1 skip=256
 * Run in DOSBox-X with machine=svga_s3 and [powervr] powervr_card=pcx1. */
	.code16
	.org 0x100
start:
	/* find PCX1: vendor 1033 device 002A */
	mov $0xB102, %ax
	mov $0x002A, %cx
	mov $0x1033, %dx
	xor %si, %si
	int $0x1A
	jc notfound
	mov %bx, busdev
	mov $msg_found, %dx
	call puts
	mov busdev, %ax
	call hex16
	call crlf
	/* config dword 0 */
	mov busdev, %bx
	mov $0xB10A, %ax
	mov $0x00, %di
	int $0x1A
	mov $msg_id, %dx
	call puts
	mov %ecx, %eax
	call hex32
	call crlf
	/* BAR0 -> 0x000D0000 */
	mov busdev, %bx
	mov $0xB10D, %ax
	mov $0x10, %di
	mov $0x000D0000, %ecx
	int $0x1A
	mov busdev, %bx
	mov $0xB10A, %ax
	mov $0x10, %di
	int $0x1A
	mov $msg_bar, %dx
	call puts
	mov %ecx, %eax
	call hex32
	call crlf
	/* registers through MMIO at D000:0000 */
	mov $0xD000, %ax
	mov %ax, %es
	mov $msg_reg, %dx
	call puts
	mov %es:0x00, %eax          /* PCX_ID */
	call hex32
	call space
	movl $2, %es:0x10           /* INTMASK = end of render */
	movl $0x1234, %es:0x3C      /* CAMERA */
	mov %es:0x3C, %eax
	call hex32
	call space
	movl $1, %es:0x14           /* START_RENDER (no frame buffer: nothing drawn) */
	mov %es:0x0C, %eax          /* INT_STATUS: expect 2 */
	call hex32
	call space
	mov %es:0x0C, %eax          /* read acknowledged it: expect 0 */
	call hex32
	call crlf
	/* texture memory BAR1 through config, then restore BAR0 */
	mov busdev, %bx
	mov $0xB10A, %ax
	mov $0x14, %di
	int $0x1A
	mov $msg_bar1, %dx
	call puts
	mov %ecx, %eax
	call hex32
	call crlf
	mov $0x4C00, %ax
	int $0x21
notfound:
	mov $msg_nf, %dx
	call puts
	mov $0x4C01, %ax
	int $0x21

puts:	mov $0x09, %ah
	int $0x21
	ret
crlf:	mov $msg_crlf, %dx
	jmp puts
space:	mov $0x02, %ah
	mov $' ', %dl
	int $0x21
	ret
hex32:	push %eax
	shr $16, %eax
	call hex16
	pop %eax
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

busdev:	.word 0
msg_found: .ascii "PCX found, bus/devfn $"
msg_id:    .ascii "config 00: $"
msg_bar:   .ascii "BAR0 after write: $"
msg_bar1:  .ascii "BAR1: $"
msg_reg:   .ascii "ID CAMERA INTSTATUS INTSTATUS: $"
msg_nf:    .ascii "PCX not found$"
msg_crlf:  .ascii "\r\n$"
