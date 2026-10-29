/*
 * vmx.h — estruturas por-vCPU, layout dos registradores salvos no VM-exit,
 *         e protótipos (C + assembly).
 */
#pragma once

#include <ntddk.h>
#include "arch.h"

#define VMM_STACK_SIZE   (0x8000)   /* 32 KiB de pilha para o host/VMM       */
#define VMM_POOL_TAG     'xmvH'     /* "Hvmx" */

/*
 * GUEST_REGS — espelho dos GPRs do guest no momento do VM-exit.
 * A ORDEM dos campos tem que bater exatamente com a ordem dos PUSH no
 * AsmVmexitHandler (asm.asm). rax fica no offset 0 (topo da pilha após os pushes).
 */
typedef struct _GUEST_REGS
{
    UINT64 rax;
    UINT64 rcx;
    UINT64 rdx;
    UINT64 rbx;
    UINT64 rsp;   /* valor ignorado (o real vem da VMCS), mantido p/ alinhamento */
    UINT64 rbp;
    UINT64 rsi;
    UINT64 rdi;
    UINT64 r8;
    UINT64 r9;
    UINT64 r10;
    UINT64 r11;
    UINT64 r12;
    UINT64 r13;
    UINT64 r14;
    UINT64 r15;
} GUEST_REGS, *PGUEST_REGS;

/*
 * VCPU — estado de virtualização de um processador lógico.
 */
typedef struct _VCPU
{
    PVOID  VmxonRegion;          /* VA da região VMXON                       */
    UINT64 VmxonRegionPhys;      /* PA da região VMXON                       */

    PVOID  VmcsRegion;           /* VA da VMCS                               */
    UINT64 VmcsRegionPhys;       /* PA da VMCS                               */

    PVOID  VmmStack;             /* base da pilha do host (VMM)              */
    PVOID  MsrBitmap;            /* VA do MSR bitmap                         */
    UINT64 MsrBitmapPhys;        /* PA do MSR bitmap                         */
    UINT64 OriginalCr0;
    UINT64 OriginalCr4;

    BOOLEAN ControlRegistersAdjusted;
    BOOLEAN VmxOnExecuted;       /* VMXON já executado neste core?           */
    BOOLEAN LaunchedOk;          /* VMLAUNCH bem-sucedido?                   */
} VCPU, *PVCPU;

/* estado global do driver */
typedef struct _HV_GLOBALS
{
    ULONG   ProcessorCount;
    PVCPU   VcpuArray;           /* um VCPU por processador lógico           */
    ULONG   FailedProcessorIndex;
    UINT64  LastVmxInstructionError;
} HV_GLOBALS, *PHV_GLOBALS;

extern HV_GLOBALS g_Hv;
extern UINT64 g_VmxStopGuestRip;
extern UINT64 g_VmxStopGuestRsp;
extern UINT64 g_VmxStopGuestRflags;

/* ---- C: vmx.c ---- */
BOOLEAN VmxIsSupported(void);
NTSTATUS VmxInitializeAllProcessors(void);
VOID     VmxShutdownAllProcessors(void);

/* ---- C: vmexit.c ---- */
/* 0 = falha, 1 = VMRESUME, 2 = VMXOFF concluído e contexto restaurado. */
UINT8   VmExitHandler(PGUEST_REGS regs);
VOID    VmResumeFailed(void);

/* ---- asm.asm ---- */
BOOLEAN AsmVmxLaunch(void);          /* seta GUEST_RSP/RIP e faz VMLAUNCH    */
VOID    AsmVmexitHandler(void);      /* host RIP (ponto de entrada do exit)  */

UINT16  AsmGetCs(void);
UINT16  AsmGetSs(void);
UINT16  AsmGetDs(void);
UINT16  AsmGetEs(void);
UINT16  AsmGetFs(void);
UINT16  AsmGetGs(void);
UINT16  AsmGetLdtr(void);
UINT16  AsmGetTr(void);
UINT64  AsmGetGdtBase(void);
UINT16  AsmGetGdtLimit(void);
UINT64  AsmGetIdtBase(void);
UINT16  AsmGetIdtLimit(void);
UINT64  AsmGetRflags(void);
