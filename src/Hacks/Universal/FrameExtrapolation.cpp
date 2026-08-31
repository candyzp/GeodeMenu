#include "../../Client/Module.hpp"
#include "../../Client/FloatSliderModule.hpp"
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCEGLView.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace geode::prelude;

class FrameExtrapolation : public Module
{
    public:
        MODULE_SETUP(FrameExtrapolation)
        {
            setName("Frame Extrapolation");
            setID("frame-extrapolation");
            setCategory("Universal");
            setDescription("Smooths between frames by predicting where the player will be the next frame using its velocity.");

            // The original module later hard-disabled this setting because of bugs.
            // Keep the normal toggle available again, but do not force it on.
            setDisabled(false);
        }
};

class FrameTimingDebugger : public Module
{
    public:
        MODULE_SETUP(FrameTimingDebugger)
        {
            setName("Frame Timing Debugger");
            setID("frame-extrapolation/frame-timing-debugger");
            setDescription("Measures real presented-frame timing, GD updates, physics delta calls, and interpolation progress without changing gameplay.");
        }
};

class FrameTimingShowOverlay : public Module
{
    public:
        MODULE_SETUP(FrameTimingShowOverlay)
        {
            setName("Show Timing Overlay");
            setID("frame-extrapolation/show-timing-overlay");
            setDescription("Shows a small live timing readout while the frame timing debugger is enabled.");
        }
};

class FrameTimingLogSpikesOnly : public Module
{
    public:
        MODULE_SETUP(FrameTimingLogSpikesOnly)
        {
            setName("Log Spikes Only");
            setID("frame-extrapolation/log-spikes-only");
            setDescription("Only writes timing logs when a rendered frame exceeds the spike threshold.");
            setDefaultEnabled(true);
        }
};

class FrameTimingSpikeThreshold : public FloatSliderModule
{
    public:
        MODULE_SETUP(FrameTimingSpikeThreshold)
        {
            setName("Spike Threshold");
            setID("frame-extrapolation/spike-threshold");
            setDescription("Rendered frame time in milliseconds that counts as a timing spike.");
            setRange(16.0f, 100.0f);
            setDefaultValue(25.0f);
            setSnapValues({16.67f, 20.0f, 25.0f, 33.33f, 50.0f});
        }
};

SUBMIT_HACK(FrameExtrapolation)
SUBMIT_OPTION(FrameExtrapolation, FrameTimingDebugger)
SUBMIT_OPTION(FrameExtrapolation, FrameTimingShowOverlay)
SUBMIT_OPTION(FrameExtrapolation, FrameTimingLogSpikesOnly)
SUBMIT_OPTION(FrameExtrapolation, FrameTimingSpikeThreshold)

namespace
{
    constexpr int kFrameTimingOverlayTag = 0x46544D47;

    struct FrameTimingStats
    {
        std::chrono::steady_clock::time_point lastPresent = {};
        bool hasPresentTime = false;

        float renderDtMs = 0.0f;
        unsigned long long presentIndex = 0;
        unsigned int updatesSincePresent = 0;
        unsigned int modifiedDeltaCallsSincePresent = 0;

        float lastModifiedDeltaMs = 0.0f;
        float lastInterpPercent = 0.0f;

        CCPoint sampledPlayerPos = CCPointZero;
        CCPoint sampledCameraPos = CCPointZero;
        CCPoint previousPresentedPlayerPos = CCPointZero;
        CCPoint previousPresentedCameraPos = CCPointZero;
        bool hasSample = false;
        bool hasPreviousPresentedSample = false;

        float playerDelta = 0.0f;
        float cameraDelta = 0.0f;
        float overlayElapsedMs = 0.0f;
    };

    FrameTimingStats g_frameTiming;

    bool frameTimingDebuggerEnabled()
    {
        return FrameTimingDebugger::get()->getRealEnabled();
    }

    float frameTimingSpikeThreshold()
    {
        return FrameTimingSpikeThreshold::get()->getValue();
    }

    float pointDistance(CCPoint const& a, CCPoint const& b)
    {
        auto dx = a.x - b.x;
        auto dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    void removeTimingOverlay()
    {
        if (auto scene = CCScene::get())
        {
            if (auto node = scene->getChildByTag(kFrameTimingOverlayTag))
                node->removeFromParentAndCleanup(true);
        }
    }

    void updateTimingOverlay()
    {
        if (!FrameTimingShowOverlay::get()->getRealEnabled())
        {
            removeTimingOverlay();
            return;
        }

        auto scene = CCScene::get();
        if (!scene)
            return;

        auto label = typeinfo_cast<CCLabelBMFont*>(scene->getChildByTag(kFrameTimingOverlayTag));
        if (!label)
        {
            label = CCLabelBMFont::create("", "bigFont.fnt");
            if (!label)
                return;

            label->setTag(kFrameTimingOverlayTag);
            label->setAnchorPoint({0.0f, 1.0f});
            label->setScale(0.32f);
            label->setZOrder(999999);

            auto winSize = CCDirector::sharedDirector()->getWinSize();
            label->setPosition({6.0f, winSize.height - 6.0f});
            scene->addChild(label);
        }

        label->setString(fmt::format(
            "Frame: {:.2f} ms\nGD updates: {}\ngetModifiedDelta: {}\nPhysics dt: {:.3f} ms\nInterp: {:.3f}\nPlayer d: {:.3f}\nCamera d: {:.3f}",
            g_frameTiming.renderDtMs,
            g_frameTiming.updatesSincePresent,
            g_frameTiming.modifiedDeltaCallsSincePresent,
            g_frameTiming.lastModifiedDeltaMs,
            g_frameTiming.lastInterpPercent,
            g_frameTiming.playerDelta,
            g_frameTiming.cameraDelta
        ).c_str());
    }

    void resetTimingSession()
    {
        g_frameTiming = {};
        removeTimingOverlay();
    }

    void onPresentedFrame()
    {
        if (!frameTimingDebuggerEnabled())
        {
            if (g_frameTiming.hasPresentTime || g_frameTiming.presentIndex != 0)
                resetTimingSession();
            return;
        }

        auto now = std::chrono::steady_clock::now();

        if (g_frameTiming.hasPresentTime)
        {
            g_frameTiming.renderDtMs = std::chrono::duration<float, std::milli>(now - g_frameTiming.lastPresent).count();
        }
        else
        {
            g_frameTiming.renderDtMs = 0.0f;
            g_frameTiming.hasPresentTime = true;
        }

        g_frameTiming.lastPresent = now;
        ++g_frameTiming.presentIndex;

        if (g_frameTiming.hasSample)
        {
            if (g_frameTiming.hasPreviousPresentedSample)
            {
                g_frameTiming.playerDelta = pointDistance(
                    g_frameTiming.sampledPlayerPos,
                    g_frameTiming.previousPresentedPlayerPos
                );
                g_frameTiming.cameraDelta = pointDistance(
                    g_frameTiming.sampledCameraPos,
                    g_frameTiming.previousPresentedCameraPos
                );
            }
            else
            {
                g_frameTiming.playerDelta = 0.0f;
                g_frameTiming.cameraDelta = 0.0f;
                g_frameTiming.hasPreviousPresentedSample = true;
            }

            g_frameTiming.previousPresentedPlayerPos = g_frameTiming.sampledPlayerPos;
            g_frameTiming.previousPresentedCameraPos = g_frameTiming.sampledCameraPos;
        }

        bool isSpike = g_frameTiming.renderDtMs >= frameTimingSpikeThreshold();
        bool logSpikesOnly = FrameTimingLogSpikesOnly::get()->getRealEnabled();

        if (g_frameTiming.renderDtMs > 0.0f && (!logSpikesOnly || isSpike))
        {
            log::info(
                "[FrameTiming] frame={} render={:.2f}ms updates={} modifiedDeltaCalls={} physicsDelta={:.3f}ms interp={:.3f} playerDelta={:.3f} cameraDelta={:.3f}{}",
                g_frameTiming.presentIndex,
                g_frameTiming.renderDtMs,
                g_frameTiming.updatesSincePresent,
                g_frameTiming.modifiedDeltaCallsSincePresent,
                g_frameTiming.lastModifiedDeltaMs,
                g_frameTiming.lastInterpPercent,
                g_frameTiming.playerDelta,
                g_frameTiming.cameraDelta,
                isSpike ? " SPIKE" : ""
            );
        }

        // Keep the overlay deliberately slow-updating so the debugger itself
        // does not become a meaningful source of frame-time noise.
        g_frameTiming.overlayElapsedMs += g_frameTiming.renderDtMs;
        if (g_frameTiming.overlayElapsedMs >= 100.0f)
        {
            updateTimingOverlay();
            g_frameTiming.overlayElapsedMs = 0.0f;
        }

        g_frameTiming.updatesSincePresent = 0;
        g_frameTiming.modifiedDeltaCallsSincePresent = 0;
    }
}

#ifdef GEODE_IS_ANDROID
class $modify(FrameTimingPresentHook, CCDirector)
{
    void drawScene()
    {
        CCDirector::drawScene();
        onPresentedFrame();
    }
};
#else
class $modify(FrameTimingPresentHook, CCEGLView)
{
    void swapBuffers()
    {
        onPresentedFrame();
        CCEGLView::swapBuffers();
    }
};
#endif

class $modify (ExtrapolatedGameLayer, GJBaseGameLayer)
{
    struct Fields
    {
        float timeTilNextTick = 0;
        float progressTilNextTick = 0;

        CCPoint lastCamPos2;
        CCPoint lastCamPos;
        float modifiedDeltaReturn = 0;

        // Small safety additions around the original state machine.
        bool hasCameraHistory = false;
        bool wasEnabled = false;
    };

    void resetExtrapolationState()
    {
        auto self = m_fields.self();
        self->timeTilNextTick = 0;
        self->progressTilNextTick = 0;
        self->modifiedDeltaReturn = 0;
        self->lastCamPos2 = CCPointZero;
        self->lastCamPos = CCPointZero;
        self->hasCameraHistory = false;
    }

    float getModifiedDelta(float dt)
    {
        auto pRet = GJBaseGameLayer::getModifiedDelta(dt);

        if (frameTimingDebuggerEnabled())
        {
            ++g_frameTiming.modifiedDeltaCallsSincePresent;
            g_frameTiming.lastModifiedDeltaMs = pRet * 1000.0f;
        }

        // Preserve the original timing source, but do not keep interpolation
        // timing alive while the menu setting is disabled.
        if (FrameExtrapolation::get()->getRealEnabled())
            m_fields->modifiedDeltaReturn = pRet;
        else
            m_fields->modifiedDeltaReturn = 0;

        return pRet;
    }

    virtual void update(float dt)
    {
        auto self = m_fields.self();

        if (frameTimingDebuggerEnabled())
            ++g_frameTiming.updatesSincePresent;

        // getModifiedDelta is called from inside the normal game update. Clearing
        // this first prevents a previous tick value being reused if a frame does
        // not produce a new modified delta.
        self->modifiedDeltaReturn = 0;

        GJBaseGameLayer::update(dt);

        auto playLayer = typeinfo_cast<PlayLayer*>(this);
        if (!playLayer)
            return;

        // Capture the authoritative post-update state before this module applies
        // any visual extrapolation. This lets the debugger compare baseline GD
        // motion and extrapolated motion without mixing the two measurements.
        if (frameTimingDebuggerEnabled())
        {
            if (m_player1)
                g_frameTiming.sampledPlayerPos = m_player1->m_position;
            if (m_objectLayer)
                g_frameTiming.sampledCameraPos = m_objectLayer->getPosition();
            g_frameTiming.hasSample = m_player1 && m_objectLayer;
        }

        if (!FrameExtrapolation::get()->getRealEnabled())
        {
            if (self->wasEnabled)
                resetExtrapolationState();

            self->wasEnabled = false;
            g_frameTiming.lastInterpPercent = 0.0f;
            return;
        }

        self->wasEnabled = true;

        // Keep the same exclusions the original implementation ended up using.
        if (isFlipping())
        {
            resetExtrapolationState();
            return;
        }

        if (!isRunning() || dt <= 0 || !std::isfinite(dt) || playLayer->m_levelEndAnimationStarted)
        {
            resetExtrapolationState();
            return;
        }

        if (self->modifiedDeltaReturn != 0)
        {
            // The original implementation uses this value as the interval until
            // the next physics tick. A giant value after a hitch/pause causes the
            // extrapolation percentage to behave badly, so simply re-baseline.
            if (!std::isfinite(self->modifiedDeltaReturn) || self->modifiedDeltaReturn <= 0 || self->modifiedDeltaReturn > 0.05f)
            {
                resetExtrapolationState();
                return;
            }

            self->timeTilNextTick = self->modifiedDeltaReturn;
            self->progressTilNextTick = 0;

            if (m_objectLayer)
            {
                auto currentCamPos = m_objectLayer->getPosition();

                // The original code started lastCamPos2 at {0, 0}, which can make
                // the first extrapolated frame jump. Seed both samples together.
                if (!self->hasCameraHistory)
                {
                    self->lastCamPos2 = currentCamPos;
                    self->lastCamPos = currentCamPos;
                    self->hasCameraHistory = true;
                }
                else
                {
                    self->lastCamPos2 = self->lastCamPos;
                    self->lastCamPos = currentCamPos;
                }
            }
        }
        else
        {
            self->progressTilNextTick += dt;

            // Do not let one bad render frame extrapolate multiple physics ticks
            // into the future. This is still their same percentage-based logic.
            if (self->timeTilNextTick > 0)
                self->progressTilNextTick = std::min(self->progressTilNextTick, self->timeTilNextTick);
        }

        if (self->timeTilNextTick <= 0 || !std::isfinite(self->timeTilNextTick))
            return;

        // The percentage towards the next tick, exactly like the original logic,
        // with a clamp so frame spikes cannot make it run away.
        float percent = std::clamp(self->progressTilNextTick / self->timeTilNextTick, 0.0f, 1.0f);
        g_frameTiming.lastInterpPercent = percent;

        if (!std::isfinite(percent))
            return;

        if (m_objectLayer && self->hasCameraHistory)
        {
            auto endCamPos = self->lastCamPos + (self->lastCamPos - self->lastCamPos2);

            m_objectLayer->setPosition(
                self->lastCamPos.x + (endCamPos.x - self->lastCamPos.x) * percent,
                self->lastCamPos.y + (endCamPos.y - self->lastCamPos.y) * percent
            );
        }

        extrapolateGround(m_groundLayer, percent);
        extrapolateGround(m_groundLayer2, percent);

        extrapolatePlayer(m_player1, percent);

        if (m_player2)
            extrapolatePlayer(m_player2, percent);
    }

    float playerGetRotatedHitbox(PlayerObject* player)
    {
        float rot = 0;

        if (player && player->m_isSideways)
        {
            rot = -90;
        }

        return rot;
    }

    void extrapolatePlayer(PlayerObject* player, float percent)
    {
        if (!player)
            return;

        // This is the original GeodeMenu extrapolation formula.
        float endXPos = player->m_position.x + (player->m_position.x - player->m_lastPosition.x);
        float endYPos = player->m_position.y + (player->m_position.y - player->m_lastPosition.y);

        float rotateSpeed = (player->m_isBall && player->m_isBallRotating) ? 1.0 : player->m_rotateSpeed;
        float endRot = ((player->m_rotationSpeed * rotateSpeed) / 240.0f);

        player->CCNode::setPosition(ccp(
            player->m_position.x + (endXPos - player->m_position.x) * percent,
            player->m_position.y + (endYPos - player->m_position.y) * percent
        ));

        if (player->m_mainLayer)
        {
            player->m_mainLayer->setRotation(
                endRot * percent + playerGetRotatedHitbox(player)
            );
        }
    }

    void extrapolateGround(GJGroundLayer* ground, float percent)
    {
        if (!ground)
            return;

        auto self = m_fields.self();

        // Keep their original ground extrapolation method too.
        float moveBy = (self->lastCamPos.x - self->lastCamPos2.x);

        auto children = ground->getChildren();
        if (!children)
            return;

        for (auto child : CCArrayExt<CCNode*>(children))
        {
            if (typeinfo_cast<CCSpriteBatchNode*>(child))
            {
                child->setPositionX(moveBy * percent);
            }
        }
    }
};
