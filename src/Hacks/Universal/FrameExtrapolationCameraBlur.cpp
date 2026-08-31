#include "../../Client/Module.hpp"
#include "../../Client/FloatSliderModule.hpp"
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/PlayLayer.hpp>

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
            setDescription("Adds a temporal camera-only motion smear on top of the selected Frame Extrapolation method. Gameplay, the player sprite, and UI stay sharp.");
        }
};

class FrameExtrapolationCameraBlurStrength : public FloatSliderModule
{
    public:
        MODULE_SETUP(FrameExtrapolationCameraBlurStrength)
        {
            setName("Blur Strength");
            setID("frame-extrapolation/camera-blur/strength");
            setDescription("How strongly the camera trails recent motion. 0 disables the visible smear while 1 uses the full trail response.");
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
            setName("Blur Trail");
            setID("frame-extrapolation/camera-blur/trail-ms");
            setDescription("Temporal length of the camera smear in milliseconds. Higher values leave a longer camera trail.");
            setRange(0.0f, 100.0f);
            setDefaultValue(32.0f);
            setSnapValues({0.0f, 8.0f, 16.0f, 24.0f, 32.0f, 50.0f, 75.0f, 100.0f});
        }
};

class FrameExtrapolationCameraBlurMaxSmear : public FloatSliderModule
{
    public:
        MODULE_SETUP(FrameExtrapolationCameraBlurMaxSmear)
        {
            setName("Max Smear");
            setID("frame-extrapolation/camera-blur/max-smear");
            setDescription("Maximum camera-only trail distance in game units. This prevents portals, respawns, and camera snaps from creating a giant streak.");
            setRange(0.0f, 24.0f);
            setDefaultValue(8.0f);
            setSnapValues({0.0f, 2.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f});
        }
};

namespace
{
    struct CameraBlurRuntime
    {
        PlayLayer* owner = nullptr;
        CCPoint camera = CCPointZero;
        std::chrono::steady_clock::time_point lastDraw = {};
        bool seeded = false;
    };

    CameraBlurRuntime g_cameraBlur;

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

    void resetCameraBlur()
    {
        g_cameraBlur = {};
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

    float getFrameDt(std::chrono::steady_clock::time_point now)
    {
        if (g_cameraBlur.lastDraw == std::chrono::steady_clock::time_point{})
        {
            g_cameraBlur.lastDraw = now;
            return 1.0f / 60.0f;
        }

        float dt = std::chrono::duration<float>(now - g_cameraBlur.lastDraw).count();
        g_cameraBlur.lastDraw = now;

        if (!std::isfinite(dt) || dt <= 0.0f)
            return 1.0f / 60.0f;

        return std::clamp(dt, 1.0f / 1000.0f, 1.0f / 20.0f);
    }
}

class $modify(FrameExtrapolationCameraBlurDrawHook, CCDirector)
{
    void drawScene()
    {
        ensureCameraBlurOptionsRegistered();

        auto playLayer = PlayLayer::get();
        if (!cameraBlurEnabled() || !playLayer || !playLayer->m_objectLayer || !playLayer->isRunning())
        {
            resetCameraBlur();
            CCDirector::drawScene();
            return;
        }

        auto now = std::chrono::steady_clock::now();
        float dt = getFrameDt(now);
        CCPoint targetCamera = playLayer->m_objectLayer->getPosition();

        if (g_cameraBlur.owner != playLayer)
        {
            resetCameraBlur();
            g_cameraBlur.owner = playLayer;
            g_cameraBlur.camera = targetCamera;
            g_cameraBlur.lastDraw = now;
            g_cameraBlur.seeded = true;
            CCDirector::drawScene();
            return;
        }

        float strength = std::clamp(FrameExtrapolationCameraBlurStrength::get()->getValue(), 0.0f, 1.0f);
        float trailMs = std::clamp(FrameExtrapolationCameraBlurTrail::get()->getValue(), 0.0f, 100.0f);
        float maxSmear = std::clamp(FrameExtrapolationCameraBlurMaxSmear::get()->getValue(), 0.0f, 24.0f);

        if (!g_cameraBlur.seeded || strength <= 0.0001f || trailMs <= 0.0001f || maxSmear <= 0.0001f)
        {
            g_cameraBlur.camera = targetCamera;
            g_cameraBlur.seeded = true;
            CCDirector::drawScene();
            return;
        }

        // Camera teleports and scene snaps should never be smeared across the
        // screen. Reseed instead of dragging the old camera through the jump.
        if (pointLength(subPoint(targetCamera, g_cameraBlur.camera)) > std::max(96.0f, maxSmear * 8.0f))
        {
            g_cameraBlur.camera = targetCamera;
            CCDirector::drawScene();
            return;
        }

        float trailSeconds = std::max(trailMs * 0.001f, 0.001f);
        float baseFollow = 1.0f - std::exp(-dt / trailSeconds);
        float follow = 1.0f - strength * (1.0f - baseFollow);
        follow = std::clamp(follow, 0.0f, 1.0f);

        g_cameraBlur.camera = addPoint(
            g_cameraBlur.camera,
            scalePoint(subPoint(targetCamera, g_cameraBlur.camera), follow)
        );

        CCPoint blurDelta = clampMagnitude(subPoint(g_cameraBlur.camera, targetCamera), maxSmear);
        g_cameraBlur.camera = addPoint(targetCamera, blurDelta);

        if (pointLength(blurDelta) <= 0.0001f)
        {
            CCDirector::drawScene();
            return;
        }

        // Save the exact presentation transforms produced by the selected
        // Frame Extrapolation method. Camera Blur is a draw-only bonus layer.
        CCPoint originalCamera = targetCamera;
        CCPoint originalPlayer1 = playLayer->m_player1 ? playLayer->m_player1->getPosition() : CCPointZero;
        CCPoint originalPlayer2 = playLayer->m_player2 ? playLayer->m_player2->getPosition() : CCPointZero;
        bool compensatePlayer1 = playLayer->m_player1 && isUnderObjectLayer(playLayer->m_player1, playLayer->m_objectLayer);
        bool compensatePlayer2 = playLayer->m_player2 && isUnderObjectLayer(playLayer->m_player2, playLayer->m_objectLayer);

        playLayer->m_objectLayer->setPosition(addPoint(originalCamera, blurDelta));

        // Keep players sharp in screen space while the rest of the camera/world
        // receives the temporal smear.
        if (compensatePlayer1)
            playLayer->m_player1->CCNode::setPosition(subPoint(originalPlayer1, blurDelta));
        if (compensatePlayer2)
            playLayer->m_player2->CCNode::setPosition(subPoint(originalPlayer2, blurDelta));

        shiftGround(playLayer->m_groundLayer, blurDelta.x);
        shiftGround(playLayer->m_groundLayer2, blurDelta.x);

        CCDirector::drawScene();

        // Restore exactly what the selected extrapolation method had produced.
        shiftGround(playLayer->m_groundLayer, -blurDelta.x);
        shiftGround(playLayer->m_groundLayer2, -blurDelta.x);
        if (compensatePlayer1)
            playLayer->m_player1->CCNode::setPosition(originalPlayer1);
        if (compensatePlayer2)
            playLayer->m_player2->CCNode::setPosition(originalPlayer2);
        playLayer->m_objectLayer->setPosition(originalCamera);
    }
};
