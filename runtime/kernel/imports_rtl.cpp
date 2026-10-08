// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Runtime-library (Rtl*) helpers. Reference: Xenia's xboxkrnl_rtl.cc, except
// RtlCompareMemory / RtlCompareMemoryUlong, which follow NT semantics (bytes
// matching up to the first difference) where Xenia counts every match.
#include <stdafx.h>
#include "function.h"

uint32_t RtlCompareMemory(const uint8_t* a, const uint8_t* b, uint32_t length)
{
    uint32_t n = 0;
    while (n < length && a[n] == b[n])
        n++;
    return n;
}

uint32_t RtlCompareMemoryUlong(const be<uint32_t>* source, uint32_t length, uint32_t pattern)
{
    uint32_t n = 0;
    while (n + 4 <= length && source[n / 4] == pattern)
        n += 4;
    return n;
}

void RtlFillMemoryUlong(be<uint32_t>* destination, uint32_t length, uint32_t pattern)
{
    for (uint32_t i = 0; i < length / 4; i++)
        destination[i] = pattern;
}

struct X_UNICODE_STRING
{
    be<uint16_t> length;
    be<uint16_t> maximumLength;
    be<uint32_t> buffer;
};

void RtlInitUnicodeString(X_UNICODE_STRING* destination, const be<uint16_t>* source)
{
    if (source == nullptr)
    {
        destination->length = 0;
        destination->maximumLength = 0;
        destination->buffer = 0;
        return;
    }
    uint16_t chars = 0;
    while (source[chars] != 0)
        chars++;
    destination->length = chars * 2;
    destination->maximumLength = (chars + 1) * 2;
    destination->buffer = g_memory.MapVirtual(source);
}

// FILETIME (100 ns since 1601) <-> calendar fields, via C++20 chrono.
constexpr int64_t FILETIME_EPOCH_DIFFERENCE = 116444736000000000LL;

void RtlTimeToTimeFields(const be<int64_t>* time, XTIME_FIELDS* fields)
{
    using namespace std::chrono;
    sys_time<microseconds> tp{ microseconds((int64_t(*time) - FILETIME_EPOCH_DIFFERENCE) / 10) };
    auto dp = floor<days>(tp);
    year_month_day ymd{ dp };
    hh_mm_ss hms{ floor<milliseconds>(tp - dp) };
    fields->Year = uint16_t(int(ymd.year()));
    fields->Month = uint16_t(unsigned(ymd.month()));
    fields->Day = uint16_t(unsigned(ymd.day()));
    fields->Hour = uint16_t(hms.hours().count());
    fields->Minute = uint16_t(hms.minutes().count());
    fields->Second = uint16_t(hms.seconds().count());
    fields->Milliseconds = uint16_t(hms.subseconds().count());
    fields->Weekday = uint16_t(weekday{ dp }.c_encoding());
}

uint32_t RtlTimeFieldsToTime(const XTIME_FIELDS* fields, be<int64_t>* time)
{
    using namespace std::chrono;
    uint16_t y = fields->Year, mo = fields->Month, d = fields->Day;
    if (y < 1601 || mo < 1 || mo > 12 || d < 1 || d > 31 || fields->Hour > 23 || fields->Minute > 59 ||
        fields->Second > 59 || fields->Milliseconds > 999)
        return 0;
    year_month_day ymd{ year{ y }, month{ mo }, day{ d } };
    if (!ymd.ok())
        return 0;
    auto tp = sys_days{ ymd } + hours{ uint16_t(fields->Hour) } + minutes{ uint16_t(fields->Minute) } +
        seconds{ uint16_t(fields->Second) } + milliseconds{ uint16_t(fields->Milliseconds) };
    *time = duration_cast<microseconds>(tp.time_since_epoch()).count() * 10 + FILETIME_EPOCH_DIFFERENCE;
    return 1;
}

GUEST_FUNCTION_HOOK(__imp__RtlCompareMemory, RtlCompareMemory);
GUEST_FUNCTION_HOOK(__imp__RtlCompareMemoryUlong, RtlCompareMemoryUlong);
GUEST_FUNCTION_HOOK(__imp__RtlFillMemoryUlong, RtlFillMemoryUlong);
GUEST_FUNCTION_HOOK(__imp__RtlInitUnicodeString, RtlInitUnicodeString);
GUEST_FUNCTION_HOOK(__imp__RtlTimeToTimeFields, RtlTimeToTimeFields);
GUEST_FUNCTION_HOOK(__imp__RtlTimeFieldsToTime, RtlTimeFieldsToTime);
