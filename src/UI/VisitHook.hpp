#pragma once

#include <Geode/Geode.hpp>

namespace qolmod
{
    class VisitHook : public cocos2d::CCGridBase
    {
    public:
#if defined(GEODE_IS_IOS)
        // Check object identity without using CCGridBase's mutable reuse count.
        static bool isVisitHookGrid(cocos2d::CCGridBase* grid);
#endif

    private:
        using VisitCallbackFunc = std::function<void(cocos2d::CCNode* node)>;

    protected:
        VisitCallbackFunc callbackPre = nullptr;
        VisitCallbackFunc callbackPost = nullptr;

        virtual bool init();

    public:
        ~VisitHook() override;
        CREATE_FUNC(VisitHook);

        void setCallbackPre(const VisitCallbackFunc& func);
        void setCallbackPost(const VisitCallbackFunc& func);

        virtual void preVisit();
        virtual void postVisit(cocos2d::CCNode* node);
    };
};
