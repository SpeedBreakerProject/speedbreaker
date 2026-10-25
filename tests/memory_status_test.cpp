// Tests for kernel/memory_status.h: the [memory] log line, the page shown when
// the guest's 4 GB can't be reserved (an iOS copy signed without Extended
// Virtual Addressing), and the low-memory notice. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/memory_status_test.cpp runtime/kernel/memory_status.cpp runtime/platform/code_signature.cpp -o build/memory_status_test
//   build/memory_status_test
#include <kernel/memory_status.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
// The MSVC C runtime has _putenv_s, not setenv and unsetenv ("" removes).
static int setenv(const char* name, const char* value, int) { return _putenv_s(name, value); }
static int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

using namespace memstatus;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace
{
    bool Has(const std::string& text, const char* part)
    {
        return text.find(part) != std::string::npos;
    }

    std::string All(const Message& m)
    {
        std::string text = m.title + "\n";
        for (const std::string& p : m.paragraphs)
            text += p + "\n";
        return text + m.details;
    }

    // An iOS copy as a sideload with a free Apple Account may leave it.
    Status IosFailed(Flag va, Flag mem)
    {
        Status s;
        s.ios = true;
        s.failure = Failure::AddressSpace;
        s.error = ENOMEM;
        s.errorText = "Cannot allocate memory";
        s.signatureRead = va != Flag::Unknown;
        s.extendedVirtualAddressing = va;
        s.increasedMemoryLimit = mem;
        s.memoryLimitMB = 2900;
        s.addressSpaceTop = 0x458000000;
        return s;
    }
}

int main()
{
    // The log line.
    {
        Status mac;
        mac.base = 0x100000000;
        CHECK(LogLine(mac) == "[memory] guest 4 GB at 0x100000000", "%s", LogLine(mac).c_str());
        CHECK(SummaryItem(mac) == "guest 4 GB at 0x100000000", "%s", SummaryItem(mac).c_str());

        Status ios = mac;
        ios.ios = true;
        ios.base = 0x7000000000;
        ios.signatureRead = true;
        ios.extendedVirtualAddressing = Flag::Yes;
        ios.increasedMemoryLimit = Flag::Yes;
        ios.memoryLimitMB = 6136;
        ios.addressSpaceTop = 0xFC0000000;
        std::string line = LogLine(ios);
        CHECK(line == "[memory] guest 4 GB at 0x7000000000; entitlements in the signature: Extended Virtual Addressing yes, "
                      "Increased Memory Limit yes; address space up to 0xfc0000000 (63 GB); memory limit 6136 MB", "%s", line.c_str());

        Status unread = ios;
        unread.signatureRead = false;
        unread.extendedVirtualAddressing = unread.increasedMemoryLimit = Flag::Unknown;
        unread.memoryLimitMB = -1;
        unread.addressSpaceTop = 0;
        CHECK(LogLine(unread) == "[memory] guest 4 GB at 0x7000000000; entitlements: not read; memory limit unknown", "%s",
            LogLine(unread).c_str());

        std::string failed = LogLine(IosFailed(Flag::No, Flag::No));
        CHECK(Has(failed, "reserving 4 GB failed: Cannot allocate memory (errno 12)"), "%s", failed.c_str());
        CHECK(Has(failed, "Extended Virtual Addressing no, Increased Memory Limit no"), "%s", failed.c_str());
        CHECK(Has(failed, "address space up to 0x458000000 (17 GB)") && Has(failed, "memory limit 2900 MB"), "%s", failed.c_str());
    }

    // The page: nothing to say when the game started.
    {
        Status ok;
        ok.base = 0x100000000;
        Message m = StartupFailure(ok);
        CHECK(m.title.empty() && m.paragraphs.empty() && m.details.empty(), "no failure, no page");
    }
    // iOS, signed with neither entitlement (SideStore 0.7 with a free Apple
    // Account): the case the IPA is most likely to meet.
    {
        Message m = StartupFailure(IosFailed(Flag::No, Flag::No));
        std::string text = All(m);
        CHECK(m.title == "This copy of SpeedBreaker can't start", "%s", m.title.c_str());
        CHECK(m.paragraphs.size() == 3, "%zu paragraphs", m.paragraphs.size());
        CHECK(Has(m.paragraphs[0], "iOS refused the 4 GB of address space") && Has(m.paragraphs[0], "signed with neither"),
            "%s", m.paragraphs[0].c_str());
        CHECK(Has(m.paragraphs[1], "This build needs the Extended Virtual Addressing entitlement")
            && Has(m.paragraphs[1], "paid Apple Developer Program account") && Has(m.paragraphs[1], "Increased Memory Limit")
            && Has(m.paragraphs[1], "free Apple Account"), "%s", m.paragraphs[1].c_str());
        CHECK(Has(text, "AltStore") && Has(text, "SideStore") && Has(text, "Xcode") && Has(text, "scripts/ios/README.md"),
            "%s", text.c_str());
        CHECK(Has(m.paragraphs[2], "app switcher"), "%s", m.paragraphs[2].c_str());
        CHECK(Has(m.details, "Details: reserving 4 GB failed") && Has(m.details, "Extended Virtual Addressing no")
            && Has(m.details, "address space up to 0x458000000"), "%s", m.details.c_str());
    }
    // Increased Memory Limit only (AltStore with a free account), on a
    // device without its larger address space.
    {
        Message m = StartupFailure(IosFailed(Flag::No, Flag::Yes));
        CHECK(Has(m.paragraphs[0], "signed with Increased Memory Limit") && Has(m.paragraphs[0], "but not with Extended"),
            "%s", m.paragraphs[0].c_str());
        CHECK(Has(All(m), "paid Apple Developer Program"), "advice kept");
    }
    // The signature couldn't be read: the same advice, worded for it.
    {
        Message m = StartupFailure(IosFailed(Flag::Unknown, Flag::Unknown));
        CHECK(Has(m.paragraphs[0], "iOS didn't give this copy the room"), "%s", m.paragraphs[0].c_str());
        CHECK(Has(All(m), "paid Apple Developer Program"), "advice kept");
        CHECK(Has(m.details, "entitlements: not read"), "%s", m.details.c_str());
    }
    // Signed with it and still refused: not an entitlement problem.
    {
        Message m = StartupFailure(IosFailed(Flag::Yes, Flag::Yes));
        std::string text = All(m);
        CHECK(Has(text, "even though this copy has the Extended Virtual Addressing entitlement"), "%s", text.c_str());
        CHECK(Has(text, "Restart the device") && !Has(text, "paid Apple Developer"), "%s", text.c_str());
    }
    // The physical memory mirrors failed (iOS and elsewhere).
    {
        Status s = IosFailed(Flag::Yes, Flag::Yes);
        s.failure = Failure::PhysicalMemory;
        s.error = 3;
        s.errorText = "(os/kern) no space available";
        Message m = StartupFailure(s);
        CHECK(Has(All(m), "512 MB") && Has(m.details, "mapping the 512 MB of physical memory failed: (os/kern) no space available (3)"),
            "%s", All(m).c_str());
        s.ios = false;
        m = StartupFailure(s);
        CHECK(m.title == "SpeedBreaker can't start" && Has(All(m), "512 MB"), "%s", All(m).c_str());
    }
    // Elsewhere (a Mac, Linux): no entitlement talk.
    {
        Status s;
        s.failure = Failure::AddressSpace;
        s.error = ENOMEM;
        s.errorText = "Cannot allocate memory";
        Message m = StartupFailure(s);
        std::string text = All(m);
        CHECK(m.title == "SpeedBreaker can't start", "%s", m.title.c_str());
        CHECK(Has(text, "ulimit -v") && !Has(text, "entitlement") && !Has(text, "Apple"), "%s", text.c_str());
        CHECK(m.details == "Details: reserving 4 GB failed: Cannot allocate memory (errno 12).", "%s", m.details.c_str());
    }

    // The low-memory notice.
    {
        Status s;
        s.ios = true;
        s.base = 0x7000000000;
        s.signatureRead = true;
        s.extendedVirtualAddressing = Flag::Yes;
        s.increasedMemoryLimit = Flag::Yes;
        s.memoryLimitMB = 6136;
        CHECK(LowMemoryNotice(s).empty(), "a big limit: nothing");
        s.memoryLimitMB = kPeakFootprintMB;
        CHECK(LowMemoryNotice(s).empty(), "exactly the peak: nothing");
        s.memoryLimitMB = 2900;
        CHECK(LowMemoryNotice(s) == "iOS lets SpeedBreaker use only 2900 MB on this device: it may close the game in busy scenes",
            "%s", LowMemoryNotice(s).c_str());
        s.increasedMemoryLimit = Flag::No;
        CHECK(LowMemoryNotice(s) == "iOS lets SpeedBreaker use only 2900 MB (no Increased Memory Limit entitlement): it may close "
                                    "the game in busy scenes", "%s", LowMemoryNotice(s).c_str());
        s.memoryLimitMB = 4500;
        CHECK(LowMemoryNotice(s).empty(), "no entitlement but room enough: nothing");
        s.memoryLimitMB = -1;
        CHECK(LowMemoryNotice(s).empty(), "limit unknown: nothing");
        s.memoryLimitMB = 2900;
        s.ios = false;
        CHECK(LowMemoryNotice(s).empty(), "not iOS: nothing");
        s.ios = true;
        s.failure = Failure::AddressSpace;
        CHECK(LowMemoryNotice(s).empty(), "the game didn't start: the page says it instead");

        // iOS's default address space: neither entitlement took effect.
        s.failure = Failure::None;
        s.increasedMemoryLimit = Flag::No;
        s.extendedVirtualAddressing = Flag::No;
        s.addressSpaceTop = 0x458000000;
        s.memoryLimitMB = 3300;
        CHECK(LowMemoryNotice(s) == "iOS gives SpeedBreaker only 17 GB of address space (no memory entitlements): it may "
                                    "close the game in busy scenes", "%s", LowMemoryNotice(s).c_str());
        s.memoryLimitMB = 2900;
        CHECK(LowMemoryNotice(s) == "iOS gives SpeedBreaker little room (17 GB of address space, 2900 MB of memory: no "
                                    "memory entitlements): it may close the game in busy scenes", "%s", LowMemoryNotice(s).c_str());
        s.addressSpaceTop = kLargeAddressSpace;  // Increased Memory Limit's 63 GB, the limit still low
        CHECK(Has(LowMemoryNotice(s), "only 2900 MB (no Increased Memory Limit"), "%s", LowMemoryNotice(s).c_str());
        s.addressSpaceTop = 0x7FC0000000;
        s.memoryLimitMB = 6136;
        CHECK(LowMemoryNotice(s).empty(), "both: nothing");
        s.addressSpaceTop = 0;
        s.memoryLimitMB = 3300;
        CHECK(LowMemoryNotice(s).empty(), "address space unknown, limit fine: nothing");
    }

    CHECK(std::string(FlagName(Flag::Yes)) == "yes" && std::string(FlagName(Flag::No)) == "no"
        && std::string(FlagName(Flag::Unknown)) == "unknown", "names");

    // Gather off iOS leaves the entitlements unknown and the line short; on
    // the machine running this it fills in the error text.
    // NFSMW_TEST_MEMORY_IOS=1 gives the iOS wording, with this executable's
    // own entitlements (a test binary has none: "no" on a Mac).
    {
        unsetenv("NFSMW_TEST_MEMORY_IOS");
        Status s;
        s.failure = Failure::AddressSpace;
        s.error = ENOMEM;
        Gather(s);
        CHECK(!s.errorText.empty(), "error text");
#if !(defined(__APPLE__) && TARGET_OS_IOS)
        CHECK(!s.ios && !s.signatureRead && s.memoryLimitMB == -1, "not iOS");
        setenv("NFSMW_TEST_MEMORY_IOS", "1", 1);
        Status t;
        t.failure = Failure::AddressSpace;
        t.error = ENOMEM;
        Gather(t);
        unsetenv("NFSMW_TEST_MEMORY_IOS");
        CHECK(t.ios && t.memoryLimitMB == -1, "the iOS wording asked for");
#ifdef __APPLE__
        CHECK(t.signatureRead && t.extendedVirtualAddressing == Flag::No && t.increasedMemoryLimit == Flag::No,
            "a test binary's signature has no entitlements (read %d)", int(t.signatureRead));
#endif
        CHECK(Has(StartupFailure(t).title, "This copy of SpeedBreaker"), "%s", StartupFailure(t).title.c_str());
#endif
    }

    if (g_failures)
    {
        printf("memory_status_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("memory_status_test: all passed\n");
    return 0;
}
