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

class FrameExtrapolationCameraBlur : public Module
{
    public:
        MODULE_SETUP(FrameExtrapolationCameraBlur)
        {
            setName("Camera Blur");
            setID("frame-extrapolation/camera-blur");
            setDescription("Adds a camera-only motion trail on top of the selected smoothing method. The player stays sharp.");
        }
};

class FrameExtrapolationCameraBlurStrength : public FloatSliderModule
{
    public:
        MODULE_SETUP(FrameExtrapolationCameraBlurStrength)
        {
            setName("Amount");
            setID("frame-extrapolation/camera-blur/strength");
            setDescription("How strong the camera blur is. Higher values make camera motion trail farther behind.");
            setRange(0.0f, 1.0f);
            setDefaultValue(0.35f);
            setSnapValues({0.0f, 0.15f, 0.25f, 0.35f, 0.5f, 0.75f, 1.0f});
        }
};

class FrameExtrapolationCameraBlurTrail : public FloatSliderModule
{
    public:
        MODULE_SETUP(FrameExtrapolationCameraBlurTrail)
        {
            setName("Length");
            setID("frame-extrapolation/camera-blur/trail-ms");
            setDescription("How long the camera trail lasts. Higher values make the blur hang behind for longer.");
            setRange(0.0f, 140.0f);
            setDefaultValue(42.0f);
            setSnapValues({0.0f, 16.0f, 24.0f, 32.0f, 42.0f, 60.0f, 90.0f, 120.0f, 140.0f});
        }
};

class FrameExtrapolationCameraBlurMaxSmear : public FloatSliderModule
{
    public:
        MODULE_SETUP(FrameExtrapolationCameraBlurMaxSmear)
        {
            setName("Max Blur");
            setID("frame-extrapolation/camera-blur/max-smear");
            setDescription("Maximum distance the camera trail may reach. This prevents portals and respawns from creating giant jumps.");
            setRange(0.0f, 64.0f);
            setDefaultValue(12.0f);
            setSnapValues({0.0f, 4.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f, 48.0f, 64.0f});
        }
};

namespace
{
    std::chrono::steady_clock::time_point g_lastBlurPresent = {};
    float g_blurPresentDt = 1.0f / 60.0f;
    unsigned long long g_blurPresentSerial = 0;

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

    float pointLength(CCPoint const& p)
    {
        return std::sqrt(p.x * p.x + p.y * p.y);
    }

    CCPoint clampMagnitude(CCPoint value, float maxLength)
    {
        if (maxLength <= 0.0f)
            return CCPointZero;

        float length = pointLength(value);
        if (length <= maxLength || length <= 0.00001f)
            return value;

        return scalePoint(value, maxLength / length);
    }

    bool isUnderObjectLayer(CCNode* node, CCNode* root)
    {
        if (!node || !root)
            return false;

        for (auto parent = node->getParent(); parent; parent = parent->getParent())
        {
            if (parent == root)
                return true;
        }
        return false;
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

    void ensureCameraBlurOptionsRegistered()
    {
        static bool registered = false;
        if (registered)
            return;

        auto parent = Module::getByID("frame-extrapolation");
        if (!parent)
            return;

        auto blur = FrameExtrapolationCameraBlur::get();
        parent->addOption(blur);
        blur->addOption(FrameExtrapolationCameraBlurStrength::get());
        blur->addOption(FrameExtrapolationCameraBlurTrail::get());
        blur->addOption(FrameExtrapolationCameraBlurMaxSmear::get());
        registered = true;
    }

    bool cameraBlurEnabled()
    {
        auto parent = Module::getByID("frame-extrapolation");
        return parent && parent->getRealEnabled() && FrameExtrapolationCameraBlur::get()->getRealEnabled();
    }

    void onBlurPresentedFrame()
    {
        auto now = std::chrono::steady_clock::now();
        if (g_lastBlurPresent != std::chrono::steady_clock::time_point{})
        {
            float measured = std::chrono::duration<float>(now - g_lastBlurPresent).count();
            if (std::isfinite(measured) && measured > 0.0f)
                g_blurPresentDt = std::clamp(measured, 1.0f / 1000.0f, 1.0f / 15.0f);
        }

        g_lastBlurPresent = now;
        ++g_blurPresentSerial;
    }
}

#ifdef GEODE_IS_ANDROID
class $modify(FrameExtrapolationCameraBlurPresentHook, CCDirector)
{
    void drawScene()
    {
        CCDirector::drawScene();
        onBlurPresentedFrame();
    }
};
#else
class $modify(FrameExtrapolationCameraBlurPresentHook, CCEGLView)
{
    void swapBuffers()
    {
        onBlurPresentedFrame();
        CCEGLView::swapBuffers();
    }
};
#endif

class $modify(FrameExtrapolationCameraBlurGameLayer, GJBaseGameLayer)
{
    struct Fields
    {
        CCPoint filteredCamera = CCPointZero;
        CCPoint previousTarget = CCPointZero;
        CCPoint savedCamera = CCPointZero;
        CCPoint savedPlayer1 = CCPointZero;
        CCPoint savedPlayer2 = CCPointZero;
        CCPoint appliedDelta = CCPointZero;

        unsigned long long lastPresentSerial = 0;
        bool seeded = false;
        bool visualApplied = false;
        bool compensatedPlayer1 = false;
        bool compensatedPlayer2 = false;
    };

    static void onModify(auto& self)
    {
        // Restore our previous visual transform before every other update hook,
        // then apply Camera Blur after every other update hook has finished.
        // This guarantees the selected Frame Extrapolation method runs first.
        if (!self.setHookPriorityPre("GJBaseGameLayer::update", Priority::First))
            log::warn("Camera Blur: failed to set early restore priority");
        if (!self.setHookPriorityPost("GJBaseGameLayer::update", Priority::Last))
            log::warn("Camera Blur: failed to set late apply priority");
    }

    void restoreCameraBlurVisual()
    {
        auto self = m_fields.self();
        if (!self->visualApplied)
            return;

        if (m_objectLayer)
            m_objectLayer->setPosition(self->savedCamera);
        if (m_player1 && self->compensatedPlayer1)
            m_player1->CCNode::setPosition(self->savedPlayer1);
        if (m_player2 && self->compensatedPlayer2)
            m_player2->CCNode::setPosition(self->savedPlayer2);

        shiftGround(m_groundLayer, -self->appliedDelta.x);
        shiftGround(m_groundLayer2, -self->appliedDelta.x);

        self->visualApplied = false;
        self->appliedDelta = CCPointZero;
    }

    void resetCameraBlurState()
    {
        auto self = m_fields.self();
        restoreCameraBlurVisual();
        self->filteredCamera = CCPointZero;
        self->previousTarget = CCPointZero;
        self->lastPresentSerial = g_blurPresentSerial;
        self->seeded = false;
        self->compensatedPlayer1 = false;
        self->compensatedPlayer2 = false;
    }

    void update(float dt)
    {
        auto self = m_fields.self();

        // Never let a presentation-only camera offset leak into gameplay.
        restoreCameraBlurVisual();

        GJBaseGameLayer::update(dt);

        ensureCameraBlurOptionsRegistered();

        auto playLayer = typeinfo_cast<PlayLayer*>(this);
        if (!playLayer || !cameraBlurEnabled() || !m_objectLayer || !isRunning() || playLayer->m_levelEndAnimationStarted)
        {
            if (self->seeded)
                resetCameraBlurState();
            return;
        }

        float amount = std::clamp(FrameExtrapolationCameraBlurStrength::get()->getValue(), 0.0f, 1.0f);
        float lengthMs = std::clamp(FrameExtrapolationCameraBlurTrail::get()->getValue(), 0.0f, 140.0f);
        float maxBlur = std::clamp(FrameExtrapolationCameraBlurMaxSmear::get()->getValue(), 0.0f, 64.0f);

        CCPoint targetCamera = m_objectLayer->getPosition();

        if (!self->seeded)
        {
            self->filteredCamera = targetCamera;
            self->previousTarget = targetCamera;
            self->lastPresentSerial = g_blurPresentSerial;
            self->seeded = true;
            return;
        }

        if (amount <= 0.0001f || lengthMs <= 0.0001f || maxBlur <= 0.0001f)
        {
            self->filteredCamera = targetCamera;
            self->previousTarget = targetCamera;
            self->lastPresentSerial = g_blurPresentSerial;
            return;
        }

        CCPoint targetJump = subPoint(targetCamera, self->previousTarget);
        if (pointLength(targetJump) > std::max(128.0f, maxBlur * 6.0f))
        {
            // Portal, respawn, camera snap, or scene discontinuity.
            self->filteredCamera = targetCamera;
            self->previousTarget = targetCamera;
            self->lastPresentSerial = g_blurPresentSerial;
            return;
        }

        if (self->lastPresentSerial != g_blurPresentSerial)
        {
            float lengthSeconds = std::max(lengthMs * 0.001f, 0.001f);
            float normalFollow = 1.0f - std::exp(-g_blurPresentDt / lengthSeconds);

            // Amount controls how much of the temporal lag is kept. At 1.0 the
            // full trail is visible; at lower values the filter catches up more.
            float follow = 1.0f - amount * (1.0f - normalFollow);
            follow = std::clamp(follow, 0.0f, 1.0f);

            self->filteredCamera = addPoint(
                self->filteredCamera,
                scalePoint(subPoint(targetCamera, self->filteredCamera), follow)
            );
            self->lastPresentSerial = g_blurPresentSerial;
        }

        self->previousTarget = targetCamera;

        CCPoint blurDelta = clampMagnitude(subPoint(self->filteredCamera, targetCamera), maxBlur);
        if (pointLength(blurDelta) <= 0.01f)
            return;

        self->savedCamera = targetCamera;
        self->savedPlayer1 = m_player1 ? m_player1->getPosition() : CCPointZero;
        self->savedPlayer2 = m_player2 ? m_player2->getPosition() : CCPointZero;
        self->compensatedPlayer1 = m_player1 && isUnderObjectLayer(m_player1, m_objectLayer);
        self->compensatedPlayer2 = m_player2 && isUnderObjectLayer(m_player2, m_objectLayer);
        self->appliedDelta = blurDelta;

        m_objectLayer->setPosition(addPoint(targetCamera, blurDelta));

        // Blur only the camera/world. If a player is parented under the object
        // layer, counter-shift it so its screen-space position stays sharp.
        if (self->compensatedPlayer1)
            m_player1->CCNode::setPosition(subPoint(self->savedPlayer1, blurDelta));
        if (self->compensatedPlayer2)
            m_player2->CCNode::setPosition(subPoint(self->savedPlayer2, blurDelta));

        shiftGround(m_groundLayer, blurDelta.x);
        shiftGround(m_groundLayer2, blurDelta.x);
        self->visualApplied = true;
    }
};
