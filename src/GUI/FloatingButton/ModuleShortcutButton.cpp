#include "ModuleShortcutButton.hpp"
#include "../../Client/ModuleNode.hpp"
#include "../../Client/ButtonModule.hpp"
#include "../../Notifications/NotificationManager.hpp"
#include "../../Localisation/LocalisationManager.hpp"

ModuleShortcutButton* ModuleShortcutButton::create(Module* module)
{
    auto pRet = new ModuleShortcutButton();

    pRet->mod = module;

    if (pRet && pRet->init())
    {
        pRet->setup();
        pRet->autorelease();
        return pRet;
    }

    CC_SAFE_DELETE(pRet);
    return nullptr;
}

void ModuleShortcutButton::setup()
{
    shortcutConfig = mod->getShortcutConfig();

    // These strings never change for a shortcut node. Building them once avoids
    // fmt/string allocation work in update() and while dragging the button.
    const auto moduleID = mod->getID();
    colourChannel = moduleID + "_shortcut";
    shortcutPosXKey = moduleID + "_shortcutpos.x";
    shortcutPosYKey = moduleID + "_shortcutpos.y";

    lastUpdated = mod->shouldShortcutShowActivated();
    updateSprs();

    this->setOnClick([this]
    {
        if (typeinfo_cast<ButtonModule*>(mod))
        {
            static_cast<ButtonModule*>(mod)->onClick();
            NotificationManager::get()->notifyToast(mod->getNotificationString());
            return;
        }

        mod->setUserEnabled(!mod->getUserEnabled());
        mod->onToggle();
        ModuleNode::updateAllNodes(nullptr);

        NotificationManager::get()->notifyToast(mod->getNotificationString());
    });

    auto def = ccp(
        CCDirector::get()->getWinSize().width - 30,
        CCDirector::get()->getWinSize().height / 2
    );
    
    auto pos = ccp(
        Mod::get()->getSavedValue<float>(shortcutPosXKey, def.x),
        Mod::get()->getSavedValue<float>(shortcutPosYKey, def.y)
    );

    this->updatePosition(pos);
    this->setPosition(pos);
}

void ModuleShortcutButton::updatePosition(cocos2d::CCPoint point)
{
    if (CCDirector::get()->getWinSize().width != 0)
    {
        auto safe = utils::getSafeAreaRect();
        point.x = std::max<float>(safe.getMinX(), point.x);
        point.x = std::min<float>(safe.getMaxX(), point.x);
        point.y = std::max<float>(safe.getMinY(), point.y);
        point.y = std::min<float>(safe.getMaxY(), point.y);
    }

    FloatingUIButton::updatePosition(point);
    position = point;

    Mod::get()->setSavedValue<float>(shortcutPosXKey, point.x);
    Mod::get()->setSavedValue<float>(shortcutPosYKey, point.y);
}

void ModuleShortcutButton::updateSprs()
{
    auto bg = mod->shouldShortcutShowActivated() ? 
        bgOnSpr : 
        bgOffSpr;

    updateSprites(bg, overlaySprite, true, true);
}

void ModuleShortcutButton::setOverlaySprite(std::string spr)
{
    this->overlaySprite = spr;
    updateSprs();
}

void ModuleShortcutButton::setBackgroundSprites(std::string bgOff, std::string bgOn)
{
    this->bgOffSpr = bgOff;
    this->bgOnSpr = bgOn;
}

void ModuleShortcutButton::update(float dt)
{
    const bool activated = mod->shouldShortcutShowActivated();

    if (lastUpdated != activated)
    {
        lastUpdated = activated;
        updateSprs();
    }
    
    FloatingUIButton::update(dt);

    // FloatingUIButton already decided this shortcut is hidden. Skip animated
    // colour work entirely until it is actually visible again.
    if (!isVisible())
        return;

    if (overlaySpr)
        overlaySpr->setColor(shortcutConfig.colour.colourForConfig(colourChannel));
}