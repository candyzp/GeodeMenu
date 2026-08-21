#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <NotificationManager.hpp>
#include <Modules/Modules.hpp>
#include "../../GUI/FloatingButton/FloatingUIManager.hpp"
#include "../../GUI/AndroidUI.hpp"
#include "../../GUI/AndroidBall.hpp"
#include <Gestures/GestureManager.hpp>
#include "../../Hacks/Universal/Paint/PaintNode.hpp"
#include "../../Hacks/Universal/ShowTouches/ShowTouchLayer.hpp"

using namespace geode::prelude;
using namespace qolmod;

#ifdef GEODE_IS_ANDROID

class $modify (QOLModRenderingHook, CCDirector)
{
    void drawScene(void)
    {
        CCDirector::drawScene();

        // The scene has already been rendered above. Calling drawScene() again
        // on an early-exit path doubles the render workload for that frame.
        if (!CCScene::get() || CCScene::get()->getChildByType<LoadingLayer>(0))
            return;

        if (!AndroidUI::get())
        {
            FloatingUIManager::get()->visit();
            AndroidBall::get()->visit();
            GestureManager::get()->visit();
        }

        if (NotificationsEnabled::get()->getRealEnabled())
            NotificationManager::get()->visit();

        qolmod::ShowTouchLayer::get()->visit();
    }
};

#else

class $modify (QOLModRenderingHook, CCEGLView)
{
    virtual void swapBuffers()
    {
        if (!CCScene::get() || CCScene::get()->getChildByType<LoadingLayer>(0))
            return CCEGLView::swapBuffers();

        if (!AndroidUI::get())
        {
            FloatingUIManager::get()->visit();
            AndroidBall::get()->visit();
            GestureManager::get()->visit();
        }

        if (NotificationsEnabled::get()->getRealEnabled())
            NotificationManager::get()->visit();

        qolmod::ShowTouchLayer::get()->visit();

        CCEGLView::swapBuffers();
    }
};

#endif