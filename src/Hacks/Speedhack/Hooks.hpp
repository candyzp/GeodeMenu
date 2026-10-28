#pragma once

#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
#include <Geode/modify/CCTransitionScene.hpp>
#endif

using namespace geode::prelude;

class $modify (CBFCheckMenuLayer, MenuLayer)
{
    virtual bool init();
};

#if defined(__APPLE__) && TARGET_OS_IPHONE
// Marks the teardown window so pauseTarget does not inspect Geode user metadata
// on nodes while an iOS scene transition is recursively exiting.
class $modify (QOLTransitionScene, CCTransitionScene)
{
    void onExit();
};
#endif

class $modify (SpeedhackScheduler, CCScheduler)
{
    virtual void update(float dt);

    void pauseTarget(CCObject *pTarget);
};

class $modify (SpeedhackBaseGameLayer, GJBaseGameLayer)
{
    virtual void update(float dt);
};