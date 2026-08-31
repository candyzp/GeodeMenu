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
            setDescription("Smooths between frames using the selected extrapolation method.");
            setDisabled(false);
        }
};

enum class FrameExtrapolationMethod
{
    LegacyLinear = 0,
    Smoothstep = 1,
    AdamsBashforth2 = 2,
    AdamsBashforth3 = 3,
    AdaptiveHybrid = 4,
};

class FrameExtrapolationMethodOption : public EnumModule
{
    public:
        MODULE_SETUP(FrameExtrapolationMethodOption)
        {
            setName("Extrapolation Method");
            setID("frame-extrapolation/method");
            setDescription("Selects the prediction method used automatically whenever Frame Extrapolation is enabled.");
            listedValues = {
                {(int)FrameExtrapolationMethod::LegacyLinear, "Legacy Linear"},
                {(int)FrameExtrapolationMethod::Smoothstep, "Smoothstep"},
                {(int)FrameExtrapolationMethod::AdamsBashforth2, "Adams-Bashforth 2"},
                {(int)FrameExtrapolationMethod::AdamsBashforth3, "Adams-Bashforth 3"},
                {(int)FrameExtrapolationMethod::AdaptiveHybrid, "Adaptive Hybrid"},
            };
            setDefaultValue((int)FrameExtrapolationMethod::AdaptiveHybrid);
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
SUBMIT_OPTION(FrameExtrapolation, FrameExtrapolationMethodOption)
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
            g_frameTiming.renderDtMs = std::chrono::duration<float, std::milli>(now - g_frameTiming.lastPresent).count();
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

        g_frameTiming.overlayElapsedMs += g_frameTiming.renderDtMs;
        if (g_frameTiming.overlayElapsedMs >= 100.0f)
        {
            updateTimingOverlay();
            g_frameTiming.overlayElapsedMs = 0.0f;
        }

        g_frameTiming.updatesSincePresent = 0;
        g_frameTiming.modifiedDeltaCallsSincePresent = 0;
    }

    float smoothstep01(float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    float pointLength(CCPoint const& p)
    {
        return std::sqrt(p.x * p.x + p.y * p.y);
    }

    CCPoint clampVectorMagnitude(CCPoint value, float maxLength)
    {
        float length = pointLength(value);
        if (length <= maxLength || length <= 0.00001f)
            return value;

        float scale = maxLength / length;
        return {value.x * scale, value.y * scale};
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
        CCPoint lastPos = CCPointZero;
        CCPoint velocity0 = CCPointZero;
        CCPoint velocity1 = CCPointZero;
        CCPoint velocity2 = CCPointZero;
        int velocitySamples = 0;
        bool seeded = false;

        void reset()
        {
            *this = {};
        }

        void push(CCPoint const& pos)
        {
            if (!seeded)
            {
                lastPos = pos;
                seeded = true;
                return;
            }

            velocity2 = velocity1;
            velocity1 = velocity0;
            velocity0 = pos - lastPos;
            lastPos = pos;
            velocitySamples = std::min(velocitySamples + 1, 3);
        }
    };

    struct Fields
    {
        float timeTilNextTick = 0;
        float progressTilNextTick = 0;
        float modifiedDeltaReturn = 0;

        MotionHistory cameraHistory;
        MotionHistory player1History;
        MotionHistory player2History;

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
        self->timeTilNextTick = 0;
        self->progressTilNextTick = 0;
        self->modifiedDeltaReturn = 0;
        self->cameraHistory.reset();
        self->player1History.reset();
        self->player2History.reset();
    }

    CCPoint getPredictedStep(MotionHistory const& history, FrameExtrapolationMethod method)
    {
        if (history.velocitySamples <= 0)
            return CCPointZero;

        CCPoint v0 = history.velocity0;
        CCPoint v1 = history.velocity1;
        CCPoint v2 = history.velocity2;

        switch (method)
        {
            case FrameExtrapolationMethod::LegacyLinear:
            case FrameExtrapolationMethod::Smoothstep:
                return v0;

            case FrameExtrapolationMethod::AdamsBashforth2:
                if (history.velocitySamples < 2)
                    return v0;
                return {
                    1.5f * v0.x - 0.5f * v1.x,
                    1.5f * v0.y - 0.5f * v1.y
                };

            case FrameExtrapolationMethod::AdamsBashforth3:
                if (history.velocitySamples < 3)
                {
                    if (history.velocitySamples >= 2)
                        return {1.5f * v0.x - 0.5f * v1.x, 1.5f * v0.y - 0.5f * v1.y};
                    return v0;
                }
                return {
                    (23.0f * v0.x - 16.0f * v1.x + 5.0f * v2.x) / 12.0f,
                    (23.0f * v0.y - 16.0f * v1.y + 5.0f * v2.y) / 12.0f
                };

            case FrameExtrapolationMethod::AdaptiveHybrid:
            {
                if (history.velocitySamples < 2)
                    return v0;

                CCPoint acceleration = v0 - v1;
                float speed = pointLength(v0);
                float accel = pointLength(acceleration);
                float ratio = accel / std::max(speed, 0.001f);

                CCPoint predicted = v0;

                if (history.velocitySamples >= 3 && ratio < 0.30f)
                {
                    predicted = CCPoint{
                        (23.0f * v0.x - 16.0f * v1.x + 5.0f * v2.x) / 12.0f,
                        (23.0f * v0.y - 16.0f * v1.y + 5.0f * v2.y) / 12.0f
                    };
                }
                else if (ratio < 0.75f)
                {
                    predicted = CCPoint{
                        1.5f * v0.x - 0.5f * v1.x,
                        1.5f * v0.y - 0.5f * v1.y
                    };
                }

                float maxPrediction = std::max(speed * 1.35f, 0.25f);
                return clampVectorMagnitude(predicted, maxPrediction);
            }
        }

        return v0;
    }

    CCPoint getVisualOffset(MotionHistory const& history, FrameExtrapolationMethod method, float percent)
    {
        float phase = method == FrameExtrapolationMethod::Smoothstep ? smoothstep01(percent) : percent;
        CCPoint step = getPredictedStep(history, method);
        return {step.x * phase, step.y * phase};
    }

    float getRotationPhase(FrameExtrapolationMethod method, float percent)
    {
        return method == FrameExtrapolationMethod::Smoothstep ? smoothstep01(percent) : percent;
    }

    void sampleAuthoritativeMotion()
    {
        auto self = m_fields.self();

        if (m_objectLayer)
            self->cameraHistory.push(m_objectLayer->getPosition());

        if (m_player1)
            self->player1History.push(m_player1->m_position);

        if (m_player2)
            self->player2History.push(m_player2->m_position);
        else
            self->player2History.reset();
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
            m_fields->modifiedDeltaReturn = 0;

        return pRet;
    }

    virtual void update(float dt)
    {
        auto self = m_fields.self();

        if (frameTimingDebuggerEnabled())
            ++g_frameTiming.updatesSincePresent;

        self->modifiedDeltaReturn = 0;
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

        if (!isRunning() || dt <= 0 || !std::isfinite(dt) || playLayer->m_levelEndAnimationStarted)
        {
            resetExtrapolationState();
            return;
        }

        if (self->modifiedDeltaReturn != 0)
        {
            if (!std::isfinite(self->modifiedDeltaReturn) || self->modifiedDeltaReturn <= 0 || self->modifiedDeltaReturn > 0.05f)
            {
                resetExtrapolationState();
                return;
            }

            self->timeTilNextTick = self->modifiedDeltaReturn;
            self->progressTilNextTick = 0;
            sampleAuthoritativeMotion();
        }
        else
        {
            self->progressTilNextTick += dt;
            if (self->timeTilNextTick > 0)
                self->progressTilNextTick = std::min(self->progressTilNextTick, self->timeTilNextTick);
        }

        if (self->timeTilNextTick <= 0 || !std::isfinite(self->timeTilNextTick))
            return;

        float percent = std::clamp(self->progressTilNextTick / self->timeTilNextTick, 0.0f, 1.0f);
        g_frameTiming.lastInterpPercent = percent;

        if (!std::isfinite(percent))
            return;

        CCPoint cameraOffset = getVisualOffset(self->cameraHistory, method, percent);

        if (m_objectLayer && self->cameraHistory.seeded)
        {
            auto base = self->cameraHistory.lastPos;
            m_objectLayer->setPosition(base + cameraOffset);
        }

        extrapolateGround(m_groundLayer, cameraOffset.x);
        extrapolateGround(m_groundLayer2, cameraOffset.x);

        extrapolatePlayer(m_player1, self->player1History, method, percent);

        if (m_player2)
            extrapolatePlayer(m_player2, self->player2History, method, percent);
    }

    float playerGetRotatedHitbox(PlayerObject* player)
    {
        if (player && player->m_isSideways)
            return -90.0f;
        return 0.0f;
    }

    void extrapolatePlayer(PlayerObject* player, MotionHistory const& history, FrameExtrapolationMethod method, float percent)
    {
        if (!player || !history.seeded)
            return;

        CCPoint offset = getVisualOffset(history, method, percent);
        player->CCNode::setPosition(history.lastPos + offset);

        float rotateSpeed = (player->m_isBall && player->m_isBallRotating) ? 1.0f : player->m_rotateSpeed;
        float tickRate = selfTickRate();
        float endRot = tickRate > 0.0f ? (player->m_rotationSpeed * rotateSpeed) / tickRate : 0.0f;
        float phase = getRotationPhase(method, percent);

        if (player->m_mainLayer)
            player->m_mainLayer->setRotation(endRot * phase + playerGetRotatedHitbox(player));
    }

    float selfTickRate()
    {
        auto self = m_fields.self();
        if (self->timeTilNextTick <= 0.0f)
            return 240.0f;
        return 1.0f / self->timeTilNextTick;
    }

    void extrapolateGround(GJGroundLayer* ground, float moveBy)
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
};
