/*
 * vmexit.c — handler de VM-exit.
 *
 * Este é o "handle da exit" pedido: a cada VM-exit ele AVISA que um exit foi
 * solicitado (número do core + motivo) e então faz o mínimo necessário para
 * a VM continuar rodando (executa a instrução interceptada e avança o RIP).
 *
 * Não há tratamento especial por tipo de instrução — qualquer exit é apenas
 * sinalizado e resumido.
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
    case 8: *value = __readcr8(); return TRUE;
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
        return __vmx_vmwrite(VMCS_GUEST_CR3, value) == 0;
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
        /* CR8 is shared with the physical APIC TPR; it has no VMCS field. */
        if ((value & ~0xFULL) != 0)
            return FALSE;
        __writecr8(value);
        return TRUE;
    default:
        return FALSE;
    }
}

/* ---------------------------------------------------------------------------
 * VmExitHandler — chamado pelo AsmVmexitHandler a cada VM-exit.
 * Retorna 1 -> VMRESUME, 0 -> falha fatal, 2 -> sai do VMX via VMXOFF.
 * ------------------------------------------------------------------------- */
UINT8 VmExitHandler(PGUEST_REGS regs)
{
    UINT64 exitReason = 0;
    UINT16 basicReason;

    if (__vmx_vmread(VMCS_EXIT_REASON, &exitReason) != 0)
        return FALSE;
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
        if (!VmxAdvanceGuestRip()) return FALSE;
        return TRUE;
    }


    case EXIT_REASON_MSR_READ:
    {
        UINT64 value = __readmsr((ULONG)regs->rcx);
        regs->rax = (UINT32)value;
        regs->rdx = (UINT32)(value >> 32);
        if (!VmxAdvanceGuestRip()) return FALSE; // Stop if RIP cannot advance
        return TRUE;

    }

    case EXIT_REASON_MSR_WRITE:
    {
        if (regs->rcx != 0x17) {
            __writemsr((ULONG)regs->rcx, (UINT64)(UINT32)regs->rax |
                ((UINT64)(UINT32)regs->rdx << 32));
        }
        if (!VmxAdvanceGuestRip()) return FALSE; // Stop if RIP cannot advance
        return TRUE;

    }

    case EXIT_REASON_CR_ACCESS:
    {
        UINT64 qualification = 0;
        UINT8 crNumber, accessType, regIndex;
        UINT64 value = 0;

        if (__vmx_vmread(VMCS_EXIT_QUALIFICATION, &qualification) != 0)
            return FALSE;

        crNumber = (UINT8)(qualification & 0xF);
        accessType = (UINT8)((qualification >> 4) & 0x3);
        regIndex = (UINT8)((qualification >> 8) & 0xF);

        if (accessType == 0) { /* MOV to CR */
            if (!VmxReadGuestGpr(regs, regIndex, &value) ||
                !VmxWriteGuestCr(crNumber, value))
                return FALSE;
        } else if (accessType == 1) { /* MOV from CR */
            if (!VmxReadGuestCr(crNumber, &value) ||
                !VmxWriteGuestGpr(regs, regIndex, value))
                return FALSE;
        } else if (accessType == 2 && crNumber == 0) { /* CLTS */
            if (!VmxReadGuestCr(0, &value) ||
                !VmxWriteGuestCr(0, value & ~(1ULL << 3)))
                return FALSE;
        } else if (accessType == 3 && crNumber == 0) { /* LMSW */
            UINT64 source = (qualification >> 16) & 0xFFFF;
            UINT64 oldValue;
            if (!VmxReadGuestCr(0, &value))
                return FALSE;
            oldValue = value;
            value = (value & ~0xFULL) | (source & 0xFULL);
            value |= oldValue & 1; /* LMSW cannot clear CR0.PE. */
            if (!VmxWriteGuestCr(0, value))
                return FALSE;
        } else {
            return FALSE;
        }

        if (!VmxAdvanceGuestRip()) return FALSE;
        return TRUE;
    }

    case EXIT_REASON_VMXOFF:
    {
        UINT64 guestRip = 0, instructionLength = 0;

        if (__vmx_vmread(VMCS_GUEST_RIP, &guestRip) != 0 ||
            __vmx_vmread(VMCS_VM_EXIT_INSTRUCTION_LEN, &instructionLength) != 0 ||
            instructionLength == 0 ||
            __vmx_vmread(VMCS_GUEST_RSP, &g_VmxStopGuestRsp) != 0 ||
            __vmx_vmread(VMCS_GUEST_RFLAGS, &g_VmxStopGuestRflags) != 0)
            return FALSE;

        g_VmxStopGuestRip = guestRip + instructionLength;

        /* VMXOFF in non-root exits here; leave VMX before returning to caller. */
        __vmx_off();
        return 2; /* assembly restores the interrupted guest context */
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
        if (!VmxAdvanceGuestRip()) return FALSE; // Stop if RIP cannot advance
        return TRUE;
        break;
    }

    default:
        // DO NOT advance RIP here.
        // If we reach default, it's an exit we didn't expect.
        // Return FALSE so the assembly takes its fatal stop path.
        return FALSE;
    }

    return TRUE;
}


/* ---------------------------------------------------------------------------
 * VmResumeFailed — chamado pelo asm se VMRESUME falhar (não deveria ocorrer).
 * ------------------------------------------------------------------------- */
VOID VmResumeFailed(void)
{
    UINT64 err = 0;
    __vmx_vmread(VMCS_VM_INSTRUCTION_ERROR, &err);
    DbgPrint("[Hvmx] VMRESUME falhou (VM-instruction-error=%llu)\n", err);
    KeBugCheckEx(0x00001337, err, 0, 0, 0);
}
