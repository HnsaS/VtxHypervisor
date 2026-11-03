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
VMCS_GUEST_RFLAGS       EQU 06820h
HV_PROGRESS_VMWRITE_RFLAGS_BEGIN EQU 10
HV_PROGRESS_VMWRITE_RFLAGS_DONE  EQU 11
HV_PROGRESS_VMWRITE_RSP_BEGIN    EQU 12
HV_PROGRESS_VMWRITE_RSP_DONE     EQU 13
HV_PROGRESS_VMWRITE_RIP_BEGIN    EQU 14
HV_PROGRESS_VMWRITE_RIP_DONE     EQU 15
HV_PROGRESS_VMLAUNCH_INSTRUCTION EQU 16
HV_PROGRESS_VMWRITE_RFLAGS_FAILED EQU 17
HV_PROGRESS_VMWRITE_RSP_FAILED    EQU 18
HV_PROGRESS_VMWRITE_RIP_FAILED    EQU 19
VMM_VCPU_POINTER_OFFSET EQU 8
VMEXIT_GUEST_REGS_SIZE  EQU 080h
VMEXIT_SAVED_SIMD_SIZE  EQU 070h
VMEXIT_VCPU_PTR_OFFSET  EQU VMEXIT_GUEST_REGS_SIZE + VMEXIT_SAVED_SIMD_SIZE + VMM_VCPU_POINTER_OFFSET
VCPU_RUNTIME_VMEXIT_ACTIVE_OFFSET EQU 0A8h
VCPU_RUNTIME_VMEXIT_STAGE_OFFSET  EQU 0ACh
VCPU_RUNTIME_VMEXIT_START_OFFSET  EQU 0B0h

EXTERN VmExitHandler:PROC
EXTERN VmResumeFailed:PROC
EXTERN VmExitFatal:PROC
EXTERN HvProgressWriteLaunchStage:PROC
EXTERN g_VmxProgressStage:DWORD
EXTERN g_VmxVmexitCount:DWORD
EXTERN g_VmxStopGuestRip:QWORD
EXTERN g_VmxStopGuestRsp:QWORD
EXTERN g_VmxStopGuestRflags:QWORD

WRITE_LAUNCH_PROGRESS MACRO stage
    mov     ecx, stage
    sub     rsp, 28h            ; shadow space + 16-byte call-site alignment
    call    HvProgressWriteLaunchStage
    add     rsp, 28h
ENDM

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
    WRITE_LAUNCH_PROGRESS HV_PROGRESS_VMWRITE_RFLAGS_BEGIN
    pushfq
    pop     rax
    mov     rcx, VMCS_GUEST_RFLAGS
    vmwrite rcx, rax
    jbe     _VmxWriteRflagsFailed ; VMfailValid/Invalid: return to C safely
    WRITE_LAUNCH_PROGRESS HV_PROGRESS_VMWRITE_RFLAGS_DONE

    WRITE_LAUNCH_PROGRESS HV_PROGRESS_VMWRITE_RSP_BEGIN
    mov     rdx, rsp            ; GUEST_RSP = rsp atual
    mov     rcx, VMCS_GUEST_RSP
    vmwrite rcx, rdx
    jbe     _VmxWriteRspFailed
    WRITE_LAUNCH_PROGRESS HV_PROGRESS_VMWRITE_RSP_DONE

    WRITE_LAUNCH_PROGRESS HV_PROGRESS_VMWRITE_RIP_BEGIN
    mov     rcx, VMCS_GUEST_RIP
    lea     rdx, _GuestResume   ; GUEST_RIP = _GuestResume
    vmwrite rcx, rdx
    jbe     _VmxWriteRipFailed
    WRITE_LAUNCH_PROGRESS HV_PROGRESS_VMWRITE_RIP_DONE

    WRITE_LAUNCH_PROGRESS HV_PROGRESS_VMLAUNCH_INSTRUCTION

    vmlaunch

    ; ---- só chega aqui se o VMLAUNCH falhar ----
_VmxLaunchPrepFailed:
    xor     rax, rax            ; FALSE
    ret

_VmxWriteRflagsFailed:
    WRITE_LAUNCH_PROGRESS HV_PROGRESS_VMWRITE_RFLAGS_FAILED
    jmp     _VmxLaunchPrepFailed
_VmxWriteRspFailed:
    WRITE_LAUNCH_PROGRESS HV_PROGRESS_VMWRITE_RSP_FAILED
    jmp     _VmxLaunchPrepFailed
_VmxWriteRipFailed:
    WRITE_LAUNCH_PROGRESS HV_PROGRESS_VMWRITE_RIP_FAILED
    jmp     _VmxLaunchPrepFailed

_GuestResume:
    ; ---- guest começa a executar aqui (modo non-root) ----
    ; Keep this path minimal. Registry I/O here would trigger VM-exits while
    ; testing the very first guest instructions and obscure the failure point.
    mov     DWORD PTR [g_VmxProgressStage], 2 ; HvLaunchTraceGuestTrampoline
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
    cli                         ; VM-exit clears IF architecturally; enforce it here too.
    ; Mark this VCPU before saving state, so the runtime watchdog can detect
    ; a stall in the assembly entry path as well as in the C handler.
    push    rax
    push    rcx
    push    rdx
    mov     rax, QWORD PTR [rsp+20h] ; original host RSP + pointer offset 8
    mov     DWORD PTR [rax+VCPU_RUNTIME_VMEXIT_STAGE_OFFSET], 3
    mov     QWORD PTR [rax+VCPU_RUNTIME_VMEXIT_START_OFFSET], 0
    mov     DWORD PTR [rax+VCPU_RUNTIME_VMEXIT_ACTIVE_OFFSET], 1
    pop     rdx
    pop     rcx
    pop     rax
    lock inc DWORD PTR [g_VmxVmexitCount]
    mov     DWORD PTR [g_VmxProgressStage], 3 ; HvLaunchTraceVmexitEntry
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
    mov     rax, QWORD PTR [rsp+VMEXIT_GUEST_REGS_SIZE+VMM_VCPU_POINTER_OFFSET]
    mov     DWORD PTR [rax+VCPU_RUNTIME_VMEXIT_STAGE_OFFSET], 7
    mov     DWORD PTR [g_VmxProgressStage], 7 ; all GPRs saved

    ; Preserve volatile SIMD state because the guest can be interrupted
    ; between arbitrary instructions and the C handler may use XMM0-XMM5.
    sub     rsp, VMEXIT_SAVED_SIMD_SIZE
    movdqu  XMMWORD PTR [rsp+00h], xmm0
    movdqu  XMMWORD PTR [rsp+10h], xmm1
    movdqu  XMMWORD PTR [rsp+20h], xmm2
    movdqu  XMMWORD PTR [rsp+30h], xmm3
    movdqu  XMMWORD PTR [rsp+40h], xmm4
    movdqu  XMMWORD PTR [rsp+50h], xmm5
    stmxcsr DWORD PTR [rsp+60h]
    mov     rax, QWORD PTR [rsp+VMEXIT_VCPU_PTR_OFFSET]
    mov     DWORD PTR [rax+VCPU_RUNTIME_VMEXIT_STAGE_OFFSET], 8
    mov     DWORD PTR [g_VmxProgressStage], 8 ; SIMD state saved

    lea     rcx, [rsp+VMEXIT_SAVED_SIMD_SIZE] ; arg1 = PGUEST_REGS
    mov     rdx, QWORD PTR [rsp+VMEXIT_VCPU_PTR_OFFSET] ; arg2 = PVCPU
    mov     DWORD PTR [rdx+VCPU_RUNTIME_VMEXIT_STAGE_OFFSET], 9
    mov     DWORD PTR [g_VmxProgressStage], 9 ; immediately before C handler
    sub     rsp, 20h            ; shadow space
    call    VmExitHandler
    add     rsp, 20h
    mov     rax, QWORD PTR [rsp+VMEXIT_VCPU_PTR_OFFSET]
    mov     DWORD PTR [rax+VCPU_RUNTIME_VMEXIT_STAGE_OFFSET], 10
    mov     DWORD PTR [g_VmxProgressStage], 10 ; C handler returned

    cmp     al, 2
    je      _LeaveVmx

    movdqu  xmm0, XMMWORD PTR [rsp+00h]
    movdqu  xmm1, XMMWORD PTR [rsp+10h]
    movdqu  xmm2, XMMWORD PTR [rsp+20h]
    movdqu  xmm3, XMMWORD PTR [rsp+30h]
    movdqu  xmm4, XMMWORD PTR [rsp+40h]
    movdqu  xmm5, XMMWORD PTR [rsp+50h]
    ldmxcsr DWORD PTR [rsp+60h]
    add     rsp, VMEXIT_SAVED_SIMD_SIZE
    test    al, al
    jz      _StopVmx

    ; The C/restore path completed. Clear the watchdog marker immediately
    ; before VMRESUME so normal guest execution is never mistaken for a hang.
    mov     rax, QWORD PTR [rsp+VMEXIT_GUEST_REGS_SIZE+VMM_VCPU_POINTER_OFFSET]
    mov     DWORD PTR [rax+VCPU_RUNTIME_VMEXIT_STAGE_OFFSET], 5
    mov     DWORD PTR [rax+VCPU_RUNTIME_VMEXIT_ACTIVE_OFFSET], 0

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

    mov     DWORD PTR [g_VmxProgressStage], 5 ; HvLaunchTraceVmresume
    vmresume
    mov     rcx, QWORD PTR [rsp+VMM_VCPU_POINTER_OFFSET]
    sub     rsp, 20h            ; shadow space da ABI x64
    call    VmResumeFailed      ; só chega aqui se o vmresume falhar
    add     rsp, 20h
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
    mov     rcx, QWORD PTR [rsp+VMM_VCPU_POINTER_OFFSET]
    sub     rsp, 20h            ; shadow space da ABI x64
    call    VmExitFatal
    add     rsp, 20h
    hlt

_LeaveVmx:
    cli
    mov     rax, QWORD PTR [rsp+VMEXIT_VCPU_PTR_OFFSET]
    mov     DWORD PTR [rax+VCPU_RUNTIME_VMEXIT_STAGE_OFFSET], 11
    mov     DWORD PTR [rax+VCPU_RUNTIME_VMEXIT_ACTIVE_OFFSET], 0
    movdqu  xmm0, XMMWORD PTR [rsp+00h]
    movdqu  xmm1, XMMWORD PTR [rsp+10h]
    movdqu  xmm2, XMMWORD PTR [rsp+20h]
    movdqu  xmm3, XMMWORD PTR [rsp+30h]
    movdqu  xmm4, XMMWORD PTR [rsp+40h]
    movdqu  xmm5, XMMWORD PTR [rsp+50h]
    ldmxcsr DWORD PTR [rsp+60h]
    add     rsp, VMEXIT_SAVED_SIMD_SIZE
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
    mov     rsp, QWORD PTR [g_VmxStopGuestRsp]
    push    QWORD PTR [g_VmxStopGuestRflags]
    popfq
    jmp     QWORD PTR [g_VmxStopGuestRip]
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
