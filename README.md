# VtxHypervisor
hwid spoofer hypervisor

Hypervisor de kernel (type-2, "thin") para Windows x64 usando Intel **VT-x**.
Ao carregar, o driver virtualiza o próprio SO em todos os cores via `VMLAUNCH`
e instala um **handler de VM-exit** que **avisa toda vez que um VM-exit é
solicitado** e resume a VM.

> Projeto educacional / de pesquisa. Requer CPU **Intel com VT-x habilitado na
> BIOS**. Não roda com Hyper-V/VBS ativo (eles já ocupam o VMX root) — teste em
> VM aninhada, máquina dedicada ou com Hyper-V desligado.

## Arquivos

| Arquivo        | Conteúdo                                                        |
|----------------|-----------------------------------------------------------------|
| `driver.c`     | `DriverEntry` / `DriverUnload`.                                 |
| `vmx.c`        | Suporte a VMX, `VMXON`, montagem da VMCS, `VMLAUNCH`, shutdown. |
| `vmexit.c`     | Handler de VM-exit (o "aviso" de exit).                         |
| `asm.asm`      | Stubs x64: `AsmVmxLaunch`, `AsmVmexitHandler`, leitura de segmentos. |
| `arch.h`       | MSRs, encodings da VMCS, bits de controle, exit reasons.        |
| `vmx.h`        | Estruturas por-vCPU e protótipos.                               |

## Como funciona (resumo)

1. `DriverEntry` → `VmxInitializeAllProcessors()` fixa a thread em cada core.
2. Por core: `VMXON` → preenche a VMCS (guest = estado atual da CPU, host =
   VMM) → `AsmVmxLaunch()` executa `VMLAUNCH`.
3. No sucesso, o guest continua o Windows normalmente; cada VM-exit cai em
   `AsmVmexitHandler` (asm) → `VmExitHandler` (C), que imprime:
   ```
   [Hvmx] VM-exit solicitada no core N (reason=R)
   ```
   e resume a VM. (CPUID é reexecutado p/ a VM não quebrar; qualquer outro
   exit apenas avança o RIP.)

## Build

Precisa de **Visual Studio + WDK** e suporte ao **MASM**.

1. Abra `VtxHypervisor.vcxproj` no Visual Studio (ou crie um projeto
   *Empty WDM Driver* e adicione os arquivos).
2. `asm.asm` já está configurado como item **Microsoft Macro Assembler** no
   `.vcxproj`; a integração MASM é fornecida pelo WDK.
3. Configuração **x64 / Debug** ou **Release** → **Build**.
4. Saída: `x64\Debug\VtxHypervisor.sys`.

O build **Debug** está configurado para não assinar automaticamente o driver.
Em Windows x64, o arquivo precisa de uma assinatura válida antes de poder ser
carregado; habilitar `TESTSIGNING` sozinho não assina o `.sys`.

### Linha de comando (EWDK)
```bat
msbuild VtxHypervisor.vcxproj /p:Configuration=Release /p:Platform=x64
```

## Instalar / testar (em máquina de teste!)

Assine o `.sys` com um certificado de teste e habilite test-signing na máquina
de teste antes de carregá-lo como serviço de kernel:

```bat
bcdedit /set testsigning on
```
(reinicie)

```bat
sc create vtxhv type= kernel binPath= C:\caminho\VtxHypervisor.sys
sc start vtxhv
```

Veja os avisos de VM-exit no **DebugView** (marque *Capture Kernel*) ou via
WinDbg. Para descarregar:

```bat
sc stop vtxhv
sc delete vtxhv
```

## O que modificar

- **Interceptar mais coisas**: ative bits em `VMCS_CPU_BASED_VM_EXEC_CONTROL`
  (ex.: `RDMSR`/`WRMSR` via MSR bitmap, acesso a CR3, etc.) em `vmx.c`.
- **Reagir a um exit específico**: adicione `case` em `VmExitHandler`
  (`vmexit.c`). As exit reasons estão em `arch.h`.
- **Driver de verdade (com device/IOCTL)**: criar `\Device\...` em `driver.c`.

## Avisos

- `MmAllocateContiguousMemory` para VMXON/VMCS e `ExAllocatePoolZero` para a
  pilha do VMM: tudo liberado no unload.
- Um erro na VMCS faz `VMLAUNCH` falhar com um *VM-instruction-error* (impresso
  no log). `VMRESUME` falho dispara um bugcheck proposital `0x1337`.
- Teste sempre em VM/kernel-debug. Bug em hypervisor = travamento da máquina.
