// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See platform/thread.h.
#include <platform/thread.h>
#include <platform/os.h>

#include <process.h>

#include <iterator>

namespace
{
    struct Start
    {
        void* (*fn)(void*);
        void* arg;
    };

    unsigned __stdcall Entry(void* p)
    {
        Start start = *static_cast<Start*>(p);
        delete static_cast<Start*>(p);
        start.fn(start.arg);
        return 0;
    }

    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
}

namespace platform
{
    int StartThread(Thread* thread, size_t stackSize, void* (*fn)(void*), void* arg)
    {
        auto* start = new Start{ fn, arg };
        // _beginthreadex, not CreateThread: the C runtime's per-thread state
        // is set up (and freed) with the thread.
        uintptr_t handle = _beginthreadex(nullptr, unsigned(stackSize), Entry, start, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (handle == 0)
        {
            int error = int(GetLastError());
            delete start;
            return error ? error : -1;
        }
        *thread = reinterpret_cast<Thread>(handle);
        return 0;
    }

    void DetachThread(Thread thread)
    {
        CloseHandle(thread);
    }

    void JoinThread(Thread thread)
    {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }

    void NameCurrentThread(const char* name)
    {
        // Windows 10 1607 and newer: looked up, so older ones still start.
        static const auto setDescription = reinterpret_cast<SetThreadDescriptionFn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
        if (!setDescription)
            return;
        wchar_t wide[64];
        if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, int(std::size(wide))) > 0)
            setDescription(GetCurrentThread(), wide);
    }
}
