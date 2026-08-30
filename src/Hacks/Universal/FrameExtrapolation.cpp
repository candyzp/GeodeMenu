#include "../../Client/Module.hpp"
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
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

SUBMIT_HACK(FrameExtrapolation)

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

        // getModifiedDelta is called from inside the normal game update. Clearing
        // this first prevents a previous tick value being reused if a frame does
        // not produce a new modified delta.
        self->modifiedDeltaReturn = 0;

        GJBaseGameLayer::update(dt);

        auto playLayer = typeinfo_cast<PlayLayer*>(this);
        if (!playLayer)
            return;

        if (!FrameExtrapolation::get()->getRealEnabled())
        {
            if (self->wasEnabled)
                resetExtrapolationState();

            self->wasEnabled = false;
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
