#include "VisitHook.hpp"

#if defined(GEODE_IS_IOS)
#include <Geode/modify/CCNode.hpp>
#include <unordered_set>

namespace {
std::unordered_set<cocos2d::CCGridBase*> s_visitHookGrids;
}
#else
#include <Geode/modify/CCGridBase.hpp>
#endif

using namespace geode::prelude;
using namespace qolmod;

VisitHook::~VisitHook()
{
#if defined(GEODE_IS_IOS)
    s_visitHookGrids.erase(this);
#endif
}

bool VisitHook::init()
{
    // VisitHook is a callback wrapper, not a real CCGridBase render target.
    // Initialize inherited fields so a missed hook cannot feed garbage
    // pointers to CCGridBase's default renderer or destructor.
    m_bActive = true;
    m_nReuseGrid = 0;
    m_sGridSize = cocos2d::CCSize(0.f, 0.f);
    m_pTexture = nullptr;
    m_obStep = cocos2d::CCPoint(0.f, 0.f);
    m_pGrabber = nullptr;
    m_bIsTextureFlipped = false;
    m_pShaderProgram = nullptr;

#if defined(GEODE_IS_IOS)
    // Track object identity separately; m_nReuseGrid is a mutable reuse count
    // and cannot safely double as a type marker.
    s_visitHookGrids.insert(this);
#endif
    return true;
}

#if defined(GEODE_IS_IOS)
bool VisitHook::isVisitHookGrid(cocos2d::CCGridBase* grid)
{
    return grid && s_visitHookGrids.find(grid) != s_visitHookGrids.end();
}
#endif

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

#if defined(GEODE_IS_IOS)
// CCGridBase::beforeDraw/afterDraw have no hookable iOS 2.2081 addresses.
// Recreate their VisitHook-specific behavior at CCNode::visit instead. The
// regular CCNode visit path remains untouched for every other grid/node.
class $modify (VisitHookNode, CCNode)
{
    void visit()
    {
        // Keep the global visit hook cheap: most nodes have no grid at all.
        auto grid = this->m_pGrid;
        if (!grid || !grid->isActive() || !VisitHook::isVisitHookGrid(grid))
            return CCNode::visit();

        auto hook = static_cast<VisitHook*>(grid);

        // Match CCNode::visit's grid path, but replace beforeDraw/afterDraw
        // with VisitHook callbacks (the same behavior used on other platforms).
        if (!this->isVisible())
            return;

        kmGLPushMatrix();
        hook->preVisit();
        this->transform();

        auto children = this->getChildren();
        unsigned int firstNonNegativeChild = 0;

        if (children && children->count() > 0)
        {
            this->sortAllChildren();

            for (auto child : CCArrayExt<CCNode*>(children))
            {
                if (child && child->getZOrder() < 0)
                {
                    child->visit();
                    ++firstNonNegativeChild;
                }
                else
                {
                    break;
                }
            }

            this->draw();

            unsigned int index = 0;
            for (auto child : CCArrayExt<CCNode*>(children))
            {
                if (index++ < firstNonNegativeChild)
                    continue;

                if (child)
                    child->visit();
            }
        }
        else
        {
            this->draw();
        }

        this->setOrderOfArrival(0);

        // Match the original afterDraw condition in case the callback changed
        // the node's grid while rendering.
        auto currentGrid = this->m_pGrid;
        if (currentGrid && currentGrid->isActive())
        {
            if (VisitHook::isVisitHookGrid(currentGrid))
                static_cast<VisitHook*>(currentGrid)->postVisit(this);
            else
                currentGrid->afterDraw(this);
        }

        kmGLPopMatrix();
    }
};
#else
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
