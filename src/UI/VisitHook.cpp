#include "VisitHook.hpp"

#if defined(GEODE_IS_IOS)
#include <Geode/modify/CCNode.hpp>
#else
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
        if (!grid || !grid->isActive())
            return CCNode::visit();

        auto hook = typeinfo_cast<VisitHook*>(grid);
        if (!hook)
            return CCNode::visit();

        // Match CCNode::visit's grid path, but replace beforeDraw/afterDraw
        // with VisitHook callbacks (the same behavior used by other platforms).
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
            if (auto currentHook = typeinfo_cast<VisitHook*>(currentGrid))
                currentHook->postVisit(this);
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
