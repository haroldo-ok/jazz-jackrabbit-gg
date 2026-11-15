; Signed 16 x 16 -> 32 bit multiply (HL * DE -> HL:DE, sdcccall(1)),
; replacing SDCC 4.2.0's library routine, which tests the operands' signs
; on bit 7 of L and E instead of H and D (so 160 counted as negative).
	.module mulfix
	.globl ___mulsint2slong
	.globl ___muluint2ulong
	.area _CODE
___mulsint2slong:
	ld	bc, #0
	bit	7, h
	jr	z, 1$
	ld	a, b
	sub	a, l
	ld	l, a
	ld	a, b
	sbc	a, h
	ld	h, a
	inc	c
1$:
	bit	7, d
	jr	z, 2$
	ld	a, b
	sub	a, e
	ld	e, a
	ld	a, b
	sbc	a, d
	ld	d, a
	inc	c
2$:
	push	bc
	call	___muluint2ulong
	pop	bc
	bit	0, c
	ret	z
	ld	a, b
	sub	a, e
	ld	e, a
	ld	a, b
	sbc	a, d
	ld	d, a
	ld	a, b
	sbc	a, l
	ld	l, a
	ld	a, b
	sbc	a, h
	ld	h, a
	ret
; 16x16->32 multiplication
___muluint2ulong:
	ld	iy, #0
	ld	b, #16
3$:
	add	iy, iy
	adc	hl, hl
	jr	NC, 4$
	add	iy, de
	jr	NC, 4$
	inc	hl
4$:
	djnz	3$
	push	iy
	pop	de
	ret

