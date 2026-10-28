#include "VisitHook.hpp"
#if !defined(GEODE_IS_IOS)
#include <Geode/modify/CCGridBase.hpp>
#endif

using namespace geode::prelude;
using namespace qolmod;

bool VisitHook::init()
{
    m_bActive = true;
    return true;
}

void VisitHook::setCallbackPre(const VisitHook::VisitCallbackFunc& func)
{
    this->callbackPre = std::move(func);
}

void VisitHook::setCallbackPost(const VisitHook::VisitCallbackFunc& func)
{
    this->callbackPost = std::move(func);
}

void VisitHook::preVisit()
{
    if (callbackPre)
        callbackPre(nullptr);
}

void VisitHook::postVisit(cocos2d::CCNode* node)
{
    if (callbackPost)
        callbackPost(node);
}

// iOS 2.2081 bindings do not expose hook addresses for CCGridBase::beforeDraw
// and CCGridBase::afterDraw. Keep this optional global hook on the platforms
// where the bindings support it; do not instantiate unavailable iOS hooks.
#if !defined(GEODE_IS_IOS)
class $modify (VisitHookGridBase, CCGridBase)
{
    void beforeDraw(void)
    {
        if (auto hook = typeinfo_cast<VisitHook*>(this))
        {
            hook->preVisit();
            return;
        }

        CCGridBase::beforeDraw();
    }

    void afterDraw(CCNode *pTarget)
    {
        if (auto hook = typeinfo_cast<VisitHook*>(this))
        {
            hook->postVisit(pTarget);
            return;
        }

        CCGridBase::afterDraw(pTarget);
    }
};
#endif
