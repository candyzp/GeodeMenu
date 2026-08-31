#include "../../Client/Module.hpp"

using namespace geode::prelude;

// Frame Extrapolation is intentionally hard-disabled.
// Keep the module registered so existing installs/settings remain compatible,
// but compile none of the presentation, smoothing, blur, debugger, or timing hooks.
class FrameExtrapolation : public Module
{
public:
    MODULE_SETUP(FrameExtrapolation)
    {
        setName("Frame Extrapolation");
        setID("frame-extrapolation");
        setCategory("Universal");
        setDescription("Temporarily hard-disabled. No frame smoothing, camera blur, debugger, or presentation hooks are active.");
        setDisabled(true);
        setDefaultEnabled(false);
    }
};

SUBMIT_HACK(FrameExtrapolation)
