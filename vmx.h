/*
 * vmx.h — estruturas por-vCPU, layout dos registradores salvos no VM-exit,
 *         e protótipos (C + assembly).
 */
#pragma once

#include <ntddk.h>
#include "arch.h"

#define VMM_STACK_SIZE   (0x8000)   /* 32 KiB de pilha para o host/VMM       */
#define VMM_STACK_HOST_RSP_OFFSET 0x100
#define VMM_STACK_VCPU_POINTER_OFFSET 0x08
#define VMM_POOL_TAG     'xmvH'     /* "Hvmx" */

#define VMEXIT_RESULT_FATAL   0
#define VMEXIT_RESULT_RESUME  1
#define VMEXIT_RESULT_VMXOFF  2

typedef enum _HV_PROGRESS_STAGE
{
    HvProgressDriverEntry = 1,
    HvProgressCpuBegin,
    HvProgressVmxonComplete,
    HvProgressVmcsReady,
    HvProgressVmlaunchBegin,
    HvProgressVmlaunchReturned,
    HvProgressCpuComplete,
    HvProgressInitializationFailed,
    HvProgressInitializationComplete,
    HvProgressVmwriteRflagsBegin,
    HvProgressVmwriteRflagsDone,
    HvProgressVmwriteRspBegin,
    HvProgressVmwriteRspDone,
    HvProgressVmwriteRipBegin,
    HvProgressVmwriteRipDone,
    HvProgressVmlaunchInstruction,
    HvProgressVmwriteRflagsFailed,
    HvProgressVmwriteRspFailed,
    HvProgressVmwriteRipFailed
} HV_PROGRESS_STAGE;
C_ASSERT(HvProgressVmwriteRflagsBegin == 10);
C_ASSERT(HvProgressVmlaunchInstruction == 16);
C_ASSERT(HvProgressVmwriteRipFailed == 19);

/* Volatile breadcrumbs used by the launch watchdog and assembly stubs. */
typedef enum _HV_LAUNCH_TRACE_STAGE
{
    HvLaunchTraceIdle = 0,
    HvLaunchTraceVmlaunchBegin = 1,
    HvLaunchTraceGuestTrampoline = 2,
    HvLaunchTraceVmexitEntry = 3,
    HvLaunchTraceVmexitHandler = 4,
    HvLaunchTraceVmresume = 5,
    HvLaunchTraceVmlaunchReturned = 6,
    HvLaunchTraceVmexitRegsSaved = 7,
    HvLaunchTraceVmexitSimdSaved = 8,
    HvLaunchTraceVmexitCallC = 9,
    HvLaunchTraceVmexitReturnedC = 10
} HV_LAUNCH_TRACE_STAGE;

VOID HvProgressOpen(PUNICODE_STRING serviceRegistryPath);
VOID HvProgressWrite(ULONG processorIndex, HV_PROGRESS_STAGE stage, NTSTATUS status);
VOID HvProgressWriteLaunchStage(ULONG stage);
VOID HvProgressClose(VOID);

extern volatile LONG g_VmxProgressStage;
extern volatile LONG g_VmxProgressProcessor;
extern volatile LONG g_VmxVmexitCount;
extern volatile LONG g_VmxLaunchInProgress;
extern volatile LONG64 g_VmxLaunchStartTime;
VOID HvLaunchWatchdogStart(ULONG launchingProcessorIndex);
VOID HvLaunchWatchdogStop(VOID);

typedef enum _VMX_FAILURE_STAGE
{
    VmxFailureNone = 0,
    VmxFailureControlRegisters,
    VmxFailureVmxon,
    VmxFailureVmclear,
    VmxFailureVmptrld,
    VmxFailureVmcsWrite,
    VmxFailureVmlaunch,
    VmxFailureVmexitRead,
    VmxFailureVmexitHandler,
    VmxFailureVmresume,
    VmxFailureAllocation,
    VmxFailureControlConfiguration,
    VmxFailureProcessorSupport,
    VmxFailurePinControlConfiguration,
    VmxFailurePrimaryControlConfiguration,
    VmxFailureExitControlConfiguration,
    VmxFailureEntryControlConfiguration,
    VmxFailureVmEntryFailure,
    VmxFailureSecondaryControlConfiguration
} VMX_FAILURE_STAGE;

typedef enum _VMEXIT_FAILURE_CODE
{
    VmexitFailureNone = 0,
    VmexitFailureReadExitReason,
    VmexitFailureReadExitState,
    VmexitFailureUnknownReason,
    VmexitFailureReadQualification,
    VmexitFailureCrOperation,
    VmexitFailureAdvanceRip,
    VmexitFailureVmxoffState,
    VmexitFailureVmEntry
} VMEXIT_FAILURE_CODE;

typedef struct _VMEXIT_DIAGNOSTICS
{
    volatile LONG Sequence; /* odd = write in progress; even = stable snapshot */
    ULONG ProcessorIndex;
    UINT64 ExitReason;
    UINT64 ExitQualification;
    UINT64 GuestRip;
    UINT64 GuestRsp;
    UINT64 InstructionLength;
    UINT64 VmInstructionError;
    INT32  VmInstructionErrorReadStatus;
    UINT32 HandlerResult;
    UINT32 FailureCode;
    UINT32 FailureStage;
    UINT32 FailedVmcsField;
    INT32  VmxOperationStatus;
    UINT64 FailureDetail;
} VMEXIT_DIAGNOSTICS, *PVMEXIT_DIAGNOSTICS;

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
C_ASSERT(sizeof(GUEST_REGS) == 0x80);
C_ASSERT(FIELD_OFFSET(GUEST_REGS, r15) == 0x78);
C_ASSERT((VMM_STACK_HOST_RSP_OFFSET & 0xF) == 0);

/*
 * VCPU — estado de virtualização de um processador lógico.
 */
typedef struct _VCPU
{
    PVOID  VmxonRegion;          /* VA da região VMXON                       */
    UINT64 VmxonRegionPhys;      /* PA da região VMXON                       */

    PVOID  VmcsRegion;           /* VA da VMCS                               */
    UINT64 VmcsRegionPhys;       /* PA da VMCS                               */

    PVOID  MsrBitmap;            /* bitmap zerado para acessos MSR nativos   */
    UINT64 MsrBitmapPhys;

    PVOID  VmmStack;             /* base da pilha do host (VMM)              */
    UINT64 OriginalCr0;
    UINT64 OriginalCr4;
    UINT32 SecondaryControls;

    VMEXIT_DIAGNOSTICS Diagnostics;

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
UINT8   VmExitHandler(PGUEST_REGS regs, PVCPU vcpu);
VOID    VmResumeFailed(PVCPU vcpu);
VOID    VmExitFatal(PVCPU vcpu);

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
