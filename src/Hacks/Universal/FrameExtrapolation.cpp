#include "../../Client/Module.hpp"

using namespace geode::prelude;

class FrameExtrapolation : public Module
{
    public:
        MODULE_SETUP(FrameExtrapolation)
        {
            setName("Frame Extrapolation");
            setID("frame-extrapolation");
            setCategory("Universal");
            setDescription("Frame extrapolation is hard-disabled in this build.");

            // HARD DISABLE:
            // Keep the module visible in the menu, but make it impossible to
            // enable and ensure getRealEnabled() can never become true.
            setDefaultEnabled(false);
            setDisabled(true);
            setDisabledMessage("Frame Extrapolation is hard-disabled in this build.");
            setForceDisabled(true);
        }

        // Module::load() normally restores the saved *_enabled value after the
        // constructor runs. Force false here so an old saved checkmark is erased
        // on startup and future attempts to enable this module are ignored.
        void setUserEnabled(bool enabled) override
        {
            (void)enabled;
            Module::setUserEnabled(false);
        }

        bool getUserEnabled() override
        {
            return false;
        }

        // A previously configured keybind must not be able to toggle it either.
        void onKeybindActivated(KeyState state) override
        {
            (void)state;
        }
};

SUBMIT_HACK(FrameExtrapolation)

// Intentionally no GJBaseGameLayer / PlayLayer hooks in this file.
// The old frame-prediction/extrapolation implementation has been removed from
// the build, so disabling the UI is backed by a real runtime hard-disable.
