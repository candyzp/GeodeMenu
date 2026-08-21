#include "Hooks.hpp"
#include "Speedhack.hpp"
#include "../../Utils/ColourUtils.hpp"
#include "../../Labels/LabelManager.hpp"

Mod* cbf = nullptr;

bool CBFCheckMenuLayer::init()
{
    cbf = Loader::get()->getLoadedMod("syzzi.click_between_frames");
    return MenuLayer::init();
}

void SpeedhackScheduler::update(float dt)
{
    auto speedhack = Speedhack::get();
    speedhack->realDeltatime = dt;

    ColourUtils::get()->update(dt);
    LabelManager::get()->update(dt);

    const float value = speedhack->getRealValue();
    const bool gameplayOnly = speedhack->getGameplayEnabled();

    static float lastPitch = 0.0f;
    float pitch = 1.0f;

    // A 1x speed value can never require a pitch change. Avoid extra module
    // checks on the overwhelmingly common neutral path.
    if (value != 1.0f && speedhack->getMusicEnabled() && (!gameplayOnly || GJBaseGameLayer::get()))
        pitch = value;

    if (lastPitch != pitch)
    {
        if (auto group = speedhack->getMasterChannel())
        {
            group->setPitch(pitch);
            lastPitch = pitch;
        }
    }

    if (gameplayOnly)
    {
        CCScheduler::update(dt);
        return;
    }

    // Do not write to CCDirector's timing fields when speedhack is neutral.
    // This keeps the default path out of CBF's timing state entirely.
    if (cbf && value != 1.0f)
    {
        auto director = CCDirector::get();

        director->m_fActualDeltaTime *= value;
        director->m_fDeltaTime *= value;
    }

    CCScheduler::update(value == 1.0f ? dt : dt * value);
}

bool isNodeUnpausable(CCNode* node)
{
    if (!node)
        return false;

    if (node->getUserObject("unpausable"_spr))
        return true;

    if (node->getParent())
        return isNodeUnpausable(node->getParent());

    return false;
}

void SpeedhackScheduler::pauseTarget(CCObject *pTarget)
{
    if (auto node = typeinfo_cast<CCNode*>(pTarget))
    {
        if (isNodeUnpausable(node))
            return;
    }

    CCScheduler::pauseTarget(pTarget);
}

void SpeedhackBaseGameLayer::update(float dt)
{
    if (Speedhack::get()->getGameplayEnabled())
    {
        dt *= Speedhack::get()->getRealValue();
    }

    GJBaseGameLayer::update(dt);
}