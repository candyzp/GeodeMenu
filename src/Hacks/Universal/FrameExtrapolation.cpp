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
            setDescription("Smooths motion between physics ticks with render-only prediction. When disabled, prediction work is completely bypassed.");
            setDefaultEnabled(false);
            setDisabled(false);
        }
};

SUBMIT_HACK(FrameExtrapolation)

class $modify(ExtrapolatedGameLayer, GJBaseGameLayer)
{
    struct PlayerHistory
    {
        PlayerObject* player = nullptr;
        CCPoint previousPosition = {0.f, 0.f};
        CCPoint currentPosition = {0.f, 0.f};
        float previousRotation = 0.f;
        float currentRotation = 0.f;
        bool valid = false;
    };

    struct Fields
    {
        float timeTilNextTick = 0.f;
        float progressTilNextTick = 0.f;
        float modifiedDeltaReturn = 0.f;

        CCPoint previousObjectLayerPosition = {0.f, 0.f};
        CCPoint currentObjectLayerPosition = {0.f, 0.f};
        bool objectLayerHistoryValid = false;

        PlayerHistory p1;
        PlayerHistory p2;

        bool predictionWasEnabled = false;
    };

    static bool finitePoint(CCPoint const& point)
    {
        return std::isfinite(point.x) && std::isfinite(point.y);
    }

    static float distanceSquared(CCPoint const& a, CCPoint const& b)
    {
        const float x = b.x - a.x;
        const float y = b.y - a.y;
        return x * x + y * y;
    }

    static CCPoint extrapolatePoint(CCPoint const& previous, CCPoint const& current, float alpha)
    {
        return {
            current.x + (current.x - previous.x) * alpha,
            current.y + (current.y - previous.y) * alpha
        };
    }

    static float shortestAngleDelta(float previous, float current)
    {
        float delta = std::fmod(current - previous, 360.f);
        if (delta > 180.f)
            delta -= 360.f;
        else if (delta < -180.f)
            delta += 360.f;
        return delta;
    }

    void clearPredictionState()
    {
        auto self = m_fields.self();
        self->timeTilNextTick = 0.f;
        self->progressTilNextTick = 0.f;
        self->modifiedDeltaReturn = 0.f;
        self->objectLayerHistoryValid = false;
        self->p1 = {};
        self->p2 = {};
    }

    void capturePlayer(PlayerHistory& history, PlayerObject* player)
    {
        if (!player)
        {
            history = {};
            return;
        }

        const CCPoint position = player->m_position;
        const float rotation = player->m_mainLayer ? player->m_mainLayer->getRotation() : 0.f;

        if (!finitePoint(position) || !std::isfinite(rotation))
        {
            history = {};
            return;
        }

        if (!history.valid || history.player != player)
        {
            history.player = player;
            history.previousPosition = position;
            history.currentPosition = position;
            history.previousRotation = rotation;
            history.currentRotation = rotation;
            history.valid = true;
            return;
        }

        history.previousPosition = history.currentPosition;
        history.currentPosition = position;
        history.previousRotation = history.currentRotation;
        history.currentRotation = rotation;
    }

    void capturePhysicsTick()
    {
        auto self = m_fields.self();

        if (m_objectLayer)
        {
            const CCPoint current = m_objectLayer->getPosition();
            if (finitePoint(current))
            {
                if (!self->objectLayerHistoryValid)
                {
                    self->previousObjectLayerPosition = current;
                    self->currentObjectLayerPosition = current;
                    self->objectLayerHistoryValid = true;
                }
                else
                {
                    self->previousObjectLayerPosition = self->currentObjectLayerPosition;
                    self->currentObjectLayerPosition = current;
                }
            }
            else
            {
                self->objectLayerHistoryValid = false;
            }
        }
        else
        {
            self->objectLayerHistoryValid = false;
        }

        capturePlayer(self->p1, m_player1);
        capturePlayer(self->p2, m_player2);
    }

    float getModifiedDelta(float dt)
    {
        const float result = GJBaseGameLayer::getModifiedDelta(dt);

        // The hook still has to call Geometry Dash's original function, but when
        // the setting is OFF we do not keep timing state or perform prediction work.
        if (FrameExtrapolation::get()->getRealEnabled())
            m_fields->modifiedDeltaReturn = result;

        return result;
    }

    void update(float dt) override
    {
        auto self = m_fields.self();
        const bool enabled = FrameExtrapolation::get()->getRealEnabled();

        // Clear this before the base update so a missed getModifiedDelta call can
        // never reuse a stale non-zero value from the previous frame.
        self->modifiedDeltaReturn = 0.f;

        GJBaseGameLayer::update(dt);

        if (!enabled)
        {
            // Turning the setting off mid-level immediately drops all cached
            // prediction history. Nothing keeps running in the background.
            if (self->predictionWasEnabled)
                clearPredictionState();
            self->predictionWasEnabled = false;
            return;
        }

        self->predictionWasEnabled = true;

        auto playLayer = typeinfo_cast<PlayLayer*>(this);
        if (!playLayer || !isRunning() || isFlipping() || playLayer->m_levelEndAnimationStarted || m_playerDied)
        {
            clearPredictionState();
            return;
        }

        const float tickDelta = self->modifiedDeltaReturn;
        const bool gotPhysicsTick = std::isfinite(tickDelta) && tickDelta > 0.000001f;

        if (gotPhysicsTick)
        {
            // A very large modified delta means the game itself just hit a hitch,
            // pause, transition or other discontinuity. Re-baseline instead of
            // converting that hitch into a giant visual prediction jump.
            if (tickDelta > 0.05f)
            {
                clearPredictionState();
                return;
            }

            self->timeTilNextTick = tickDelta;
            self->progressTilNextTick = 0.f;
            capturePhysicsTick();
            return;
        }

        if (self->timeTilNextTick <= 0.f)
            return;

        // Clamp accumulated render time to one physics interval. This is the main
        // anti-spike guard: a slow frame can reach the predicted next state, but it
        // can never extrapolate multiple ticks into the future.
        const float safeDt = std::clamp(dt, 0.f, 0.05f);
        self->progressTilNextTick = std::min(
            self->progressTilNextTick + safeDt,
            self->timeTilNextTick
        );
    }

    void applyPlayerPrediction(PlayerHistory const& history, float alpha)
    {
        if (!history.valid || !history.player || history.player->m_isDead)
            return;

        // Portals, respawns and teleports are discontinuities, not velocity. Do
        // not try to predict through them or they become one-frame visual spikes.
        constexpr float kMaxPlayerStep = 96.f;
        if (distanceSquared(history.previousPosition, history.currentPosition) > kMaxPlayerStep * kMaxPlayerStep)
            return;

        const CCPoint predicted = extrapolatePoint(
            history.previousPosition,
            history.currentPosition,
            alpha
        );

        if (finitePoint(predicted))
            history.player->CCNode::setPosition(predicted);

        if (history.player->m_mainLayer)
        {
            const float rotationDelta = shortestAngleDelta(
                history.previousRotation,
                history.currentRotation
            );

            // Reject impossible one-tick rotation jumps, which usually means the
            // icon state changed or a portal/transition just happened.
            if (std::isfinite(rotationDelta) && std::abs(rotationDelta) <= 120.f)
            {
                history.player->m_mainLayer->setRotation(
                    history.currentRotation + rotationDelta * alpha
                );
            }
        }
    }

    void visit() override
    {
        auto self = m_fields.self();

        // OFF really means OFF: vanilla render path, no interpolation/extrapolation
        // calculations, no node traversal and no temporary transforms.
        if (!FrameExtrapolation::get()->getRealEnabled())
        {
            GJBaseGameLayer::visit();
            return;
        }

        auto playLayer = typeinfo_cast<PlayLayer*>(this);
        if (!playLayer || !isRunning() || isFlipping() || playLayer->m_levelEndAnimationStarted || m_playerDied || self->timeTilNextTick <= 0.f)
        {
            GJBaseGameLayer::visit();
            return;
        }

        const float alpha = std::clamp(
            self->progressTilNextTick / self->timeTilNextTick,
            0.f,
            1.f
        );

        if (!std::isfinite(alpha) || alpha <= 0.f)
        {
            GJBaseGameLayer::visit();
            return;
        }

        const CCPoint originalObjectLayerPosition = m_objectLayer ? m_objectLayer->getPosition() : CCPoint{0.f, 0.f};
        const CCPoint originalGround1Position = m_groundLayer ? m_groundLayer->getPosition() : CCPoint{0.f, 0.f};
        const CCPoint originalGround2Position = m_groundLayer2 ? m_groundLayer2->getPosition() : CCPoint{0.f, 0.f};

        const CCPoint originalP1Position = m_player1 ? m_player1->m_position : CCPoint{0.f, 0.f};
        const CCPoint originalP2Position = m_player2 ? m_player2->m_position : CCPoint{0.f, 0.f};
        const float originalP1Rotation = (m_player1 && m_player1->m_mainLayer) ? m_player1->m_mainLayer->getRotation() : 0.f;
        const float originalP2Rotation = (m_player2 && m_player2->m_mainLayer) ? m_player2->m_mainLayer->getRotation() : 0.f;

        float groundCameraOffsetX = 0.f;

        if (m_objectLayer && self->objectLayerHistoryValid)
        {
            constexpr float kMaxCameraStep = 160.f;
            if (distanceSquared(self->previousObjectLayerPosition, self->currentObjectLayerPosition) <= kMaxCameraStep * kMaxCameraStep)
            {
                const CCPoint predictedObjectLayer = extrapolatePoint(
                    self->previousObjectLayerPosition,
                    self->currentObjectLayerPosition,
                    alpha
                );

                if (finitePoint(predictedObjectLayer))
                {
                    groundCameraOffsetX = predictedObjectLayer.x - self->currentObjectLayerPosition.x;
                    m_objectLayer->setPosition(predictedObjectLayer);
                }
            }
        }

        // Move the ground only by the temporary camera prediction. This avoids the
        // old per-frame child traversal and its allocation/cache-spike potential.
        if (m_groundLayer)
            m_groundLayer->setPositionX(originalGround1Position.x + groundCameraOffsetX);
        if (m_groundLayer2)
            m_groundLayer2->setPositionX(originalGround2Position.x + groundCameraOffsetX);

        applyPlayerPrediction(self->p1, alpha);
        applyPlayerPrediction(self->p2, alpha);

        // Prediction exists only for drawing this frame. Physics always receives
        // the real positions because every temporary transform is restored before
        // the next update.
        GJBaseGameLayer::visit();

        if (m_player1)
        {
            m_player1->CCNode::setPosition(originalP1Position);
            if (m_player1->m_mainLayer)
                m_player1->m_mainLayer->setRotation(originalP1Rotation);
        }

        if (m_player2)
        {
            m_player2->CCNode::setPosition(originalP2Position);
            if (m_player2->m_mainLayer)
                m_player2->m_mainLayer->setRotation(originalP2Rotation);
        }

        if (m_groundLayer)
            m_groundLayer->setPosition(originalGround1Position);
        if (m_groundLayer2)
            m_groundLayer2->setPosition(originalGround2Position);
        if (m_objectLayer)
            m_objectLayer->setPosition(originalObjectLayerPosition);
    }
};
