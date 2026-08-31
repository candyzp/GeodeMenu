#include "../../Client/Module.hpp"
#include "../../Client/FloatSliderModule.hpp"
#include "../../Client/EnumModule.hpp"
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
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
        setDescription("Smooths the rendered player and camera after gameplay physics has finished. Physics and CBF stay authoritative.");
        setDisabled(false);
    }
};

enum class FrameExtrapolationMethod
{
    Classic = 0,
    CameraSync = 1,
    Snapshot = 2,
    Spring = 3,
    Tracker = 4,
};

class FrameExtrapolationMethodOption : public EnumModule
{
public:
    MODULE_SETUP(FrameExtrapolationMethodOption)
    {
        setName("Method");
        setID("frame-extrapolation/method-v2");
        setDescription("Classic predicts the latest motion. Camera Sync works in screen space. Snapshot blends two real states. Spring follows with damping. Tracker estimates and corrects motion.");
        listedValues = {
            {(int)FrameExtrapolationMethod::Classic, "Classic"},
            {(int)FrameExtrapolationMethod::CameraSync, "Camera Sync"},
            {(int)FrameExtrapolationMethod::Snapshot, "Snapshot"},
            {(int)FrameExtrapolationMethod::Spring, "Spring"},
            {(int)FrameExtrapolationMethod::Tracker, "Tracker"},
        };
        setDefaultValue((int)FrameExtrapolationMethod::Snapshot);
    }
};

class FrameExtrapolationCameraBlur : public Module
{
public:
    MODULE_SETUP(FrameExtrapolationCameraBlur)
    {
        setName("Camera Blur");
        setID("frame-extrapolation/camera-blur");
        setDescription("Adds a temporal camera-motion smear after the selected smoothing method. The player is compensated so the camera/world trails while the icon stays sharp.");
    }
};

class FrameExtrapolationCameraBlurAmount : public FloatSliderModule
{
public:
    MODULE_SETUP(FrameExtrapolationCameraBlurAmount)
    {
        setName("Amount");
        setID("frame-extrapolation/camera-blur/strength");
        setDescription("How much of the camera trail is visible.");
        setRange(0.0f, 1.0f);
        setDefaultValue(0.35f);
        setSnapValues({0.0f, 0.15f, 0.25f, 0.35f, 0.5f, 0.75f, 1.0f});
    }
};

class FrameExtrapolationCameraBlurLength : public FloatSliderModule
{
public:
    MODULE_SETUP(FrameExtrapolationCameraBlurLength)
    {
        setName("Length");
        setID("frame-extrapolation/camera-blur/trail-ms");
        setDescription("How long the camera trail follows behind, in milliseconds.");
        setRange(0.0f, 150.0f);
        setDefaultValue(40.0f);
        setSnapValues({0.0f, 12.0f, 20.0f, 32.0f, 40.0f, 60.0f, 90.0f, 120.0f, 150.0f});
    }
};

class FrameExtrapolationCameraBlurMax : public FloatSliderModule
{
public:
    MODULE_SETUP(FrameExtrapolationCameraBlurMax)
    {
        setName("Max Blur");
        setID("frame-extrapolation/camera-blur/max-smear");
        setDescription("Maximum distance the camera trail may fall behind. This also prevents giant streaks on portals and camera snaps.");
        setRange(0.0f, 32.0f);
        setDefaultValue(10.0f);
        setSnapValues({0.0f, 2.0f, 4.0f, 6.0f, 8.0f, 10.0f, 16.0f, 24.0f, 32.0f});
    }
};

class FrameTimingDebugger : public Module
{
public:
    MODULE_SETUP(FrameTimingDebugger)
    {
        setName("Debugger");
        setID("frame-extrapolation/frame-timing-debugger");
        setDescription("Shows whether the presentation pass, selected method, and camera blur are actually contributing visual motion.");
    }
};

class FrameTimingShowOverlay : public Module
{
public:
    MODULE_SETUP(FrameTimingShowOverlay)
    {
        setName("Show Stats");
        setID("frame-extrapolation/show-timing-overlay");
        setDescription("Shows the live method, method effect, blur effect, frame time, and movement values.");
    }
};

class FrameTimingLogSpikesOnly : public Module
{
public:
    MODULE_SETUP(FrameTimingLogSpikesOnly)
    {
        setName("Spike Logs");
        setID("frame-extrapolation/log-spikes-only");
        setDescription("Only writes timing logs when a rendered frame exceeds the spike limit.");
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
        setDescription("Frame time in milliseconds that counts as a spike.");
        setRange(16.0f, 100.0f);
        setDefaultValue(25.0f);
        setSnapValues({16.67f, 20.0f, 25.0f, 33.33f, 50.0f});
    }
};

SUBMIT_HACK(FrameExtrapolation)
SUBMIT_OPTION(FrameExtrapolation, FrameExtrapolationMethodOption)
SUBMIT_OPTION(FrameExtrapolation, FrameExtrapolationCameraBlur)
SUBMIT_OPTION(FrameExtrapolationCameraBlur, FrameExtrapolationCameraBlurAmount)
SUBMIT_OPTION(FrameExtrapolationCameraBlur, FrameExtrapolationCameraBlurLength)
SUBMIT_OPTION(FrameExtrapolationCameraBlur, FrameExtrapolationCameraBlurMax)
SUBMIT_OPTION(FrameExtrapolation, FrameTimingDebugger)
SUBMIT_OPTION(FrameExtrapolation, FrameTimingShowOverlay)
SUBMIT_OPTION(FrameExtrapolation, FrameTimingLogSpikesOnly)
SUBMIT_OPTION(FrameExtrapolation, FrameTimingSpikeThreshold)

namespace
{
    constexpr int kFrameTimingOverlayTag = 0x46544D47;

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
        return CCPoint{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    }

    float pointLength(CCPoint const& p)
    {
        return std::sqrt(p.x * p.x + p.y * p.y);
    }

    float pointDistance(CCPoint const& a, CCPoint const& b)
    {
        return pointLength(subPoint(a, b));
    }

    CCPoint clampMagnitude(CCPoint value, float maxLength)
    {
        if (maxLength <= 0.0f)
            return CCPointZero;

        float length = pointLength(value);
        if (!std::isfinite(length) || length <= 0.00001f || length <= maxLength)
            return value;

        return scalePoint(value, maxLength / length);
    }

    struct Snapshot
    {
        PlayLayer* owner = nullptr;
        CCPoint camera = CCPointZero;
        CCPoint player1 = CCPointZero;
        CCPoint player2 = CCPointZero;
        bool hasPlayer2 = false;
        std::chrono::steady_clock::time_point time = {};
        bool valid = false;
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
            constexpr float stiffness = 185.0f;
            constexpr float damping = 27.2f;

            CCPoint displacement = subPoint(target, position);
            CCPoint acceleration = subPoint(scalePoint(displacement, stiffness), scalePoint(velocity, damping));
            velocity = addPoint(velocity, scalePoint(acceleration, dt));
            position = addPoint(position, scalePoint(velocity, dt));
        }
    };

    struct TrackerState
    {
        CCPoint position = CCPointZero;
        CCPoint velocity = CCPointZero;
        bool seeded = false;

        void reset()
        {
            *this = {};
        }

        void seed(CCPoint const& measurement)
        {
            position = measurement;
            velocity = CCPointZero;
            seeded = true;
        }

        void observe(CCPoint const& measurement)
        {
            if (!seeded || pointDistance(position, measurement) > 256.0f)
            {
                seed(measurement);
                return;
            }

            CCPoint predicted = addPoint(position, velocity);
            CCPoint residual = subPoint(measurement, predicted);
            constexpr float alpha = 0.72f;
            constexpr float beta = 0.20f;

            position = addPoint(predicted, scalePoint(residual, alpha));
            velocity = addPoint(velocity, scalePoint(residual, beta));
        }
    };

    struct BlurState
    {
        CCPoint camera = CCPointZero;
        bool seeded = false;

        void reset()
        {
            *this = {};
        }

        void seed(CCPoint const& target)
        {
            camera = target;
            seeded = true;
        }
    };

    struct PresentationRuntime
    {
        PlayLayer* owner = nullptr;
        Snapshot previous;
        Snapshot current;

        SpringState springCamera;
        SpringState springPlayer1;
        SpringState springPlayer2;

        TrackerState trackerCamera;
        TrackerState trackerPlayer1;
        TrackerState trackerPlayer2;

        BlurState blur;

        bool visualApplied = false;
        CCPoint restoreCamera = CCPointZero;
        CCPoint restorePlayer1 = CCPointZero;
        CCPoint restorePlayer2 = CCPointZero;
        bool restoreHasPlayer2 = false;
        float appliedGroundShift = 0.0f;

        int lastMethod = -1;
        float lastMethodEffect = 0.0f;
        float lastBlurEffect = 0.0f;
        bool lastPresentationApplied = false;
        unsigned long long presentationSerial = 0;

        void clearFilters()
        {
            springCamera.reset();
            springPlayer1.reset();
            springPlayer2.reset();
            trackerCamera.reset();
            trackerPlayer1.reset();
            trackerPlayer2.reset();
            blur.reset();
            lastMethod = -1;
        }

        void clearSnapshots()
        {
            previous = {};
            current = {};
            owner = nullptr;
            clearFilters();
            lastMethodEffect = 0.0f;
            lastBlurEffect = 0.0f;
            lastPresentationApplied = false;
        }
    };

    PresentationRuntime g_runtime;

    struct FrameTimingStats
    {
        std::chrono::steady_clock::time_point lastPresent = {};
        bool hasLastPresent = false;
        float frameMs = 16.67f;
        unsigned long long frameIndex = 0;
        unsigned int schedulerUpdates = 0;
        unsigned int modifiedDeltaCalls = 0;
        float lastModifiedDeltaMs = 0.0f;
        float playerMove = 0.0f;
        float cameraMove = 0.0f;
        CCPoint previousPresentedPlayer = CCPointZero;
        CCPoint previousPresentedCamera = CCPointZero;
        bool hasPreviousPresented = false;
    };

    FrameTimingStats g_timing;

    FrameExtrapolationMethod selectedMethod()
    {
        int value = std::clamp(FrameExtrapolationMethodOption::get()->getValue(), 0, 4);
        return static_cast<FrameExtrapolationMethod>(value);
    }

    char const* methodName(FrameExtrapolationMethod method)
    {
        switch (method)
        {
            case FrameExtrapolationMethod::Classic: return "Classic";
            case FrameExtrapolationMethod::CameraSync: return "Camera Sync";
            case FrameExtrapolationMethod::Snapshot: return "Snapshot";
            case FrameExtrapolationMethod::Spring: return "Spring";
            case FrameExtrapolationMethod::Tracker: return "Tracker";
        }
        return "Unknown";
    }

    bool debuggerEnabled()
    {
        return FrameTimingDebugger::get()->getRealEnabled();
    }

    bool cameraBlurEnabled()
    {
        return FrameExtrapolation::get()->getRealEnabled() && FrameExtrapolationCameraBlur::get()->getRealEnabled();
    }

    bool validPlayLayer(PlayLayer* pl)
    {
        return pl &&
            pl->m_objectLayer &&
            pl->m_player1 &&
            pl->isRunning() &&
            !pl->m_levelEndAnimationStarted &&
            !pl->m_player1->m_isDead;
    }

    void shiftGround(GJGroundLayer* ground, float deltaX)
    {
        if (!ground || std::abs(deltaX) <= 0.00001f)
            return;

        auto children = ground->getChildren();
        if (!children)
            return;

        for (auto child : CCArrayExt<CCNode*>(children))
        {
            if (typeinfo_cast<CCSpriteBatchNode*>(child))
                child->setPositionX(child->getPositionX() + deltaX);
        }
    }

    void restorePresentation()
    {
        if (!g_runtime.visualApplied)
            return;

        auto pl = PlayLayer::get();
        if (pl && pl == g_runtime.owner)
        {
            if (pl->m_objectLayer)
                pl->m_objectLayer->setPosition(g_runtime.restoreCamera);
            if (pl->m_player1)
                pl->m_player1->CCNode::setPosition(g_runtime.restorePlayer1);
            if (pl->m_player2 && g_runtime.restoreHasPlayer2)
                pl->m_player2->CCNode::setPosition(g_runtime.restorePlayer2);

            shiftGround(pl->m_groundLayer, -g_runtime.appliedGroundShift);
            shiftGround(pl->m_groundLayer2, -g_runtime.appliedGroundShift);
        }

        g_runtime.visualApplied = false;
        g_runtime.appliedGroundShift = 0.0f;
    }

    Snapshot readSnapshot(PlayLayer* pl)
    {
        Snapshot out;
        out.owner = pl;
        out.camera = pl->m_objectLayer->getPosition();
        out.player1 = pl->m_player1->m_position;
        out.hasPlayer2 = pl->m_player2 != nullptr;
        if (out.hasPlayer2)
            out.player2 = pl->m_player2->m_position;
        out.time = std::chrono::steady_clock::now();
        out.valid = true;
        return out;
    }

    bool snapshotDiscontinuity(Snapshot const& a, Snapshot const& b)
    {
        if (!a.valid || !b.valid || a.owner != b.owner)
            return true;

        if (pointDistance(a.camera, b.camera) > 192.0f)
            return true;
        if (pointDistance(a.player1, b.player1) > 256.0f)
            return true;
        if (a.hasPlayer2 != b.hasPlayer2)
            return true;
        if (a.hasPlayer2 && pointDistance(a.player2, b.player2) > 256.0f)
            return true;

        return false;
    }

    void seedFiltersFromCurrent()
    {
        if (!g_runtime.current.valid)
            return;

        g_runtime.springCamera.seed(g_runtime.current.camera);
        g_runtime.springPlayer1.seed(g_runtime.current.player1);
        if (g_runtime.current.hasPlayer2)
            g_runtime.springPlayer2.seed(g_runtime.current.player2);
        else
            g_runtime.springPlayer2.reset();

        g_runtime.trackerCamera.seed(g_runtime.current.camera);
        g_runtime.trackerPlayer1.seed(g_runtime.current.player1);
        if (g_runtime.current.hasPlayer2)
            g_runtime.trackerPlayer2.seed(g_runtime.current.player2);
        else
            g_runtime.trackerPlayer2.reset();

        g_runtime.blur.seed(g_runtime.current.camera);
    }

    void captureAuthoritative(PlayLayer* pl)
    {
        Snapshot next = readSnapshot(pl);

        if (g_runtime.owner != pl || !g_runtime.current.valid)
        {
            g_runtime.clearSnapshots();
            g_runtime.owner = pl;
            g_runtime.previous = next;
            g_runtime.current = next;
            seedFiltersFromCurrent();
            return;
        }

        g_runtime.previous = g_runtime.current;
        g_runtime.current = next;

        if (snapshotDiscontinuity(g_runtime.previous, g_runtime.current))
        {
            g_runtime.previous = g_runtime.current;
            g_runtime.clearFilters();
            seedFiltersFromCurrent();
            return;
        }

        g_runtime.trackerCamera.observe(g_runtime.current.camera);
        g_runtime.trackerPlayer1.observe(g_runtime.current.player1);
        if (g_runtime.current.hasPlayer2)
            g_runtime.trackerPlayer2.observe(g_runtime.current.player2);
        else
            g_runtime.trackerPlayer2.reset();
    }

    float snapshotIntervalSeconds(float schedulerDt)
    {
        if (g_runtime.previous.valid && g_runtime.current.valid && g_runtime.previous.time != g_runtime.current.time)
        {
            float measured = std::chrono::duration<float>(g_runtime.current.time - g_runtime.previous.time).count();
            if (std::isfinite(measured) && measured > 0.0001f)
                return std::clamp(measured, 1.0f / 1000.0f, 0.05f);
        }

        if (!std::isfinite(schedulerDt) || schedulerDt <= 0.0f)
            schedulerDt = 1.0f / 60.0f;
        return std::clamp(schedulerDt, 1.0f / 1000.0f, 0.05f);
    }

    void computeClassic(float schedulerDt, CCPoint& camera, CCPoint& p1, CCPoint& p2)
    {
        float interval = snapshotIntervalSeconds(schedulerDt);
        float ratio = std::clamp(schedulerDt / interval, 0.5f, 1.5f);
        float lead = std::clamp(0.35f * ratio, 0.20f, 0.55f);

        camera = addPoint(g_runtime.current.camera, scalePoint(subPoint(g_runtime.current.camera, g_runtime.previous.camera), lead));
        p1 = addPoint(g_runtime.current.player1, scalePoint(subPoint(g_runtime.current.player1, g_runtime.previous.player1), lead));
        if (g_runtime.current.hasPlayer2)
            p2 = addPoint(g_runtime.current.player2, scalePoint(subPoint(g_runtime.current.player2, g_runtime.previous.player2), lead));
    }

    void computeCameraSync(float schedulerDt, CCPoint& camera, CCPoint& p1, CCPoint& p2)
    {
        float interval = snapshotIntervalSeconds(schedulerDt);
        float ratio = std::clamp(schedulerDt / interval, 0.5f, 1.5f);
        float cameraLead = std::clamp(0.48f * ratio, 0.25f, 0.70f);
        float screenLead = std::clamp(0.32f * ratio, 0.18f, 0.50f);

        CCPoint cameraVelocity = subPoint(g_runtime.current.camera, g_runtime.previous.camera);
        camera = addPoint(g_runtime.current.camera, scalePoint(cameraVelocity, cameraLead));

        CCPoint previousScreen1 = addPoint(g_runtime.previous.camera, g_runtime.previous.player1);
        CCPoint currentScreen1 = addPoint(g_runtime.current.camera, g_runtime.current.player1);
        CCPoint desiredScreen1 = addPoint(currentScreen1, scalePoint(subPoint(currentScreen1, previousScreen1), screenLead));
        p1 = subPoint(desiredScreen1, camera);

        if (g_runtime.current.hasPlayer2)
        {
            CCPoint previousScreen2 = addPoint(g_runtime.previous.camera, g_runtime.previous.player2);
            CCPoint currentScreen2 = addPoint(g_runtime.current.camera, g_runtime.current.player2);
            CCPoint desiredScreen2 = addPoint(currentScreen2, scalePoint(subPoint(currentScreen2, previousScreen2), screenLead));
            p2 = subPoint(desiredScreen2, camera);
        }
    }

    void computeSnapshot(CCPoint& camera, CCPoint& p1, CCPoint& p2)
    {
        // Half-frame-ish visual delay. Both endpoints are real authoritative
        // states, so this method never predicts a future gameplay position.
        constexpr float alpha = 0.58f;
        camera = lerpPoint(g_runtime.previous.camera, g_runtime.current.camera, alpha);
        p1 = lerpPoint(g_runtime.previous.player1, g_runtime.current.player1, alpha);
        if (g_runtime.current.hasPlayer2)
            p2 = lerpPoint(g_runtime.previous.player2, g_runtime.current.player2, alpha);
    }

    void computeSpring(float schedulerDt, CCPoint& camera, CCPoint& p1, CCPoint& p2)
    {
        g_runtime.springCamera.step(g_runtime.current.camera, schedulerDt);
        g_runtime.springPlayer1.step(g_runtime.current.player1, schedulerDt);
        if (g_runtime.current.hasPlayer2)
            g_runtime.springPlayer2.step(g_runtime.current.player2, schedulerDt);

        camera = g_runtime.springCamera.position;
        p1 = g_runtime.springPlayer1.position;
        if (g_runtime.current.hasPlayer2)
            p2 = g_runtime.springPlayer2.position;
    }

    void computeTracker(float schedulerDt, CCPoint& camera, CCPoint& p1, CCPoint& p2)
    {
        float interval = snapshotIntervalSeconds(schedulerDt);
        float ratio = std::clamp(schedulerDt / interval, 0.5f, 1.5f);
        float lead = std::clamp(0.24f * ratio, 0.12f, 0.36f);

        camera = addPoint(g_runtime.trackerCamera.position, scalePoint(g_runtime.trackerCamera.velocity, lead));
        p1 = addPoint(g_runtime.trackerPlayer1.position, scalePoint(g_runtime.trackerPlayer1.velocity, lead));
        if (g_runtime.current.hasPlayer2)
            p2 = addPoint(g_runtime.trackerPlayer2.position, scalePoint(g_runtime.trackerPlayer2.velocity, lead));
    }

    CCPoint computeBlurOffset(CCPoint const& methodCamera, float schedulerDt)
    {
        if (!cameraBlurEnabled())
        {
            g_runtime.blur.seed(methodCamera);
            return CCPointZero;
        }

        float amount = std::clamp(FrameExtrapolationCameraBlurAmount::get()->getValue(), 0.0f, 1.0f);
        float lengthMs = std::clamp(FrameExtrapolationCameraBlurLength::get()->getValue(), 0.0f, 150.0f);
        float maxBlur = std::clamp(FrameExtrapolationCameraBlurMax::get()->getValue(), 0.0f, 32.0f);

        if (amount <= 0.0001f || lengthMs <= 0.0001f || maxBlur <= 0.0001f)
        {
            g_runtime.blur.seed(methodCamera);
            return CCPointZero;
        }

        if (!g_runtime.blur.seeded || pointDistance(g_runtime.blur.camera, methodCamera) > std::max(96.0f, maxBlur * 6.0f))
        {
            g_runtime.blur.seed(methodCamera);
            return CCPointZero;
        }

        float dt = schedulerDt;
        if (!std::isfinite(dt) || dt <= 0.0f)
            dt = 1.0f / 60.0f;
        dt = std::clamp(dt, 1.0f / 1000.0f, 0.05f);

        float trailSeconds = std::max(lengthMs * 0.001f, 0.001f);
        float follow = 1.0f - std::exp(-dt / trailSeconds);
        g_runtime.blur.camera = addPoint(g_runtime.blur.camera, scalePoint(subPoint(methodCamera, g_runtime.blur.camera), follow));

        CCPoint rawTrail = subPoint(g_runtime.blur.camera, methodCamera);
        return clampMagnitude(scalePoint(rawTrail, amount), maxBlur);
    }

    void resetFiltersForMethod(FrameExtrapolationMethod method)
    {
        int methodValue = static_cast<int>(method);
        if (g_runtime.lastMethod == methodValue)
            return;

        if (g_runtime.current.valid)
        {
            g_runtime.springCamera.seed(g_runtime.current.camera);
            g_runtime.springPlayer1.seed(g_runtime.current.player1);
            if (g_runtime.current.hasPlayer2)
                g_runtime.springPlayer2.seed(g_runtime.current.player2);

            g_runtime.trackerCamera.seed(g_runtime.current.camera);
            g_runtime.trackerPlayer1.seed(g_runtime.current.player1);
            if (g_runtime.current.hasPlayer2)
                g_runtime.trackerPlayer2.seed(g_runtime.current.player2);

            g_runtime.blur.seed(g_runtime.current.camera);
        }

        g_runtime.lastMethod = methodValue;
    }

    void applyPresentation(PlayLayer* pl, float schedulerDt)
    {
        g_runtime.lastPresentationApplied = false;
        g_runtime.lastMethodEffect = 0.0f;
        g_runtime.lastBlurEffect = 0.0f;

        if (!FrameExtrapolation::get()->getRealEnabled() || !g_runtime.previous.valid || !g_runtime.current.valid)
            return;

        auto method = selectedMethod();
        resetFiltersForMethod(method);

        CCPoint visualCamera = g_runtime.current.camera;
        CCPoint visualPlayer1 = g_runtime.current.player1;
        CCPoint visualPlayer2 = g_runtime.current.player2;

        switch (method)
        {
            case FrameExtrapolationMethod::Classic:
                computeClassic(schedulerDt, visualCamera, visualPlayer1, visualPlayer2);
                break;
            case FrameExtrapolationMethod::CameraSync:
                computeCameraSync(schedulerDt, visualCamera, visualPlayer1, visualPlayer2);
                break;
            case FrameExtrapolationMethod::Snapshot:
                computeSnapshot(visualCamera, visualPlayer1, visualPlayer2);
                break;
            case FrameExtrapolationMethod::Spring:
                computeSpring(schedulerDt, visualCamera, visualPlayer1, visualPlayer2);
                break;
            case FrameExtrapolationMethod::Tracker:
                computeTracker(schedulerDt, visualCamera, visualPlayer1, visualPlayer2);
                break;
        }

        if (!std::isfinite(visualCamera.x) || !std::isfinite(visualCamera.y) ||
            !std::isfinite(visualPlayer1.x) || !std::isfinite(visualPlayer1.y))
            return;

        g_runtime.lastMethodEffect = std::max(
            pointDistance(visualCamera, g_runtime.current.camera),
            pointDistance(visualPlayer1, g_runtime.current.player1)
        );
        if (g_runtime.current.hasPlayer2)
            g_runtime.lastMethodEffect = std::max(g_runtime.lastMethodEffect, pointDistance(visualPlayer2, g_runtime.current.player2));

        CCPoint blurOffset = computeBlurOffset(visualCamera, schedulerDt);
        g_runtime.lastBlurEffect = pointLength(blurOffset);

        g_runtime.restoreCamera = pl->m_objectLayer->getPosition();
        g_runtime.restorePlayer1 = pl->m_player1->getPosition();
        g_runtime.restoreHasPlayer2 = pl->m_player2 != nullptr;
        if (g_runtime.restoreHasPlayer2)
            g_runtime.restorePlayer2 = pl->m_player2->getPosition();

        CCPoint finalCamera = addPoint(visualCamera, blurOffset);
        CCPoint finalPlayer1 = subPoint(visualPlayer1, blurOffset);
        CCPoint finalPlayer2 = subPoint(visualPlayer2, blurOffset);

        pl->m_objectLayer->setPosition(finalCamera);
        pl->m_player1->CCNode::setPosition(finalPlayer1);
        if (pl->m_player2 && g_runtime.current.hasPlayer2)
            pl->m_player2->CCNode::setPosition(finalPlayer2);

        g_runtime.appliedGroundShift = finalCamera.x - g_runtime.current.camera.x;
        shiftGround(pl->m_groundLayer, g_runtime.appliedGroundShift);
        shiftGround(pl->m_groundLayer2, g_runtime.appliedGroundShift);

        g_runtime.visualApplied = true;
        g_runtime.lastPresentationApplied = true;
        ++g_runtime.presentationSerial;
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
        if (!debuggerEnabled() || !FrameTimingShowOverlay::get()->getRealEnabled())
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

        auto method = selectedMethod();
        label->setString(fmt::format(
            "METHOD: {}\nPRESENT: {}\nMETHOD FX: {:.3f}\nBLUR FX: {:.3f}\nFRAME: {:.2f} ms\nPLAYER MOVE: {:.3f}\nCAMERA MOVE: {:.3f}\nPHYSICS CALLS: {}",
            methodName(method),
            g_runtime.lastPresentationApplied ? "YES" : "NO",
            g_runtime.lastMethodEffect,
            g_runtime.lastBlurEffect,
            g_timing.frameMs,
            g_timing.playerMove,
            g_timing.cameraMove,
            g_timing.modifiedDeltaCalls
        ).c_str());
    }

    void onPresentedFrame()
    {
        auto now = std::chrono::steady_clock::now();
        if (g_timing.hasLastPresent)
        {
            float measured = std::chrono::duration<float>(now - g_timing.lastPresent).count();
            if (std::isfinite(measured) && measured > 0.0f)
                g_timing.frameMs = std::clamp(measured * 1000.0f, 1.0f, 100.0f);
        }
        else
        {
            g_timing.hasLastPresent = true;
        }

        g_timing.lastPresent = now;
        ++g_timing.frameIndex;

        if (g_runtime.current.valid)
        {
            if (g_timing.hasPreviousPresented)
            {
                g_timing.playerMove = pointDistance(g_runtime.current.player1, g_timing.previousPresentedPlayer);
                g_timing.cameraMove = pointDistance(g_runtime.current.camera, g_timing.previousPresentedCamera);
            }
            else
            {
                g_timing.hasPreviousPresented = true;
            }

            g_timing.previousPresentedPlayer = g_runtime.current.player1;
            g_timing.previousPresentedCamera = g_runtime.current.camera;
        }

        if (debuggerEnabled())
        {
            bool spike = g_timing.frameMs >= FrameTimingSpikeThreshold::get()->getValue();
            if (!FrameTimingLogSpikesOnly::get()->getRealEnabled() || spike)
            {
                log::info(
                    "[FrameTiming] method={} present={} methodFx={:.3f} blurFx={:.3f} frame={:.2f}ms playerMove={:.3f} cameraMove={:.3f} physicsCalls={}{}",
                    methodName(selectedMethod()),
                    g_runtime.lastPresentationApplied,
                    g_runtime.lastMethodEffect,
                    g_runtime.lastBlurEffect,
                    g_timing.frameMs,
                    g_timing.playerMove,
                    g_timing.cameraMove,
                    g_timing.modifiedDeltaCalls,
                    spike ? " SPIKE" : ""
                );
            }

            updateTimingOverlay();
        }
        else
        {
            removeTimingOverlay();
        }

        g_timing.schedulerUpdates = 0;
        g_timing.modifiedDeltaCalls = 0;
    }
}

// CBF also hooks CCScheduler::update. This hook intentionally does not alter
// CBF or physics. It restores last frame's visual-only transforms, lets the
// entire scheduler/CBF/game update finish, samples the authoritative result,
// then applies one presentation transform for the frame that is about to draw.
class $modify(FramePresentationScheduler, CCScheduler)
{
    void update(float dt)
    {
        restorePresentation();

        CCScheduler::update(dt);
        ++g_timing.schedulerUpdates;

        auto pl = PlayLayer::get();
        if (!validPlayLayer(pl))
        {
            if (g_runtime.owner != pl)
                g_runtime.clearSnapshots();
            g_runtime.lastPresentationApplied = false;
            g_runtime.lastMethodEffect = 0.0f;
            g_runtime.lastBlurEffect = 0.0f;
            return;
        }

        captureAuthoritative(pl);
        applyPresentation(pl, dt);
    }
};

// Debug-only observation. We return the exact value produced by the rest of
// the hook chain, including CBF, and never modify physics timing here.
class $modify(FrameTimingDeltaObserver, GJBaseGameLayer)
{
    double getModifiedDelta(float dt)
    {
        double result = GJBaseGameLayer::getModifiedDelta(dt);
        if (debuggerEnabled())
        {
            ++g_timing.modifiedDeltaCalls;
            g_timing.lastModifiedDeltaMs = static_cast<float>(result * 1000.0);
        }
        return result;
    }
};

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
