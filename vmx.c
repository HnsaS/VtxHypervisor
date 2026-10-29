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
 * Ajusta um valor de controle pelos bits allowed-0/allowed-1 do MSR de
 * capacidade (a parte baixa marca controles cujo valor 0 não é permitido;
 * a parte alta marca controles cujo valor 1 é permitido).
 * ------------------------------------------------------------------------- */
static UINT32 VmxAdjustControls(UINT32 desired, UINT32 capabilityMsr)
{
    UINT64 capability = __readmsr(capabilityMsr);
    UINT32 mustBeOne = (UINT32)capability;
    UINT32 allowedOne = (UINT32)(capability >> 32);

    desired |= mustBeOne;      /* força 1 nos bits cujo zero não é permitido */
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

    max.QuadPart = MAXULONG64;
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

/* ---------------------------------------------------------------------------
 * VmxEnterRoot — aloca a região VMXON e executa VMXON neste core.
 * ------------------------------------------------------------------------- */
static NTSTATUS VmxEnterRoot(PVCPU vcpu)
{
    UINT64 vmxBasic;

    VmxAdjustControlRegisters(vcpu);

    vcpu->VmxonRegion = VmxAllocContiguousPage(&vcpu->VmxonRegionPhys);
    if (!vcpu->VmxonRegion)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* revision identifier (bits 30:0 do IA32_VMX_BASIC) no início da região */
    vmxBasic = __readmsr(MSR_IA32_VMX_BASIC);
    *(UINT32*)vcpu->VmxonRegion = (UINT32)(vmxBasic & 0x7FFFFFFF);

    if (__vmx_on(&vcpu->VmxonRegionPhys) != 0)
    {
        DbgPrint("[Hvmx] VMXON falhou no core %lu\n", KeGetCurrentProcessorNumber());
        return STATUS_INVALID_DEVICE_STATE;
    }

    vcpu->VmxOnExecuted = TRUE;
    return STATUS_SUCCESS;
}

/* wrapper curto p/ vmwrite */
#define VMWRITE(field, value)  __vmx_vmwrite((field), (UINT64)(value))

/* ---------------------------------------------------------------------------
 * VmxSetupVmcs — preenche a VMCS com guest-state (= estado atual da CPU),
 * host-state (= onde o VMM roda) e campos de controle.
 * ------------------------------------------------------------------------- */
static NTSTATUS VmxSetupVmcs(PVCPU vcpu)
{
    UINT64 vmxBasic;

    vcpu->VmcsRegion = VmxAllocContiguousPage(&vcpu->VmcsRegionPhys);
    if (!vcpu->VmcsRegion)
        return STATUS_INSUFFICIENT_RESOURCES;

    vmxBasic = __readmsr(MSR_IA32_VMX_BASIC);
    *(UINT32*)vcpu->VmcsRegion = (UINT32)(vmxBasic & 0x7FFFFFFF);

    if (__vmx_vmclear(&vcpu->VmcsRegionPhys) != 0)
        return STATUS_INVALID_PARAMETER;
    if (__vmx_vmptrld(&vcpu->VmcsRegionPhys) != 0)
        return STATUS_INVALID_PARAMETER;

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
    VMWRITE(VMCS_HOST_CR0, __readcr0());
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
    VMWRITE(VMCS_HOST_RSP, (UINT64)vcpu->VmmStack + VMM_STACK_SIZE - 0x100);
    VMWRITE(VMCS_HOST_RIP, (UINT64)AsmVmexitHandler);

    /* ---------------- CONTROLS ---------------- */

    VMWRITE(VMCS_PIN_BASED_VM_EXEC_CONTROL,
        VmxAdjustControls(0,
            VmxGetControlMsr(MSR_IA32_VMX_PINBASED_CTLS,
                             MSR_IA32_VMX_TRUE_PINBASED_CTLS)));

    // CPUID always causes a VM exit. Use the bitmap for the selected MSR.
    {
        const UINT32 requiredControls =
            CPU_BASED_VM_EXEC_CONTROL_USE_MSR_BITMAPS |
            CPU_BASED_CR3_LOAD_EXITING |
            CPU_BASED_CR3_STORE_EXITING |
            CPU_BASED_CR8_LOAD_EXITING |
            CPU_BASED_CR8_STORE_EXITING;
        UINT32 primaryControls = VmxAdjustControls(requiredControls,
            VmxGetControlMsr(MSR_IA32_VMX_PROCBASED_CTLS,
                             MSR_IA32_VMX_TRUE_PROCBASED_CTLS));

        if ((primaryControls & requiredControls) != requiredControls)
            return STATUS_NOT_SUPPORTED;
        if (VMWRITE(VMCS_CPU_BASED_VM_EXEC_CONTROL, primaryControls) != 0)
            return STATUS_INVALID_PARAMETER;
    }

    // MSR bitmap: intercept reads of IA32_PLATFORM_ID (MSR 0x17) only.
    vcpu->MsrBitmap = VmxAllocContiguousPage(&vcpu->MsrBitmapPhys);
    if (!vcpu->MsrBitmap) return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(vcpu->MsrBitmap, PAGE_SIZE_4K);
    UINT32* bitmap = (UINT32*)vcpu->MsrBitmap;
    /* Intercept read of IA32_PLATFORM_ID (MSR 0x17); do not intercept writes. */
    bitmap[0x17 / 32] |= (1UL << (0x17 % 32));

    VMWRITE(VMCS_MSR_BITMAP, vcpu->MsrBitmapPhys);

    VMWRITE(VMCS_EXCEPTION_BITMAP, 0);
    VMWRITE(VMCS_VM_EXIT_CONTROLS,
        VmxAdjustControls(VM_EXIT_HOST_ADDR_SPACE_SIZE,
            VmxGetControlMsr(MSR_IA32_VMX_EXIT_CTLS,
                             MSR_IA32_VMX_TRUE_EXIT_CTLS)));
    VMWRITE(VMCS_VM_ENTRY_CONTROLS,
        VmxAdjustControls(VM_ENTRY_IA32E_MODE,
            VmxGetControlMsr(MSR_IA32_VMX_ENTRY_CTLS,
                             MSR_IA32_VMX_TRUE_ENTRY_CTLS)));
    VMWRITE(VMCS_CR0_GUEST_HOST_MASK, MAXULONGLONG);
    VMWRITE(VMCS_CR4_GUEST_HOST_MASK, MAXULONGLONG);
    VMWRITE(VMCS_CR0_READ_SHADOW, __readcr0());
    VMWRITE(VMCS_CR4_READ_SHADOW, __readcr4());

    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * VmxVirtualizeCurrentCpu — executa a sequência completa no core atual.
 * Roda com IRQL elevado e afinidade fixada (ver VmxInitializeAllProcessors).
 * ------------------------------------------------------------------------- */
static NTSTATUS VmxVirtualizeCurrentCpu(PVCPU vcpu, ULONG processorIndex)
{
    NTSTATUS status;

    /* pilha do host (VMM) */
    vcpu->VmmStack = ExAllocatePoolZero(NonPagedPool, VMM_STACK_SIZE, VMM_POOL_TAG);
    if (!vcpu->VmmStack)
        return STATUS_INSUFFICIENT_RESOURCES;

    status = VmxEnterRoot(vcpu);
    if (!NT_SUCCESS(status))
        return status;

    status = VmxSetupVmcs(vcpu);
    if (!NT_SUCCESS(status))
        return status;

    /* VMLAUNCH. Em sucesso, AsmVmxLaunch retorna TRUE já como guest. */
    if (!AsmVmxLaunch())
    {
        UINT64 err = 0;
        __vmx_vmread(VMCS_VM_INSTRUCTION_ERROR, &err);
        g_Hv.FailedProcessorIndex = processorIndex;
        g_Hv.LastVmxInstructionError = err;
        DbgPrint("[Hvmx] VMLAUNCH falhou no core %lu (VM-instruction-error=%llu)\n",
                 KeGetCurrentProcessorNumber(), err);
        __vmx_off();
        vcpu->VmxOnExecuted = FALSE;
        return STATUS_UNSUCCESSFUL;
    }

    vcpu->LaunchedOk = TRUE;
    DbgPrint("[Hvmx] core %lu virtualizado (VMLAUNCH ok)\n",
             KeGetCurrentProcessorNumber());
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * VmxInitializeAllProcessors — fixa a thread em cada core e virtualiza.
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

    g_Hv.ProcessorCount = KeQueryActiveProcessorCount(NULL);
    g_Hv.VcpuArray = ExAllocatePoolZero(NonPagedPool,
                                        sizeof(VCPU) * g_Hv.ProcessorCount,
                                        VMM_POOL_TAG);
    if (!g_Hv.VcpuArray)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (i = 0; i < g_Hv.ProcessorCount; i++)
    {
        PROCESSOR_NUMBER procNum;
        GROUP_AFFINITY affinity, oldAffinity;

        KeGetProcessorNumberFromIndex(i, &procNum);
        RtlZeroMemory(&affinity, sizeof(affinity));
        affinity.Group = procNum.Group;
        affinity.Mask = (KAFFINITY)1 << procNum.Number;
        KeSetSystemGroupAffinityThread(&affinity, &oldAffinity);

        status = VmxVirtualizeCurrentCpu(&g_Hv.VcpuArray[i], i);
        if (!NT_SUCCESS(status))
        {
            DbgPrint("[Hvmx] falha ao virtualizar core %lu (status=0x%08X).\n",
                     i, status);
            KeRevertToUserGroupAffinityThread(&oldAffinity);
            return status;
        }

        KeRevertToUserGroupAffinityThread(&oldAffinity);
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
            __writecr4(vcpu->OriginalCr4);
            __writecr0(vcpu->OriginalCr0);
            vcpu->ControlRegistersAdjusted = FALSE;
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
