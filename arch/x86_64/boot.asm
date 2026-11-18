; arch/x86_64/boot.asm - E820 + 1GB identity + LBA load + CHS fallback (255H/63S)
; TigxOS 1.1.12 — arch split (boot layer)
[org 0x7c00]
[bits 16]

KERNEL_OFFSET  equ 0x10000
MEMMAP_COUNT   equ 0x9000
MEMMAP_ENTRIES equ 0x9004
MEMMAP_MAX     equ 64
KERNEL_SECTS   equ 320

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    mov [BOOT_DRIVE], dl
    call detect_memory
    call load_kernel
    call enable_a20
    call switch_to_pm

detect_memory:
    xor ebx, ebx
    mov di, MEMMAP_ENTRIES
    mov dword [MEMMAP_COUNT], 0
.e820_loop:
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150
    mov dword [di+20], 1
    int 0x15
    jc .e820_done
    cmp eax, 0x534D4150
    jne .e820_done
    mov ecx, [di+8]
    or ecx, [di+12]
    jz .skip
    add di, 24
    inc dword [MEMMAP_COUNT]
    cmp dword [MEMMAP_COUNT], MEMMAP_MAX
    jae .e820_done
.skip:
    test ebx, ebx
    jnz .e820_loop
.e820_done:
    ret

load_kernel:
    mov word [DAP_OFF], 0
    mov word [DAP_SEG], KERNEL_OFFSET >> 4
    mov dword [DAP_LBA], 1
    mov dword [DAP_LBA+4], 0
    mov cx, KERNEL_SECTS
.lba_loop:
    test cx, cx
    jz .ok
    mov ax, 64
    cmp cx, ax
    jae .chunk
    mov ax, cx
.chunk:
    mov [DAP_SECTS], ax
    push cx
    push ax
    mov si, DAP
    mov ah, 0x42
    mov dl, [BOOT_DRIVE]
    int 0x13
    pop ax
    pop cx
    jc .try_chs
    sub cx, ax
    movzx eax, ax
    add dword [DAP_LBA], eax
    adc dword [DAP_LBA+4], 0
    shl ax, 5
    add [DAP_SEG], ax
    jmp .lba_loop

; CHS: LBA→CHS via 255 heads / 63 spt (common BIOS translation)
.try_chs:
    mov ax, KERNEL_OFFSET >> 4
    mov es, ax
    xor bx, bx
    mov si, KERNEL_SECTS
    mov di, 1
.chs_one:
    test si, si
    jz .ok
    mov ax, di
    xor dx, dx
    mov cx, 63
    div cx
    mov cl, dl
    inc cl
    xor dx, dx
    mov bx, 255
    div bx
    mov ch, al
    mov dh, dl
    shr ax, 2
    and al, 0xC0
    or cl, al
    xor bx, bx
    mov dl, [BOOT_DRIVE]
    mov ax, 0x0201
    int 0x13
    jc disk_error
    mov ax, es
    add ax, 32
    mov es, ax
    inc di
    dec si
    jmp .chs_one
.ok:
    ret

disk_error:
    int 0x19
    jmp $

enable_a20:
    in al, 0x92
    test al, 2
    jnz .done
    or al, 2
    out 0x92, al
.done:
    ret

gdt_start:
    dq 0
gdt_code32:
    dw 0xffff, 0
    db 0, 10011010b, 11001111b, 0
gdt_data:
    dw 0xffff, 0
    db 0, 10010010b, 11001111b, 0
gdt_code64:
    dw 0, 0
    db 0, 10011000b, 00100000b, 0
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE32_SEG equ gdt_code32 - gdt_start
DATA_SEG   equ gdt_data - gdt_start
CODE64_SEG equ gdt_code64 - gdt_start

switch_to_pm:
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp CODE32_SEG:init_pm

[bits 32]
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov esp, 0x90000
    mov edi, 0x1000
    mov cr3, edi
    xor eax, eax
    mov ecx, 0x1000
    rep stosd
    mov edi, 0x1000
    mov dword [edi], 0x2007          ; P+RW+U → PDPT
    mov dword [edi + 0x1000], 0x3007 ; P+RW+U → PD
    mov edi, 0x3000
    mov eax, 0x00000087              ; P+RW+U+PS (2MB, user-accessible)
    mov ecx, 512
.map:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .map
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11)   ; LME + NXE (no-execute page bit)
    wrmsr
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    jmp CODE64_SEG:long_mode_start

[bits 64]
long_mode_start:
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov rsp, 0x90000
    mov rax, KERNEL_OFFSET
    jmp rax

BOOT_DRIVE db 0

DAP:
    db 0x10, 0
DAP_SECTS: dw 0
DAP_OFF:   dw 0
DAP_SEG:   dw 0
DAP_LBA:   dq 0

times 510-($-$$) db 0
dw 0xaa55
