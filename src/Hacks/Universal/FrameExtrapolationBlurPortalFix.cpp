#include "../../Client/Module.hpp"
#include "../../Client/FloatSliderModule.hpp"
#include <Geode/modify/CCScheduler.hpp>

#include <algorithm>
#include <array>
#include <cmath>

using namespace geode::prelude;

namespace
{
    CCPoint addPoint(CCPoint const& a, CCPoint const& b)
    {
        return CCPoint{a.x + b.x, a.y + b.y};
    }

    CCPoint subPoint(CCPoint const& a, CCPoint const& b)
    {
        return CCPoint{a.x - b.x, a.y - b.y};
    }

    CCPoint scalePoint(CCPoint const& p, float scale)
    {
        return CCPoint{p.x * scale, p.y * scale};
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
        if (!std::isfinite(length) || length <= 0.0001f || length <= maxLength)
            return value;

        return scalePoint(value, maxLength / length);
    }

    Module* moduleByID(char const* id)
    {
        return Module::getByID(id);
    }

    float sliderValue(char const* id, float fallback)
    {
        auto module = moduleByID(id);
        if (!module)
            return fallback;
        return static_cast<FloatSliderModule*>(module)->getValue();
    }

    class CameraBlurTrailNode : public CCNode
    {
    private:
        PlayLayer* m_owner = nullptr;
        geode::Ref<CCRenderTexture> m_target = nullptr;
        std::array<geode::Ref<CCSprite>, 4> m_samples = {};
        CCPoint m_center = CCPointZero;

    public:
        static CameraBlurTrailNode* create(PlayLayer* owner)
        {
            auto ret = new CameraBlurTrailNode();
            if (ret && ret->init(owner))
            {
                ret->autorelease();
                return ret;
            }

            CC_SAFE_DELETE(ret);
            return nullptr;
        }

        bool init(PlayLayer* owner)
        {
            if (!CCNode::init() || !owner)
                return false;

            m_owner = owner;
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            m_center = CCPoint{winSize.width * 0.5f, winSize.height * 0.5f};
            setContentSize(winSize);

            m_target = CCRenderTexture::create(
                winSize.width,
                winSize.height,
                kCCTexture2DPixelFormat_RGBA8888
            );
            if (!m_target || !m_target->getSprite() || !m_target->getSprite()->getTexture())
                return false;

            m_target->getSprite()->getTexture()->setAntiAliasTexParameters();
            auto texture = m_target->getSprite()->getTexture();

            for (size_t i = 0; i < m_samples.size(); ++i)
            {
                auto sample = CCSprite::createWithTexture(texture);
                if (!sample)
                    return false;

                sample->setFlipY(true);
                sample->setAnchorPoint({0.5f, 0.5f});
                sample->setPosition(m_center);
                sample->setOpacity(0);
                addChild(sample, static_cast<int>(i));
                m_samples[i] = sample;
            }

            setVisible(false);
            return true;
        }

        void configure(CCPoint const& trail, float amount)
        {
            amount = std::clamp(amount, 0.0f, 1.0f);
            static constexpr float opacityWeights[4] = {42.0f, 32.0f, 24.0f, 16.0f};

            for (size_t i = 0; i < m_samples.size(); ++i)
            {
                float t = static_cast<float>(i + 1) / static_cast<float>(m_samples.size());
                m_samples[i]->setPosition(addPoint(m_center, scalePoint(trail, t)));
                m_samples[i]->setOpacity(static_cast<GLubyte>(
                    std::clamp(opacityWeights[i] * amount, 0.0f, 255.0f)
                ));
            }

            setVisible(amount > 0.0001f && pointLength(trail) > 0.01f);
        }

        void hide()
        {
            setVisible(false);
        }

        void visit() override
        {
            if (!isVisible() || !m_owner || !m_target || !m_owner->m_objectLayer)
                return;

            // Capture the moving world without the players. This is the key
            // difference from the old blur: the player transform is never
            // counter-shifted or otherwise modified by Camera Blur.
            bool player1Visible = m_owner->m_player1 && m_owner->m_player1->isVisible();
            bool player2Visible = m_owner->m_player2 && m_owner->m_player2->isVisible();

            if (m_owner->m_player1)
                m_owner->m_player1->setVisible(false);
            if (m_owner->m_player2)
                m_owner->m_player2->setVisible(false);

            m_target->beginWithClear(0.0f, 0.0f, 0.0f, 0.0f);
            m_owner->m_objectLayer->visit();
            m_target->end();

            if (m_owner->m_player1)
                m_owner->m_player1->setVisible(player1Visible);
            if (m_owner->m_player2)
                m_owner->m_player2->setVisible(player2Visible);

            CCNode::visit();
        }
    };

    geode::Ref<CameraBlurTrailNode> g_blurOverlay = nullptr;
    PlayLayer* g_blurOwner = nullptr;
    CCPoint g_lastVisualCamera = CCPointZero;
    bool g_hasLastVisualCamera = false;
    float g_realBlurEffect = 0.0f;

    struct PortalState
    {
        PlayLayer* owner = nullptr;
        GameObject* player1Portal = nullptr;
        GameObject* player2Portal = nullptr;
        bool seeded = false;
        int suppressSmoothingFrames = 0;
    };

    PortalState g_portalState;

    void clearBlurOverlay()
    {
        if (g_blurOverlay && g_blurOverlay->getParent())
            g_blurOverlay->removeFromParentAndCleanup(true);

        g_blurOverlay = nullptr;
        g_blurOwner = nullptr;
        g_hasLastVisualCamera = false;
        g_realBlurEffect = 0.0f;
    }

    CameraBlurTrailNode* ensureBlurOverlay(PlayLayer* pl)
    {
        if (!pl)
            return nullptr;

        if (g_blurOwner != pl)
            clearBlurOverlay();

        if (!g_blurOverlay)
        {
            auto overlay = CameraBlurTrailNode::create(pl);
            if (!overlay)
                return nullptr;

            pl->addChild(overlay, pl->m_objectLayer ? pl->m_objectLayer->getZOrder() + 1 : 1);
            g_blurOverlay = overlay;
            g_blurOwner = pl;
        }

        return g_blurOverlay;
    }

    void hideBlurAndResetCamera()
    {
        if (g_blurOverlay)
            g_blurOverlay->hide();
        g_hasLastVisualCamera = false;
        g_realBlurEffect = 0.0f;
    }

    bool detectPortalTransition(PlayLayer* pl)
    {
        if (!pl)
        {
            g_portalState = {};
            return false;
        }

        if (g_portalState.owner != pl || !g_portalState.seeded)
        {
            g_portalState.owner = pl;
            g_portalState.player1Portal = pl->m_gameState.m_lastActivatedPortal1;
            g_portalState.player2Portal = pl->m_gameState.m_lastActivatedPortal2;
            g_portalState.seeded = true;
            g_portalState.suppressSmoothingFrames = 0;
            return false;
        }

        auto portal1 = pl->m_gameState.m_lastActivatedPortal1;
        auto portal2 = pl->m_gameState.m_lastActivatedPortal2;

        bool changed =
            (portal1 && portal1 != g_portalState.player1Portal) ||
            (portal2 && portal2 != g_portalState.player2Portal);

        g_portalState.player1Portal = portal1;
        g_portalState.player2Portal = portal2;
        return changed;
    }

    void correctPlayerOnPortalFrame(PlayLayer* pl)
    {
        if (!pl)
            return;

        // Frame Extrapolation only changes the CCNode presentation position;
        // m_position remains CBF/GD's authoritative gameplay position.
        if (pl->m_player1)
            pl->m_player1->CCNode::setPosition(pl->m_player1->m_position);
        if (pl->m_player2)
            pl->m_player2->CCNode::setPosition(pl->m_player2->m_position);
    }

    void updateRealCameraBlur(PlayLayer* pl, float dt, bool blurWanted, bool masterWanted, bool portalTransition, bool smoothingSuppressed)
    {
        if (!pl || !pl->m_objectLayer || !blurWanted || !masterWanted || portalTransition || smoothingSuppressed)
        {
            hideBlurAndResetCamera();
            return;
        }

        CCPoint camera = pl->m_objectLayer->getPosition();
        if (!g_hasLastVisualCamera)
        {
            g_lastVisualCamera = camera;
            g_hasLastVisualCamera = true;
            if (g_blurOverlay)
                g_blurOverlay->hide();
            g_realBlurEffect = 0.0f;
            return;
        }

        float amount = std::clamp(sliderValue("frame-extrapolation/camera-blur/strength", 0.35f), 0.0f, 1.0f);
        float lengthMs = std::clamp(sliderValue("frame-extrapolation/camera-blur/trail-ms", 40.0f), 0.0f, 150.0f);
        float maxBlur = std::clamp(sliderValue("frame-extrapolation/camera-blur/max-smear", 10.0f), 0.0f, 32.0f);

        float safeDt = dt;
        if (!std::isfinite(safeDt) || safeDt <= 0.0f)
            safeDt = 1.0f / 60.0f;
        safeDt = std::clamp(safeDt, 1.0f / 1000.0f, 0.05f);

        CCPoint cameraMotion = subPoint(camera, g_lastVisualCamera);
        g_lastVisualCamera = camera;

        if (amount <= 0.0001f || lengthMs <= 0.0001f || maxBlur <= 0.0001f || pointLength(cameraMotion) <= 0.0001f)
        {
            if (g_blurOverlay)
                g_blurOverlay->hide();
            g_realBlurEffect = 0.0f;
            return;
        }

        // Convert the requested trail duration into a distance based on the
        // camera's measured per-frame motion. Four faded copies then span this
        // vector, creating a directional screen-space smear instead of moving
        // the actual camera/player transforms.
        float frameSpan = std::clamp(lengthMs / (safeDt * 1000.0f), 0.0f, 12.0f);
        CCPoint trail = clampMagnitude(scalePoint(cameraMotion, -frameSpan * amount), maxBlur);
        g_realBlurEffect = pointLength(trail);

        if (g_realBlurEffect <= 0.01f)
        {
            if (g_blurOverlay)
                g_blurOverlay->hide();
            return;
        }

        if (auto overlay = ensureBlurOverlay(pl))
            overlay->configure(trail, amount);
        else
            g_realBlurEffect = 0.0f;
    }
}

class $modify(FrameBlurPortalCompatibility, CCScheduler)
{
    static void onModify(auto& self)
    {
        // Run before the existing FramePresentationScheduler hook. We suppress
        // only its old fake Camera Blur while the hook chain executes, then
        // draw the real blur after the rest of CBF/GD/frame smoothing is done.
        (void)self.setHookPriorityPre("cocos2d::CCScheduler::update", Priority::First);
    }

    void update(float dt)
    {
        auto blurModule = moduleByID("frame-extrapolation/camera-blur");
        auto masterModule = moduleByID("frame-extrapolation");

        bool blurWanted = blurModule && blurModule->getRealEnabled();
        bool masterWanted = masterModule && masterModule->getRealEnabled();

        bool oldBlurForced = blurModule && blurModule->getForceDisabled();
        bool oldMasterForced = masterModule && masterModule->getForceDisabled();

        bool suppressSmoothing = g_portalState.suppressSmoothingFrames > 0;
        if (suppressSmoothing)
            --g_portalState.suppressSmoothingFrames;

        if (blurModule)
            blurModule->setForceDisabled(true);
        if (suppressSmoothing && masterModule)
            masterModule->setForceDisabled(true);

        CCScheduler::update(dt);

        if (blurModule)
            blurModule->setForceDisabled(oldBlurForced);
        if (suppressSmoothing && masterModule)
            masterModule->setForceDisabled(oldMasterForced);

        auto pl = PlayLayer::get();
        if (!pl || !pl->m_objectLayer || !pl->m_player1 || pl->m_player1->m_isDead)
        {
            hideBlurAndResetCamera();
            if (!pl)
                g_portalState = {};
            return;
        }

        bool portalTransition = detectPortalTransition(pl);
        if (portalTransition)
        {
            // Remove the stale pre-portal visual position immediately, then let
            // one clean authoritative frame pass so the existing method history
            // contains only post-portal samples.
            correctPlayerOnPortalFrame(pl);
            g_portalState.suppressSmoothingFrames = 1;
            hideBlurAndResetCamera();
        }

        updateRealCameraBlur(pl, dt, blurWanted, masterWanted, portalTransition, suppressSmoothing);
    }
};
