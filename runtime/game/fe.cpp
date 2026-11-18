// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See fe.h.
#include <stdafx.h>
#include <game/fe.h>
#include <region/region.h>

#include <array>

#include <kernel/function.h>
#include <kernel/memory.h>

namespace
{
    using namespace game::fe;

    static_assert(kPadBack == 0x911AB364 && Hash("COMMON_YES") == 0x70E01038 &&
                  Hash("COMMON_NO") == 0x417B25E4);

    constexpr uint32_t kScreenPackage = 0x10;     // screen -> package
    constexpr uint32_t kPackageCallback = 0x40;   // package -> callback (sub_822D66D0)
    constexpr uint32_t kCallbackScreen = 0x1C;    // callback -> screen
    constexpr uint32_t kPackageResponses = 0x5C;  // responses: next +4, message +0xC

    // The Yes/No dialog, as the game calls it.
    constexpr uint32_t kYesNoDialog = 0x822BFB60;
    constexpr uint32_t kDefaultDialog = 0x82063E40;  // "": FE default dialog layout
    constexpr uint32_t kCommonYes = Hash("COMMON_YES");
    constexpr uint32_t kCommonNo = Hash("COMMON_NO");

    std::array<UnhandledButton, 8> s_listeners{};
    size_t s_listenerCount = 0;

    uint32_t Load32(uint32_t address)
    {
        return *static_cast<const be<uint32_t>*>(g_memory.Translate(address));
    }

    bool HasResponse(uint32_t package, uint32_t message)
    {
        for (uint32_t r = Load32(package + kPackageResponses); r; r = Load32(r + 4))
            if (Load32(r + 0xC) == message)
                return true;
        return false;
    }
}

namespace game::fe
{
    uint32_t ScreenOf(uint32_t package)
    {
        const uint32_t callback = package ? Load32(package + kPackageCallback) : 0;
        return callback ? Load32(callback + kCallbackScreen) : 0;
    }

    bool IsScreen(uint32_t screen, uint32_t vtable)
    {
        return screen && Load32(screen) == vtable;
    }

    void OnUnhandledButton(UnhandledButton listener)
    {
        if (s_listenerCount < s_listeners.size())
            s_listeners[s_listenerCount++] = listener;
    }

    void ShowYesNoDialog(const PPCContext& ctx, uint8_t* base, uint32_t screen,
        uint32_t text, uint32_t yes, uint32_t no)
    {
        PPCContext call = ctx;
        constexpr uint32_t kFrame = 0x80;
        call.r1.u32 = ctx.r1.u32 - kFrame;
        PPC_STORE_U32(call.r1.u32, ctx.r1.u32);  // back chain
        PPC_STORE_U32(call.r1.u32 + 0x54, 1);    // 9th: as in the game's calls
        PPC_STORE_U32(call.r1.u32 + 0x5C, text); // 10th: the text
        call.r3.u64 = Load32(screen + kScreenPackage);
        call.r4.u64 = region::Addr(kDefaultDialog);
        call.r5.u64 = 1;  // as in the game's calls
        call.r6.u64 = kCommonYes;
        call.r7.u64 = kCommonNo;
        call.r8.u64 = yes;
        call.r9.u64 = no;   // No
        call.r10.u64 = no;  // B
        g_memory.FindFunction(region::Addr(kYesNoDialog))(call, base);
    }
}

// The FE pad processor (sub_822FD3A8; r31: the button message, [r1+0x1CC]:
// the package), where the focused object has no response for the button.
// Tells the listeners when the package has none either; changes nothing.
void FeUnhandledButton(PPCRegister& r31, PPCRegister& r1)
{
    if (!s_listenerCount)
        return;
    const uint32_t package = Load32(r1.u32 + 0x1CC);
    if (!package || HasResponse(package, r31.u32))
        return;
    for (size_t i = 0; i < s_listenerCount; i++)
        s_listeners[i](r31.u32, package);
}
