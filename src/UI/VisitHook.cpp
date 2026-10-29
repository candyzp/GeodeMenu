#include "VisitHook.hpp"

#if !defined(GEODE_IS_IOS)
#include <Geode/modify/CCGridBase.hpp>
#endif

using namespace geode::prelude;
using namespace qolmod;

bool VisitHook::init()
{
    // VisitHook is a callback wrapper, not a real CCGridBase render target.
    // Initialize inherited fields so this object is never treated as a
    // normal render grid if the engine inspects it.
#if defined(GEODE_IS_IOS)
    // The iOS bindings have no hookable CCGridBase::beforeDraw/afterDraw
    // addresses. Keep this callback-only grid inactive rather than installing
    // a global CCNode::visit replacement or sending it through the native
    // OpenGL grid renderer with no texture/grabber.
    m_bActive = false;
#else
    m_bActive = true;
#endif
    m_nReuseGrid = 0;
    m_sGridSize = cocos2d::CCSize(0.f, 0.f);
    m_pTexture = nullptr;
    m_obStep = cocos2d::CCPoint(0.f, 0.f);
    m_pGrabber = nullptr;
    m_bIsTextureFlipped = false;
    m_pShaderProgram = nullptr;
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
