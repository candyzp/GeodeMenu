#include "NotificationManager.hpp"
#include "../Utils/AdvancedLabel/AdvLabelBMFont.hpp"
#include "../GUI/EasyBG.hpp"
#include "Modules/Modules.hpp"
#include <Speedhack.hpp>

using namespace geode::prelude;

NotificationManager* NotificationManager::get()
{
    static NotificationManager* instance = nullptr;

    if (!instance)
    {
        instance = new NotificationManager();
        instance->scheduleUpdate();
        instance->onEnter();
    }

    return instance;
}

void NotificationManager::notifyToast(std::string toastStr, float time)
{
    if (time == -1)
        time = NotificationsDuration::get()->getStringFloat();

    auto n = NotificationNode::create(toastStr, time);
    n->setPosition(ccp(5, 100));
    this->addChild(n);
}

void NotificationManager::removeNotification(NotificationNode* node)
{
    if (node)
        node->removeFromParent();
}

void NotificationManager::update(float dt)
{
    const auto childCount = getChildrenCount();

    // This manager is scheduled for the entire session. With no active toast
    // there is nothing to animate or lay out, so keep the idle path empty.
    if (childCount == 0)
    {
        if (getPositionY() != 0)
            setPositionY(0);

        return;
    }

    dt = Speedhack::get()->getRealDeltaTime();
    bool right = NotificationsRight::get()->getRealEnabled();
    auto winSize = CCDirector::get()->getWinSize();

    float y = 0;
    int i = 0;
    for (auto node : CCArrayExt<NotificationNode*>(getChildren()))
    {
        node->setPosition(ccp(right ? winSize.width - 5 : 5, winSize.height - 5 + y));
        node->setAnchorPoint(ccp(right ? 1 : 0, 1));

        if (i != childCount - 1)
            y += node->getScaledContentHeight() + 5;

        i++;
    }

    if (getPositionY() < -y)
        setPositionY(-y);

    this->setPositionY(std::lerp<double>(getPositionY(), -y, 10 * dt));
}