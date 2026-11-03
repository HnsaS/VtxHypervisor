/*
 * driver.c — ponto de entrada do driver WDM.
 *
 * DriverEntry:  checa VT-x e inicia o hypervisor em todos os cores (VMLAUNCH).
 * DriverUnload: executa VMXOFF e libera tudo.
 *
 * O driver "padrão" não expõe IOCTLs — ele só sobe a VM ao carregar. Depois
 * é só modificar à vontade.
 */

#include "vmx.h"
#include <ntstrsafe.h>

DRIVER_UNLOAD DriverUnload;

static HANDLE g_HvProgressKey;
static KTIMER g_HvLaunchWatchdogTimer;
static KDPC g_HvLaunchWatchdogDpc;
static volatile LONG g_HvLaunchWatchdogBugcheckIssued;
static volatile LONG g_HvLaunchWatchdogActive;
static volatile LONG g_HvLaunchWatchdogInitialized;
static volatile LONG g_HvRuntimeWatchdogActive;
static volatile LONG g_HvRuntimeWatchdogBugcheckIssued;

#define HV_RUNTIME_WATCHDOG_PERIOD_MS       250
#define HV_RUNTIME_VMEXIT_TIMEOUT_100NS     (2LL * 1000LL * 1000LL * 10LL)
#define HV_BUGCHECK_RUNTIME_VMEXIT_STALLED  0x00001339

static PCWSTR HvProgressStageName(HV_PROGRESS_STAGE stage)
{
    switch (stage)
    {
    case HvProgressDriverEntry: return L"DRIVER_ENTRY";
    case HvProgressCpuBegin: return L"CPU_BEGIN";
    case HvProgressVmxonComplete: return L"VMXON_COMPLETE";
    case HvProgressVmcsReady: return L"VMCS_READY";
    case HvProgressVmlaunchBegin: return L"VMLAUNCH_BEGIN";
    case HvProgressVmlaunchReturned: return L"VMLAUNCH_RETURNED";
    case HvProgressCpuComplete: return L"CPU_COMPLETE";
    case HvProgressInitializationFailed: return L"INITIALIZATION_FAILED";
    case HvProgressInitializationComplete: return L"INITIALIZATION_COMPLETE";
    case HvProgressVmwriteRflagsBegin: return L"VMWRITE_RFLAGS_BEGIN";
    case HvProgressVmwriteRflagsDone: return L"VMWRITE_RFLAGS_DONE";
    case HvProgressVmwriteRspBegin: return L"VMWRITE_GUEST_RSP_BEGIN";
    case HvProgressVmwriteRspDone: return L"VMWRITE_GUEST_RSP_DONE";
    case HvProgressVmwriteRipBegin: return L"VMWRITE_GUEST_RIP_BEGIN";
    case HvProgressVmwriteRipDone: return L"VMWRITE_GUEST_RIP_DONE";
    case HvProgressVmlaunchInstruction: return L"VMLAUNCH_INSTRUCTION";
    case HvProgressVmwriteRflagsFailed: return L"VMWRITE_RFLAGS_FAILED";
    case HvProgressVmwriteRspFailed: return L"VMWRITE_GUEST_RSP_FAILED";
    case HvProgressVmwriteRipFailed: return L"VMWRITE_GUEST_RIP_FAILED";
    default: return L"UNKNOWN";
    }
}

static VOID HvLaunchWatchdogDpcRoutine(
    PKDPC dpc,
    PVOID deferredContext,
    PVOID systemArgument1,
    PVOID systemArgument2)
{
    LONG64 startTime;
    ULONG64 now;
    ULONG64 elapsed100ns;

    UNREFERENCED_PARAMETER(dpc);
    UNREFERENCED_PARAMETER(deferredContext);
    UNREFERENCED_PARAMETER(systemArgument1);
    UNREFERENCED_PARAMETER(systemArgument2);

    if (InterlockedCompareExchange(&g_VmxLaunchInProgress, 0, 0) == 0)
        return;

    startTime = InterlockedCompareExchange64(&g_VmxLaunchStartTime, 0, 0);
    now = KeQueryInterruptTime();
    if (startTime <= 0 || now < (ULONG64)startTime)
        return;

    elapsed100ns = now - (ULONG64)startTime;
    if (elapsed100ns < 15ULL * 1000ULL * 1000ULL * 10ULL)
        return;

    if (InterlockedCompareExchange(&g_HvLaunchWatchdogBugcheckIssued, 1, 0) == 0)
    {
        /* Bugcheck 0xE2 with trace stage, CPU, VM-exit count, and elapsed ms. */
        KeBugCheckEx(0x000000E2,
            (ULONG_PTR)InterlockedCompareExchange(&g_VmxProgressStage, 0, 0),
            (ULONG_PTR)InterlockedCompareExchange(&g_VmxProgressProcessor, -1, -1),
            (ULONG_PTR)InterlockedCompareExchange(&g_VmxVmexitCount, 0, 0),
            (ULONG_PTR)(elapsed100ns / 10000ULL));
    }
}

VOID HvLaunchWatchdogStart(ULONG launchingProcessorIndex)
{
    LARGE_INTEGER dueTime;
    ULONG i, processorCount;
    PROCESSOR_NUMBER targetProcessor;
    BOOLEAN targetSelected = FALSE;
    NTSTATUS targetStatus;

    if (InterlockedCompareExchange(&g_HvLaunchWatchdogInitialized, 1, 0) == 0)
    {
        KeInitializeTimerEx(&g_HvLaunchWatchdogTimer, NotificationTimer);
        KeInitializeDpc(&g_HvLaunchWatchdogDpc, HvLaunchWatchdogDpcRoutine, NULL);
    }
    else
    {
        (VOID)KeCancelTimer(&g_HvLaunchWatchdogTimer);
        /* Ensure the previous timer DPC is no longer queued before retargeting. */
        KeFlushQueuedDpcs();
    }
    InterlockedExchange(&g_HvLaunchWatchdogBugcheckIssued, 0);
    processorCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    for (i = 0; i < processorCount; ++i)
    {
        if (i == launchingProcessorIndex)
            continue;
        if (KeGetProcessorNumberFromIndex(i, &targetProcessor) != STATUS_SUCCESS)
            continue;

        targetStatus = KeSetTargetProcessorDpcEx(&g_HvLaunchWatchdogDpc,
                                                  &targetProcessor);
        if (NT_SUCCESS(targetStatus))
        {
            targetSelected = TRUE;
            break;
        }
    }

    /* On a one-CPU machine the DPC cannot monitor a CPU stuck with interrupts
       disabled. Still arm it as a best effort; multi-CPU systems target a
       different processor explicitly. */
    UNREFERENCED_PARAMETER(targetSelected);
    dueTime.QuadPart = -10000000LL; /* first check in one second */
    KeSetTimerEx(&g_HvLaunchWatchdogTimer, dueTime, 1000,
                 &g_HvLaunchWatchdogDpc);
    InterlockedExchange(&g_HvLaunchWatchdogActive, 1);
}

VOID HvLaunchWatchdogStop(VOID)
{
    if (InterlockedExchange(&g_HvLaunchWatchdogActive, 0) != 0)
    {
        (VOID)KeCancelTimer(&g_HvLaunchWatchdogTimer);
        KeFlushQueuedDpcs();
    }
}

static VOID HvRuntimeWatchdogDpcRoutine(
    PKDPC dpc,
    PVOID deferredContext,
    PVOID systemArgument1,
    PVOID systemArgument2)
{
    ULONG i;
    ULONG64 now;

    UNREFERENCED_PARAMETER(dpc);
    UNREFERENCED_PARAMETER(deferredContext);
    UNREFERENCED_PARAMETER(systemArgument1);
    UNREFERENCED_PARAMETER(systemArgument2);

    if (InterlockedCompareExchange(&g_HvRuntimeWatchdogActive, 0, 0) == 0 ||
        !g_Hv.VcpuArray)
        return;

    now = KeQueryInterruptTime();
    for (i = 0; i < g_Hv.ProcessorCount; ++i)
    {
        PVCPU vcpu = &g_Hv.VcpuArray[i];
        LONG active = InterlockedCompareExchange(&vcpu->RuntimeVmExitActive,
                                                  0, 0);
        LONG64 startTime;

        if (active == 0)
            continue;

        startTime = InterlockedCompareExchange64(
            &vcpu->RuntimeVmExitStartTime, 0, 0);
        if (startTime == 0)
        {
            /* The assembly entry marks the VCPU before C can take a timestamp. */
            InterlockedCompareExchange64(&vcpu->RuntimeVmExitStartTime,
                                         (LONG64)now, 0);
            continue;
        }
        if (startTime < 0 || now < (ULONG64)startTime ||
            now - (ULONG64)startTime < HV_RUNTIME_VMEXIT_TIMEOUT_100NS)
            continue;

        if (InterlockedCompareExchange(&g_HvRuntimeWatchdogBugcheckIssued,
                                       1, 0) == 0)
        {
            /* 0x1339: VM-exit path remained active for two seconds.
               Arguments: CPU, assembly/C stage, exit reason, guest RIP. */
            KeBugCheckEx(HV_BUGCHECK_RUNTIME_VMEXIT_STALLED,
                vcpu->Diagnostics.ProcessorIndex,
                (ULONG_PTR)InterlockedCompareExchange(
                    &vcpu->RuntimeVmExitStage, 0, 0),
                (ULONG_PTR)vcpu->Diagnostics.ExitReason,
                (ULONG_PTR)vcpu->Diagnostics.GuestRip);
        }
        return;
    }
}

VOID HvRuntimeWatchdogStart(VOID)
{
    ULONG i;
    ULONG started = 0;
    LARGE_INTEGER dueTime;

    if (!g_Hv.VcpuArray || g_Hv.ProcessorCount == 0 ||
        InterlockedCompareExchange(&g_HvRuntimeWatchdogActive, 1, 0) != 0)
        return;

    InterlockedExchange(&g_HvRuntimeWatchdogBugcheckIssued, 0);
    dueTime.QuadPart = -((LONGLONG)HV_RUNTIME_WATCHDOG_PERIOD_MS * 10000LL);

    for (i = 0; i < g_Hv.ProcessorCount; ++i)
    {
        PVCPU vcpu = &g_Hv.VcpuArray[i];
        PROCESSOR_NUMBER processorNumber;
        NTSTATUS status;

        if (!vcpu->RuntimeWatchdogTimerInitialized)
        {
            KeInitializeTimerEx(&vcpu->RuntimeWatchdogTimer,
                                NotificationTimer);
            KeInitializeDpc(&vcpu->RuntimeWatchdogDpc,
                            HvRuntimeWatchdogDpcRoutine, vcpu);
            vcpu->RuntimeWatchdogTimerInitialized = TRUE;
        }

        if (!NT_SUCCESS(KeGetProcessorNumberFromIndex(i, &processorNumber)))
            continue;

        status = KeSetTargetProcessorDpcEx(&vcpu->RuntimeWatchdogDpc,
                                           &processorNumber);
        if (!NT_SUCCESS(status))
            continue;

        KeSetTimerEx(&vcpu->RuntimeWatchdogTimer, dueTime,
                     HV_RUNTIME_WATCHDOG_PERIOD_MS,
                     &vcpu->RuntimeWatchdogDpc);
        ++started;
    }

    if (started == 0)
        InterlockedExchange(&g_HvRuntimeWatchdogActive, 0);
}

VOID HvRuntimeWatchdogStop(VOID)
{
    ULONG i;

    if (InterlockedExchange(&g_HvRuntimeWatchdogActive, 0) == 0)
        return;

    if (g_Hv.VcpuArray)
    {
        for (i = 0; i < g_Hv.ProcessorCount; ++i)
        {
            PVCPU vcpu = &g_Hv.VcpuArray[i];
            if (vcpu->RuntimeWatchdogTimerInitialized)
                KeCancelTimer(&vcpu->RuntimeWatchdogTimer);
        }
    }

    /* Ensure no watchdog DPC can still access VCPU state before it is freed. */
    KeFlushQueuedDpcs();
}

VOID HvProgressOpen(PUNICODE_STRING serviceRegistryPath)
{

    if (serviceRegistryPath == NULL || serviceRegistryPath->Buffer == NULL) {
        DbgPrint("[Hvmx] No registry path provided; skipping progress logging.\n");
        return;
    }

    OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    if (!serviceRegistryPath || !serviceRegistryPath->Buffer ||
        serviceRegistryPath->Length == 0 || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;

    InitializeObjectAttributes(&attributes, serviceRegistryPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwOpenKey(&g_HvProgressKey,
        KEY_QUERY_VALUE | KEY_SET_VALUE, &attributes);
    if (!NT_SUCCESS(status))
        g_HvProgressKey = NULL;
}

VOID HvProgressWrite(ULONG processorIndex, HV_PROGRESS_STAGE stage, NTSTATUS status)
{
    static const WCHAR valueNameBuffer[] = L"LastStartProgress";
    WCHAR text[128];
    UNICODE_STRING valueName;
    size_t chars;

    if (!g_HvProgressKey || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;

    if (!NT_SUCCESS(RtlStringCchPrintfW(text, RTL_NUMBER_OF(text),
        L"cpu=%lu;stage=%s;status=0x%08X", processorIndex,
        HvProgressStageName(stage), (ULONG)status)))
        return;

    RtlInitUnicodeString(&valueName, valueNameBuffer);
    if (!NT_SUCCESS(RtlStringCchLengthW(text, RTL_NUMBER_OF(text), &chars)))
        return;

    if (NT_SUCCESS(ZwSetValueKey(g_HvProgressKey, &valueName, 0, REG_SZ,
        text, (ULONG)((chars + 1) * sizeof(WCHAR)))))
    {
        /* Force the breadcrumb to the SYSTEM hive before entering VMX. */
        (VOID)ZwFlushKey(g_HvProgressKey);
    }
}

/* Called only by AsmVmxLaunch before entering VMX non-root. That path is
   still at PASSIVE_LEVEL, so the registry breadcrumb can be flushed safely. */
VOID HvProgressWriteLaunchStage(ULONG stage)
{
    LONG processorIndex = InterlockedCompareExchange(&g_VmxProgressProcessor,
                                                      -1, -1);

    if (processorIndex >= 0)
        HvProgressWrite((ULONG)processorIndex, (HV_PROGRESS_STAGE)stage,
                        STATUS_SUCCESS);
}

VOID HvProgressClose(VOID)
{
    if (g_HvProgressKey)
    {
        ZwClose(g_HvProgressKey);
        g_HvProgressKey = NULL;
    }
}

typedef struct _HV_START_ERROR_DATA
{
    ULONG ProcessorIndex;
    UINT64 VmxInstructionError;
    INT32 VmxInstructionErrorReadStatus;
    UINT32 FailureStage;
    UINT32 FailedVmcsField;
    INT32 VmxOperationStatus;
    UINT64 FailureDetail;
} HV_START_ERROR_DATA, *PHV_START_ERROR_DATA;

static VOID VmxWriteStartErrorLog(PDRIVER_OBJECT driverObject, NTSTATUS status)
{
    UCHAR entrySize = (UCHAR)(sizeof(IO_ERROR_LOG_PACKET) + sizeof(HV_START_ERROR_DATA));
    PIO_ERROR_LOG_PACKET packet;
    HV_START_ERROR_DATA data;

    packet = (PIO_ERROR_LOG_PACKET)IoAllocateErrorLogEntry(driverObject, entrySize);
    if (!packet)
        return;

    RtlZeroMemory(packet, entrySize);
    RtlZeroMemory(&data, sizeof(data));
    data.ProcessorIndex = g_Hv.FailedProcessorIndex;
    data.VmxInstructionError = g_Hv.LastVmxInstructionError;
    if (g_Hv.VcpuArray && g_Hv.FailedProcessorIndex < g_Hv.ProcessorCount)
    {
        PVMEXIT_DIAGNOSTICS diagnostics =
            &g_Hv.VcpuArray[g_Hv.FailedProcessorIndex].Diagnostics;
        data.FailureStage = diagnostics->FailureStage;
        data.FailedVmcsField = diagnostics->FailedVmcsField;
        data.VmxInstructionErrorReadStatus = diagnostics->VmInstructionErrorReadStatus;
        data.VmxOperationStatus = diagnostics->VmxOperationStatus;
        data.FailureDetail = diagnostics->FailureDetail;
    }

    packet->ErrorCode = status;
    packet->FinalStatus = status;
    packet->UniqueErrorValue = 0x56545801;
    packet->DumpDataSize = sizeof(data);
    RtlCopyMemory(packet->DumpData, &data, sizeof(data));
    IoWriteErrorLogEntry(packet);
}

// Change the signature so it doesn't require a PDRIVER_OBJECT
VOID ShutdownHypervisor()
{
    HvRuntimeWatchdogStop();
    VmxShutdownAllProcessors();
    HvProgressClose();
    DbgPrint("[Hvmx] hypervisor descarregado manualmente.\n");
}

VOID DriverUnload(PDRIVER_OBJECT driverObject)
{
    UNREFERENCED_PARAMETER(driverObject);
    HvRuntimeWatchdogStop();
    VmxShutdownAllProcessors();
    HvProgressClose();
    DbgPrint("[Hvmx] driver descarregado.\n");
}
// driver.c

// driver.c

// 1. Move all your actual logic here
// 1. FIXED: Now accepts both parameters so it can use driverObject for logging
NTSTATUS InitializeHypervisor(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
    NTSTATUS status;

    DbgPrint("[Hvmx] Iniciando hypervisor VT-x...\n");

    // HvProgressOpen must be written to handle NULL registryPath safely
    HvProgressOpen(registryPath);
    HvProgressWrite(MAXULONG, HvProgressDriverEntry, STATUS_SUCCESS);

    status = VmxInitializeAllProcessors();
    HvLaunchWatchdogStop();

    if (!NT_SUCCESS(status))
    {
        HvProgressWrite(g_Hv.FailedProcessorIndex,
            HvProgressInitializationFailed, status);

        DbgPrint("[Hvmx] falha ao inicializar VT-x (0x%08X, cpu=%lu).\n",
            status, g_Hv.FailedProcessorIndex);

        if (g_Hv.VcpuArray && g_Hv.FailedProcessorIndex < g_Hv.ProcessorCount)
        {
            PVMEXIT_DIAGNOSTICS diagnostics =
                &g_Hv.VcpuArray[g_Hv.FailedProcessorIndex].Diagnostics;
            DbgPrint("[Hvmx] InitDiag stage=%lu detail=0x%I64X vmcsField=0x%04X opStatus=0x%08X vmerr=%I64u vmerrRead=%d\n",
                diagnostics->FailureStage, diagnostics->FailureDetail,
                diagnostics->FailedVmcsField, diagnostics->VmxOperationStatus,
                diagnostics->VmInstructionError, diagnostics->VmInstructionErrorReadStatus);
        }

        // FIXED: driverObject is now a parameter, so this is now valid
        if (driverObject != NULL) {
            VmxWriteStartErrorLog(driverObject, status);
        }

        VmxShutdownAllProcessors();
        HvProgressClose();
        return status;
    }

    HvRuntimeWatchdogStart();
    HvProgressWrite(MAXULONG, HvProgressInitializationComplete, STATUS_SUCCESS);
    DbgPrint("[Hvmx] hypervisor ativo em %lu core(s).\n", g_Hv.ProcessorCount);
    return STATUS_SUCCESS;
}

// 2. FIXED: Signature matches what kdmapper expects
NTSTATUS CustomDriverEntry(PDRIVER_OBJECT kdmapperParam1, PUNICODE_STRING kdmapperParam2)
{
    UNREFERENCED_PARAMETER(kdmapperParam1);
    UNREFERENCED_PARAMETER(kdmapperParam2);

    // FIXED: Pass NULL for both because we are manually mapped
    return InitializeHypervisor(NULL, NULL);
}

// 3. FIXED: Pass arguments to CustomDriverEntry to avoid C2198
NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
    if (driverObject == NULL)
    {
        // FIXED: Added NULL, NULL to match the signature of CustomDriverEntry
        return CustomDriverEntry(NULL, NULL);
    }

    driverObject->DriverUnload = DriverUnload;

    // FIXED: Now correctly passes both arguments
    return InitializeHypervisor(driverObject, registryPath);
}



// Keep the original DriverEntry for standard loading,
// but kdmapper will be configured to call CustomDriverEntry.
