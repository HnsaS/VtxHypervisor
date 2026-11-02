# VtxHypervisor
hwid spoofer hypervisor

A minimal, experimental type-2 kernel hypervisor for Windows x64 using Intel
**VT-x**. The current diagnostic configuration intercepts **CPUID only** and
does not implement EPT or general VM-exit emulation.

> Educational/research project. Requires an **Intel CPU with VT-x enabled in
> the BIOS**. Startup refuses when CPUID reports an existing hypervisor; this
> project has no nested-VMX support. Test in a nested VM only if nested VMX is
> exposed and the detection check is deliberately adapted for that setup, or
> on a dedicated machine with Hyper-V/VBS disabled.

## Files

| File | Contents |
|---|---|
| `driver.c` | `DriverEntry` / `DriverUnload`. |
| `vmx.c` | VMX support, `VMXON`, VMCS setup, `VMLAUNCH`, and shutdown. |
| `vmexit.c` | VM-exit handler. |
| `asm.asm` | x64 stubs: `AsmVmxLaunch`, `AsmVmexitHandler`, and segment readers. |
| `arch.h` | MSRs, VMCS encodings, control bits, and VM-exit reasons. |
| `vmx.h` | Per-vCPU structures and prototypes. |

## How it works

1. `DriverEntry` calls `VmxInitializeAllProcessors()`, allocates every vCPU's
   resources, and starts all logical processors through a synchronized DPC
   broadcast.
2. On each processor: `VMXON` → populate the VMCS (guest = current CPU state,
   host = VMM) → `AsmVmxLaunch()` executes `VMLAUNCH`.
3. On success, Windows continues as the guest. CPUID causes a VM-exit, is
   emulated by `VmExitHandler`, and then returns through `VMRESUME`. Other
   VM-exits are not currently implemented; the handler records the failure
   context and stops through a bug check rather than retrying an unknown exit.

## Build

Requires **Visual Studio + WDK** and **MASM** support.

1. Open `VtxHypervisor.vcxproj` in Visual Studio (or create an *Empty WDM
   Driver* project and add the files).
2. `asm.asm` is already configured as a **Microsoft Macro Assembler** item in
   the `.vcxproj`; MASM integration is supplied by the WDK.
3. Select **x64 / Debug** or **Release**, then choose **Build**.
4. Output: `x64\Debug\VtxHypervisor.sys`.

The **Debug** configuration is set not to sign the driver automatically. On
Windows x64, the file needs a valid signature before it can be loaded; enabling
`TESTSIGNING` by itself does not sign the `.sys` file.

### Command line (EWDK)

```bat
msbuild VtxHypervisor.vcxproj /p:Configuration=Release /p:Platform=x64
```

## Install / test (on a test machine!)

Sign the `.sys` file with a test certificate and enable test signing on the test
machine before loading it as a kernel service:

```bat
bcdedit /set testsigning on
```
(Restart.)

```bat
sc create vtxhv type= kernel binPath= C:\path\VtxHypervisor.sys
sc start vtxhv
```

Startup messages go to **DebugView** (enable *Capture Kernel*) or WinDbg.
Fatal VM-exit details are recorded for kernel-dump analysis. To unload:

```bat
sc stop vtxhv
sc delete vtxhv
```

## What to modify

- **Intercept more events**: implement and validate the corresponding VM-exit
  handler first, then add its control bit in `vmx.c`. Startup intentionally
  rejects mandatory controls outside the currently implemented CPUID path.
- **Handle a specific exit**: add a `case` to `VmExitHandler` (`vmexit.c`). The
  exit reasons are in `arch.h`.
- **Make a full driver (with a device/IOCTL)**: create `\Device\...` in
  `driver.c`.

## Warnings

- VMXON/VMCS pages and all VMM stacks are allocated before any CPU enters VMX;
  the stack uses non-executable nonpaged pool. VMX regions are kept below 4 GiB.
- An invalid VMCS causes `VMLAUNCH` to fail with a *VM-instruction error*
  (printed in the log). A failed `VMRESUME` intentionally triggers bug check
  `0x1337`.
- Always test in a VM/kernel-debug environment. A hypervisor bug can crash the
  machine.
