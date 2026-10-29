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

DRIVER_UNLOAD DriverUnload;

typedef struct _HV_START_ERROR_DATA
{
    ULONG ProcessorIndex;
    UINT64 VmxInstructionError;
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
    data.ProcessorIndex = g_Hv.FailedProcessorIndex;
    data.VmxInstructionError = g_Hv.LastVmxInstructionError;

    packet->ErrorCode = status;
    packet->FinalStatus = status;
    packet->UniqueErrorValue = 0x56545801;
    packet->DumpDataSize = sizeof(data);
    RtlCopyMemory(packet->DumpData, &data, sizeof(data));
    IoWriteErrorLogEntry(packet);
}

VOID DriverUnload(PDRIVER_OBJECT driverObject)
{
    UNREFERENCED_PARAMETER(driverObject);
    VmxShutdownAllProcessors();
    DbgPrint("[Hvmx] driver descarregado.\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(registryPath);

    DbgPrint("[Hvmx] DriverEntry: iniciando hypervisor VT-x...\n");

    driverObject->DriverUnload = DriverUnload;

    status = VmxInitializeAllProcessors();
    if (!NT_SUCCESS(status))
    {
        DbgPrint("[Hvmx] falha ao inicializar VT-x (0x%08X).\n", status);
        VmxWriteStartErrorLog(driverObject, status);
        VmxShutdownAllProcessors();
        return status;
    }

    DbgPrint("[Hvmx] hypervisor ativo em %lu core(s).\n", g_Hv.ProcessorCount);
    return STATUS_SUCCESS;
}
