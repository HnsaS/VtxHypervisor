;==============================================================================
; asm.asm — stubs em assembly x64 (MASM) para o hypervisor VT-x.
;
;   * AsmVmxLaunch       : grava GUEST_RSP/GUEST_RIP na VMCS e executa VMLAUNCH.
;   * AsmVmexitHandler   : ponto de entrada do host no VM-exit (HOST_RIP).
;   * AsmGet*            : leitura de segmentos, GDTR/IDTR, RFLAGS.
;
; Montar com ml64. No projeto do WDK marque este arquivo com o item build
; "Microsoft Macro Assembler".
;==============================================================================

OPTION CASEMAP:NONE

; encodings da VMCS usados aqui (precisam bater com arch.h)
VMCS_GUEST_RSP          EQU 0681Ch
VMCS_GUEST_RIP          EQU 0681Eh

EXTERN VmExitHandler:PROC
EXTERN VmResumeFailed:PROC

.CODE

;------------------------------------------------------------------------------
; BOOLEAN AsmVmxLaunch(void)
;
; Captura o RSP/RIP atuais como estado do guest e dispara VMLAUNCH.
;   - Em caso de FALHA (VMLAUNCH caiu), retorna FALSE (rax=0), ainda em root.
;   - Em caso de SUCESSO, o guest reinicia em _GuestResume com o mesmo RSP e
;     retorna TRUE (rax=1) ao chamador, agora em VMX non-root (guest).
;------------------------------------------------------------------------------
AsmVmxLaunch PROC
    mov     rdx, rsp            ; GUEST_RSP = rsp atual
    mov     rcx, VMCS_GUEST_RSP
    vmwrite rcx, rdx

    mov     rcx, VMCS_GUEST_RIP
    lea     rdx, _GuestResume   ; GUEST_RIP = _GuestResume
    vmwrite rcx, rdx

    vmlaunch

    ; ---- só chega aqui se o VMLAUNCH falhar ----
    xor     rax, rax            ; FALSE
    ret

_GuestResume:
    ; ---- guest começa a executar aqui (modo non-root) ----
    mov     rax, 1              ; TRUE
    ret
AsmVmxLaunch ENDP

;------------------------------------------------------------------------------
; VOID AsmVmexitHandler(void)   — HOST_RIP
;
; Salva todos os GPRs formando um GUEST_REGS na pilha, chama o handler C,
; e então VMRESUME (ou para, se o handler retornar FALSE).
;------------------------------------------------------------------------------
AsmVmexitHandler PROC
    push    r15
    push    r14
    push    r13
    push    r12
    push    r11
    push    r10
    push    r9
    push    r8
    push    rdi
    push    rsi
    push    rbp
    push    rbp                 ; placeholder p/ rsp (ignorado; usa VMCS)
    push    rbx
    push    rdx
    push    rcx
    push    rax

    mov     rcx, rsp            ; arg1 = PGUEST_REGS
    sub     rsp, 20h            ; shadow space
    call    VmExitHandler
    add     rsp, 20h

    test    al, al
    jz      _StopVmx

    pop     rax
    pop     rcx
    pop     rdx
    pop     rbx
    add     rsp, 8              ; descarta placeholder de rsp
    pop     rbp
    pop     rsi
    pop     rdi
    pop     r8
    pop     r9
    pop     r10
    pop     r11
    pop     r12
    pop     r13
    pop     r14
    pop     r15

    vmresume
    call    VmResumeFailed      ; só chega aqui se o vmresume falhar
    hlt

_StopVmx:
    pop     rax
    pop     rcx
    pop     rdx
    pop     rbx
    add     rsp, 8
    pop     rbp
    pop     rsi
    pop     rdi
    pop     r8
    pop     r9
    pop     r10
    pop     r11
    pop     r12
    pop     r13
    pop     r14
    pop     r15
    call    VmResumeFailed
    hlt
AsmVmexitHandler ENDP

;------------------------------------------------------------------------------
; leitura de registradores de segmento
;------------------------------------------------------------------------------
AsmGetCs PROC
    mov     ax, cs
    ret
AsmGetCs ENDP

AsmGetSs PROC
    mov     ax, ss
    ret
AsmGetSs ENDP

AsmGetDs PROC
    mov     ax, ds
    ret
AsmGetDs ENDP

AsmGetEs PROC
    mov     ax, es
    ret
AsmGetEs ENDP

AsmGetFs PROC
    mov     ax, fs
    ret
AsmGetFs ENDP

AsmGetGs PROC
    mov     ax, gs
    ret
AsmGetGs ENDP

AsmGetLdtr PROC
    sldt    ax
    ret
AsmGetLdtr ENDP

AsmGetTr PROC
    str     ax
    ret
AsmGetTr ENDP

;------------------------------------------------------------------------------
; GDTR / IDTR (pseudo-descritor = 2 bytes limit + 8 bytes base)
;------------------------------------------------------------------------------
AsmGetGdtBase PROC
    sub     rsp, 16
    sgdt    [rsp]
    mov     rax, [rsp + 2]
    add     rsp, 16
    ret
AsmGetGdtBase ENDP

AsmGetGdtLimit PROC
    sub     rsp, 16
    sgdt    [rsp]
    movzx   rax, WORD PTR [rsp]
    add     rsp, 16
    ret
AsmGetGdtLimit ENDP

AsmGetIdtBase PROC
    sub     rsp, 16
    sidt    [rsp]
    mov     rax, [rsp + 2]
    add     rsp, 16
    ret
AsmGetIdtBase ENDP

AsmGetIdtLimit PROC
    sub     rsp, 16
    sidt    [rsp]
    movzx   rax, WORD PTR [rsp]
    add     rsp, 16
    ret
AsmGetIdtLimit ENDP

AsmGetRflags PROC
    pushfq
    pop     rax
    ret
AsmGetRflags ENDP

END
