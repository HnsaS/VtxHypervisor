# VtxHypervisor
hwid spoofer hypervisor

A thin, type-2 kernel hypervisor for Windows x64 using Intel **VT-x**.
When loaded, the driver virtualizes the running OS on all logical processors
with `VMLAUNCH` and installs a **VM-exit handler** that reports each requested
VM-exit and resumes the VM.

> Educational/research project. Requires an **Intel CPU with VT-x enabled in
> the BIOS**. It does not run while Hyper-V/VBS is active (they already occupy
> VMX root); test in a nested VM, on a dedicated machine, or with Hyper-V
> disabled.

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

1. `DriverEntry` calls `VmxInitializeAllProcessors()` and pins the thread to
   each logical processor.
2. On each processor: `VMXON` → populate the VMCS (guest = current CPU state,
   host = VMM) → `AsmVmxLaunch()` executes `VMLAUNCH`.
3. On success, the guest continues running Windows. Each VM-exit reaches
   `AsmVmexitHandler` (assembly) → `VmExitHandler` (C), which prints:
   ```
   [Hvmx] VM-exit solicitada no core N (reason=R)
   ```
   The handler then resumes the VM. CPUID leaves 0 and 1 are handled by the
   guest handler; other leaves are queried from the host with `__cpuidex`.

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

View VM-exit messages in **DebugView** (enable *Capture Kernel*) or through
WinDbg. To unload:

```bat
sc stop vtxhv
sc delete vtxhv
```

## What to modify

- **Intercept more events**: enable bits in `VMCS_CPU_BASED_VM_EXEC_CONTROL`
  (for example, `RDMSR`/`WRMSR` via the MSR bitmap, or CR3 access) in `vmx.c`.
- **Handle a specific exit**: add a `case` to `VmExitHandler` (`vmexit.c`). The
  exit reasons are in `arch.h`.
- **Make a full driver (with a device/IOCTL)**: create `\Device\...` in
  `driver.c`.

## Warnings

- `MmAllocateContiguousMemory` is used for VMXON/VMCS and `ExAllocatePoolZero`
  for the VMM stack; everything is freed on unload.
- An invalid VMCS causes `VMLAUNCH` to fail with a *VM-instruction error*
  (printed in the log). A failed `VMRESUME` intentionally triggers bug check
  `0x1337`.
- Always test in a VM/kernel-debug environment. A hypervisor bug can crash the
  machine.
