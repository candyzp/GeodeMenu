#include "FloatingUIButton.hpp"
#include "FloatingUIManager.hpp"
#include "../../Hacks/Speedhack/Speedhack.hpp"
#include "../../Utils/RealtimeAction.hpp"
#include "../Modules/SmoothMoveButton.hpp"

using namespace geode::prelude;

#define BUTTON_RADIUS 40.0f
#define ICON_SIZE 22.0f

FloatingUIButton* FloatingUIButton::create(std::function<void()> onClick)
{
    auto pRet = new FloatingUIButton();

    pRet->setOnClick(onClick);

    if (pRet && pRet->init())
    {
        pRet->autorelease();
        return pRet;
    }

    CC_SAFE_DELETE(pRet);
    return nullptr;
}

bool FloatingUIButton::init()
{
    FloatingUIManager::get()->addButton(this);

    this->setAnchorPoint(ccp(0.5f, 0.5f));
    this->setContentSize(ccp(BUTTON_RADIUS, BUTTON_RADIUS));
    this->ignoreAnchorPointForPosition(false);
    this->scheduleUpdate();
    this->onEnter();

    return true;
}

bool FloatingUIButtonVisibility::shouldShow()
{
    if (PlayLayer::get())
    {
        auto scene = CCScene::get();
        const bool paused = scene && scene->getChildByType<PauseLayer>(0);
        return paused ? showInPauseMenu : showInGame;
    }

    if (auto ed = LevelEditorLayer::get())
    {
        const bool paused = ed->getChildByType<EditorPauseLayer>(0);
        return paused ? showInEditorPauseMenu : showInEditor;
    }

    return showInMenu;
}

void FloatingUIButton::updateSprites(std::string background, std::string overlay, bool backgroundSpriteSheet, bool overlaySpriteSheet)
{
    this->background = background;
    this->overlay = overlay;
    this->backgroundUsesSpriteSheet = backgroundSpriteSheet;
    this->overlayUsesSpriteSheet = overlaySpriteSheet;

    updateSprites();
}

void FloatingUIButton::updateSprites()
{
    if (!CCDirector::get()->m_pobOpenGLView)
        return;

    this->removeAllChildren();
    overlaySpr = nullptr;
    lastAppliedOpacity = -1;

    if (!background.empty())
    {
        CCSprite* bg = nullptr;

        if (backgroundUsesSpriteSheet)
            bg = CCSprite::createWithSpriteFrameName(background.c_str());
        else
            bg = CCSprite::create(background.c_str());

        if (bg)
        {
            bg->setPosition(getContentSize() / 2);
            bg->setScale(BUTTON_RADIUS / std::max<float>(bg->getContentWidth(), bg->getContentHeight()));
            bg->setScale(scale * bg->getScale());
            bg->setUserObject("flag"_spr, CCNode::create());
            this->addChild(bg);
        }
    }

    if (!overlay.empty())
    {
        CCSprite* ov = nullptr;

        if (overlayUsesSpriteSheet)
            ov = CCSprite::createWithSpriteFrameName(overlay.c_str());
        else
            ov = CCSprite::create(overlay.c_str());

        if (ov)
        {
            ov->setPosition(getContentSize() / 2);
            ov->setScale((ICON_SIZE / std::max<float>(ov->getContentWidth(), ov->getContentHeight())) * scale);
            overlaySpr = ov;
            overlaySpr->setUserObject("flag"_spr, CCNode::create());
            this->addChild(ov);
        }
    }

    setupChildren();
}

void FloatingUIButton::update(float dt)
{
    const bool v = visibilityConf.shouldShow();

    if (isVisible() != v)
        this->setVisible(v);

    if (!v)
        return;

    dt = Speedhack::get()->getRealDeltaTime();
    const float t = std::min(1.0f, 10.0f * dt);

    this->setPosition(ccp(
        std::lerp<double>(getPositionX(), position.x, t),
        std::lerp<double>(getPositionY(), position.y, t)
    ));

    _opacity = std::lerp<double>(_opacity, isSelected ? 1.0f : opacity, t);

    // Most frames the button opacity is unchanged at the byte level. Avoid
    // walking every child and reapplying the exact same value in that case.
    const int opacityByte = static_cast<int>(255 * _opacity);
    if (lastAppliedOpacity == opacityByte)
        return;

    lastAppliedOpacity = opacityByte;

    for (auto child : CCArrayExt<CCNode*>(getChildren()))
    {
        if (!child->getUserObject("flag"_spr))
            continue;

        if (auto spr = typeinfo_cast<CCSprite*>(child))
            spr->setOpacity(opacityByte);
    }
}

void FloatingUIButton::animate(bool release, bool clicked)
{
    this->stopAllActions();

    bool useGDAnim = animation == FloatingButtonAnimationType::GD;

    if (useGDAnim)
    {
        if (!release)
            this->runAction(RealtimeAction::create(CCEaseBounceOut::create(CCScaleTo::create(0.3f, 1.26f))));
        else
        {
            if (clicked)
            {
                this->stopAllActions();
                this->setScale(1.0f);
            }
            else
                this->runAction(RealtimeAction::create(CCEaseBounceOut::create(CCScaleTo::create(0.4f, 1.0f))));
        }
    }
    else
    {
        if (release)
            this->runAction(RealtimeAction::create(CCEaseBackOut::create(CCScaleTo::create(0.35f, 1))));
        else
            this->runAction(RealtimeAction::create(CCEaseInOut::create(CCScaleTo::create(0.1f, 0.9f), 2)));
    }
}

FloatingUIButton::~FloatingUIButton()
{
    FloatingUIManager::get()->removeButton(this);
}

void FloatingUIButton::setOnClick(std::function<void()> onClick)
{
    this->onClick = onClick;
}

void FloatingUIButton::setButtonVisibilityConfig(FloatingUIButtonVisibility conf)
{
    this->visibilityConf = conf;
}

void FloatingUIButton::setMovable(bool movable)
{
    this->movable = movable;
}

void FloatingUIButton::setBaseScale(float scale)
{
    if (scale > 1)
        scale = 1;

    if (scale < 0.1f)
        scale = 0.1f;

    this->scale = scale;

    updateSprites();
}

void FloatingUIButton::setBaseOpacity(float opacity)
{
    if (opacity > 1)
        opacity = 1;

    if (opacity < 0.1f)
        opacity = 0.1f;

    _opacity = opacity;
    this->opacity = opacity;
    lastAppliedOpacity = -1;
}

void FloatingUIButton::setAnimation(FloatingButtonAnimationType anim)
{
    this->animation = anim;
}

void FloatingUIButton::updatePosition(cocos2d::CCPoint point)
{
    auto safe = utils::getSafeAreaRect();
    point.x = std::max<float>(safe.getMinX(), point.x);
    point.x = std::min<float>(safe.getMaxX(), point.x);
    point.y = std::max<float>(safe.getMinY(), point.y);
    point.y = std::min<float>(safe.getMaxY(), point.y);

    this->position = point;

    if (!SmoothMoveButton::get()->getRealEnabled())
        setPosition(position);
}

bool FloatingUIButton::ccTouchBegan(qolmod::Touch* touch)
{
    if (!visibilityConf.shouldShow())
        return false;

    if (cocos2d::ccpDistance(position, touch->location) < (BUTTON_RADIUS / 2) * scale)
    {
        setZOrder(FloatingUIManager::get()->getHighestButtonZ() + 1);

        animate(false);

        isMoving = false;
        isSelected = true;
        return true;
    }

    return false;
}

void FloatingUIButton::ccTouchMoved(qolmod::Touch* touch)
{
    if (movable && !isMoving)
    {
        if (cocos2d::ccpDistance(touch->startLocation, touch->location) > 5)
        {
            isMoving = true;
        }
    }

    if (isMoving)
    {
        updatePosition(touch->location);
    }
}

void FloatingUIButton::ccTouchEnded(qolmod::Touch* touch)
{
    bool clicked = false;

    if (movable)
    {
        if (!isMoving)
        {
            if (onClick)
                onClick();

            clicked = true;
        }
    }
    else
    {
        if (cocos2d::ccpDistance(position, touch->location) < (BUTTON_RADIUS / 2) * scale)
        {
            if (onClick)
                onClick();

            clicked = true;
        }
    }

    animate(true, clicked);
    isSelected = false;
}

void FloatingUIButton::setupChildren()
{

}