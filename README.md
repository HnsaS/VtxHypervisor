# VtxHypervisor

VtxHypervisor is a Windows x64 kernel driver that virtualizes the currently
running Windows instance with Intel VT-x (a type-2 design). The current
implementation has reached a stable operating baseline on the target test
system: the previously observed load-time freezes and crashes are no longer
reproducing. This status describes the configuration tested so far; new
virtualization features will continue to be validated incrementally.

The current driver does not yet implement EPT or nested VMX. It detects and
rejects an already active hypervisor, so it currently runs directly on
supported hardware rather than inside Hyper-V, VBS, or another hypervisor.

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
  execute natively in the guest. XSETBV is validated, and invalid guest
  requests receive #UD or #GP. INVD is handled with WBINVD to avoid discarding
  dirty host cache lines. Intercepted VMX/TXT instructions are reflected as
  #UD; VMXOFF retains the project's guest-unload path.
- RDTSC exiting is enabled so the RDTSC/RDTSCP handler can apply its configured
  fixed TSC offset. This causes frequent VM-exits and can noticeably reduce
  system performance.
- VM-exit handling covers the instructions and controls enabled by this
  implementation. Unsupported exit reasons and handler failures take a fatal
  path. The MSR bitmap leaves ordinary accesses native and intercepts only MSR
  `0x80` for the current test path; because that reserved index is not
  emulated, the handler injects #GP. Other MSRs execute natively. Any MSR
  intended for interception needs validated, allowlisted emulation.
- Startup progress is written to the driver's service registry key as
  `LastStartProgress`. Assembly stages and a launch watchdog provide additional
  in-memory diagnostics.

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
- The current implementation must run without an active outer hypervisor.
  Nested VMX support is a planned feature; exposing nested virtualization in a
  VM alone is not sufficient with the current hypervisor-detection check.

VT-d is not used by the current implementation. Disabling Hyper-V/VBS may be
necessary to ensure Windows does not already own VT-x. Firmware menus use
different names; look for “Intel Virtualization Technology,” “Intel VT-x,” or
a similar setting.

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

The current implementation has been running stably on the target test system.
When changing VMX controls or adding EPT/nested-VMX support, validate those
changes in a recoverable test environment and keep kernel debugging available.

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
viewer. The registry value reports the most recent persisted startup stage; it
is not a per-exit trace.

## Roadmap

Planned development includes:

- **EPT:** add second-level guest-physical memory translation, EPT
  configuration, and handlers for EPT violations and misconfiguration.
- **Nested virtualization:** virtualize the guest-visible VMX instructions,
  VMCS state, and nested VM-exit behavior so another hypervisor can run inside
  the VtxHypervisor guest.
- **Broader VM-exit coverage:** expand exception/event injection and add
  validated, explicit policies for additional MSRs and CPUID leaves.
- **Lifecycle and validation:** improve start/stop and processor hot-plug
  handling, then add repeatable functional and regression checks for supported
  hardware configurations.

## Development notes

Add and validate a VM-exit handler before enabling a control that can cause
that exit. Unknown exits are fatal by design while the handler is incomplete.
Keep VMX region formats, VMCS field encodings, register-save layout, and
assembly/C structure offsets synchronized when changing the low-level path.
