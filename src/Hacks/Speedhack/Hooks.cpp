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

#if defined(__APPLE__) && TARGET_OS_IPHONE
namespace {
    // CCTransitionScene::onExit recursively calls CCNode::onExit, which pauses
    // schedulers for nodes that are leaving the scene. On iOS 2.2081, inspecting
    // Geode user metadata from that teardown path can dereference a stale legacy
    // m_pUserObject pointer (observed in GeodeNodeMetadata::set). Do not query
    // user objects while the transition is exiting; those nodes are being torn
    // down anyway and must follow the engine's normal pause path.
    unsigned g_transitionExitDepth = 0;

    struct TransitionExitScope {
        TransitionExitScope() { ++g_transitionExitDepth; }
        ~TransitionExitScope() { --g_transitionExitDepth; }
    };
}

void QOLTransitionScene::onExit()
{
    TransitionExitScope scope;
    CCTransitionScene::onExit();
}
#endif

// Iterative ancestor walk avoids recursive calls for deep node trees.
bool isNodeUnpausable(CCNode* node)
{
    for (auto* current = node; current; current = current->getParent())
    {
        if (current->getUserObject("unpausable"_spr))
            return true;
    }
    return false;
}

void SpeedhackScheduler::pauseTarget(CCObject *pTarget)
{
#if defined(__APPLE__) && TARGET_OS_IPHONE
    if (g_transitionExitDepth == 0)
    {
#endif
        if (auto node = typeinfo_cast<CCNode*>(pTarget))
        {
            if (isNodeUnpausable(node))
                return;
        }
#if defined(__APPLE__) && TARGET_OS_IPHONE
    }
#endif

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