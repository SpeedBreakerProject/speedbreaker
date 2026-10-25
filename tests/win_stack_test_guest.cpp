// Half of tests/win_stack_test.cpp: functions shaped and named as the
// recompiled code is (ppc/ppc_context.h): an extern "C" body
// __imp__sub_<address> and a C++ function sub_<address>, built, as ppc/ is,
// without debug information, so the PDB knows them by their public symbols
// alone.
extern "C" volatile int g_guestSink = 0;

extern "C" __declspec(noinline) void __imp__sub_82000010(void (*callback)(void*), void* arg)
{
    callback(arg);
    g_guestSink = g_guestSink + 1;  // (not a tail call: this frame stays)
}

__declspec(noinline) void sub_82000020(void (*callback)(void*), void* arg)
{
    __imp__sub_82000010(callback, arg);
    g_guestSink = g_guestSink + 1;
}

// A body named after the guest's own symbol, as XenonRecomp names _xstart's
// (its first underscore is part of the name: no 32-bit decoration here).
extern "C" __declspec(noinline) void __imp___xstart(void (*callback)(void*), void* arg)
{
    sub_82000020(callback, arg);
    g_guestSink = g_guestSink + 1;
}

// What the test calls (it never names __imp__ itself: an undefined
// __imp_ symbol means a DLL import to the linker).
extern "C" __declspec(noinline) void StackTestGuestRun(void (*callback)(void*), void* arg)
{
    __imp___xstart(callback, arg);
    g_guestSink = g_guestSink + 1;
}
