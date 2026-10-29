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

/* avança o RIP do guest pelo tamanho da instrução que causou o exit */
static void VmxAdvanceGuestRip(void)
{
    UINT64 rip = 0, len = 0;
    __vmx_vmread(VMCS_GUEST_RIP, &rip);
    __vmx_vmread(VMCS_VM_EXIT_INSTRUCTION_LEN, &len);

    if (len == 0) {
        // This should never happen, but if it does, we must 
        // force advance or the system will hang forever.
        len = 1;
    }

    __vmx_vmwrite(VMCS_GUEST_RIP, rip + len);
}

/* ---------------------------------------------------------------------------
 * VmExitHandler — chamado pelo AsmVmexitHandler a cada VM-exit.
 * Retorna TRUE  -> VMRESUME (continua a VM).
 *         FALSE -> encerra (não usado aqui; sempre continuamos).
 * ------------------------------------------------------------------------- */
BOOLEAN VmExitHandler(PGUEST_REGS regs)
{
    UINT64 exitReason = 0;
    UINT16 basicReason;

    __vmx_vmread(VMCS_EXIT_REASON, &exitReason);
    basicReason = (UINT16)(exitReason & 0xFFFF);

    /* >>> aviso de que uma VM-exit foi solicitada <<< */
    /*DbgPrint("[Hvmx] VM-exit solicitada no core %lu (reason=%u)\n",
             KeGetCurrentProcessorNumber(), basicReason);*/

    /*
     * Mantém a VM viva. A maioria dos exits "padrão" é de instrução
     * (CPUID, INVD, GETSEC, XSETBV, RDMSR/WRMSR quando interceptados, etc.):
     * reexecuta o efeito e avança o RIP.
     */
    switch (basicReason)
    {
    case EXIT_REASON_CPUID:
    {
        if (regs->rax == 0) {
            regs->rax = 0x756e4547; // "GenU"
            regs->rbx = 0x656e696e; // "ine "
            regs->rcx = 0x656c7465; // "elte"
            regs->rdx = 0x4c204665;   // literal original sem separadores inválidos
        }
        else if (regs->rax == 1) {
            // You must spoof the family/model to match the "Intel" string
            // Example for a common Intel CPU:
            regs->rax = 0x000306a5; // Family 6, Model 165
            regs->rbx = 0x00000000;
            regs->rcx = 0x00000000;
            regs->rdx = 0x00000000;
        }
        else {
            int out[4];
            __cpuidex(out, (int)regs->rax, (int)regs->rcx);
            regs->rax = (UINT32)out[0];
            regs->rbx = (UINT32)out[1];
            regs->rcx = (UINT32)out[2];
            regs->rdx = (UINT32)out[3];
        }
        VmxAdvanceGuestRip();
        break;
    }

    case EXIT_REASON_MSR_READ:
    {
        // Example: Spoofing a specific MSR (e.g., IA32_PLATFORM_ID)
        if (regs->rcx == 0x80) {
            regs->rax = 0x12345678;
            regs->rdx = 0x9abcdef0;
        }
        else {
            {
                UINT64 value = __readmsr((ULONG)regs->rcx);
                regs->rax = (UINT32)value;
                regs->rdx = (UINT32)(value >> 32);
            }
        }
        VmxAdvanceGuestRip();
        break;
    }

    case EXIT_REASON_CR_ACCESS:
    {
        UINT64 cr_accessed = 0;
        __vmx_vmread(VMCS_EXIT_QUALIFICATION, &cr_accessed);
        if (!(cr_accessed & 1)) { // Read
            if ((cr_accessed & 0x3) == 0) regs->rax = __readcr0();
            if ((cr_accessed & 0x3) == 1) regs->rax = __readcr3();
            if ((cr_accessed & 0x3) == 2) regs->rax = __readcr4();
            if ((cr_accessed & 0x3) == 3) regs->rax = __readcr8();
        }
        else { // Write
            // You MUST actually perform the write or the guest will loop forever
            if ((cr_accessed & 0x3) == 0) __writecr0(regs->rax);
            if ((cr_accessed & 0x3) == 1) __writecr3(regs->rax);
            if ((cr_accessed & 0x3) == 2) __writecr4(regs->rax);
            if ((cr_accessed & 0x3) == 3) __writecr8(regs->rax);
        }
        VmxAdvanceGuestRip();
        break;
    }
    case EXIT_REASON_RDTSC:
    case EXIT_REASON_RDTSCP:
    {
        UINT64 tsc = __rdtsc();
        // Subtract the time spent in the handler itself
        // This is a rough way to hide the VM Exit overhead
        regs->rax = (UINT32)(tsc - 1000);
        regs->rdx = (UINT32)((tsc - 1000) >> 32);
        VmxAdvanceGuestRip();
        break;
    }
    case EXIT_REASON_MSR_WRITE:
    {
        if (regs->rcx == 0x80) {
            // Just pretend we wrote it. Don't actually write to the real MSR.
        }
        else {
            __writemsr((ULONG)regs->rcx, (UINT64)(UINT32)regs->rax |
                       ((UINT64)(UINT32)regs->rdx << 32));
        }
        VmxAdvanceGuestRip();
        break;
    }

    default:
        VmxAdvanceGuestRip();
        break;
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
