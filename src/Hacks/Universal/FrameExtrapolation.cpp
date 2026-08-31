#include "../../Client/Module.hpp"
#include "../../Client/FloatSliderModule.hpp"
#include "../../Client/EnumModule.hpp"
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
            setDescription("Smooths visual motion between Geometry Dash updates using the selected method.");
            setDisabled(false);
        }
};

enum class FrameExtrapolationMethod
{
    LegacyLinear = 0,
    CameraRelative = 1,
    SnapshotInterpolation = 2,
    SpringFilter = 3,
    AlphaBetaTracker = 4,
};

class FrameExtrapolationMethodOption : public EnumModule
{
    public:
        MODULE_SETUP(FrameExtrapolationMethodOption)
        {
            setName("Method");
            // V2 intentionally uses a new saved-value key so the old AB/Hybrid
            // selection cannot silently map to a completely different method.
            setID("frame-extrapolation/method-v2");
            setDescription("Choose how frames are smoothed. Classic predicts motion, Camera Sync keeps camera/player motion together, Snapshot blends real states, Spring softly follows motion, and Tracker estimates corrected motion.");
            listedValues = {
                {(int)FrameExtrapolationMethod::LegacyLinear, "Classic"},
                {(int)FrameExtrapolationMethod::CameraRelative, "Camera Sync"},
                {(int)FrameExtrapolationMethod::SnapshotInterpolation, "Snapshot"},
                {(int)FrameExtrapolationMethod::SpringFilter, "Spring"},
                {(int)FrameExtrapolationMethod::AlphaBetaTracker, "Tracker"},
            };
            setDefaultValue((int)FrameExtrapolationMethod::SnapshotInterpolation);
        }
};

class FrameTimingDebugger : public Module
{
    public:
        MODULE_SETUP(FrameTimingDebugger)
        {
            setName("Debugger");
            setID("frame-extrapolation/frame-timing-debugger");
            setDescription("Measures real presented-frame timing, GD updates, physics delta calls, interpolation progress, and the active visual offset.");
        }
};

class FrameTimingShowOverlay : public Module
{
    public:
        MODULE_SETUP(FrameTimingShowOverlay)
        {
            setName("Show Stats");
            setID("frame-extrapolation/show-timing-overlay");
            setDescription("Shows a small live timing readout while the frame timing debugger is enabled.");
        }
};

class FrameTimingLogSpikesOnly : public Module
{
    public:
        MODULE_SETUP(FrameTimingLogSpikesOnly)
        {
            setName("Spike Logs");
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
            setName("Spike Limit");
            setID("frame-extrapolation/spike-threshold");
            setDescription("Rendered frame time in milliseconds that counts as a timing spike.");
            setRange(16.0f, 100.0f);
            setDefaultValue(25.0f);
            setSnapValues({16.67f, 20.0f, 25.0f, 33.33f, 50.0f});
        }
};

SUBMIT_HACK(FrameExtrapolation)
SUBMIT_OPTION(FrameExtrapolation, FrameExtrapolationMethodOption)
SUBMIT_OPTION(FrameExtrapolation, FrameTimingDebugger)
SUBMIT_OPTION(FrameExtrapolation, FrameTimingShowOverlay)
SUBMIT_OPTION(FrameExtrapolation, FrameTimingLogSpikesOnly)
SUBMIT_OPTION(FrameExtrapolation, FrameTimingSpikeThreshold)

namespace
{
    constexpr int kFrameTimingOverlayTag = 0x46544D47;

    std::chrono::steady_clock::time_point g_lastPresentation = {};
    bool g_hasPresentationTime = false;
    float g_presentDtSeconds = 1.0f / 60.0f;
    unsigned long long g_presentSerial = 0;

    struct FrameTimingStats
    {
        float renderDtMs = 0.0f;
        unsigned long long presentIndex = 0;
        unsigned int updatesSincePresent = 0;
        unsigned int modifiedDeltaCallsSincePresent = 0;

        float lastModifiedDeltaMs = 0.0f;
        float lastInterpPercent = 0.0f;
        float lastFxOffset = 0.0f;
        int activeMethod = 0;

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

    char const* methodName(int value)
    {
        switch (static_cast<FrameExtrapolationMethod>(std::clamp(value, 0, 4)))
        {
            case FrameExtrapolationMethod::LegacyLinear: return "Classic";
            case FrameExtrapolationMethod::CameraRelative: return "Camera Sync";
            case FrameExtrapolationMethod::SnapshotInterpolation: return "Snapshot";
            case FrameExtrapolationMethod::SpringFilter: return "Spring";
            case FrameExtrapolationMethod::AlphaBetaTracker: return "Tracker";
        }
        return "Unknown";
    }

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
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    CCPoint addPoint(CCPoint const& a, CCPoint const& b)
    {
        return CCPoint{a.x + b.x, a.y + b.y};
    }

    CCPoint subPoint(CCPoint const& a, CCPoint const& b)
    {
        return CCPoint{a.x - b.x, a.y - b.y};
    }

    CCPoint scalePoint(CCPoint const& p, float amount)
    {
        return CCPoint{p.x * amount, p.y * amount};
    }

    CCPoint lerpPoint(CCPoint const& a, CCPoint const& b, float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        return CCPoint{
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t
        };
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
            "METHOD: {}\nFRAME: {:.2f} ms\nUPDATES: {}\nPHYSICS CALLS: {}\nPHYSICS DT: {:.3f} ms\nBLEND: {:.3f}\nEFFECT: {:.3f}\nPLAYER MOVE: {:.3f}\nCAMERA MOVE: {:.3f}",
            methodName(g_frameTiming.activeMethod),
            g_frameTiming.renderDtMs,
            g_frameTiming.updatesSincePresent,
            g_frameTiming.modifiedDeltaCallsSincePresent,
            g_frameTiming.lastModifiedDeltaMs,
            g_frameTiming.lastInterpPercent,
            g_frameTiming.lastFxOffset,
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
        auto now = std::chrono::steady_clock::now();
        if (g_hasPresentationTime)
        {
            float measured = std::chrono::duration<float>(now - g_lastPresentation).count();
            if (std::isfinite(measured) && measured > 0.0f)
                g_presentDtSeconds = std::clamp(measured, 1.0f / 1000.0f, 0.10f);
        }
        else
        {
            g_hasPresentationTime = true;
        }

        g_lastPresentation = now;
        ++g_presentSerial;

        if (!frameTimingDebuggerEnabled())
        {
            if (g_frameTiming.presentIndex != 0)
                resetTimingSession();
            return;
        }

        // Read the selector directly every presented frame. The previous
        // debugger only updated this value after an effect successfully applied,
        // which made the overlay lie and stay on Classic when Blend was 0.
        g_frameTiming.activeMethod = std::clamp(FrameExtrapolationMethodOption::get()->getValue(), 0, 4);

        g_frameTiming.renderDtMs = g_presentDtSeconds * 1000.0f;
        ++g_frameTiming.presentIndex;

        if (g_frameTiming.hasSample)
        {
            if (g_frameTiming.hasPreviousPresentedSample)
            {
                g_frameTiming.playerDelta = pointDistance(g_frameTiming.sampledPlayerPos, g_frameTiming.previousPresentedPlayerPos);
                g_frameTiming.cameraDelta = pointDistance(g_frameTiming.sampledCameraPos, g_frameTiming.previousPresentedCameraPos);
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

        if (!logSpikesOnly || isSpike)
        {
            log::info(
                "[FrameTiming] method={} frame={} render={:.2f}ms updates={} modifiedDeltaCalls={} physicsDelta={:.3f}ms interp={:.3f} fxOffset={:.3f} playerDelta={:.3f} cameraDelta={:.3f}{}",
                methodName(g_frameTiming.activeMethod),
                g_frameTiming.presentIndex,
                g_frameTiming.renderDtMs,
                g_frameTiming.updatesSincePresent,
                g_frameTiming.modifiedDeltaCallsSincePresent,
                g_frameTiming.lastModifiedDeltaMs,
                g_frameTiming.lastInterpPercent,
                g_frameTiming.lastFxOffset,
                g_frameTiming.playerDelta,
                g_frameTiming.cameraDelta,
                isSpike ? " SPIKE" : ""
            );
        }

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
    struct MotionHistory
    {
        CCPoint previousPos = CCPointZero;
        CCPoint lastPos = CCPointZero;
        CCPoint velocity = CCPointZero;
        bool seeded = false;
        bool hasPrevious = false;

        void reset()
        {
            *this = {};
        }

        void push(CCPoint const& pos)
        {
            if (!seeded)
            {
                previousPos = pos;
                lastPos = pos;
                velocity = CCPointZero;
                seeded = true;
                hasPrevious = false;
                return;
            }

            // Treat very large one-tick jumps as a discontinuity instead of
            // feeding a portal/camera snap into a visual predictor.
            if (pointDistance(pos, lastPos) > 512.0f)
            {
                previousPos = pos;
                lastPos = pos;
                velocity = CCPointZero;
                hasPrevious = false;
                return;
            }

            previousPos = lastPos;
            velocity = subPoint(pos, lastPos);
            lastPos = pos;
            hasPrevious = true;
        }
    };

    struct SpringState
    {
        CCPoint position = CCPointZero;
        CCPoint velocity = CCPointZero;
        bool seeded = false;

        void reset()
        {
            *this = {};
        }

        void seed(CCPoint const& target)
        {
            position = target;
            velocity = CCPointZero;
            seeded = true;
        }

        void step(CCPoint const& target, float dt)
        {
            if (!seeded || pointDistance(position, target) > 256.0f)
            {
                seed(target);
                return;
            }

            dt = std::clamp(dt, 1.0f / 1000.0f, 1.0f / 30.0f);
            constexpr float stiffness = 150.0f;
            constexpr float damping = 24.5f;

            CCPoint displacement = subPoint(target, position);
            CCPoint acceleration = subPoint(scalePoint(displacement, stiffness), scalePoint(velocity, damping));
            velocity = addPoint(velocity, scalePoint(acceleration, dt));
            position = addPoint(position, scalePoint(velocity, dt));
        }
    };

    struct AlphaBetaState
    {
        CCPoint position = CCPointZero;
        CCPoint velocity = CCPointZero;
        bool seeded = false;

        void reset()
        {
            *this = {};
        }

        void observe(CCPoint const& measurement)
        {
            if (!seeded)
            {
                position = measurement;
                velocity = CCPointZero;
                seeded = true;
                return;
            }

            CCPoint predicted = addPoint(position, velocity);
            CCPoint residual = subPoint(measurement, predicted);

            if (pointDistance(predicted, measurement) > 256.0f)
            {
                position = measurement;
                velocity = CCPointZero;
                return;
            }

            constexpr float alpha = 0.72f;
            constexpr float beta = 0.18f;
            position = addPoint(predicted, scalePoint(residual, alpha));
            velocity = addPoint(velocity, scalePoint(residual, beta));
        }
    };

    struct Fields
    {
        float timeTilNextTick = 0.0f;
        float progressTilNextTick = 0.0f;
        float modifiedDeltaReturn = 0.0f;

        MotionHistory cameraHistory;
        MotionHistory player1History;
        MotionHistory player2History;
        MotionHistory player1ScreenHistory;
        MotionHistory player2ScreenHistory;

        SpringState cameraSpring;
        SpringState player1Spring;
        SpringState player2Spring;

        AlphaBetaState cameraTracker;
        AlphaBetaState player1Tracker;
        AlphaBetaState player2Tracker;

        unsigned long long lastSpringPresentSerial = 0;
        bool visualApplied = false;
        bool wasEnabled = false;
        int lastMethod = -1;
    };

    FrameExtrapolationMethod getMethod()
    {
        int value = FrameExtrapolationMethodOption::get()->getValue();
        value = std::clamp(value, 0, 4);
        return static_cast<FrameExtrapolationMethod>(value);
    }

    void resetExtrapolationState()
    {
        auto self = m_fields.self();
        self->timeTilNextTick = 0.0f;
        self->progressTilNextTick = 0.0f;
        self->modifiedDeltaReturn = 0.0f;
        self->cameraHistory.reset();
        self->player1History.reset();
        self->player2History.reset();
        self->player1ScreenHistory.reset();
        self->player2ScreenHistory.reset();
        self->cameraSpring.reset();
        self->player1Spring.reset();
        self->player2Spring.reset();
        self->cameraTracker.reset();
        self->player1Tracker.reset();
        self->player2Tracker.reset();
        self->lastSpringPresentSerial = g_presentSerial;
        self->visualApplied = false;
        g_frameTiming.lastFxOffset = 0.0f;
    }

    void setGroundOffset(GJGroundLayer* ground, float moveBy)
    {
        if (!ground)
            return;

        auto children = ground->getChildren();
        if (!children)
            return;

        for (auto child : CCArrayExt<CCNode*>(children))
        {
            if (typeinfo_cast<CCSpriteBatchNode*>(child))
                child->setPositionX(moveBy);
        }
    }

    void restoreVisualState()
    {
        auto self = m_fields.self();
        if (!self->visualApplied)
            return;

        if (m_objectLayer && self->cameraHistory.seeded)
            m_objectLayer->setPosition(self->cameraHistory.lastPos);

        if (m_player1 && self->player1History.seeded)
            m_player1->CCNode::setPosition(self->player1History.lastPos);

        if (m_player2 && self->player2History.seeded)
            m_player2->CCNode::setPosition(self->player2History.lastPos);

        setGroundOffset(m_groundLayer, 0.0f);
        setGroundOffset(m_groundLayer2, 0.0f);
        self->visualApplied = false;
    }

    void sampleAuthoritativeMotion()
    {
        auto self = m_fields.self();

        CCPoint cameraPos = m_objectLayer ? m_objectLayer->getPosition() : CCPointZero;
        if (m_objectLayer)
        {
            self->cameraHistory.push(cameraPos);
            self->cameraTracker.observe(cameraPos);
            if (!self->cameraSpring.seeded)
                self->cameraSpring.seed(cameraPos);
        }

        if (m_player1)
        {
            CCPoint playerPos = m_player1->m_position;
            self->player1History.push(playerPos);
            // In object-layer coordinates, screen motion is local player motion
            // plus the object-layer translation. This lets Camera Relative
            // predict the small residual seen on screen instead of two large
            // independent world motions.
            self->player1ScreenHistory.push(addPoint(playerPos, cameraPos));
            self->player1Tracker.observe(playerPos);
            if (!self->player1Spring.seeded)
                self->player1Spring.seed(playerPos);
        }

        if (m_player2)
        {
            CCPoint playerPos = m_player2->m_position;
            self->player2History.push(playerPos);
            self->player2ScreenHistory.push(addPoint(playerPos, cameraPos));
            self->player2Tracker.observe(playerPos);
            if (!self->player2Spring.seeded)
                self->player2Spring.seed(playerPos);
        }
        else
        {
            self->player2History.reset();
            self->player2ScreenHistory.reset();
            self->player2Spring.reset();
            self->player2Tracker.reset();
        }
    }

    float getModifiedDelta(float dt)
    {
        auto pRet = GJBaseGameLayer::getModifiedDelta(dt);

        if (frameTimingDebuggerEnabled())
        {
            ++g_frameTiming.modifiedDeltaCallsSincePresent;
            g_frameTiming.lastModifiedDeltaMs = pRet * 1000.0f;
        }

        if (FrameExtrapolation::get()->getRealEnabled())
            m_fields->modifiedDeltaReturn = pRet;
        else
            m_fields->modifiedDeltaReturn = 0.0f;

        return pRet;
    }

    void applyLegacyLinear(float percent)
    {
        auto self = m_fields.self();
        CCPoint cameraOffset = scalePoint(self->cameraHistory.velocity, percent);

        if (m_objectLayer && self->cameraHistory.seeded)
            m_objectLayer->setPosition(addPoint(self->cameraHistory.lastPos, cameraOffset));

        setGroundOffset(m_groundLayer, cameraOffset.x);
        setGroundOffset(m_groundLayer2, cameraOffset.x);

        if (m_player1 && self->player1History.seeded)
            m_player1->CCNode::setPosition(addPoint(self->player1History.lastPos, scalePoint(self->player1History.velocity, percent)));

        if (m_player2 && self->player2History.seeded)
            m_player2->CCNode::setPosition(addPoint(self->player2History.lastPos, scalePoint(self->player2History.velocity, percent)));
    }

    void applyCameraRelative(float percent)
    {
        auto self = m_fields.self();
        CCPoint cameraOffset = scalePoint(self->cameraHistory.velocity, percent);

        if (m_objectLayer && self->cameraHistory.seeded)
            m_objectLayer->setPosition(addPoint(self->cameraHistory.lastPos, cameraOffset));

        setGroundOffset(m_groundLayer, cameraOffset.x);
        setGroundOffset(m_groundLayer2, cameraOffset.x);

        if (m_player1 && self->player1History.seeded)
        {
            CCPoint desiredScreenMotion = scalePoint(self->player1ScreenHistory.velocity, percent);
            CCPoint localResidual = subPoint(desiredScreenMotion, cameraOffset);
            m_player1->CCNode::setPosition(addPoint(self->player1History.lastPos, localResidual));
        }

        if (m_player2 && self->player2History.seeded)
        {
            CCPoint desiredScreenMotion = scalePoint(self->player2ScreenHistory.velocity, percent);
            CCPoint localResidual = subPoint(desiredScreenMotion, cameraOffset);
            m_player2->CCNode::setPosition(addPoint(self->player2History.lastPos, localResidual));
        }
    }

    void applySnapshotInterpolation(float percent)
    {
        auto self = m_fields.self();
        CCPoint visualCamera = self->cameraHistory.lastPos;

        if (self->cameraHistory.hasPrevious)
            visualCamera = lerpPoint(self->cameraHistory.previousPos, self->cameraHistory.lastPos, percent);

        CCPoint cameraOffset = subPoint(visualCamera, self->cameraHistory.lastPos);

        if (m_objectLayer && self->cameraHistory.seeded)
            m_objectLayer->setPosition(visualCamera);

        setGroundOffset(m_groundLayer, cameraOffset.x);
        setGroundOffset(m_groundLayer2, cameraOffset.x);

        if (m_player1 && self->player1History.seeded)
        {
            CCPoint visual = self->player1History.hasPrevious
                ? lerpPoint(self->player1History.previousPos, self->player1History.lastPos, percent)
                : self->player1History.lastPos;
            m_player1->CCNode::setPosition(visual);
        }

        if (m_player2 && self->player2History.seeded)
        {
            CCPoint visual = self->player2History.hasPrevious
                ? lerpPoint(self->player2History.previousPos, self->player2History.lastPos, percent)
                : self->player2History.lastPos;
            m_player2->CCNode::setPosition(visual);
        }
    }

    void applySpringFilter()
    {
        auto self = m_fields.self();

        if (self->lastSpringPresentSerial != g_presentSerial)
        {
            if (self->cameraHistory.seeded)
                self->cameraSpring.step(self->cameraHistory.lastPos, g_presentDtSeconds);
            if (self->player1History.seeded)
                self->player1Spring.step(self->player1History.lastPos, g_presentDtSeconds);
            if (self->player2History.seeded)
                self->player2Spring.step(self->player2History.lastPos, g_presentDtSeconds);

            self->lastSpringPresentSerial = g_presentSerial;
        }

        CCPoint cameraOffset = CCPointZero;
        if (m_objectLayer && self->cameraSpring.seeded && self->cameraHistory.seeded)
        {
            cameraOffset = subPoint(self->cameraSpring.position, self->cameraHistory.lastPos);
            m_objectLayer->setPosition(self->cameraSpring.position);
        }

        setGroundOffset(m_groundLayer, cameraOffset.x);
        setGroundOffset(m_groundLayer2, cameraOffset.x);

        if (m_player1 && self->player1Spring.seeded)
            m_player1->CCNode::setPosition(self->player1Spring.position);

        if (m_player2 && self->player2Spring.seeded)
            m_player2->CCNode::setPosition(self->player2Spring.position);
    }

    void applyAlphaBetaTracker(float percent)
    {
        auto self = m_fields.self();
        CCPoint visualCamera = self->cameraTracker.seeded
            ? addPoint(self->cameraTracker.position, scalePoint(self->cameraTracker.velocity, percent))
            : self->cameraHistory.lastPos;
        CCPoint cameraOffset = subPoint(visualCamera, self->cameraHistory.lastPos);

        if (m_objectLayer && self->cameraHistory.seeded)
            m_objectLayer->setPosition(visualCamera);

        setGroundOffset(m_groundLayer, cameraOffset.x);
        setGroundOffset(m_groundLayer2, cameraOffset.x);

        if (m_player1 && self->player1History.seeded)
        {
            CCPoint visual = self->player1Tracker.seeded
                ? addPoint(self->player1Tracker.position, scalePoint(self->player1Tracker.velocity, percent))
                : self->player1History.lastPos;
            m_player1->CCNode::setPosition(visual);
        }

        if (m_player2 && self->player2History.seeded)
        {
            CCPoint visual = self->player2Tracker.seeded
                ? addPoint(self->player2Tracker.position, scalePoint(self->player2Tracker.velocity, percent))
                : self->player2History.lastPos;
            m_player2->CCNode::setPosition(visual);
        }
    }

    void updateDebugEffectMagnitude(FrameExtrapolationMethod method)
    {
        auto self = m_fields.self();
        float magnitude = 0.0f;

        if (m_objectLayer && self->cameraHistory.seeded)
            magnitude = std::max(magnitude, pointDistance(m_objectLayer->getPosition(), self->cameraHistory.lastPos));
        if (m_player1 && self->player1History.seeded)
            magnitude = std::max(magnitude, pointDistance(m_player1->getPosition(), self->player1History.lastPos));
        if (m_player2 && self->player2History.seeded)
            magnitude = std::max(magnitude, pointDistance(m_player2->getPosition(), self->player2History.lastPos));

        g_frameTiming.activeMethod = static_cast<int>(method);
        g_frameTiming.lastFxOffset = magnitude;
    }

    virtual void update(float dt)
    {
        auto self = m_fields.self();

        // Undo the previous visual-only transform before Geometry Dash advances
        // authoritative gameplay. This prevents our presentation state from
        // leaking back into the next physics sample.
        restoreVisualState();

        if (frameTimingDebuggerEnabled())
            ++g_frameTiming.updatesSincePresent;

        self->modifiedDeltaReturn = 0.0f;
        GJBaseGameLayer::update(dt);

        auto playLayer = typeinfo_cast<PlayLayer*>(this);
        if (!playLayer)
            return;

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
            self->lastMethod = -1;
            g_frameTiming.lastInterpPercent = 0.0f;
            g_frameTiming.lastFxOffset = 0.0f;
            return;
        }

        self->wasEnabled = true;

        auto method = getMethod();
        int methodValue = static_cast<int>(method);
        if (self->lastMethod != methodValue)
        {
            resetExtrapolationState();
            self->lastMethod = methodValue;
        }

        if (isFlipping())
        {
            resetExtrapolationState();
            return;
        }

        if (!isRunning() || dt <= 0.0f || !std::isfinite(dt) || playLayer->m_levelEndAnimationStarted)
        {
            resetExtrapolationState();
            return;
        }

        if (self->modifiedDeltaReturn != 0.0f)
        {
            if (!std::isfinite(self->modifiedDeltaReturn) || self->modifiedDeltaReturn <= 0.0f || self->modifiedDeltaReturn > 0.05f)
            {
                resetExtrapolationState();
                return;
            }

            self->timeTilNextTick = self->modifiedDeltaReturn;
            self->progressTilNextTick = 0.0f;
            sampleAuthoritativeMotion();
        }
        else
        {
            self->progressTilNextTick += dt;
            if (self->timeTilNextTick > 0.0f)
                self->progressTilNextTick = std::min(self->progressTilNextTick, self->timeTilNextTick);
        }

        if (self->timeTilNextTick <= 0.0f || !std::isfinite(self->timeTilNextTick) || !self->cameraHistory.seeded)
            return;

        float percent = std::clamp(self->progressTilNextTick / self->timeTilNextTick, 0.0f, 1.0f);
        if (!std::isfinite(percent))
            return;

        g_frameTiming.lastInterpPercent = percent;

        switch (method)
        {
            case FrameExtrapolationMethod::LegacyLinear:
                applyLegacyLinear(percent);
                break;
            case FrameExtrapolationMethod::CameraRelative:
                applyCameraRelative(percent);
                break;
            case FrameExtrapolationMethod::SnapshotInterpolation:
                applySnapshotInterpolation(percent);
                break;
            case FrameExtrapolationMethod::SpringFilter:
                applySpringFilter();
                break;
            case FrameExtrapolationMethod::AlphaBetaTracker:
                applyAlphaBetaTracker(percent);
                break;
        }

        self->visualApplied = true;
        updateDebugEffectMagnitude(method);
    }
};
