

%ifndef TEST
cpu 286
%endif

%macro SATR 1
    jno %%ok
    mov %1,7FFFh
    js %%ok
    mov %1,8001h
%%ok:
%endmacro

segment sidfast_TEXT class=CODE use16 public align=2
global sid_fast_
global sid_fast
sid_fast_:
sid_fast:
%ifndef TEST
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push es
%else
    mov bx,SEL_ST
    mov ds,bx
%endif
    cld
    mov si,ax
    les di,[si]
samp:
    xor bp,bp
    xor cx,cx

    mov ax,[si+26]
    add [si+22],ax
    mov ax,[si+24]
    mov [si+48],ax
    adc ax,[si+28]
    mov [si+24],ax

    mov ax,[si+54]
    add [si+50],ax
    mov ax,[si+52]
    mov [si+76],ax
    adc ax,[si+56]
    mov [si+52],ax

    mov ax,[si+82]
    add [si+78],ax
    mov ax,[si+80]
    mov [si+104],ax
    adc ax,[si+84]
    mov [si+80],ax

    mov bx,[si+30]
    jmp word [cs:bx+vt0]
v0_rng:
    mov al,[si+25]
    mov ah,[si+81]
    and ah,80h
    xor al,ah
    mov bx,[si+40]
    xlatb
    imul byte [si+38]
    jmp short v0_add
v0_tbl:
    mov bx,[si+40]
    mov al,[si+25]
    xlatb
    imul byte [si+38]
    jmp short v0_add
v0_pul:
    mov ax,[si+24]
    cmp ax,[si+32]
    mov ax,[si+34]
    jae v0_add
    mov ax,[si+36]
    jmp short v0_add
v0_add:
    test byte [si+6],1
    jz v0_fo
    add bp,ax
    jmp short v0_off
v0_fo:
    add cx,ax
v0_off:

    mov bx,[si+58]
    jmp word [cs:bx+vt1]
v1_rng:
    mov al,[si+53]
    mov ah,[si+25]
    and ah,80h
    xor al,ah
    mov bx,[si+68]
    xlatb
    imul byte [si+66]
    jmp short v1_add
v1_tbl:
    mov bx,[si+68]
    mov al,[si+53]
    xlatb
    imul byte [si+66]
    jmp short v1_add
v1_pul:
    mov ax,[si+52]
    cmp ax,[si+60]
    mov ax,[si+62]
    jae v1_add
    mov ax,[si+64]
    jmp short v1_add
v1_add:
    test byte [si+6],2
    jz v1_fo
    add bp,ax
    jmp short v1_off
v1_fo:
    add cx,ax
v1_off:

    mov bx,[si+86]
    jmp word [cs:bx+vt2]
v2_rng:
    mov al,[si+81]
    mov ah,[si+53]
    and ah,80h
    xor al,ah
    mov bx,[si+96]
    xlatb
    imul byte [si+94]
    jmp short v2_add
v2_tbl:
    mov bx,[si+96]
    mov al,[si+81]
    xlatb
    imul byte [si+94]
    jmp short v2_add
v2_pul:
    mov ax,[si+80]
    cmp ax,[si+88]
    mov ax,[si+90]
    jae v2_add
    mov ax,[si+92]
    jmp short v2_add
v2_add:
    test byte [si+6],4
    jz v2_fo
    add bp,ax
    jmp short v2_off
v2_fo:
    add cx,ax
v2_off:

    test byte [si+6],80h
    jnz dofilt
    jmp outp
dofilt:
    mov bx,[si+18]
fsub:
    mov ax,[si+10]
    imul word [si+16]
    shl dx,1
    mov ax,bp
    sub ax,[si+8]
    SATR ax
    sub ax,dx
    SATR ax
    sub ax,dx
    SATR ax
    mov [si+12],ax
    imul word [si+14]
    shl dx,1
    add dx,[si+10]
    SATR dx
    mov [si+10],dx
    mov ax,dx
    imul word [si+14]
    shl dx,1
    add dx,[si+8]
    SATR dx
    mov [si+8],dx
    dec bx
    jnz fsub
    test byte [si+6],10h
    jz m1
    add cx,[si+8]
    SATR cx
m1:
    test byte [si+6],20h
    jz m2
    add cx,[si+10]
    SATR cx
m2:
    test byte [si+6],40h
    jz outp
    add cx,[si+12]
    SATR cx
outp:
    mov ax,cx
    imul word [si+20]
    add dx,128
    or dh,dh
    jz st
    mov dl,0
    js st
    mov dl,255
st:
    mov al,dl
    stosb
    dec word [si+4]
    jz done
    jmp samp
done:
%ifdef TEST
    xor ax,ax
    mov ds,ax
    mov es,ax
    jmp dword 0x33:0
%else
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    retf
%endif

align 2
vt0: dw v0_off,v0_tbl,v0_pul,v0_noi,v0_rng
vt1: dw v1_off,v1_tbl,v1_pul,v1_noi,v1_rng
vt2: dw v2_off,v2_tbl,v2_pul,v2_noi,v2_rng

v0_noi:
    mov dx,[si+48]
    mov ax,[si+24]
    mov bl,dh
    shr bl,3
    mov al,ah
    shr al,3
    sub al,bl
    and al,31
    add al,bl
    inc al
    shr al,1
    inc bl
    shr bl,1
    sub al,bl
    jz v0_nzo
    mov bl,al
v0_nl:
    mov ax,[si+44]
    mov dx,ax
    shr dx,5
    xor dx,ax
    shr dx,2
    rcl word [si+42],1
    rcl word [si+44],1
    and word [si+44],7Fh
    dec bl
    jnz v0_nl
    mov dx,[si+42]
    mov bx,[si+44]
    xor al,al
    test bl,40h
    jz v0_n1
    or al,80h
v0_n1:
    test bl,8
    jz v0_n2
    or al,40h
v0_n2:
    test dh,80h
    jz v0_n3
    or al,20h
v0_n3:
    test dh,8
    jz v0_n4
    or al,10h
v0_n4:
    test dl,80h
    jz v0_n5
    or al,8
v0_n5:
    test dl,10h
    jz v0_n6
    or al,4
v0_n6:
    test dl,4
    jz v0_n7
    or al,2
v0_n7:
    test dl,1
    jz v0_n8
    or al,1
v0_n8:
    mov [si+46],al
v0_nzo:
    mov al,[si+46]
    sub al,128
    imul byte [si+38]
    jmp v0_add

v1_noi:
    mov dx,[si+76]
    mov ax,[si+52]
    mov bl,dh
    shr bl,3
    mov al,ah
    shr al,3
    sub al,bl
    and al,31
    add al,bl
    inc al
    shr al,1
    inc bl
    shr bl,1
    sub al,bl
    jz v1_nzo
    mov bl,al
v1_nl:
    mov ax,[si+72]
    mov dx,ax
    shr dx,5
    xor dx,ax
    shr dx,2
    rcl word [si+70],1
    rcl word [si+72],1
    and word [si+72],7Fh
    dec bl
    jnz v1_nl
    mov dx,[si+70]
    mov bx,[si+72]
    xor al,al
    test bl,40h
    jz v1_n1
    or al,80h
v1_n1:
    test bl,8
    jz v1_n2
    or al,40h
v1_n2:
    test dh,80h
    jz v1_n3
    or al,20h
v1_n3:
    test dh,8
    jz v1_n4
    or al,10h
v1_n4:
    test dl,80h
    jz v1_n5
    or al,8
v1_n5:
    test dl,10h
    jz v1_n6
    or al,4
v1_n6:
    test dl,4
    jz v1_n7
    or al,2
v1_n7:
    test dl,1
    jz v1_n8
    or al,1
v1_n8:
    mov [si+74],al
v1_nzo:
    mov al,[si+74]
    sub al,128
    imul byte [si+66]
    jmp v1_add

v2_noi:
    mov dx,[si+104]
    mov ax,[si+80]
    mov bl,dh
    shr bl,3
    mov al,ah
    shr al,3
    sub al,bl
    and al,31
    add al,bl
    inc al
    shr al,1
    inc bl
    shr bl,1
    sub al,bl
    jz v2_nzo
    mov bl,al
v2_nl:
    mov ax,[si+100]
    mov dx,ax
    shr dx,5
    xor dx,ax
    shr dx,2
    rcl word [si+98],1
    rcl word [si+100],1
    and word [si+100],7Fh
    dec bl
    jnz v2_nl
    mov dx,[si+98]
    mov bx,[si+100]
    xor al,al
    test bl,40h
    jz v2_n1
    or al,80h
v2_n1:
    test bl,8
    jz v2_n2
    or al,40h
v2_n2:
    test dh,80h
    jz v2_n3
    or al,20h
v2_n3:
    test dh,8
    jz v2_n4
    or al,10h
v2_n4:
    test dl,80h
    jz v2_n5
    or al,8
v2_n5:
    test dl,10h
    jz v2_n6
    or al,4
v2_n6:
    test dl,4
    jz v2_n7
    or al,2
v2_n7:
    test dl,1
    jz v2_n8
    or al,1
v2_n8:
    mov [si+102],al
v2_nzo:
    mov al,[si+102]
    sub al,128
    imul byte [si+94]
    jmp v2_add
