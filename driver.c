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
        VmxShutdownAllProcessors();
        return status;
    }

    DbgPrint("[Hvmx] hypervisor ativo em %lu core(s).\n", g_Hv.ProcessorCount);
    return STATUS_SUCCESS;
}
