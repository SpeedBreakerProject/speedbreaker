// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See platform/fault.h.
#include <stdafx.h>
#include <platform/fault.h>
#include <kernel/write_watch.h>
#include <report/report.h>

#include <csignal>
#include <cstdlib>

namespace
{
    LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
    {
        const EXCEPTION_RECORD* record = info->ExceptionRecord;
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        {
            // ExceptionInformation[0]: 0 a read, 1 a write, 8 an execution
            // (DEP); [1]: the address. The probe saw both exact.
            bool isWrite = record->ExceptionInformation[0] == 1;
            void* address = reinterpret_cast<void*>(record->ExceptionInformation[1]);
            writewatch::t_faultPc = record->ExceptionAddress;
            if (writewatch::HandleFault(address, isWrite))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* info)
    {
        report::WriteCrashReport(info);
        TerminateProcess(GetCurrentProcess(), info->ExceptionRecord->ExceptionCode);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    void OnAbort(int)
    {
        // abort() raises SIGABRT, not an exception: a record of our own,
        // with this thread's registers, so the report has a stack. It starts
        // where this handler was called (the C runtime's raise, in abort), as
        // a POSIX report's does: this function's own frame is unwound.
        static CONTEXT context;
        static EXCEPTION_RECORD record;
        RtlCaptureContext(&context);
        DWORD64 imageBase = 0;
        if (PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr))
        {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
        }
        record = {};
        record.ExceptionCode = 0x40000015;  // STATUS_FATAL_APP_EXIT, what Windows calls an abort
        record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
        EXCEPTION_POINTERS pointers{ &record, &context };
        report::WriteCrashReport(&pointers);
        _exit(3);
    }
}

namespace platform
{
    bool KeepCrashFilter()
    {
        LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(OnUnhandled);
        return previous == OnUnhandled;
    }

    void InstallFaultHandlers()
    {
        AddVectoredExceptionHandler(1, OnException);
        SetUnhandledExceptionFilter(OnUnhandled);
        // Assertions stay on in every build: their message goes to stderr
        // (the log), never a dialog box, and abort() neither asks Windows
        // Error Reporting nor prints its own line.
        _set_error_mode(_OUT_TO_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        signal(SIGABRT, OnAbort);
    }
}
