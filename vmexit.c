/*
 * vmexit.c — handler de VM-exit.
 *
 * Despacha VM-exits, registra o estado por processador e executa os casos
 * explicitamente interceptados. Falhas preservam o último contexto para dump.
 */

#include "vmx.h"
#include <intrin.h>

UINT64 g_VmxStopGuestRip;
UINT64 g_VmxStopGuestRsp;
UINT64 g_VmxStopGuestRflags;

/* avança o RIP do guest pelo tamanho da instrução que causou o exit */
static BOOLEAN VmxAdvanceGuestRip(void)
{
    UINT64 rip = 0, len = 0;

    if (__vmx_vmread(VMCS_GUEST_RIP, &rip) != 0) return FALSE;
    if (__vmx_vmread(VMCS_VM_EXIT_INSTRUCTION_LEN, &len) != 0) return FALSE;

    if (len == 0) return FALSE; // This is an asynchronous exit; do not advance

    return (__vmx_vmwrite(VMCS_GUEST_RIP, rip + len) == 0);
}

static BOOLEAN VmxReadGuestGpr(PGUEST_REGS regs, UINT8 regIndex, UINT64* value)
{
    if (regIndex == 4)
        return __vmx_vmread(VMCS_GUEST_RSP, value) == 0;

    if (regIndex > 15)
        return FALSE;

    /* GUEST_REGS follows Intel's GPR encoding except RSP, stored in the VMCS. */
    *value = *((UINT64*)((UINT8*)regs + regIndex * sizeof(UINT64)));
    return TRUE;
}

static BOOLEAN VmxWriteGuestGpr(PGUEST_REGS regs, UINT8 regIndex, UINT64 value)
{
    if (regIndex == 4)
        return __vmx_vmwrite(VMCS_GUEST_RSP, value) == 0;

    if (regIndex > 15)
        return FALSE;

    *((UINT64*)((UINT8*)regs + regIndex * sizeof(UINT64))) = value;
    return TRUE;
}

static BOOLEAN VmxReadGuestCr(UINT8 crNumber, UINT64* value)
{
    switch (crNumber)
    {
    case 0: return __vmx_vmread(VMCS_GUEST_CR0, value) == 0;
    case 3: return __vmx_vmread(VMCS_GUEST_CR3, value) == 0;
    case 4: return __vmx_vmread(VMCS_GUEST_CR4, value) == 0;
    /* CR8 has no guest VMCS field; do not read the host's physical CR8. */
    case 8: return FALSE;
    default: return FALSE;
    }
}

static BOOLEAN VmxWriteGuestCr(UINT8 crNumber, UINT64 value)
{
    switch (crNumber)
    {
    case 0:
    {
        UINT64 fixed0 = __readmsr(MSR_IA32_VMX_CR0_FIXED0);
        UINT64 fixed1 = __readmsr(MSR_IA32_VMX_CR0_FIXED1);
        if ((value & fixed0) != fixed0 || (value & ~fixed1) != 0)
            return FALSE;
        if (((value & (1ULL << 31)) != 0 && (value & 1) == 0) ||
            ((value & (1ULL << 29)) != 0 && (value & (1ULL << 30)) == 0))
            return FALSE;
        if (__vmx_vmwrite(VMCS_GUEST_CR0, value) != 0)
            return FALSE;
        return __vmx_vmwrite(VMCS_CR0_READ_SHADOW, value) == 0;
    }
    case 3:
    {
        UINT64 guestCr4 = 0;

        /* Bit 63 is the PCID no-flush request on MOV CR3. It is an
           instruction operand flag, never part of the VMCS guest-CR3 field.
           Preserve normal validation when PCID is disabled. */
        if (__vmx_vmread(VMCS_GUEST_CR4, &guestCr4) != 0)
            return FALSE;
        if (value & CR3_PCID_NOFLUSH)
        {
            if ((guestCr4 & CR4_PCIDE) == 0)
                return FALSE;
            value &= ~CR3_PCID_NOFLUSH;
        }
        return __vmx_vmwrite(VMCS_GUEST_CR3, value) == 0;
    }
    case 4:
    {
        UINT64 fixed0 = __readmsr(MSR_IA32_VMX_CR4_FIXED0);
        UINT64 fixed1 = __readmsr(MSR_IA32_VMX_CR4_FIXED1);
        if ((value & fixed0) != fixed0 || (value & ~fixed1) != 0)
            return FALSE;
        if (__vmx_vmwrite(VMCS_GUEST_CR4, value) != 0)
            return FALSE;
        return __vmx_vmwrite(VMCS_CR4_READ_SHADOW, value) == 0;
    }
    case 8:
        /* Do not modify the host's physical APIC TPR while emulating the guest. */
        return FALSE;
    default:
        return FALSE;
    }
}

/* ---------------------------------------------------------------------------
 * VmExitHandler — chamado pelo AsmVmexitHandler a cada VM-exit.
 * Retorna 1 -> VMRESUME, 0 -> falha fatal, 2 -> sai do VMX via VMXOFF.
 * ------------------------------------------------------------------------- */
static UINT8 VmExitHandlerCore(
    PGUEST_REGS regs,
    UINT64 exitReason,
    UINT64 exitQualification,
    VMEXIT_FAILURE_CODE* failureCode)
{
    UINT16 basicReason;

    /* VM-entry failures after guest-state loading are reported as VM-exits
       with bit 31 set; they do not return through the VMLAUNCH failure path. */
    if ((exitReason & (1ULL << 31)) != 0) {
        *failureCode = VmexitFailureVmEntry;
        return VMEXIT_RESULT_FATAL;
    }

    basicReason = (UINT16)(exitReason & 0xFFFF);

    // 1. Handle the "Must Advance" exits (Instructions we spoofed)
    switch (basicReason)
    {
    case EXIT_REASON_CPUID:
    {
        if (regs->rax == 0) {
            int out[4];
            __cpuidex(out, 0, 0);
            regs->rax = (UINT32)out[0];
            // GenuineIntel vendor string is returned in EBX, EDX, ECX order.
            regs->rbx = 0x756e4547; // "GenU"
            regs->rdx = 0x49656e69; // "ineI"
            regs->rcx = 0x6c65746e; // "ntel"
        }
        else if (regs->rax == 1) {
            int out[4];
            __cpuidex(out, (int)regs->rax, (int)regs->rcx);
            regs->rax = (UINT32)out[0];
            regs->rbx = (UINT32)out[1];
            regs->rcx = (UINT32)out[2];
            regs->rdx = (UINT32)out[3];
            // Mask out the Hypervisor Present bit to hide the VM
            regs->rcx &= ~(1ULL << 31);
        }
        else {
            int out[4];
            __cpuidex(out, (int)regs->rax, (int)regs->rcx);
            regs->rax = (UINT32)out[0];
            regs->rbx = (UINT32)out[1];
            regs->rcx = (UINT32)out[2];
            regs->rdx = (UINT32)out[3];
        }
        if (!VmxAdvanceGuestRip()) { *failureCode = VmexitFailureAdvanceRip; return VMEXIT_RESULT_FATAL; }
        return VMEXIT_RESULT_RESUME;
    }


    case EXIT_REASON_MSR_READ:
    {
        UINT64 value = __readmsr((ULONG)regs->rcx);
        regs->rax = (UINT32)value;
        regs->rdx = (UINT32)(value >> 32);
        if (!VmxAdvanceGuestRip()) { *failureCode = VmexitFailureAdvanceRip; return VMEXIT_RESULT_FATAL; }
        return VMEXIT_RESULT_RESUME;

    }

    case EXIT_REASON_MSR_WRITE:
    {
        if (regs->rcx != 0x17) {
            __writemsr((ULONG)regs->rcx, (UINT64)(UINT32)regs->rax |
                ((UINT64)(UINT32)regs->rdx << 32));
        }
        if (!VmxAdvanceGuestRip()) { *failureCode = VmexitFailureAdvanceRip; return VMEXIT_RESULT_FATAL; }
        return VMEXIT_RESULT_RESUME;

    }

    case EXIT_REASON_CR_ACCESS:
    {
        UINT8 crNumber, accessType, regIndex;
        UINT64 value = 0;

        crNumber = (UINT8)(exitQualification & 0xF);
        accessType = (UINT8)((exitQualification >> 4) & 0x3);
        regIndex = (UINT8)((exitQualification >> 8) & 0xF);

        if (accessType == 0) { /* MOV to CR */
            if (!VmxReadGuestGpr(regs, regIndex, &value) ||
                !VmxWriteGuestCr(crNumber, value))
                { *failureCode = VmexitFailureCrOperation; return VMEXIT_RESULT_FATAL; }
        } else if (accessType == 1) { /* MOV from CR */
            if (!VmxReadGuestCr(crNumber, &value) ||
                !VmxWriteGuestGpr(regs, regIndex, value))
                { *failureCode = VmexitFailureCrOperation; return VMEXIT_RESULT_FATAL; }
        } else if (accessType == 2 && crNumber == 0) { /* CLTS */
            if (!VmxReadGuestCr(0, &value) ||
                !VmxWriteGuestCr(0, value & ~(1ULL << 3)))
                { *failureCode = VmexitFailureCrOperation; return VMEXIT_RESULT_FATAL; }
        } else if (accessType == 3 && crNumber == 0) { /* LMSW */
            UINT64 source = (exitQualification >> 16) & 0xFFFF;
            UINT64 oldValue;
            if (!VmxReadGuestCr(0, &value))
                { *failureCode = VmexitFailureCrOperation; return VMEXIT_RESULT_FATAL; }
            oldValue = value;
            value = (value & ~0xFULL) | (source & 0xFULL);
            value |= oldValue & 1; /* LMSW cannot clear CR0.PE. */
            if (!VmxWriteGuestCr(0, value))
                { *failureCode = VmexitFailureCrOperation; return VMEXIT_RESULT_FATAL; }
        } else {
            { *failureCode = VmexitFailureCrOperation; return VMEXIT_RESULT_FATAL; }
        }

        if (!VmxAdvanceGuestRip()) { *failureCode = VmexitFailureAdvanceRip; return VMEXIT_RESULT_FATAL; }
        return VMEXIT_RESULT_RESUME;
    }

    case EXIT_REASON_VMXOFF:
    {
        UINT64 guestRip = 0, instructionLength = 0;

        if (__vmx_vmread(VMCS_GUEST_RIP, &guestRip) != 0 ||
            __vmx_vmread(VMCS_VM_EXIT_INSTRUCTION_LEN, &instructionLength) != 0 ||
            instructionLength == 0 ||
            __vmx_vmread(VMCS_GUEST_RSP, &g_VmxStopGuestRsp) != 0 ||
            __vmx_vmread(VMCS_GUEST_RFLAGS, &g_VmxStopGuestRflags) != 0)
            { *failureCode = VmexitFailureVmxoffState; return VMEXIT_RESULT_FATAL; }

        g_VmxStopGuestRip = guestRip + instructionLength;

        /* VMXOFF in non-root exits here; leave VMX before returning to caller. */
        __vmx_off();
        return VMEXIT_RESULT_VMXOFF; /* assembly restores the interrupted guest context */
    }



    case EXIT_REASON_RDTSC:
    case EXIT_REASON_RDTSCP:
    {
        UINT64 tsc;
        if (basicReason == EXIT_REASON_RDTSCP) {
            unsigned int tscAux = 0;
            tsc = __rdtscp(&tscAux);
            regs->rcx = tscAux;
        } else {
            tsc = __rdtsc();
        }
        regs->rax = (UINT32)(tsc - 1000);
        regs->rdx = (UINT32)((tsc - 1000) >> 32);
        if (!VmxAdvanceGuestRip()) { *failureCode = VmexitFailureAdvanceRip; return VMEXIT_RESULT_FATAL; }
        return VMEXIT_RESULT_RESUME;
    }

    default:
        // DO NOT advance RIP here.
        // If we reach default, it's an exit we didn't expect.
        // Return FALSE so the assembly takes its fatal stop path.
        *failureCode = VmexitFailureUnknownReason;
        return VMEXIT_RESULT_FATAL;
    }

    return VMEXIT_RESULT_RESUME;
}

UINT8 VmExitHandler(PGUEST_REGS regs, PVCPU vcpu)
{
    ULONG processorIndex;
    PVMEXIT_DIAGNOSTICS diagnostics;
    UINT64 exitReason = 0;
    UINT8 result;
    VMEXIT_FAILURE_CODE failureCode = VmexitFailureNone;

    if (!vcpu)
        return VMEXIT_RESULT_FATAL;

    InterlockedExchange(&g_VmxProgressStage, HvLaunchTraceVmexitHandler);

    processorIndex = vcpu->Diagnostics.ProcessorIndex;
    diagnostics = &vcpu->Diagnostics;
    InterlockedIncrement(&diagnostics->Sequence); /* odd: update in progress */
    diagnostics->ProcessorIndex = processorIndex;
    diagnostics->ExitReason = 0;
    diagnostics->ExitQualification = 0;
    diagnostics->GuestRip = 0;
    diagnostics->GuestRsp = 0;
    diagnostics->InstructionLength = 0;
    diagnostics->VmInstructionError = 0;
    diagnostics->VmInstructionErrorReadStatus = 0;
    diagnostics->HandlerResult = MAXULONG;
    diagnostics->FailureCode = VmexitFailureNone;
    diagnostics->FailureStage = VmxFailureNone;
    diagnostics->FailedVmcsField = 0;
    diagnostics->VmxOperationStatus = 0;
    diagnostics->FailureDetail = 0;

    if (__vmx_vmread(VMCS_EXIT_REASON, &exitReason) != 0) {
        failureCode = VmexitFailureReadExitReason;
        result = VMEXIT_RESULT_FATAL;
        goto done;
    }
    diagnostics->ExitReason = exitReason;

    if (__vmx_vmread(VMCS_EXIT_QUALIFICATION, &diagnostics->ExitQualification) != 0) {
        failureCode = VmexitFailureReadQualification;
        result = VMEXIT_RESULT_FATAL;
        goto done;
    }

    if (__vmx_vmread(VMCS_GUEST_RIP, &diagnostics->GuestRip) != 0 ||
        __vmx_vmread(VMCS_GUEST_RSP, &diagnostics->GuestRsp) != 0 ||
        __vmx_vmread(VMCS_VM_EXIT_INSTRUCTION_LEN, &diagnostics->InstructionLength) != 0) {
        failureCode = VmexitFailureReadExitState;
        result = VMEXIT_RESULT_FATAL;
        goto done;
    }

    result = VmExitHandlerCore(regs, exitReason,
        diagnostics->ExitQualification, &failureCode);

done:
    diagnostics->HandlerResult = result;
    diagnostics->FailureCode = failureCode;
    if (result == VMEXIT_RESULT_FATAL) {
        if (failureCode == VmexitFailureVmEntry) {
            diagnostics->FailureStage = VmxFailureVmEntryFailure;
        } else {
            diagnostics->FailureStage =
                (failureCode == VmexitFailureReadExitReason ||
                 failureCode == VmexitFailureReadExitState ||
                 failureCode == VmexitFailureReadQualification)
                    ? VmxFailureVmexitRead : VmxFailureVmexitHandler;
        }
        diagnostics->VmxOperationStatus = STATUS_UNSUCCESSFUL;
    }
    InterlockedIncrement(&diagnostics->Sequence); /* even: stable snapshot */
    return result;
}


/* ---------------------------------------------------------------------------
 * VmResumeFailed — chamado pelo asm se VMRESUME falhar (não deveria ocorrer).
 * ------------------------------------------------------------------------- */
VOID VmResumeFailed(PVCPU vcpu)
{
    UINT64 err = 0;
    ULONG processorIndex = vcpu ? vcpu->Diagnostics.ProcessorIndex : MAXULONG;
    int vmreadStatus;

    vmreadStatus = __vmx_vmread(VMCS_VM_INSTRUCTION_ERROR, &err);
    if (vcpu) {
        InterlockedIncrement(&vcpu->Diagnostics.Sequence);
        vcpu->Diagnostics.FailureStage = VmxFailureVmresume;
        vcpu->Diagnostics.VmInstructionError = err;
        vcpu->Diagnostics.VmInstructionErrorReadStatus = vmreadStatus;
        vcpu->Diagnostics.VmxOperationStatus = STATUS_UNSUCCESSFUL;
        InterlockedIncrement(&vcpu->Diagnostics.Sequence);
    }

    /*
     * Do not log here: VM-exit host RFLAGS have IF cleared, and this path may
     * inherit an elevated guest IRQL. Keep the details in per-CPU diagnostics
     * and bugcheck arguments so a debugger/dump can inspect them safely.
     */
    KeBugCheckEx(0x00001337, processorIndex, err,
        vcpu ? vcpu->Diagnostics.ExitReason : 0,
        vcpu ? vcpu->Diagnostics.GuestRip : 0);
}

VOID VmExitFatal(PVCPU vcpu)
{
    ULONG processorIndex = vcpu ? vcpu->Diagnostics.ProcessorIndex : MAXULONG;

    if (vcpu) {
        /* See VmResumeFailed: avoid debugger logging on the VM-exit path. */
        KeBugCheckEx(0x00001338, processorIndex,
            vcpu->Diagnostics.ExitReason, vcpu->Diagnostics.ExitQualification,
            vcpu->Diagnostics.GuestRip);
    }

    KeBugCheckEx(0x00001338, processorIndex, 0, 0, 0);
}
