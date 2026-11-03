#include <intrin.h>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>

struct CpuidResult {
    std::uint32_t eax, ebx, ecx, edx;
};

static CpuidResult QueryCpuid(std::uint32_t leaf, std::uint32_t subleaf = 0)
{
    int r[4]{};
    __cpuidex(r, static_cast<int>(leaf), static_cast<int>(subleaf));

    return {
        static_cast<std::uint32_t>(r[0]),
        static_cast<std::uint32_t>(r[1]),
        static_cast<std::uint32_t>(r[2]),
        static_cast<std::uint32_t>(r[3])
    };
}

static void PrintLeaf(std::uint32_t leaf, std::uint32_t subleaf = 0)
{
    const auto r = QueryCpuid(leaf, subleaf);

    std::cout << "CPUID("
        << std::hex << std::showbase << leaf
        << ", " << subleaf << ")"
        << " EAX=" << r.eax
        << " EBX=" << r.ebx
        << " ECX=" << r.ecx
        << " EDX=" << r.edx << '\n';
}

int main()
{
    const auto basic = QueryCpuid(0);
    char vendor[13]{};
    std::memcpy(vendor + 0, &basic.ebx, 4);
    std::memcpy(vendor + 4, &basic.edx, 4);
    std::memcpy(vendor + 8, &basic.ecx, 4);

    const auto leaf1 = QueryCpuid(1);
    const bool hypervisorBit = (leaf1.ecx & (1u << 31)) != 0;

    std::cout << "Vendor: " << vendor << '\n';
    std::cout << "Hypervisor-present bit: "
        << (hypervisorBit ? "1" : "0") << "\n\n";

    PrintLeaf(0);
    PrintLeaf(1);
    PrintLeaf(7, 0);

    const auto extended = QueryCpuid(0x80000000);
    std::cout << "\nMaior folha estendida: "
        << std::hex << std::showbase << extended.eax << '\n';

    if (extended.eax >= 0x80000004)
    {
        char brand[49]{};

        for (std::uint32_t leaf = 0x80000002; leaf <= 0x80000004; ++leaf)
        {
            const auto r = QueryCpuid(leaf);
            const std::size_t offset = (leaf - 0x80000002) * 16;

            std::memcpy(brand + offset + 0, &r.eax, 4);
            std::memcpy(brand + offset + 4, &r.ebx, 4);
            std::memcpy(brand + offset + 8, &r.ecx, 4);
            std::memcpy(brand + offset + 12, &r.edx, 4);

            PrintLeaf(leaf);
        }

        std::cout << "\nCPU brand string: " << brand << '\n';
    }
    else
    {
        std::cout << "A CPU não anuncia as folhas de brand string.\n";
    }

    return 0;
}