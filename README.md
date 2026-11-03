# VtxHypervisor

VtxHypervisor is an experimental Windows x64 kernel driver that explores Intel
VT-x virtualization of an already running Windows system (a type-2 design).
It is a learning and debugging project, not a production hypervisor. It does
not currently implement EPT, nested virtualization, or general VM-exit
emulation.

> **Stability:** this code can hang or crash the machine. Use a disposable test
> system or a virtual machine with nested VMX enabled and a recoverable
> snapshot. Do not test it on a machine containing data you cannot afford to
> lose. Hyper-V/VBS and an already active hypervisor are unsupported.

## Current implementation

- Intel VT-x setup: `VMXON`, one VMCS per logical processor, and `VMLAUNCH`.
- Logical processors are initialized serially while the startup thread is
  pinned to each processor. All vCPU resources are allocated before the first
  processor enters VMX operation.
- The existing Windows kernel continues as the guest. VM-exit host state uses
  the current Windows CR3, GDT, and IDT, with a per-vCPU VMM stack.
- CPUID exits are handled. Leaf 0 reports the Intel vendor string and retains
  the host's maximum basic leaf; leaf 1 hides VMX, SMX, and the
  hypervisor-present bit. Extended brand leaves return the configured generic
  `Intel(R) Core(TM) i7` string. RDTSCP, INVPCID, WAITPKG, and XSAVES feature
  bits are exposed only when their corresponding VMX controls were accepted.
- Selected secondary controls are constrained by the VMX capability MSR.
  CPUID feature bits are masked when their corresponding VMX controls were not
  accepted. XSAVES/XRSTORS are enabled with an empty XSS-exiting bitmap so they
  execute natively in the guest. XSETBV is validated and invalid guest requests receive
  #UD or #GP. INVD is handled with WBINVD to avoid discarding dirty host cache
  lines. Intercepted VMX/TXT instructions are reflected as #UD; VMXOFF retains
  the project's guest-unload path.
- RDTSC exiting is enabled so the RDTSC/RDTSCP handler can apply its configured
  fixed TSC offset. This causes frequent VM-exits and can noticeably reduce
  system performance.
- VM-exit handling remains incomplete. Unsupported exit reasons and handler
  failures take a fatal path; there is no EPT, nested VMX, or general-purpose
  exception/event reinjection framework. The MSR bitmap leaves ordinary
  accesses native and intercepts only MSR `0x80` for the current test path;
  because that reserved index is not emulated, the handler injects #GP. Other MSRs execute natively. Any MSR intended to
  work through interception needs validated, allowlisted emulation.
- Startup progress is written to the driver's service registry key as
  `LastStartProgress`. Assembly stages and a launch watchdog provide additional
  in-memory diagnostics, but those volatile values do not survive a hard
  freeze or forced reboot.

This project is not a complete or stable hypervisor. In particular, a
successful build does not prove that `VMLAUNCH` or the first VM-exit will work
on a given CPU, firmware, or Windows configuration.

## Source files

| File | Responsibility |
|---|---|
| `driver.c` | WDM `DriverEntry`/unload routines, startup breadcrumbs, and launch watchdog. |
| `vmx.c` | VT-x capability checks, VMXON/VMCS setup, per-CPU launch, and shutdown. |
| `vmexit.c` | Supported VM-exit handling and failure diagnostics. |
| `asm.asm` | MASM launch and VM-exit stubs, register save/restore, and segment helpers. |
| `arch.h` | Intel MSR numbers, VMCS encodings, control bits, and exit reasons. |
| `vmx.h` | Per-vCPU/global structures, progress stages, and C/assembly interfaces. |
| `cpuid_probe.cpp` | Standalone user-mode utility to inspect CPUID values returned to Windows. |

## Requirements

- 64-bit Windows and an Intel processor that supports VT-x.
- Intel Virtualization Technology (VT-x) enabled in firmware/BIOS.
- Visual Studio with the Windows Driver Kit (WDK) matching the selected target
  SDK, plus x64 MASM build support.
- For a virtual-machine test, nested virtualization must be exposed to the
  guest. This driver currently rejects CPUID configurations that report an
  active hypervisor, so that check must be adapted deliberately for nested-VMX
  testing.

VT-d is not required by the current implementation because it does not use
DMA remapping or EPT. Disabling Hyper-V/VBS may be necessary to ensure Windows
does not already own VT-x. Firmware menus use different names; look for
“Intel Virtualization Technology,” “Intel VT-x,” or a similar setting.

## Build

Open `VtxHypervisor.vcxproj` in Visual Studio, select **x64 / Debug** or
**x64 / Release**, and build. The project includes `asm.asm` as a Microsoft
Macro Assembler input.

The default output is:

```text
x64\Debug\VtxHypervisor.sys
```

Command-line build from a WDK-enabled Developer Command Prompt:

```bat
msbuild VtxHypervisor.vcxproj /t:Rebuild /p:Configuration=Debug /p:Platform=x64
```

The Debug configuration does not automatically sign the driver. Test-signing
mode alone does not sign a `.sys` file; Windows still requires an acceptable
test certificate and catalog/signing setup for the target machine.

## Test setup

Use an isolated test machine or VM. Before loading, arrange kernel debugging
and crash-dump collection if available, and take a VM snapshot. Kernel
virtualization failures can prevent normal system recovery.

After signing the driver for the test machine, it can be registered as a
kernel service from an elevated command prompt (replace the path as needed):

```bat
sc.exe create VtxHypervisor type= kernel binPath= "C:\path\VtxHypervisor.sys"
sc.exe start VtxHypervisor
```

To remove the service after it has been stopped:

```bat
sc.exe stop VtxHypervisor
sc.exe delete VtxHypervisor
```

Startup breadcrumbs are stored under the service key at
`HKLM\SYSTEM\CurrentControlSet\Services\VtxHypervisor`, in the
`LastStartProgress` value. Launch preparation has separate persistent markers
for the beginning and successful completion of each `VMWRITE` (`RFLAGS`, guest
`RSP`, and guest `RIP`). A `*_FAILED` marker means the corresponding `VMWRITE`
returned a VMfail status. `VMLAUNCH_INSTRUCTION` means all three writes
returned successfully and that marker was flushed immediately before the
`VMLAUNCH` instruction; it cannot prove the CPU retired the instruction. A
successful `VMLAUNCH_RETURNED` marker is written only after the guest trampoline
returns to C. Debug output can be viewed with WinDbg or a kernel-debug-output
viewer. A hard freeze may leave no dump and does not guarantee that the watchdog
can run.

## Extending the implementation

Add and validate a VM-exit handler before enabling a control that can cause
that exit. Unknown exits are fatal by design while the handler is incomplete.
Keep VMX region formats, VMCS field encodings, register-save layout, and
assembly/C structure offsets synchronized when changing the low-level path.
