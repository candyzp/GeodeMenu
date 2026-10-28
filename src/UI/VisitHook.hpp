#pragma once

#include <Geode/Geode.hpp>

namespace qolmod
{
    class VisitHook : public cocos2d::CCGridBase
    {
    public:
        // CCGridBase::beforeDraw/afterDraw are not hookable on iOS 2.2081.
        // Use a private-to-this-grid sentinel so CCNode::visit can identify
        // VisitHook without depending on runtime typeinfo being available.
        static constexpr int GRID_MARKER = 0x514F4C4D;

    private:
        using VisitCallbackFunc = std::function<void(cocos2d::CCNode* node)>;

        protected:
            VisitCallbackFunc callbackPre = nullptr;
            VisitCallbackFunc callbackPost = nullptr;

            virtual bool init();

        public:
            CREATE_FUNC(VisitHook);

            void setCallbackPre(const VisitCallbackFunc& func);
            void setCallbackPost(const VisitCallbackFunc& func);
        
            virtual void preVisit();
            virtual void postVisit(cocos2d::CCNode* node);
    };
};