/*
 * arch.h — Intel VT-x architectural definitions (MSRs, VMCS field encodings,
 *          control-register bits, VM-exit reasons).
 *
 * Referência: Intel SDM Vol. 3C, Apêndice B (VMCS field encodings) e
 *             Apêndice C (VM-exit reasons).
 */
#pragma once

#include <ntddk.h>

/* ---------------------------------------------------------------------------
 * MSRs usados na inicialização do VMX
 * ------------------------------------------------------------------------- */
#define MSR_IA32_FEATURE_CONTROL        0x0000003A
#define MSR_IA32_VMX_BASIC              0x00000480
#define MSR_IA32_VMX_PINBASED_CTLS      0x00000481
#define MSR_IA32_VMX_PROCBASED_CTLS     0x00000482
#define MSR_IA32_VMX_EXIT_CTLS          0x00000483
#define MSR_IA32_VMX_ENTRY_CTLS         0x00000484
#define MSR_IA32_VMX_PROCBASED_CTLS2    0x0000048B
#define MSR_IA32_VMX_TRUE_PINBASED_CTLS 0x0000048D
#define MSR_IA32_VMX_TRUE_PROCBASED_CTLS 0x0000048E
#define MSR_IA32_VMX_TRUE_EXIT_CTLS     0x0000048F
#define MSR_IA32_VMX_TRUE_ENTRY_CTLS    0x00000490
#define MSR_IA32_VMX_CR0_FIXED0         0x00000486
#define MSR_IA32_VMX_CR0_FIXED1         0x00000487
#define MSR_IA32_VMX_CR4_FIXED0         0x00000488
#define MSR_IA32_VMX_CR4_FIXED1         0x00000489

#define MSR_IA32_SYSENTER_CS            0x00000174
#define MSR_IA32_SYSENTER_ESP           0x00000175
#define MSR_IA32_SYSENTER_EIP           0x00000176
#define MSR_IA32_DEBUGCTL               0x000001D9
#define MSR_IA32_FS_BASE                0xC0000100
#define MSR_IA32_GS_BASE                0xC0000101

/* IA32_FEATURE_CONTROL bits */
#define FEATURE_CONTROL_LOCK            (1ULL << 0)
#define FEATURE_CONTROL_VMXON_OUTSIDE_SMX (1ULL << 2)

/* CR4.VMXE */
#define CR4_VMXE                        (1ULL << 13)

/* ---------------------------------------------------------------------------
 * VM-exit reasons (os que importam para este driver)
 * ------------------------------------------------------------------------- */
#define EXIT_REASON_EXCEPTION_NMI       0
#define EXIT_REASON_CPUID               10
#define EXIT_REASON_VMCALL              18
#define EXIT_REASON_CR_ACCESS           28
#define EXIT_REASON_MSR_READ            31
#define EXIT_REASON_MSR_WRITE           32
#define EXIT_REASON_RDMSR               EXIT_REASON_MSR_READ
#define EXIT_REASON_RDTSC               0x40
#define EXIT_REASON_RDTSCP              0x41

/* ---------------------------------------------------------------------------
 * VMCS field encodings (Intel SDM Vol.3C Apêndice B)
 * ------------------------------------------------------------------------- */

/* 16-bit guest-state */
#define VMCS_GUEST_ES_SELECTOR          0x0800
#define VMCS_GUEST_CS_SELECTOR          0x0802
#define VMCS_GUEST_SS_SELECTOR          0x0804
#define VMCS_GUEST_DS_SELECTOR          0x0806
#define VMCS_GUEST_FS_SELECTOR          0x0808
#define VMCS_GUEST_GS_SELECTOR          0x080A
#define VMCS_GUEST_LDTR_SELECTOR        0x080C
#define VMCS_GUEST_TR_SELECTOR          0x080E

/* 16-bit host-state */
#define VMCS_HOST_ES_SELECTOR           0x0C00
#define VMCS_HOST_CS_SELECTOR           0x0C02
#define VMCS_HOST_SS_SELECTOR           0x0C04
#define VMCS_HOST_DS_SELECTOR           0x0C06
#define VMCS_HOST_FS_SELECTOR           0x0C08
#define VMCS_HOST_GS_SELECTOR           0x0C0A
#define VMCS_HOST_TR_SELECTOR           0x0C0C

/* 64-bit control */
#define VMCS_IO_BITMAP_A                0x2000
#define VMCS_MSR_BITMAP                 0x2004
#define VMCS_VMCS_LINK_POINTER          0x2800

/* 32-bit control */
#define VMCS_PIN_BASED_VM_EXEC_CONTROL  0x4000
#define VMCS_CPU_BASED_VM_EXEC_CONTROL  0x4002
#define VMCS_EXCEPTION_BITMAP           0x4004
#define VMCS_VM_EXIT_CONTROLS           0x400C
#define VMCS_VM_ENTRY_CONTROLS          0x4012
#define VMCS_SECONDARY_VM_EXEC_CONTROL  0x401E

/* 32-bit read-only data */
#define VMCS_VM_INSTRUCTION_ERROR       0x4400
#define VMCS_EXIT_REASON                0x4402
#define VMCS_VM_EXIT_INSTRUCTION_LEN    0x440C
#define VMCS_HOST_SYSENTER_CS           0x4C00
#define VMCS_EXIT_QUALIFICATION         0x6400

/* 32-bit guest-state */
#define VMCS_GUEST_ES_LIMIT             0x4800
#define VMCS_GUEST_CS_LIMIT             0x4802
#define VMCS_GUEST_SS_LIMIT             0x4804
#define VMCS_GUEST_DS_LIMIT             0x4806
#define VMCS_GUEST_FS_LIMIT             0x4808
#define VMCS_GUEST_GS_LIMIT             0x480A
#define VMCS_GUEST_LDTR_LIMIT           0x480C
#define VMCS_GUEST_TR_LIMIT             0x480E
#define VMCS_GUEST_GDTR_LIMIT           0x4810
#define VMCS_GUEST_IDTR_LIMIT           0x4812
#define VMCS_GUEST_ES_AR_BYTES          0x4814
#define VMCS_GUEST_CS_AR_BYTES          0x4816
#define VMCS_GUEST_SS_AR_BYTES          0x4818
#define VMCS_GUEST_DS_AR_BYTES          0x481A
#define VMCS_GUEST_FS_AR_BYTES          0x481C
#define VMCS_GUEST_GS_AR_BYTES          0x481E
#define VMCS_GUEST_LDTR_AR_BYTES        0x4820
#define VMCS_GUEST_TR_AR_BYTES          0x4822
#define VMCS_GUEST_SYSENTER_CS          0x482A

/* natural-width control */
#define VMCS_CR0_GUEST_HOST_MASK        0x6000
#define VMCS_CR4_GUEST_HOST_MASK        0x6002
#define VMCS_CR0_READ_SHADOW            0x6004
#define VMCS_CR4_READ_SHADOW            0x6006

/* natural-width guest-state */
#define VMCS_GUEST_CR0                  0x6800
#define VMCS_GUEST_CR3                  0x6802
#define VMCS_GUEST_CR4                  0x6804
#define VMCS_GUEST_ES_BASE              0x6806
#define VMCS_GUEST_CS_BASE              0x6808
#define VMCS_GUEST_SS_BASE              0x680A
#define VMCS_GUEST_DS_BASE              0x680C
#define VMCS_GUEST_FS_BASE              0x680E
#define VMCS_GUEST_GS_BASE              0x6810
#define VMCS_GUEST_LDTR_BASE            0x6812
#define VMCS_GUEST_TR_BASE              0x6814
#define VMCS_GUEST_GDTR_BASE            0x6816
#define VMCS_GUEST_IDTR_BASE            0x6818
#define VMCS_GUEST_DR7                  0x681A
#define VMCS_GUEST_RSP                  0x681C
#define VMCS_GUEST_RIP                  0x681E
#define VMCS_GUEST_RFLAGS               0x6820
#define VMCS_GUEST_SYSENTER_ESP         0x6824
#define VMCS_GUEST_SYSENTER_EIP         0x6826

/* natural-width host-state */
#define VMCS_HOST_CR0                   0x6C00
#define VMCS_HOST_CR3                   0x6C02
#define VMCS_HOST_CR4                   0x6C04
#define VMCS_HOST_FS_BASE               0x6C06
#define VMCS_HOST_GS_BASE               0x6C08
#define VMCS_HOST_TR_BASE               0x6C0A
#define VMCS_HOST_GDTR_BASE             0x6C0C
#define VMCS_HOST_IDTR_BASE             0x6C0E
#define VMCS_HOST_SYSENTER_ESP          0x6C10
#define VMCS_HOST_SYSENTER_EIP          0x6C12
#define VMCS_HOST_RSP                   0x6C14
#define VMCS_HOST_RIP                   0x6C16

/* ---------------------------------------------------------------------------
 * Bits de controle usados
 * ------------------------------------------------------------------------- */
#define CPU_BASED_ACTIVATE_SECONDARY_CONTROLS (1UL << 31)
#define SECONDARY_EXEC_ENABLE_RDTSCP          (1UL << 3)
#define SECONDARY_EXEC_ENABLE_XSAVES          (1UL << 20)

#define VM_EXIT_HOST_ADDR_SPACE_SIZE          (1UL << 9)   /* host é 64-bit */
#define VM_ENTRY_IA32E_MODE                   (1UL << 9)   /* guest é 64-bit */

#define PAGE_SIZE_4K                          0x1000
 
 /* CPU-Based VM-Execution Controls */
#define CPU_BASED_VM_EXEC_CONTROL_CPUID               (1UL << 0)
#define CPU_BASED_VM_EXEC_CONTROL_MSR_READ            (1UL << 2)
#define CPU_BASED_VM_EXEC_CONTROL_MSR_WRITE           (1UL << 3)
#define CPU_BASED_VM_EXEC_CONTROL_CR_ACCESS           (1UL << 10)



