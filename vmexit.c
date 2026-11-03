/*
 * vmexit.c — handler de VM-exit.
 *
 * Despacha VM-exits, registra o estado por processador e executa os casos
 * explicitamente interceptados. Falhas preservam o último contexto para dump.
 */

#include "vmx.h"
#include <intrin.h>
#include <immintrin.h> /* _xsetbv */

UINT64 g_VmxStopGuestRip;
UINT64 g_VmxStopGuestRsp;
UINT64 g_VmxStopGuestRflags;

/* ---------------------------------------------------------------------------
 * Raw debug channel. Plain port I/O only: no Windows API, no IRQL/IF
 * requirements, so it still works inside VMX root and just before a hang.
 *   - 0xE9  : QEMU/Bochs "debugcon" (-debugcon file:hv.log)
 *   - 0x3F8 : COM1 (VMware serial-to-file, or a real UART)
 * On hardware with neither, the writes are harmless no-ops.
 * ------------------------------------------------------------------------- */
#ifndef HV_TRACE_EXITS
#define HV_TRACE_EXITS      1     /* log the first N VM-exits */
#endif
#define HV_TRACE_MAX_EXITS  512

static void HvDbgChar(char c)
{
    int spin;
    __outbyte(0xE9, (unsigned char)c);
    for (spin = 0; spin < 20000 && (__inbyte(0x3FD) & 0x20) == 0; spin++)
        ;
    __outbyte(0x3F8, (unsigned char)c);
}

static void HvDbgStr(const char* s)
{
    while (*s) HvDbgChar(*s++);
}

static void HvDbgHex(UINT64 v)
{
    int shift;
    HvDbgChar('0'); HvDbgChar('x');
    for (shift = 60; shift >= 0; shift -= 4)
        HvDbgChar("0123456789ABCDEF"[(v >> shift) & 0xF]);
}

static void HvDbgExit(const char* tag, ULONG cpu, UINT64 reason, UINT64 qual, UINT64 rip)
{
    HvDbgStr("[HV] "); HvDbgStr(tag);
    HvDbgStr(" cpu="); HvDbgHex(cpu);
    HvDbgStr(" reason="); HvDbgHex(reason);
    HvDbgStr(" qual="); HvDbgHex(qual);
    HvDbgStr(" rip="); HvDbgHex(rip);
    HvDbgStr("\r\n");
}

/* Queue a hardware exception for delivery on the next VM-entry. Callers must
   NOT advance guest RIP for a fault. */
static BOOLEAN VmxInjectException(UINT8 vector, BOOLEAN hasErrorCode, UINT32 errorCode)
{
    UINT64 info = (UINT64)vector | (3ULL << 8) /* hardware exception */ | (1ULL << 31);

    if (hasErrorCode)
    {
        info |= (1ULL << 11);
        if (__vmx_vmwrite(VMCS_VM_ENTRY_EXCEPTION_ERROR_CODE, errorCode) != 0)
            return FALSE;
    }
    return __vmx_vmwrite(VMCS_VM_ENTRY_INTERRUPTION_INFO, info) == 0;
}

/* RDMSR/WRMSR outside these two ranges always VM-exit; real hardware #GPs. */
static BOOLEAN VmxMsrIndexInArchitecturalRange(UINT32 msr)
{
    return msr <= 0x1FFF || (msr >= 0xC0000000u && msr <= 0xC0001FFFu);
}

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
    PVCPU vcpu,
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
        UINT32 leaf = (UINT32)regs->rax;
        UINT32 subleaf = (UINT32)regs->rcx;
        int out[4];
        static const UCHAR brandPrefix[] = "Intel(R) Core(TM) i7";

        /* Start with the native CPUID response and override only selected
           values. CPUID results are 32-bit, even though GPRs are 64-bit. */
        __cpuidex(out, (int)leaf, (int)subleaf);
        regs->rax = (UINT32)out[0];
        regs->rbx = (UINT32)out[1];
        regs->rcx = (UINT32)out[2];
        regs->rdx = (UINT32)out[3];

        if (leaf == 0)
        {
            /* Vendor is returned in EBX, EDX, ECX order. */
            regs->rbx = 0x756e6547; /* "Genu" */
            regs->rdx = 0x49656e69; /* "ineI" */
            regs->rcx = 0x6c65746e; /* "ntel" */
        }
        else if (leaf >= 0x80000002 && leaf <= 0x80000004)
        {
            UCHAR brand[48];
            UINT32 words[12];
            UINT32 wordIndex = (leaf - 0x80000002) * 4;

            RtlFillMemory(brand, sizeof(brand), ' ');
            RtlCopyMemory(brand, brandPrefix, sizeof(brandPrefix) - 1);
            RtlCopyMemory(words, brand, sizeof(brand));
            regs->rax = words[wordIndex + 0];
            regs->rbx = words[wordIndex + 1];
            regs->rcx = words[wordIndex + 2];
            regs->rdx = words[wordIndex + 3];
        }

        /* Don't advertise execution features unavailable in the VMCS. */
        if (leaf == 1)
            regs->rcx &= ~((1U << 5) | (1U << 6) | (1U << 31));
        else if (leaf == 7 && subleaf == 0)
        {
            if ((vcpu->SecondaryControls & SECONDARY_EXEC_ENABLE_INVPCID) == 0)
                regs->rbx &= ~(1U << 10);
            if ((vcpu->SecondaryControls & SECONDARY_EXEC_ENABLE_USER_WAIT_PAUSE) == 0)
                regs->rcx &= ~(1U << 5);
        }
        else if (leaf == 0x0D && subleaf == 1 &&
                 (vcpu->SecondaryControls & SECONDARY_EXEC_ENABLE_XSAVES) == 0)
        {
            regs->rax &= ~(1U << 3); /* XSAVES/XRSTORS */
            regs->rcx = 0;
            regs->rdx = 0;
        }
        else if (leaf == 0x80000001 &&
                 (vcpu->SecondaryControls & SECONDARY_EXEC_ENABLE_RDTSCP) == 0)
            regs->rdx &= ~(1U << 27);

        if (!VmxAdvanceGuestRip()) { *failureCode = VmexitFailureAdvanceRip; return VMEXIT_RESULT_FATAL; }
        return VMEXIT_RESULT_RESUME;
    }


    case EXIT_REASON_MSR_READ:
    {
        UINT64 value;
        /* Test index 0x80 is reserved and is intentionally reflected as #GP;
           never execute RDMSR for it in VMX root mode. */
        if ((UINT32)regs->rcx == 0x80) {
            if (!VmxInjectException(13, TRUE, 0)) { *failureCode = VmexitFailureAdvanceRip; return VMEXIT_RESULT_FATAL; }
            return VMEXIT_RESULT_RESUME;
        }
        /* An invalid MSR in root mode would #GP inside the VMM (on a pool
           stack, IF=0) and wedge the box. Reflect the #GP to the guest. */
        if (!VmxMsrIndexInArchitecturalRange((UINT32)regs->rcx)) {
            if (!VmxInjectException(13, TRUE, 0)) { *failureCode = VmexitFailureAdvanceRip; return VMEXIT_RESULT_FATAL; }
            return VMEXIT_RESULT_RESUME;
        }
        value = __readmsr((ULONG)regs->rcx);
        regs->rax = (UINT32)value;
        regs->rdx = (UINT32)(value >> 32);
        if (!VmxAdvanceGuestRip()) { *failureCode = VmexitFailureAdvanceRip; return VMEXIT_RESULT_FATAL; }
        return VMEXIT_RESULT_RESUME;

    }

    case EXIT_REASON_MSR_WRITE:
    {
        if ((UINT32)regs->rcx == 0x80) {
            if (!VmxInjectException(13, TRUE, 0)) { *failureCode = VmexitFailureAdvanceRip; return VMEXIT_RESULT_FATAL; }
            return VMEXIT_RESULT_RESUME;
        }
        if (!VmxMsrIndexInArchitecturalRange((UINT32)regs->rcx)) {
            if (!VmxInjectException(13, TRUE, 0)) { *failureCode = VmexitFailureAdvanceRip; return VMEXIT_RESULT_FATAL; }
            return VMEXIT_RESULT_RESUME;
        }
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



    /* XSETBV exits unconditionally in VMX non-root. Windows executes it during
       CPU bring-up and resume; leaving it unhandled is fatal. */
    case EXIT_REASON_XSETBV:
    {
        int leaf[4];
        UINT32 index = (UINT32)regs->rcx;
        UINT64 value = ((UINT64)(UINT32)regs->rdx << 32) | (UINT32)regs->rax;
        UINT64 supported;

        __cpuidex(leaf, 0x0D, 0);
        supported = ((UINT64)(UINT32)leaf[3] << 32) | (UINT32)leaf[0];

        /* Only XCR0 exists; x87 must stay on; no unsupported components;
           AVX requires SSE. Anything else is a #GP on real hardware, and
           executing it here would #GP inside the VMM instead. */
        if (index != 0 ||
            (value & 1) == 0 ||
            (value & ~supported) != 0 ||
            ((value & 4) != 0 && (value & 2) == 0))
        {
            if (!VmxInjectException(13, TRUE, 0)) { *failureCode = VmexitFailureCrOperation; return VMEXIT_RESULT_FATAL; }
            return VMEXIT_RESULT_RESUME;
        }
        _xsetbv(index, value);
        if (!VmxAdvanceGuestRip()) { *failureCode = VmexitFailureAdvanceRip; return VMEXIT_RESULT_FATAL; }
        return VMEXIT_RESULT_RESUME;
    }

    case EXIT_REASON_INVD:
        __wbinvd(); /* INVD can lose dirty lines; WBINVD is the safe stand-in */
        if (!VmxAdvanceGuestRip()) { *failureCode = VmexitFailureAdvanceRip; return VMEXIT_RESULT_FATAL; }
        return VMEXIT_RESULT_RESUME;

    /* VMX instructions and GETSEC are not available to this guest: #UD. */
    case EXIT_REASON_GETSEC:
    case EXIT_REASON_VMCALL:
    case 19: case 20: case 21: case 22: case 23: case 24: case 25: /* VMCLEAR..VMWRITE */
    case 27:                                                      /* VMXON  */
    case EXIT_REASON_INVEPT:
    case EXIT_REASON_INVVPID:
        if (!VmxInjectException(6, FALSE, 0)) { *failureCode = VmexitFailureAdvanceRip; return VMEXIT_RESULT_FATAL; }
        return VMEXIT_RESULT_RESUME;

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

#if HV_TRACE_EXITS
    /* Logged BEFORE handling: if the box wedges, the last line on the debug
       channel is the exit that was being processed. */
    if ((ULONG)g_VmxVmexitCount <= HV_TRACE_MAX_EXITS)
        HvDbgExit("exit ", processorIndex, exitReason,
                  diagnostics->ExitQualification, diagnostics->GuestRip);
#endif

    result = VmExitHandlerCore(regs, vcpu, exitReason,
        diagnostics->ExitQualification, &failureCode);

done:
    diagnostics->HandlerResult = result;
    diagnostics->FailureCode = failureCode;
    if (result == VMEXIT_RESULT_FATAL) {
        HvDbgExit("FATAL", processorIndex, exitReason,
                  diagnostics->ExitQualification, diagnostics->GuestRip);
        HvDbgStr("[HV]   failureCode="); HvDbgHex((UINT64)failureCode);
        HvDbgStr(" guestRsp="); HvDbgHex(diagnostics->GuestRsp);
        HvDbgStr("\r\n");
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
