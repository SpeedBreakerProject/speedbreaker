// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See ../thermal.h.
//
// macOS and iOS: NSProcessInfo's thermal state (Foundation only, so it
// builds for both; UIKit glue is platform/*.mm, iOS only). Manual reference
// counting like the rest of the runtime, though nothing here is allocated:
// processInfo is the shared instance.
#include <platform/thermal.h>

#import <Foundation/Foundation.h>

namespace platform::thermal::detail
{
    State ReadSystem()
    {
        @autoreleasepool
        {
            switch ([NSProcessInfo processInfo].thermalState)
            {
            case NSProcessInfoThermalStateNominal: return State::Nominal;
            case NSProcessInfoThermalStateFair: return State::Fair;
            case NSProcessInfoThermalStateSerious: return State::Serious;
            case NSProcessInfoThermalStateCritical: return State::Critical;
            }
            return State::Unknown;
        }
    }
}
