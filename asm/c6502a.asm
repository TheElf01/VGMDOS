

%ifdef TEST
%define VS gs
%else
cpu 286
%define VS cs
%endif

%define ST_A 0
%define ST_X 1
%define ST_Y 2
%define ST_S 3
%define ST_FL 4
%define ST_FV 5
%define ST_FD 6
%define ST_FI 7
%define ST_PC 8
%define ST_SEG 10
%define ST_TICKS 12
%define ST_IOA 14
%define ST_IOV 16
%define ST_STAT 17

%macro DISPATCH 0
    lodsb
    mov di,ax
    mov bp,ax
    lea bp,[bp+di]
    jmp word [cs:bp+optab]
%endmacro

%macro NEXT 0
%ifdef STEP
    lahf
    mov byte [VS:stat],3
    jmp do_exit
%else
    DISPATCH
%endif
%endmacro

%macro TICK 0
    lahf
    dec word [VS:ticks]
    jnz %%go
    jmp tick_exit
%%go:
    inc byte [0D012h]
    jnz %%a
    xor byte [0D011h],80h
%%a:
    add byte [0D41Bh],7
    sahf
    mov ah,0
%endmacro

%macro STORE_END 1
    lahf
    cmp di,0D000h
    jae %%io
    sahf
    mov ah,0
    NEXT
%%io:
    mov [VS:io_a],di
    mov [VS:io_v],%1
    mov byte [VS:stat],2
    jmp do_exit
%endmacro

%macro SETP 0
    mov dh,al
    and dh,40h
    mov [VS:fV],dh
    mov dh,al
    and dh,8
    mov [VS:fD],dh
    mov dh,al
    and dh,4
    mov [VS:fI],dh
    mov ah,0
    test al,80h
    jz %%1
    or ah,80h
%%1:
    test al,2
    jz %%2
    or ah,40h
%%2:
    test al,1
    jz %%3
    or ah,1
%%3:
    sahf
    mov ah,0
%endmacro

%macro PUSHP 0
    lahf
    mov al,30h
    test ah,80h
    jz %%1
    or al,80h
%%1:
    test ah,40h
    jz %%2
    or al,2
%%2:
    test ah,1
    jz %%3
    or al,1
%%3:
    cmp byte [VS:fV],0
    je %%4
    or al,40h
%%4:
    cmp byte [VS:fD],0
    je %%5
    or al,8
%%5:
    cmp byte [VS:fI],0
    je %%6
    or al,4
%%6:
    mov di,[VS:S16]
    mov [di],al
    lea di,[di-1]
    mov [VS:S16],di
    sahf
    mov ah,0
%endmacro

segment c6502a_TEXT class=CODE use16 public align=2
global c6_run_
global c6_run
c6_run_:
c6_run:
%ifndef TEST
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es
%else
    mov bx,SEL_VAR
    mov gs,bx
    mov bx,SEL_ST
    mov ds,bx
%endif
    cld
    mov [VS:stp],ax
    mov [VS:stseg],ds
    mov si,ax
    mov dl,[si+ST_A]
    mov bl,[si+ST_X]
    mov cl,[si+ST_Y]
    mov al,[si+ST_S]
    mov ah,1
    mov [VS:S16],ax
    mov al,[si+ST_FV]
    mov [VS:fV],al
    mov al,[si+ST_FD]
    mov [VS:fD],al
    mov al,[si+ST_FI]
    mov [VS:fI],al
    mov ax,[si+ST_TICKS]
    mov [VS:ticks],ax
    mov ah,[si+ST_FL]
    mov bp,[si+ST_PC]
    mov di,[si+ST_SEG]
    mov si,bp
    mov es,di
    mov ds,di
    mov bh,0
    mov ch,0
    sahf
    mov ah,0
    DISPATCH

tick_exit:
    mov byte [VS:stat],0
    jmp do_exit

h_bad:
    lea si,[si-1]
    lahf
    mov byte [VS:stat],1
do_exit:
    mov es,[VS:stseg]
    mov bp,[VS:stp]
    mov [es:bp+ST_A],dl
    mov [es:bp+ST_X],bl
    mov [es:bp+ST_Y],cl
    mov al,[VS:S16]
    mov [es:bp+ST_S],al
    mov [es:bp+ST_FL],ah
    mov al,[VS:fV]
    mov [es:bp+ST_FV],al
    mov al,[VS:fD]
    mov [es:bp+ST_FD],al
    mov al,[VS:fI]
    mov [es:bp+ST_FI],al
    mov [es:bp+ST_PC],si
    mov ax,[VS:ticks]
    mov [es:bp+ST_TICKS],ax
    mov ax,[VS:io_a]
    mov [es:bp+ST_IOA],ax
    mov al,[VS:io_v]
    mov [es:bp+ST_IOV],al
    mov al,[VS:stat]
    mov [es:bp+ST_STAT],al
    mov ah,0
%ifdef TEST
    xor ax,ax
    mov ds,ax
    mov es,ax
    mov gs,ax
ret_far:
    jmp dword 0x33:0
%else
    mov ds,[VS:stseg]
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    retf
%endif

h_01:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    mov di,[di]
    jc .c1
    or dl,[di]
    NEXT
.c1:
    or dl,[di]
    stc
    NEXT

h_04:
    lea si,[si+1]
    NEXT

h_05:
    lodsb
    mov di,ax
    jc .c1
    or dl,[di]
    NEXT
.c1:
    or dl,[di]
    stc
    NEXT

h_06:
    lodsb
    mov di,ax
    shl byte [di],1
    NEXT

h_08:
    PUSHP
    NEXT

h_09:
    mov di,si
    lea si,[si+1]
    jc .c1
    or dl,[di]
    NEXT
.c1:
    or dl,[di]
    stc
    NEXT

h_0A:
    shl dl,1
    NEXT

h_0C:
    lea si,[si+2]
    NEXT

h_0D:
    mov di,[si]
    lea si,[si+2]
    jc .c1
    or dl,[di]
    NEXT
.c1:
    or dl,[di]
    stc
    NEXT

h_0E:
    mov di,[si]
    lea si,[si+2]
    shl byte [di],1
    NEXT

h_10:
    lodsb
    js .nt
    cbw
    mov bp,ax
    mov ah,0
    lea si,[si+bp]
    TICK
.nt:
    NEXT

h_11:
    lodsb
    mov di,ax
    mov di,[di]
    mov bp,cx
    lea di,[bp+di]
    jc .c1
    or dl,[di]
    NEXT
.c1:
    or dl,[di]
    stc
    NEXT

h_14:
    lea si,[si+1]
    NEXT

h_15:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    jc .c1
    or dl,[di]
    NEXT
.c1:
    or dl,[di]
    stc
    NEXT

h_16:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    shl byte [di],1
    NEXT

h_18:
    clc
    NEXT

h_19:
    mov di,[si]
    lea si,[si+2]
    mov bp,cx
    lea di,[bp+di]
    jc .c1
    or dl,[di]
    NEXT
.c1:
    or dl,[di]
    stc
    NEXT

h_1A:
    NEXT

h_1C:
    lea si,[si+2]
    NEXT

h_1D:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    jc .c1
    or dl,[di]
    NEXT
.c1:
    or dl,[di]
    stc
    NEXT

h_1E:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    shl byte [di],1
    NEXT

h_20:
    mov bp,[si]
    lea si,[si+1]
    mov di,[VS:S16]
    mov [di-1],si
    lea di,[di-2]
    mov [VS:S16],di
    mov si,bp
    TICK
    NEXT

h_21:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    mov di,[di]
    jc .c1
    and dl,[di]
    NEXT
.c1:
    and dl,[di]
    stc
    NEXT

h_24:
    lodsb
    mov di,ax
    sbb ah,ah
    and ah,1
    mov dh,[di]
    mov al,dh
    and al,40h
    mov [VS:fV],al
    test dh,80h
    jz .a
    or ah,80h
.a:
    test dh,dl
    jnz .b
    or ah,40h
.b:
    sahf
    mov ah,0
    NEXT

h_25:
    lodsb
    mov di,ax
    jc .c1
    and dl,[di]
    NEXT
.c1:
    and dl,[di]
    stc
    NEXT

h_26:
    lodsb
    mov di,ax
    rcl byte [di],1
    mov dh,[di]
    inc dh
    dec dh
    NEXT

h_28:
    mov di,[VS:S16]
    lea di,[di+1]
    mov [VS:S16],di
    mov al,[di]
    SETP
    NEXT

h_29:
    mov di,si
    lea si,[si+1]
    jc .c1
    and dl,[di]
    NEXT
.c1:
    and dl,[di]
    stc
    NEXT

h_2A:
    rcl dl,1
    inc dl
    dec dl
    NEXT

h_2C:
    mov di,[si]
    lea si,[si+2]
    sbb ah,ah
    and ah,1
    mov dh,[di]
    mov al,dh
    and al,40h
    mov [VS:fV],al
    test dh,80h
    jz .a
    or ah,80h
.a:
    test dh,dl
    jnz .b
    or ah,40h
.b:
    sahf
    mov ah,0
    NEXT

h_2D:
    mov di,[si]
    lea si,[si+2]
    jc .c1
    and dl,[di]
    NEXT
.c1:
    and dl,[di]
    stc
    NEXT

h_2E:
    mov di,[si]
    lea si,[si+2]
    rcl byte [di],1
    mov dh,[di]
    inc dh
    dec dh
    NEXT

h_30:
    lodsb
    jns .nt
    cbw
    mov bp,ax
    mov ah,0
    lea si,[si+bp]
    TICK
.nt:
    NEXT

h_31:
    lodsb
    mov di,ax
    mov di,[di]
    mov bp,cx
    lea di,[bp+di]
    jc .c1
    and dl,[di]
    NEXT
.c1:
    and dl,[di]
    stc
    NEXT

h_34:
    lea si,[si+1]
    NEXT

h_35:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    jc .c1
    and dl,[di]
    NEXT
.c1:
    and dl,[di]
    stc
    NEXT

h_36:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    rcl byte [di],1
    mov dh,[di]
    inc dh
    dec dh
    NEXT

h_38:
    stc
    NEXT

h_39:
    mov di,[si]
    lea si,[si+2]
    mov bp,cx
    lea di,[bp+di]
    jc .c1
    and dl,[di]
    NEXT
.c1:
    and dl,[di]
    stc
    NEXT

h_3A:
    NEXT

h_3C:
    lea si,[si+2]
    NEXT

h_3D:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    jc .c1
    and dl,[di]
    NEXT
.c1:
    and dl,[di]
    stc
    NEXT

h_3E:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    rcl byte [di],1
    mov dh,[di]
    inc dh
    dec dh
    NEXT

h_40:
    mov di,[VS:S16]
    mov al,[di+1]
    mov si,[di+2]
    lea di,[di+3]
    mov [VS:S16],di
    SETP
    TICK
    NEXT

h_41:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    mov di,[di]
    jc .c1
    xor dl,[di]
    NEXT
.c1:
    xor dl,[di]
    stc
    NEXT

h_44:
    lea si,[si+1]
    NEXT

h_45:
    lodsb
    mov di,ax
    jc .c1
    xor dl,[di]
    NEXT
.c1:
    xor dl,[di]
    stc
    NEXT

h_46:
    lodsb
    mov di,ax
    shr byte [di],1
    NEXT

h_48:
    mov di,[VS:S16]
    mov [di],dl
    lea di,[di-1]
    mov [VS:S16],di
    NEXT

h_49:
    mov di,si
    lea si,[si+1]
    jc .c1
    xor dl,[di]
    NEXT
.c1:
    xor dl,[di]
    stc
    NEXT

h_4A:
    shr dl,1
    NEXT

h_4C:
    mov si,[si]
    TICK
    NEXT

h_4D:
    mov di,[si]
    lea si,[si+2]
    jc .c1
    xor dl,[di]
    NEXT
.c1:
    xor dl,[di]
    stc
    NEXT

h_4E:
    mov di,[si]
    lea si,[si+2]
    shr byte [di],1
    NEXT

h_50:
    lodsb
    mov dh,al
    lahf
    cmp byte [VS:fV],0
    jne .nt
    mov al,dh
    mov dh,ah
    cbw
    mov bp,ax
    mov ah,dh
    sahf
    mov ah,0
    lea si,[si+bp]
    TICK
    jmp .nx
.nt:
    sahf
    mov ah,0
.nx:
    NEXT

h_51:
    lodsb
    mov di,ax
    mov di,[di]
    mov bp,cx
    lea di,[bp+di]
    jc .c1
    xor dl,[di]
    NEXT
.c1:
    xor dl,[di]
    stc
    NEXT

h_54:
    lea si,[si+1]
    NEXT

h_55:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    jc .c1
    xor dl,[di]
    NEXT
.c1:
    xor dl,[di]
    stc
    NEXT

h_56:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    shr byte [di],1
    NEXT

h_58:
    mov byte [VS:fI],0
    NEXT

h_59:
    mov di,[si]
    lea si,[si+2]
    mov bp,cx
    lea di,[bp+di]
    jc .c1
    xor dl,[di]
    NEXT
.c1:
    xor dl,[di]
    stc
    NEXT

h_5A:
    NEXT

h_5C:
    lea si,[si+2]
    NEXT

h_5D:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    jc .c1
    xor dl,[di]
    NEXT
.c1:
    xor dl,[di]
    stc
    NEXT

h_5E:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    shr byte [di],1
    NEXT

h_60:
    mov di,[VS:S16]
    mov si,[di+1]
    lea di,[di+2]
    mov [VS:S16],di
    lea si,[si+1]
    TICK
    NEXT

h_61:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    mov di,[di]
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    adc dl,[di]
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    xchg al,dl
    adc al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    daa
    xchg al,dl
    NEXT

h_64:
    lea si,[si+1]
    NEXT

h_65:
    lodsb
    mov di,ax
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    adc dl,[di]
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    xchg al,dl
    adc al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    daa
    xchg al,dl
    NEXT

h_66:
    lodsb
    mov di,ax
    rcr byte [di],1
    mov dh,[di]
    inc dh
    dec dh
    NEXT

h_68:
    mov di,[VS:S16]
    lea di,[di+1]
    mov [VS:S16],di
    mov dl,[di]
    inc dl
    dec dl
    NEXT

h_69:
    mov di,si
    lea si,[si+1]
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    adc dl,[di]
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    xchg al,dl
    adc al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    daa
    xchg al,dl
    NEXT

h_6A:
    rcr dl,1
    inc dl
    dec dl
    NEXT

h_6C:
    mov di,[si]
    mov si,[di]
    TICK
    NEXT

h_6D:
    mov di,[si]
    lea si,[si+2]
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    adc dl,[di]
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    xchg al,dl
    adc al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    daa
    xchg al,dl
    NEXT

h_6E:
    mov di,[si]
    lea si,[si+2]
    rcr byte [di],1
    mov dh,[di]
    inc dh
    dec dh
    NEXT

h_70:
    lodsb
    mov dh,al
    lahf
    cmp byte [VS:fV],0
    je .nt
    mov al,dh
    mov dh,ah
    cbw
    mov bp,ax
    mov ah,dh
    sahf
    mov ah,0
    lea si,[si+bp]
    TICK
    jmp .nx
.nt:
    sahf
    mov ah,0
.nx:
    NEXT

h_71:
    lodsb
    mov di,ax
    mov di,[di]
    mov bp,cx
    lea di,[bp+di]
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    adc dl,[di]
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    xchg al,dl
    adc al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    daa
    xchg al,dl
    NEXT

h_74:
    lea si,[si+1]
    NEXT

h_75:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    adc dl,[di]
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    xchg al,dl
    adc al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    daa
    xchg al,dl
    NEXT

h_76:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    rcr byte [di],1
    mov dh,[di]
    inc dh
    dec dh
    NEXT

h_78:
    mov byte [VS:fI],1
    NEXT

h_79:
    mov di,[si]
    lea si,[si+2]
    mov bp,cx
    lea di,[bp+di]
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    adc dl,[di]
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    xchg al,dl
    adc al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    daa
    xchg al,dl
    NEXT

h_7A:
    NEXT

h_7C:
    lea si,[si+2]
    NEXT

h_7D:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    adc dl,[di]
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    xchg al,dl
    adc al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    daa
    xchg al,dl
    NEXT

h_7E:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    rcr byte [di],1
    mov dh,[di]
    inc dh
    dec dh
    NEXT

h_80:
    lea si,[si+1]
    NEXT

h_81:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    mov di,[di]
    mov [di],dl
    STORE_END dl

h_82:
    lea si,[si+1]
    NEXT

h_84:
    lodsb
    mov di,ax
    mov [di],cl
    NEXT

h_85:
    lodsb
    mov di,ax
    mov [di],dl
    NEXT

h_86:
    lodsb
    mov di,ax
    mov [di],bl
    NEXT

h_88:
    dec cl
    NEXT

h_89:
    lea si,[si+1]
    NEXT

h_8A:
    mov dl,bl
    inc dl
    dec dl
    NEXT

h_8C:
    mov di,[si]
    lea si,[si+2]
    mov [di],cl
    STORE_END cl

h_8D:
    mov di,[si]
    lea si,[si+2]
    mov [di],dl
    STORE_END dl

h_8E:
    mov di,[si]
    lea si,[si+2]
    mov [di],bl
    STORE_END bl

h_90:
    lodsb
    jc .nt
    cbw
    mov bp,ax
    mov ah,0
    lea si,[si+bp]
    TICK
.nt:
    NEXT

h_91:
    lodsb
    mov di,ax
    mov di,[di]
    mov bp,cx
    lea di,[bp+di]
    mov [di],dl
    STORE_END dl

h_94:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    mov [di],cl
    NEXT

h_95:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    mov [di],dl
    NEXT

h_96:
    lodsb
    mov di,ax
    mov bp,cx
    lea di,[bp+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    mov [di],bl
    NEXT

h_98:
    mov dl,cl
    inc dl
    dec dl
    NEXT

h_99:
    mov di,[si]
    lea si,[si+2]
    mov bp,cx
    lea di,[bp+di]
    mov [di],dl
    STORE_END dl

h_9A:
    mov [VS:S16],bl
    NEXT

h_9D:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    mov [di],dl
    STORE_END dl

h_A0:
    mov di,si
    lea si,[si+1]
    mov cl,[di]
    inc cl
    dec cl
    NEXT

h_A1:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    mov di,[di]
    mov dl,[di]
    inc dl
    dec dl
    NEXT

h_A2:
    mov di,si
    lea si,[si+1]
    mov bl,[di]
    inc bl
    dec bl
    NEXT

h_A4:
    lodsb
    mov di,ax
    mov cl,[di]
    inc cl
    dec cl
    NEXT

h_A5:
    lodsb
    mov di,ax
    mov dl,[di]
    inc dl
    dec dl
    NEXT

h_A6:
    lodsb
    mov di,ax
    mov bl,[di]
    inc bl
    dec bl
    NEXT

h_A8:
    mov cl,dl
    inc cl
    dec cl
    NEXT

h_A9:
    mov di,si
    lea si,[si+1]
    mov dl,[di]
    inc dl
    dec dl
    NEXT

h_AA:
    mov bl,dl
    inc bl
    dec bl
    NEXT

h_AC:
    mov di,[si]
    lea si,[si+2]
    mov cl,[di]
    inc cl
    dec cl
    NEXT

h_AD:
    mov di,[si]
    lea si,[si+2]
    mov dl,[di]
    inc dl
    dec dl
    NEXT

h_AE:
    mov di,[si]
    lea si,[si+2]
    mov bl,[di]
    inc bl
    dec bl
    NEXT

h_B0:
    lodsb
    jnc .nt
    cbw
    mov bp,ax
    mov ah,0
    lea si,[si+bp]
    TICK
.nt:
    NEXT

h_B1:
    lodsb
    mov di,ax
    mov di,[di]
    mov bp,cx
    lea di,[bp+di]
    mov dl,[di]
    inc dl
    dec dl
    NEXT

h_B4:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    mov cl,[di]
    inc cl
    dec cl
    NEXT

h_B5:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    mov dl,[di]
    inc dl
    dec dl
    NEXT

h_B6:
    lodsb
    mov di,ax
    mov bp,cx
    lea di,[bp+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    mov bl,[di]
    inc bl
    dec bl
    NEXT

h_B8:
    mov byte [VS:fV],0
    NEXT

h_B9:
    mov di,[si]
    lea si,[si+2]
    mov bp,cx
    lea di,[bp+di]
    mov dl,[di]
    inc dl
    dec dl
    NEXT

h_BA:
    mov bl,[VS:S16]
    inc bl
    dec bl
    NEXT

h_BC:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    mov cl,[di]
    inc cl
    dec cl
    NEXT

h_BD:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    mov dl,[di]
    inc dl
    dec dl
    NEXT

h_BE:
    mov di,[si]
    lea si,[si+2]
    mov bp,cx
    lea di,[bp+di]
    mov bl,[di]
    inc bl
    dec bl
    NEXT

h_C0:
    mov di,si
    lea si,[si+1]
    cmp cl,[di]
    cmc
    NEXT

h_C1:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    mov di,[di]
    cmp dl,[di]
    cmc
    NEXT

h_C2:
    lea si,[si+1]
    NEXT

h_C4:
    lodsb
    mov di,ax
    cmp cl,[di]
    cmc
    NEXT

h_C5:
    lodsb
    mov di,ax
    cmp dl,[di]
    cmc
    NEXT

h_C6:
    lodsb
    mov di,ax
    dec byte [di]
    NEXT

h_C8:
    inc cl
    NEXT

h_C9:
    mov di,si
    lea si,[si+1]
    cmp dl,[di]
    cmc
    NEXT

h_CA:
    dec bl
    NEXT

h_CC:
    mov di,[si]
    lea si,[si+2]
    cmp cl,[di]
    cmc
    NEXT

h_CD:
    mov di,[si]
    lea si,[si+2]
    cmp dl,[di]
    cmc
    NEXT

h_CE:
    mov di,[si]
    lea si,[si+2]
    dec byte [di]
    NEXT

h_D0:
    lodsb
    jz .nt
    cbw
    mov bp,ax
    mov ah,0
    lea si,[si+bp]
    TICK
.nt:
    NEXT

h_D1:
    lodsb
    mov di,ax
    mov di,[di]
    mov bp,cx
    lea di,[bp+di]
    cmp dl,[di]
    cmc
    NEXT

h_D4:
    lea si,[si+1]
    NEXT

h_D5:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    cmp dl,[di]
    cmc
    NEXT

h_D6:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    dec byte [di]
    NEXT

h_D8:
    mov byte [VS:fD],0
    NEXT

h_D9:
    mov di,[si]
    lea si,[si+2]
    mov bp,cx
    lea di,[bp+di]
    cmp dl,[di]
    cmc
    NEXT

h_DA:
    NEXT

h_DC:
    lea si,[si+2]
    NEXT

h_DD:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    cmp dl,[di]
    cmc
    NEXT

h_DE:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    dec byte [di]
    NEXT

h_E0:
    mov di,si
    lea si,[si+1]
    cmp bl,[di]
    cmc
    NEXT

h_E1:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    mov di,[di]
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    cmc
    sbb dl,[di]
    cmc
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    cmc
    xchg al,dl
    sbb al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    das
    cmc
    xchg al,dl
    NEXT

h_E2:
    lea si,[si+1]
    NEXT

h_E4:
    lodsb
    mov di,ax
    cmp bl,[di]
    cmc
    NEXT

h_E5:
    lodsb
    mov di,ax
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    cmc
    sbb dl,[di]
    cmc
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    cmc
    xchg al,dl
    sbb al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    das
    cmc
    xchg al,dl
    NEXT

h_E6:
    lodsb
    mov di,ax
    inc byte [di]
    NEXT

h_E8:
    inc bl
    NEXT

h_E9:
    mov di,si
    lea si,[si+1]
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    cmc
    sbb dl,[di]
    cmc
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    cmc
    xchg al,dl
    sbb al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    das
    cmc
    xchg al,dl
    NEXT

h_EA:
    NEXT

h_EC:
    mov di,[si]
    lea si,[si+2]
    cmp bl,[di]
    cmc
    NEXT

h_ED:
    mov di,[si]
    lea si,[si+2]
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    cmc
    sbb dl,[di]
    cmc
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    cmc
    xchg al,dl
    sbb al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    das
    cmc
    xchg al,dl
    NEXT

h_EE:
    mov di,[si]
    lea si,[si+2]
    inc byte [di]
    NEXT

h_F0:
    lodsb
    jnz .nt
    cbw
    mov bp,ax
    mov ah,0
    lea si,[si+bp]
    TICK
.nt:
    NEXT

h_F1:
    lodsb
    mov di,ax
    mov di,[di]
    mov bp,cx
    lea di,[bp+di]
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    cmc
    sbb dl,[di]
    cmc
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    cmc
    xchg al,dl
    sbb al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    das
    cmc
    xchg al,dl
    NEXT

h_F4:
    lea si,[si+1]
    NEXT

h_F5:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    cmc
    sbb dl,[di]
    cmc
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    cmc
    xchg al,dl
    sbb al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    das
    cmc
    xchg al,dl
    NEXT

h_F6:
    lodsb
    mov di,ax
    lea di,[bx+di]
    mov bp,di
    mov al,[cs:bp+wrap]
    mov di,ax
    inc byte [di]
    NEXT

h_F8:
    mov byte [VS:fD],1
    NEXT

h_F9:
    mov di,[si]
    lea si,[si+2]
    mov bp,cx
    lea di,[bp+di]
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    cmc
    sbb dl,[di]
    cmc
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    cmc
    xchg al,dl
    sbb al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    das
    cmc
    xchg al,dl
    NEXT

h_FA:
    NEXT

h_FC:
    lea si,[si+2]
    NEXT

h_FD:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    sbb dh,dh
    cmp byte [VS:fD],0
    jne .d
    add dh,dh
    cmc
    sbb dl,[di]
    cmc
    jo .v1
    mov byte [VS:fV],0
    NEXT
.v1:
    mov byte [VS:fV],1
    NEXT
.d:
    add dh,dh
    cmc
    xchg al,dl
    sbb al,[di]
    jo .dv1
    mov byte [VS:fV],0
    jmp .dd
.dv1:
    mov byte [VS:fV],1
.dd:
    das
    cmc
    xchg al,dl
    NEXT

h_FE:
    mov di,[si]
    lea si,[si+2]
    lea di,[bx+di]
    inc byte [di]
    NEXT

optab:
    dw h_bad,h_01,h_bad,h_bad,h_04,h_05,h_06,h_bad
    dw h_08,h_09,h_0A,h_bad,h_0C,h_0D,h_0E,h_bad
    dw h_10,h_11,h_bad,h_bad,h_14,h_15,h_16,h_bad
    dw h_18,h_19,h_1A,h_bad,h_1C,h_1D,h_1E,h_bad
    dw h_20,h_21,h_bad,h_bad,h_24,h_25,h_26,h_bad
    dw h_28,h_29,h_2A,h_bad,h_2C,h_2D,h_2E,h_bad
    dw h_30,h_31,h_bad,h_bad,h_34,h_35,h_36,h_bad
    dw h_38,h_39,h_3A,h_bad,h_3C,h_3D,h_3E,h_bad
    dw h_40,h_41,h_bad,h_bad,h_44,h_45,h_46,h_bad
    dw h_48,h_49,h_4A,h_bad,h_4C,h_4D,h_4E,h_bad
    dw h_50,h_51,h_bad,h_bad,h_54,h_55,h_56,h_bad
    dw h_58,h_59,h_5A,h_bad,h_5C,h_5D,h_5E,h_bad
    dw h_60,h_61,h_bad,h_bad,h_64,h_65,h_66,h_bad
    dw h_68,h_69,h_6A,h_bad,h_6C,h_6D,h_6E,h_bad
    dw h_70,h_71,h_bad,h_bad,h_74,h_75,h_76,h_bad
    dw h_78,h_79,h_7A,h_bad,h_7C,h_7D,h_7E,h_bad
    dw h_80,h_81,h_82,h_bad,h_84,h_85,h_86,h_bad
    dw h_88,h_89,h_8A,h_bad,h_8C,h_8D,h_8E,h_bad
    dw h_90,h_91,h_bad,h_bad,h_94,h_95,h_96,h_bad
    dw h_98,h_99,h_9A,h_bad,h_bad,h_9D,h_bad,h_bad
    dw h_A0,h_A1,h_A2,h_bad,h_A4,h_A5,h_A6,h_bad
    dw h_A8,h_A9,h_AA,h_bad,h_AC,h_AD,h_AE,h_bad
    dw h_B0,h_B1,h_bad,h_bad,h_B4,h_B5,h_B6,h_bad
    dw h_B8,h_B9,h_BA,h_bad,h_BC,h_BD,h_BE,h_bad
    dw h_C0,h_C1,h_C2,h_bad,h_C4,h_C5,h_C6,h_bad
    dw h_C8,h_C9,h_CA,h_bad,h_CC,h_CD,h_CE,h_bad
    dw h_D0,h_D1,h_bad,h_bad,h_D4,h_D5,h_D6,h_bad
    dw h_D8,h_D9,h_DA,h_bad,h_DC,h_DD,h_DE,h_bad
    dw h_E0,h_E1,h_E2,h_bad,h_E4,h_E5,h_E6,h_bad
    dw h_E8,h_E9,h_EA,h_bad,h_EC,h_ED,h_EE,h_bad
    dw h_F0,h_F1,h_bad,h_bad,h_F4,h_F5,h_F6,h_bad
    dw h_F8,h_F9,h_FA,h_bad,h_FC,h_FD,h_FE,h_bad
wrap:
    db 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    db 16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    db 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    db 48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
    db 64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79
    db 80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95
    db 96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111
    db 112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
    db 128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143
    db 144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159
    db 160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175
    db 176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191
    db 192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207
    db 208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223
    db 224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239
    db 240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255
    db 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    db 16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    db 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    db 48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
    db 64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79
    db 80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95
    db 96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111
    db 112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
    db 128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143
    db 144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159
    db 160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175
    db 176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191
    db 192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207
    db 208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223
    db 224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239
    db 240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255

ticks dw 0
S16 dw 0
io_a dw 0
stp dw 0
stseg dw 0
fV db 0
fD db 0
fI db 0
io_v db 0
stat db 0
align 2
