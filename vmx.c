/*
 * vmx.c — inicialização do VMX, montagem da VMCS e VMLAUNCH.
 *
 * Fluxo por processador lógico:
 *   1. Checa suporte a VMX (CPUID.1:ECX[5]) e trava IA32_FEATURE_CONTROL.
 *   2. Liga CR4.VMXE, ajusta CR0/CR4 pelos MSRs FIXED, e faz VMXON.
 *   3. VMCLEAR + VMPTRLD da VMCS.
 *   4. Preenche guest-state, host-state e campos de controle.
 *   5. AsmVmxLaunch() -> VMLAUNCH. O guest continua o próprio SO virtualizado.
 */

#include "vmx.h"
#include <intrin.h>

HV_GLOBALS g_Hv = { 0 };

volatile LONG g_VmxProgressStage = HvLaunchTraceIdle;
volatile LONG g_VmxProgressProcessor = -1;
volatile LONG g_VmxVmexitCount = 0;
volatile LONG g_VmxLaunchInProgress = 0;
DECLSPEC_ALIGN(8) volatile LONG64 g_VmxLaunchStartTime = 0;

/* ----- descritor de segmento (GDT) ------------------------------------- */
#pragma pack(push, 1)
typedef struct _SEG_DESCRIPTOR
{
    UINT16 LimitLow;
    UINT16 BaseLow;
    UINT8  BaseMid;
    UINT8  Access;      /* P | DPL | S | Type                               */
    UINT8  Granularity; /* G | D/B | L | AVL | LimitHigh[3:0]               */
    UINT8  BaseHigh;
} SEG_DESCRIPTOR, *PSEG_DESCRIPTOR;
#pragma pack(pop)

/* ---------------------------------------------------------------------------
 * Ajusta os controles conforme IA32_VMX_*_CTLS: se um bit da metade baixa
 * está em 1, o controle correspondente não pode ser 0; a metade alta indica
 * quais controles podem ser 1.
 * ------------------------------------------------------------------------- */
static UINT32 VmxAdjustControls(UINT32 desired, UINT32 capabilityMsr)
{
    UINT64 capability = __readmsr(capabilityMsr);
    UINT32 mustBeOne = (UINT32)capability;
    UINT32 allowedOne = (UINT32)(capability >> 32);

    desired |= mustBeOne;      /* bits 1 na metade baixa precisam ficar em 1 */
    desired &= allowedOne;     /* remove bits que não podem ser 1            */
    return desired;
}

/* Use the true-control MSRs when the processor reports support for them. */
static UINT32 VmxGetControlMsr(UINT32 legacyMsr, UINT32 trueMsr)
{
    UINT64 vmxBasic = __readmsr(MSR_IA32_VMX_BASIC);

    return (vmxBasic & (1ULL << 55)) ? trueMsr : legacyMsr;
}

/* ---------------------------------------------------------------------------
 * Converte um seletor em access-rights (formato VMX) e base linear.
 * ------------------------------------------------------------------------- */
static UINT32 VmxGetSegmentAccessRights(UINT16 selector)
{
    UINT32 ar;
    PSEG_DESCRIPTOR desc;
    UINT64 gdtBase;

    if ((selector & ~0x7) == 0)
        return 0x10000;                 /* seletor nulo -> unusable         */

    gdtBase = AsmGetGdtBase();
    desc = (PSEG_DESCRIPTOR)(gdtBase + (selector & ~0x7));

    ar  = desc->Access;                 /* Type|S|DPL|P  -> bits 0..7        */
    ar |= ((UINT32)(desc->Granularity >> 4) & 0x0F) << 12; /* AVL|L|DB|G     */
    if ((desc->Access & 0x80) == 0)     /* P == 0                           */
        ar |= 0x10000;                  /* unusable                          */

    return ar;
}

static UINT64 VmxGetSegmentBase(UINT16 selector)
{
    PSEG_DESCRIPTOR desc;
    UINT64 gdtBase, base;

    if ((selector & ~0x7) == 0)
        return 0;

    gdtBase = AsmGetGdtBase();
    desc = (PSEG_DESCRIPTOR)(gdtBase + (selector & ~0x7));

    base = ((UINT64)desc->BaseLow) |
           ((UINT64)desc->BaseMid << 16) |
           ((UINT64)desc->BaseHigh << 24);

    /* Descritores de sistema (LDT/TSS, S==0) têm base de 64 bits. */
    if ((desc->Access & 0x10) == 0)
        base |= (UINT64)(*(UINT32*)((UINT8*)desc + 8)) << 32;

    return base;
}

/* ---------------------------------------------------------------------------
 * Alocação de página física contígua (4 KiB) zerada.
 * ------------------------------------------------------------------------- */
static PVOID VmxAllocContiguousPage(PUINT64 physOut)
{
    PHYSICAL_ADDRESS max;
    PVOID va;
    /* Keep VMX regions below 4 GiB for CPUs with a 32-bit VMX region limit. */
    max.QuadPart = 0xFFFFFFFFULL;
    va = MmAllocateContiguousMemory(PAGE_SIZE_4K, max);
    if (va)
    {
        RtlZeroMemory(va, PAGE_SIZE_4K);
        *physOut = MmGetPhysicalAddress(va).QuadPart;
    }
    return va;
}

/* ---------------------------------------------------------------------------
 * VmxIsSupported — CPUID.1:ECX[5] (VMX) e checagem do IA32_FEATURE_CONTROL.
 * ------------------------------------------------------------------------- */
BOOLEAN VmxIsSupported(void)
{
    int regs[4];
    UINT64 featureControl;

    __cpuid(regs, 1);
    if ((regs[2] & (1 << 5)) == 0)          /* ECX bit 5 = VMX              */
        return FALSE;
    /* Do not attempt to claim VMX when Windows is already under a hypervisor
       (Hyper-V/VBS or a parent VM). This project has no nested-VMX support. */
    if ((regs[2] & (1U << 31)) != 0)
        return FALSE;

    /* These allocations are cached/WB. Refuse a VMX format this allocator
       cannot satisfy, before changing CR0/CR4 or entering VMX operation. */
    {
        UINT64 vmxBasic = __readmsr(MSR_IA32_VMX_BASIC);
        UINT32 vmcsSize = (UINT32)((vmxBasic >> 32) & 0x1FFF);
        UINT32 vmcsMemoryType = (UINT32)((vmxBasic >> 50) & 0xF);
        if (vmcsSize > PAGE_SIZE_4K || vmcsMemoryType != 6)
            return FALSE;
    }

    featureControl = __readmsr(MSR_IA32_FEATURE_CONTROL);
    if (featureControl & FEATURE_CONTROL_LOCK)
    {
        /* Já travado: só serve se VMXON-fora-de-SMX estiver habilitado.    */
        if ((featureControl & FEATURE_CONTROL_VMXON_OUTSIDE_SMX) == 0)
            return FALSE;
    }
    else
    {
        /* Destravado: habilita e trava.                                    */
        featureControl |= FEATURE_CONTROL_LOCK | FEATURE_CONTROL_VMXON_OUTSIDE_SMX;
        __writemsr(MSR_IA32_FEATURE_CONTROL, featureControl);
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * Força CR0/CR4 a satisfazer os bits fixos exigidos pelo VMX.
 * ------------------------------------------------------------------------- */
static void VmxAdjustControlRegisters(PVCPU vcpu)
{
    UINT64 cr0 = __readcr0();
    UINT64 cr4 = __readcr4();

    vcpu->OriginalCr0 = cr0;
    vcpu->OriginalCr4 = cr4;
    vcpu->ControlRegistersAdjusted = TRUE;

    cr0 |= __readmsr(MSR_IA32_VMX_CR0_FIXED0);
    cr0 &= __readmsr(MSR_IA32_VMX_CR0_FIXED1);
    __writecr0(cr0);

    cr4 |= __readmsr(MSR_IA32_VMX_CR4_FIXED0);
    cr4 &= __readmsr(MSR_IA32_VMX_CR4_FIXED1);
    __writecr4(cr4 | CR4_VMXE);     /* garante VMXE ligado                  */
}

/* Roll back on the same processor before returning to ordinary kernel code. */
static void VmxRestoreControlRegisters(PVCPU vcpu)
{
    if (!vcpu->ControlRegistersAdjusted)
        return;

    __writecr4(vcpu->OriginalCr4);
    __writecr0(vcpu->OriginalCr0);
    vcpu->ControlRegistersAdjusted = FALSE;
}

/* ---------------------------------------------------------------------------
 * VmxEnterRoot — prepara os registradores de controle e executa VMXON.
 * ------------------------------------------------------------------------- */
static NTSTATUS VmxEnterRoot(PVCPU vcpu)
{
    UINT64 vmxBasic;

    /* VMX capabilities and FEATURE_CONTROL are processor-local. */
    if (!VmxIsSupported())
    {
        vcpu->Diagnostics.FailureStage = VmxFailureProcessorSupport;
        vcpu->Diagnostics.VmxOperationStatus = STATUS_NOT_SUPPORTED;
        vcpu->Diagnostics.FailureDetail = 1; /* processor-local VMX support check */
        return STATUS_NOT_SUPPORTED;
    }

    VmxAdjustControlRegisters(vcpu);

    /* revision identifier (bits 30:0 do IA32_VMX_BASIC) no início da região */
    vmxBasic = __readmsr(MSR_IA32_VMX_BASIC);
    *(UINT32*)vcpu->VmxonRegion = (UINT32)(vmxBasic & 0x7FFFFFFF);

    if (__vmx_on(&vcpu->VmxonRegionPhys) != 0)
    {
        vcpu->Diagnostics.FailureStage = VmxFailureVmxon;
        vcpu->Diagnostics.VmxOperationStatus = STATUS_INVALID_DEVICE_STATE;
        return STATUS_INVALID_DEVICE_STATE;
    }

    vcpu->VmxOnExecuted = TRUE;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * VmxSetupVmcs — preenche a VMCS com guest-state (= estado atual da CPU),
 * host-state (= onde o VMM roda) e campos de controle.
 * ------------------------------------------------------------------------- */
static NTSTATUS VmxSetupVmcs(PVCPU vcpu)
{
    UINT64 vmxBasic;

    vmxBasic = __readmsr(MSR_IA32_VMX_BASIC);
    *(UINT32*)vcpu->VmcsRegion = (UINT32)(vmxBasic & 0x7FFFFFFF);

    if (__vmx_vmclear(&vcpu->VmcsRegionPhys) != 0)
    {
        vcpu->Diagnostics.FailureStage = VmxFailureVmclear;
        return STATUS_INVALID_PARAMETER;
    }
    if (__vmx_vmptrld(&vcpu->VmcsRegionPhys) != 0)
    {
        vcpu->Diagnostics.FailureStage = VmxFailureVmptrld;
        return STATUS_INVALID_PARAMETER;
    }

#define VMWRITE(field, value) do { \
        int vmwriteStatus = __vmx_vmwrite((field), (UINT64)(value)); \
        if (vmwriteStatus != 0) { \
            vcpu->Diagnostics.FailureStage = VmxFailureVmcsWrite; \
            vcpu->Diagnostics.FailedVmcsField = (UINT32)(field); \
            vcpu->Diagnostics.VmxOperationStatus = vmwriteStatus; \
            return STATUS_INVALID_PARAMETER; \
        } \
    } while (0)

    /* ---------------- GUEST STATE ---------------- */
    VMWRITE(VMCS_GUEST_CR0, __readcr0());
    VMWRITE(VMCS_GUEST_CR3, __readcr3());
    VMWRITE(VMCS_GUEST_CR4, __readcr4());
    VMWRITE(VMCS_GUEST_DR7, 0x400);
    VMWRITE(VMCS_GUEST_RFLAGS, AsmGetRflags());
    VMWRITE(VMCS_GUEST_CS_SELECTOR, AsmGetCs());
    VMWRITE(VMCS_GUEST_SS_SELECTOR, AsmGetSs());
    VMWRITE(VMCS_GUEST_DS_SELECTOR, AsmGetDs());
    VMWRITE(VMCS_GUEST_ES_SELECTOR, AsmGetEs());
    VMWRITE(VMCS_GUEST_FS_SELECTOR, AsmGetFs());
    VMWRITE(VMCS_GUEST_GS_SELECTOR, AsmGetGs());
    VMWRITE(VMCS_GUEST_LDTR_SELECTOR, AsmGetLdtr());
    VMWRITE(VMCS_GUEST_TR_SELECTOR, AsmGetTr());
    /* Enter the guest in the normal active state, without an STI/MOV-SS
       interrupt shadow or pending debug exception. Do not rely on the
       implementation-defined contents of an uninitialized VMCS field. */
    VMWRITE(VMCS_GUEST_INTERRUPTIBILITY_STATE, 0);
    VMWRITE(VMCS_GUEST_ACTIVITY_STATE, 0); /* Active */
    VMWRITE(VMCS_GUEST_PENDING_DBG_EXCEPTIONS, 0);
    VMWRITE(VMCS_GUEST_CS_LIMIT, __segmentlimit(AsmGetCs()));
    VMWRITE(VMCS_GUEST_SS_LIMIT, __segmentlimit(AsmGetSs()));
    VMWRITE(VMCS_GUEST_DS_LIMIT, __segmentlimit(AsmGetDs()));
    VMWRITE(VMCS_GUEST_ES_LIMIT, __segmentlimit(AsmGetEs()));
    VMWRITE(VMCS_GUEST_FS_LIMIT, __segmentlimit(AsmGetFs()));
    VMWRITE(VMCS_GUEST_GS_LIMIT, __segmentlimit(AsmGetGs()));
    VMWRITE(VMCS_GUEST_LDTR_LIMIT, __segmentlimit(AsmGetLdtr()));
    VMWRITE(VMCS_GUEST_TR_LIMIT, __segmentlimit(AsmGetTr()));
    VMWRITE(VMCS_GUEST_GDTR_LIMIT, AsmGetGdtLimit());
    VMWRITE(VMCS_GUEST_IDTR_LIMIT, AsmGetIdtLimit());
    VMWRITE(VMCS_GUEST_CS_AR_BYTES, VmxGetSegmentAccessRights(AsmGetCs()));
    VMWRITE(VMCS_GUEST_SS_AR_BYTES, VmxGetSegmentAccessRights(AsmGetSs()));
    VMWRITE(VMCS_GUEST_DS_AR_BYTES, VmxGetSegmentAccessRights(AsmGetDs()));
    VMWRITE(VMCS_GUEST_ES_AR_BYTES, VmxGetSegmentAccessRights(AsmGetEs()));
    VMWRITE(VMCS_GUEST_FS_AR_BYTES, VmxGetSegmentAccessRights(AsmGetFs()));
    VMWRITE(VMCS_GUEST_GS_AR_BYTES, VmxGetSegmentAccessRights(AsmGetGs()));
    VMWRITE(VMCS_GUEST_LDTR_AR_BYTES, VmxGetSegmentAccessRights(AsmGetLdtr()));
    VMWRITE(VMCS_GUEST_TR_AR_BYTES, VmxGetSegmentAccessRights(AsmGetTr()));
    VMWRITE(VMCS_GUEST_CS_BASE, VmxGetSegmentBase(AsmGetCs()));
    VMWRITE(VMCS_GUEST_SS_BASE, VmxGetSegmentBase(AsmGetSs()));
    VMWRITE(VMCS_GUEST_DS_BASE, VmxGetSegmentBase(AsmGetDs()));
    VMWRITE(VMCS_GUEST_ES_BASE, VmxGetSegmentBase(AsmGetEs()));
    VMWRITE(VMCS_GUEST_FS_BASE, __readmsr(MSR_IA32_FS_BASE));
    VMWRITE(VMCS_GUEST_GS_BASE, __readmsr(MSR_IA32_GS_BASE));
    VMWRITE(VMCS_GUEST_LDTR_BASE, VmxGetSegmentBase(AsmGetLdtr()));
    VMWRITE(VMCS_GUEST_TR_BASE, VmxGetSegmentBase(AsmGetTr()));
    VMWRITE(VMCS_GUEST_GDTR_BASE, AsmGetGdtBase());
    VMWRITE(VMCS_GUEST_IDTR_BASE, AsmGetIdtBase());
    VMWRITE(VMCS_GUEST_SYSENTER_CS, __readmsr(MSR_IA32_SYSENTER_CS));
    VMWRITE(VMCS_GUEST_SYSENTER_ESP, __readmsr(MSR_IA32_SYSENTER_ESP));
    VMWRITE(VMCS_GUEST_SYSENTER_EIP, __readmsr(MSR_IA32_SYSENTER_EIP));
    VMWRITE(VMCS_VMCS_LINK_POINTER, ~0ULL);

    /* ---------------- HOST STATE ---------------- */
    {
        UINT64 hostCr0 = __readcr0() & ~(CR0_EM | CR0_TS);
        UINT64 fixed0 = __readmsr(MSR_IA32_VMX_CR0_FIXED0);
        UINT64 fixed1 = __readmsr(MSR_IA32_VMX_CR0_FIXED1);

        /* The VM-exit stub saves XMM0-XMM5 with MOVDQU. CR0.TS must be clear
           in host mode or that first SIMD access raises #NM before C can run.
           Guest CR0 is still captured unchanged above and restored by VMRESUME. */
        if ((__readcr4() & CR4_OSFXSR) == 0)
        {
            vcpu->Diagnostics.FailureStage = VmxFailureControlRegisters;
            vcpu->Diagnostics.FailedVmcsField = VMCS_HOST_CR0;
            vcpu->Diagnostics.FailureDetail = __readcr4();
            vcpu->Diagnostics.VmxOperationStatus = STATUS_NOT_SUPPORTED;
            return STATUS_NOT_SUPPORTED;
        }
        if ((hostCr0 & fixed0) != fixed0 || (hostCr0 & ~fixed1) != 0)
        {
            vcpu->Diagnostics.FailureStage = VmxFailureControlRegisters;
            vcpu->Diagnostics.FailedVmcsField = VMCS_HOST_CR0;
            vcpu->Diagnostics.FailureDetail = hostCr0;
            vcpu->Diagnostics.VmxOperationStatus = STATUS_NOT_SUPPORTED;
            return STATUS_NOT_SUPPORTED;
        }
        VMWRITE(VMCS_HOST_CR0, hostCr0);
    }
    VMWRITE(VMCS_HOST_CR3, __readcr3());
    VMWRITE(VMCS_HOST_CR4, __readcr4());
    VMWRITE(VMCS_HOST_CS_SELECTOR, AsmGetCs() & 0xF8);
    VMWRITE(VMCS_HOST_SS_SELECTOR, AsmGetSs() & 0xF8);
    VMWRITE(VMCS_HOST_DS_SELECTOR, AsmGetDs() & 0xF8);
    VMWRITE(VMCS_HOST_ES_SELECTOR, AsmGetEs() & 0xF8);
    VMWRITE(VMCS_HOST_FS_SELECTOR, AsmGetFs() & 0xF8);
    VMWRITE(VMCS_HOST_GS_SELECTOR, AsmGetGs() & 0xF8);
    VMWRITE(VMCS_HOST_TR_SELECTOR, AsmGetTr() & 0xF8);
    VMWRITE(VMCS_HOST_FS_BASE, __readmsr(MSR_IA32_FS_BASE));
    VMWRITE(VMCS_HOST_GS_BASE, __readmsr(MSR_IA32_GS_BASE));
    VMWRITE(VMCS_HOST_TR_BASE, VmxGetSegmentBase(AsmGetTr()));
    VMWRITE(VMCS_HOST_GDTR_BASE, AsmGetGdtBase());
    VMWRITE(VMCS_HOST_IDTR_BASE, AsmGetIdtBase());
    VMWRITE(VMCS_HOST_SYSENTER_CS, __readmsr(MSR_IA32_SYSENTER_CS));
    VMWRITE(VMCS_HOST_SYSENTER_ESP, __readmsr(MSR_IA32_SYSENTER_ESP));
    VMWRITE(VMCS_HOST_SYSENTER_EIP, __readmsr(MSR_IA32_SYSENTER_EIP));
    VMWRITE(VMCS_HOST_RSP, (UINT64)vcpu->VmmStack + VMM_STACK_SIZE -
            VMM_STACK_HOST_RSP_OFFSET);
    VMWRITE(VMCS_HOST_RIP, (UINT64)AsmVmexitHandler);
    VMWRITE(VMCS_MSR_BITMAP, vcpu->MsrBitmapPhys);

    /* ---------------- CONTROLS ---------------- */

    {
        UINT32 pinControls = VmxAdjustControls(0,
            VmxGetControlMsr(MSR_IA32_VMX_PINBASED_CTLS,
                             MSR_IA32_VMX_TRUE_PINBASED_CTLS));
        if ((pinControls & ~PIN_BASED_DEFAULT1_CONTROLS) != 0)
        {
            vcpu->Diagnostics.FailureStage = VmxFailurePinControlConfiguration;
            vcpu->Diagnostics.FailedVmcsField = VMCS_PIN_BASED_VM_EXEC_CONTROL;
            vcpu->Diagnostics.FailureDetail = pinControls;
            vcpu->Diagnostics.VmxOperationStatus = STATUS_NOT_SUPPORTED;
            return STATUS_NOT_SUPPORTED;
        }
        VMWRITE(VMCS_PIN_BASED_VM_EXEC_CONTROL, pinControls);
    }

    // CPUID exits unconditionally in VMX non-root; there is no CPUID control
    // bit. Use a zeroed MSR bitmap so ordinary MSR accesses stay native
    // instead of generating an exit for every access.
    {
        UINT32 requiredControls = CPU_BASED_USE_MSR_BITMAPS;
        UINT32 permittedControls = requiredControls | CPU_BASED_DEFAULT1_CONTROLS;
        UINT32 primaryControls = VmxAdjustControls(requiredControls,
            VmxGetControlMsr(MSR_IA32_VMX_PROCBASED_CTLS,
                             MSR_IA32_VMX_TRUE_PROCBASED_CTLS));

        if ((primaryControls & requiredControls) != requiredControls ||
            (primaryControls & ~permittedControls) != 0)
        {
            vcpu->Diagnostics.FailureStage = VmxFailurePrimaryControlConfiguration;
            vcpu->Diagnostics.FailedVmcsField = VMCS_CPU_BASED_VM_EXEC_CONTROL;
            vcpu->Diagnostics.FailureDetail = primaryControls;
            vcpu->Diagnostics.VmxOperationStatus = STATUS_NOT_SUPPORTED;
            return STATUS_NOT_SUPPORTED;
        }
        VMWRITE(VMCS_CPU_BASED_VM_EXEC_CONTROL, primaryControls);

        /* CR3-load exiting is mandatory on some VMX processors. A zero
           target count makes every guest MOV to CR3 exit to our handler. */
        VMWRITE(VMCS_CR3_TARGET_COUNT, 0);
    }

    VMWRITE(VMCS_EXCEPTION_BITMAP, 0);
    {
        UINT32 exitControls = VmxAdjustControls(VM_EXIT_HOST_ADDR_SPACE_SIZE,
            VmxGetControlMsr(MSR_IA32_VMX_EXIT_CTLS,
                             MSR_IA32_VMX_TRUE_EXIT_CTLS));
        UINT32 entryControls = VmxAdjustControls(VM_ENTRY_IA32E_MODE,
            VmxGetControlMsr(MSR_IA32_VMX_ENTRY_CTLS,
                             MSR_IA32_VMX_TRUE_ENTRY_CTLS));
        /* VMX may require the documented default-1 VM-exit controls. They
           are part of the processor's mandatory baseline, not unsupported
           optional features. Keep rejecting any forced bit outside that
           baseline and the required 64-bit host setting. */
        if ((exitControls & VM_EXIT_HOST_ADDR_SPACE_SIZE) == 0 ||
            (exitControls & ~(VM_EXIT_HOST_ADDR_SPACE_SIZE |
                              VM_EXIT_DEFAULT1_CONTROLS)) != 0)
        {
            vcpu->Diagnostics.FailureStage = VmxFailureExitControlConfiguration;
            vcpu->Diagnostics.FailedVmcsField = VMCS_VM_EXIT_CONTROLS;
            vcpu->Diagnostics.FailureDetail = exitControls;
            vcpu->Diagnostics.VmxOperationStatus = STATUS_NOT_SUPPORTED;
            return STATUS_NOT_SUPPORTED;
        }
        if ((entryControls & VM_ENTRY_IA32E_MODE) == 0 ||
            (entryControls & ~(VM_ENTRY_IA32E_MODE |
                               VM_ENTRY_DEFAULT1_CONTROLS)) != 0)
        {
            vcpu->Diagnostics.FailureStage = VmxFailureEntryControlConfiguration;
            vcpu->Diagnostics.FailedVmcsField = VMCS_VM_ENTRY_CONTROLS;
            vcpu->Diagnostics.FailureDetail = entryControls;
            vcpu->Diagnostics.VmxOperationStatus = STATUS_NOT_SUPPORTED;
            return STATUS_NOT_SUPPORTED;
        }
        VMWRITE(VMCS_VM_EXIT_CONTROLS, exitControls);
        VMWRITE(VMCS_VM_ENTRY_CONTROLS, entryControls);
    }
    VMWRITE(VMCS_CR0_GUEST_HOST_MASK, 0);
    VMWRITE(VMCS_CR4_GUEST_HOST_MASK, 0);
    VMWRITE(VMCS_CR0_READ_SHADOW, __readcr0());
    VMWRITE(VMCS_CR4_READ_SHADOW, __readcr4());

#undef VMWRITE
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * VmxVirtualizeCurrentCpu — executa a sequência no processador do DPC atual.
 * ------------------------------------------------------------------------- */
static NTSTATUS VmxVirtualizeCurrentCpu(PVCPU vcpu)
{
    NTSTATUS status;

    HvProgressWrite(vcpu->Diagnostics.ProcessorIndex,
                    HvProgressCpuBegin, STATUS_SUCCESS);
    status = VmxEnterRoot(vcpu);
    if (!NT_SUCCESS(status))
    {
        HvProgressWrite(vcpu->Diagnostics.ProcessorIndex,
                        HvProgressInitializationFailed, status);
        VmxRestoreControlRegisters(vcpu);
        return status;
    }
    HvProgressWrite(vcpu->Diagnostics.ProcessorIndex,
                    HvProgressVmxonComplete, STATUS_SUCCESS);

    status = VmxSetupVmcs(vcpu);
    if (!NT_SUCCESS(status))
    {
        HvProgressWrite(vcpu->Diagnostics.ProcessorIndex,
                        HvProgressInitializationFailed, status);
        __vmx_off();
        vcpu->VmxOnExecuted = FALSE;
        VmxRestoreControlRegisters(vcpu);
        return status;
    }
    HvProgressWrite(vcpu->Diagnostics.ProcessorIndex,
                    HvProgressVmcsReady, STATUS_SUCCESS);

    /* VMLAUNCH. Em sucesso, AsmVmxLaunch retorna TRUE já como guest. */
    InterlockedExchange(&g_VmxProgressProcessor,
                        (LONG)vcpu->Diagnostics.ProcessorIndex);
    InterlockedExchange(&g_VmxVmexitCount, 0);
    InterlockedExchange(&g_VmxProgressStage, HvLaunchTraceVmlaunchBegin);
    HvLaunchWatchdogStart(vcpu->Diagnostics.ProcessorIndex);
    InterlockedExchange64(&g_VmxLaunchStartTime, (LONG64)KeQueryInterruptTime());
    InterlockedExchange(&g_VmxLaunchInProgress, 1);
    HvProgressWrite(vcpu->Diagnostics.ProcessorIndex,
                    HvProgressVmlaunchBegin, STATUS_SUCCESS);
    if (!AsmVmxLaunch())
    {
        InterlockedExchange(&g_VmxProgressStage, HvLaunchTraceVmlaunchReturned);
        UINT64 err = 0;
        int vmreadStatus = __vmx_vmread(VMCS_VM_INSTRUCTION_ERROR, &err);
        vcpu->Diagnostics.FailureStage = VmxFailureVmlaunch;
        vcpu->Diagnostics.VmInstructionError = err;
        vcpu->Diagnostics.VmInstructionErrorReadStatus = vmreadStatus;
        vcpu->Diagnostics.VmxOperationStatus = STATUS_UNSUCCESSFUL;
        /* Make a returned VMLAUNCH failure distinguishable from a hard hang
           at VMLAUNCH_BEGIN after the next forced reboot. */
        HvProgressWrite(vcpu->Diagnostics.ProcessorIndex,
                        HvProgressVmlaunchReturned, STATUS_UNSUCCESSFUL);
        __vmx_off();
        vcpu->VmxOnExecuted = FALSE;
        VmxRestoreControlRegisters(vcpu);
        DbgPrint("[Hvmx] VMLAUNCH falhou no core %lu (VM-instruction-error=%I64u)\n",
                 vcpu->Diagnostics.ProcessorIndex, err);
        return STATUS_UNSUCCESSFUL;
    }

    InterlockedExchange(&g_VmxProgressStage, HvLaunchTraceVmlaunchReturned);
    vcpu->LaunchedOk = TRUE;
    /* This registry write runs only after the minimal guest trampoline has
       returned to C, keeping Zw* calls out of the first VMX non-root steps. */
    HvProgressWrite(vcpu->Diagnostics.ProcessorIndex,
                    HvProgressVmlaunchReturned, STATUS_SUCCESS);
    DbgPrint("[Hvmx] core %lu virtualizado (VMLAUNCH ok)\n",
             vcpu->Diagnostics.ProcessorIndex);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * VmxInitializeAllProcessors — prepara recursos e inicializa um CPU por vez.
 * ------------------------------------------------------------------------- */
NTSTATUS VmxInitializeAllProcessors(void)
{
    ULONG i;
    NTSTATUS status;

    g_Hv.FailedProcessorIndex = MAXULONG;
    g_Hv.LastVmxInstructionError = 0;

    if (!VmxIsSupported())
    {
        DbgPrint("[Hvmx] VT-x nao suportado / nao habilitado na BIOS.\n");
        return STATUS_NOT_SUPPORTED;
    }

    /* This code uses systemwide processor indexes and group affinity. */
    g_Hv.ProcessorCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    g_Hv.VcpuArray = ExAllocatePoolZero(NonPagedPoolNx,
                                        sizeof(VCPU) * g_Hv.ProcessorCount,
                                        VMM_POOL_TAG);
    if (!g_Hv.VcpuArray)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Complete every allocation before the first processor enters VMX. That
       way an allocation failure cannot leave a partially virtualized system. */
    for (i = 0; i < g_Hv.ProcessorCount; i++)
    {
        PVCPU vcpu = &g_Hv.VcpuArray[i];
        vcpu->Diagnostics.ProcessorIndex = i;
        vcpu->VmmStack = ExAllocatePoolZero(NonPagedPoolNx, VMM_STACK_SIZE,
                                             VMM_POOL_TAG);
        if (vcpu->VmmStack)
            *(PVCPU*)((PUCHAR)vcpu->VmmStack + VMM_STACK_SIZE -
                      VMM_STACK_HOST_RSP_OFFSET +
                      VMM_STACK_VCPU_POINTER_OFFSET) = vcpu;
        if (vcpu->VmmStack)
            vcpu->VmxonRegion = VmxAllocContiguousPage(&vcpu->VmxonRegionPhys);
        if (vcpu->VmxonRegion)
            vcpu->VmcsRegion = VmxAllocContiguousPage(&vcpu->VmcsRegionPhys);
        if (vcpu->VmcsRegion)
            vcpu->MsrBitmap = VmxAllocContiguousPage(&vcpu->MsrBitmapPhys);
        if (!vcpu->VmmStack || !vcpu->VmxonRegion || !vcpu->VmcsRegion ||
            !vcpu->MsrBitmap)
        {
            g_Hv.FailedProcessorIndex = i;
            vcpu->Diagnostics.FailureStage = VmxFailureAllocation;
            vcpu->Diagnostics.VmxOperationStatus = STATUS_INSUFFICIENT_RESOURCES;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    /*
     * Start CPUs serially at PASSIVE_LEVEL. A broadcast DPC creates a global
     * rendezvous: if one CPU stalls during VMLAUNCH or its first VM-exit, every
     * other CPU can remain blocked at KeSignalCallDpcSynchronize. Pin this
     * thread to one processor for each transition so a bad CPU cannot strand
     * the other startup callbacks at a barrier.
     */
    for (i = 0; i < g_Hv.ProcessorCount; i++)
    {
        PROCESSOR_NUMBER processorNumber;
        GROUP_AFFINITY affinity, oldAffinity;
        PVCPU vcpu = &g_Hv.VcpuArray[i];

        KeGetProcessorNumberFromIndex(i, &processorNumber);
        RtlZeroMemory(&affinity, sizeof(affinity));
        affinity.Group = processorNumber.Group;
        affinity.Mask = ((KAFFINITY)1) << processorNumber.Number;
        KeSetSystemGroupAffinityThread(&affinity, &oldAffinity);

        vcpu->Diagnostics.ProcessorIndex = i;
        status = VmxVirtualizeCurrentCpu(vcpu);
        InterlockedExchange(&vcpu->Diagnostics.VmxOperationStatus, status);

        if (NT_SUCCESS(status))
            HvProgressWrite(i, HvProgressCpuComplete, STATUS_SUCCESS);

        KeRevertToUserGroupAffinityThread(&oldAffinity);
        InterlockedExchange(&g_VmxLaunchInProgress, 0);
        HvLaunchWatchdogStop();

        if (!NT_SUCCESS(status))
        {
            g_Hv.FailedProcessorIndex = i;
            g_Hv.LastVmxInstructionError = vcpu->Diagnostics.VmInstructionError;
            DbgPrint("[Hvmx] falha ao virtualizar core %lu (status=0x%08X).\n",
                     i, status);
            return status;
        }

    }

    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * VmxShutdownAllProcessors — VMXOFF + libera recursos (chamada no unload).
 * ------------------------------------------------------------------------- */
VOID VmxShutdownAllProcessors(void)
{
    ULONG i;

    if (!g_Hv.VcpuArray)
        return;

    for (i = 0; i < g_Hv.ProcessorCount; i++)
    {
        PVCPU vcpu = &g_Hv.VcpuArray[i];

        if (vcpu->VmxOnExecuted)
        {
            PROCESSOR_NUMBER procNum;
            GROUP_AFFINITY affinity, oldAffinity;

            KeGetProcessorNumberFromIndex(i, &procNum);
            RtlZeroMemory(&affinity, sizeof(affinity));
            affinity.Group = procNum.Group;
            affinity.Mask = (KAFFINITY)1 << procNum.Number;
            KeSetSystemGroupAffinityThread(&affinity, &oldAffinity);

            __vmx_off();
            /* VMXOFF returns here for both root and the special guest exit path. */
            __writecr4(vcpu->OriginalCr4);
            __writecr0(vcpu->OriginalCr0);
            vcpu->VmxOnExecuted = FALSE;
            vcpu->LaunchedOk = FALSE;
            vcpu->ControlRegistersAdjusted = FALSE;
            KeRevertToUserGroupAffinityThread(&oldAffinity);
        }

        if (vcpu->ControlRegistersAdjusted)
        {
            PROCESSOR_NUMBER procNum;
            GROUP_AFFINITY affinity, oldAffinity;
            KeGetProcessorNumberFromIndex(i, &procNum);
            RtlZeroMemory(&affinity, sizeof(affinity));
            affinity.Group = procNum.Group;
            affinity.Mask = (KAFFINITY)1 << procNum.Number;
            KeSetSystemGroupAffinityThread(&affinity, &oldAffinity);
            VmxRestoreControlRegisters(vcpu);
            KeRevertToUserGroupAffinityThread(&oldAffinity);
        }

        if (vcpu->VmcsRegion)  MmFreeContiguousMemory(vcpu->VmcsRegion);
        if (vcpu->VmxonRegion) MmFreeContiguousMemory(vcpu->VmxonRegion);
        if (vcpu->MsrBitmap)   MmFreeContiguousMemory(vcpu->MsrBitmap);
        if (vcpu->VmmStack)    ExFreePoolWithTag(vcpu->VmmStack, VMM_POOL_TAG);
    }

    ExFreePoolWithTag(g_Hv.VcpuArray, VMM_POOL_TAG);
    g_Hv.VcpuArray = NULL;
    DbgPrint("[Hvmx] hypervisor desligado.\n");
}
