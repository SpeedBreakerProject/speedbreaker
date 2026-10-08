// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The executable module as the kernel sees it: a guest copy of the XEX
// header and an LDR_DATA_TABLE_ENTRY (layout from Xenia's xmodule.h), plus
// the contents of the kernel's data exports (from Xenia's Register*Exports).
#pragma once
#include <cstdint>
#include <vector>

namespace module
{
    // Call after the image is in guest memory, before the entry point.
    void Initialize(const std::vector<uint8_t>& xexFile, uint32_t imageBase, uint32_t imageSize, uint32_t entryPoint);

    uint32_t ExecutableHandle();   // guest address of the LDR entry
    uint32_t XexHeader();          // guest address of the header copy

    // Fills a data export's guest block (called by the loader). `name` is the
    // export name without the __imp__ prefix; returns false if unknown.
    bool InitializeDataExport(const char* name, uint32_t guestAddress);
}
