#include "Koga/CharacterEventObserver.hpp"

// Required for .data to match.
dummy_float_data()

namespace Koga {

    CharacterEvent::CharacterEvent(const Message& message) {
        mMessage = message;
        m_4 = 0;
        m_8 = 0;
        m_C = 0;
        m_10 = 0;
        m_14 = 0;
        m_18 = 0;
    }

    bool CharacterEventObserver::receiveMessage(CharacterEvent* msg) {
        bool bResult;

        switch (msg->mMessage) {
        default:
            bResult = false;
            break;
        case CharacterEvent::MESSAGE_TRUE:
            bResult = true;
            break;
        case CharacterEvent::MESSAGE_ENTERED_FLASHLIGHT_BEAM:
            bResult = onEnteredFlashlightBeam(msg);
            break;
        case CharacterEvent::MESSAGE_CAPTURE_BEGIN:
            bResult = onCaptureBegin(msg);
            break;
        case CharacterEvent::MESSAGE_CAPTURE_END:
            bResult = onCaptureEnd(msg);
            break;
        case CharacterEvent::MESSAGE_UNK8:
            bResult = vt_18(msg);
            break;
        case CharacterEvent::MESSAGE_COLLIDE_WITH_PLAYER:
            bResult = onCollideWithPlayer(msg);
            break;
        case CharacterEvent::MESSAGE_SPRAYED_WITH_FIRE:
            bResult = onSprayedWithFire(msg);
            break;
        case CharacterEvent::MESSAGE_SPRAYED_WITH_ICE:
            bResult = onSprayedWithIce(msg);
            break;
        case CharacterEvent::MESSAGE_SPRAYED_WITH_WATER:
            bResult = onSprayedWithWater(msg);
            break;
        case CharacterEvent::MESSAGE_HIT_WITH_FIREBALL:
            bResult = onHitWithFireBall(msg);
            break;
        case CharacterEvent::MESSAGE_HIT_WITH_ICEBALL:
            bResult = onHitWithIceBall(msg);
            break;
        case CharacterEvent::MESSAGE_HIT_WITH_WATERBALL:
            bResult = onHitWithWaterBall(msg);
            break;
        case CharacterEvent::MESSAGE_FISHING_BEGIN:
            bResult = onFishingBegin(msg);
            break;
        case CharacterEvent::MESSAGE_PLAYER_LEFT_ROOM:
            bResult = onPlayerLeftRoom(msg);
            break;
        case CharacterEvent::MESSAGE_PLAYER_ENTERED_ROOM:
            bResult = onPlayerEnteredRoom(msg);
            break;
        case CharacterEvent::MESSAGE_PLAYER_TAPPED:
            bResult = onPlayerTapped(msg);
            break;
        case CharacterEvent::MESSAGE_UNK_29:
            bResult = vt_48(msg);
            break;
        }

        return bResult;
    }

    /* Called while the actor is inside the flashlight cone. */
    bool CharacterEventObserver::onEnteredFlashlightBeam(CharacterEvent* msg) {
        return false;
    }

    /* Called when the actor's HP hits 0 while being fished. */
    bool CharacterEventObserver::onCaptureBegin(CharacterEvent* msg) {
        return false;
    }

    /* Called when the actor has been sucked into the Poltergust. */
    bool CharacterEventObserver::onCaptureEnd(CharacterEvent* msg) {
        return false;
    }

    /* TODO: What does this do? */
    bool CharacterEventObserver::vt_18(CharacterEvent* msg) {
        return false;
    }

    /* Called when the actor collides with the player. */
    bool CharacterEventObserver::onCollideWithPlayer(CharacterEvent* msg) {
        return false;
    }

    /* Called when the actor is exposed to fire. */
    bool CharacterEventObserver::onSprayedWithFire(CharacterEvent* msg) {
        return false;
    }

    /* Called when the actor is exposed to ice. */
    bool CharacterEventObserver::onSprayedWithIce(CharacterEvent* msg) {
        return false;
    }

    /* Called when the actor is exposed to water. */
    bool CharacterEventObserver::onSprayedWithWater(CharacterEvent* msg) {
        return false;
    }

    /* Called when the actor is hit with a fire elemental ball. */
    bool CharacterEventObserver::onHitWithFireBall(CharacterEvent* msg) {
        return onSprayedWithFire(msg);
    }

    /* Called when the actor is hit with an ice elemental ball. */
    bool CharacterEventObserver::onHitWithIceBall(CharacterEvent* msg) {
        return onSprayedWithIce(msg);
    }

    /* Called when the actor is hit with a water elemental ball. */
    bool CharacterEventObserver::onHitWithWaterBall(CharacterEvent* msg) {
        return onSprayedWithWater(msg);
    }

    /* Called when the actor is hooked by the Poltergust. */
    bool CharacterEventObserver::onFishingBegin(CharacterEvent* msg) {
        return false;
    }

    /* Called when the player leaves the actor's current room. */
    bool CharacterEventObserver::onPlayerLeftRoom(CharacterEvent* msg) {
        return false;
    }

    /* Called when the player enters the actor's current room. */
    bool CharacterEventObserver::onPlayerEnteredRoom(CharacterEvent* msg) {
        return false;
    }

    /* Called when the player presses the A button on the actor; treasure chests only? */
    bool CharacterEventObserver::onPlayerTapped(CharacterEvent* msg) {
        return false;
    }

    /* TODO: What does this do? */
    bool CharacterEventObserver::vt_48(CharacterEvent* msg) {
        return false;
    }

};
