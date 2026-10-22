// ============================================================
//  Wave Assist v5 (Geode, Windows) - everything below is active in wave only
//
//  1. Input buffer: a jump tapped in the air is replayed on the
//     physics tick you land, if it was within the buffer window.
//  2. Trajectory: two invisible "ghost" players are cloned from
//     you and run through GD's own physics/collision:
//       green  = path if you click / keep holding
//       red    = path if you don't click / let go
//       yellow = part of the red path where a tap gets buffered
//  3. Click Shifting (per physics tick):
//       when a click (or release) would kill you and not doing it
//       is safe, it's held back and registers on the first tick
//       where it's safe. Dropped if never safe within Max Delay.
//  4. Auto Click (optional): clicks for you when not clicking
//     dies and clicking is safe.
//  Smart features step aside on orbs and while dashing.
// ============================================================
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <algorithm>
#include <chrono>
#include <vector>

using namespace geode::prelude;

// ------------------------------------------------------------
// helpers
// ------------------------------------------------------------
static constexpr int JUMP = 1;

static double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

template <class T>
static T setting(char const* key) {
    return Mod::get()->getSettingValue<T>(key);
}

static bool isGroundJumpMode(PlayerObject* p) {
    return !p->m_isShip && !p->m_isBird && !p->m_isDart && !p->m_isSwing;
}
static bool isHoldMode(PlayerObject* p) {
    return p->m_isShip || p->m_isDart || p->m_isRobot;
}
static bool isTouchingOrb(PlayerObject* p) {
    return p->m_touchingRings && p->m_touchingRings->count() > 0;
}

// ------------------------------------------------------------
// global state
// ------------------------------------------------------------
static PlayerObject* g_ghostA = nullptr;
static PlayerObject* g_ghostB = nullptr;
static GJBaseGameLayer* g_ghostLayer = nullptr;
static bool g_simulating = false;
static bool g_ghostStopped = false;
static bool g_ghostDied = false;
static float g_lastRealDt = 0.25f;
static uint64_t g_tick = 0;
static double g_msPerTick = 1000.0 / 240.0;

static bool isGhost(PlayerObject* p) {
    return p && (p == g_ghostA || p == g_ghostB);
}

static int msToTicks(int64_t ms) {
    return std::clamp((int)(ms / g_msPerTick), 1, 1000);
}

struct BufferState {
    bool pending = false;
    double pressTime = 0.0;
    bool held = false;            // physically held
    bool releaseNextTick = false;
};
static BufferState g_p1, g_p2;

struct SmartState {
    bool gdPressed = false;       // what GD currently thinks (P1 jump)
    bool tapRequest = false;      // user pressed, not yet given to GD
    double requestTime = 0.0;
    bool autoHeld = false;        // Auto Click is holding (hold modes)
    bool tapRelease = false;      // release a handed-back tap next tick
    uint64_t pressTick = 0;       // tick the last press reached GD
    bool wasActive = false;
    bool planned = false;         // input scheduled for a safer tick
    uint64_t planTick = 0;
    uint64_t nextPlanTick = 0;    // throttle re-planning while holding back
};
static SmartState g_smart;
static bool g_injecting = false;

static void resetInputState() {
    g_p1 = {};
    g_p2 = {};
    g_smart = {};
}

// ------------------------------------------------------------
// ghosts + simulation
// ------------------------------------------------------------
enum class SimInput { Release, Hold, Tap };

struct PathResult {
    std::vector<CCPoint> points;
    bool died = false;
    bool stopped = false;
    int deathIndex = -1;
    int landingIndex = -1;
};

static void hideGhostVisuals(PlayerObject* g) {
    g->setVisible(false);
    if (g->m_regularTrail) g->m_regularTrail->setVisible(false);
    if (g->m_waveTrail) g->m_waveTrail->setVisible(false);
}

static PlayerObject* makeGhost(GJBaseGameLayer* layer) {
    auto parent = layer->m_objectLayer;
    if (!parent) return nullptr;
    auto before = parent->getChildrenCount();
    auto ghost = PlayerObject::create(1, 1, layer, parent, false);
    if (!ghost) return nullptr;
    ghost->m_playEffects = false;
    parent->addChild(ghost);
    auto children = parent->getChildren();
    for (auto i = before; children && i < children->count(); i++) {
        if (auto node = typeinfo_cast<CCNode*>(children->objectAtIndex(i))) node->setVisible(false);
    }
    hideGhostVisuals(ghost);
    ghost->retain();
    return ghost;
}

static void releaseGhosts() {
    for (auto g : {g_ghostA, g_ghostB}) {
        if (g) {
            g->removeFromParent();
            g->release();
        }
    }
    g_ghostA = g_ghostB = nullptr;
    g_ghostLayer = nullptr;
}

static bool ensureGhosts(GJBaseGameLayer* layer) {
    if (g_ghostLayer != layer) {
        // different level: forget old ghosts (their layer already cleaned them up)
        g_ghostA = g_ghostB = nullptr;
        g_ghostLayer = layer;
    }
    if (!g_ghostA) g_ghostA = makeGhost(layer);
    if (!g_ghostB) g_ghostB = makeGhost(layer);
    return g_ghostA && g_ghostB;
}

static void syncMode(PlayerObject* g, PlayerObject* p) {
    if (g->m_isShip && !p->m_isShip) g->toggleFlyMode(false, true);
    if (g->m_isBall && !p->m_isBall) g->toggleRollMode(false, true);
    if (g->m_isBird && !p->m_isBird) g->toggleBirdMode(false, true);
    if (g->m_isDart && !p->m_isDart) g->toggleDartMode(false, true);
    if (g->m_isRobot && !p->m_isRobot) g->toggleRobotMode(false, true);
    if (g->m_isSpider && !p->m_isSpider) g->toggleSpiderMode(false, true);
    if (g->m_isSwing && !p->m_isSwing) g->toggleSwingMode(false, true);

    if (p->m_isShip && !g->m_isShip) g->toggleFlyMode(true, true);
    if (p->m_isBall && !g->m_isBall) g->toggleRollMode(true, true);
    if (p->m_isBird && !g->m_isBird) g->toggleBirdMode(true, true);
    if (p->m_isDart && !g->m_isDart) g->toggleDartMode(true, true);
    if (p->m_isRobot && !g->m_isRobot) g->toggleRobotMode(true, true);
    if (p->m_isSpider && !g->m_isSpider) g->toggleSpiderMode(true, true);
    if (p->m_isSwing && !g->m_isSwing) g->toggleSwingMode(true, true);
}

static void copyState(PlayerObject* g, PlayerObject* p) {
    syncMode(g, p);
    bool mini = p->m_vehicleSize != 1.0f;
    if ((g->m_vehicleSize != 1.0f) != mini) g->togglePlayerScale(mini, true);
    g->m_isUpsideDown = p->m_isUpsideDown;

    g->copyAttributes(p);
    g->setPosition(p->getPosition());
    g->m_position = p->m_position;
    g->m_yVelocity = p->m_yVelocity;
    g->m_isOnGround = p->m_isOnGround;
    g->m_playerSpeed = p->m_playerSpeed;
    g->m_gravityMod = p->m_gravityMod;
    g->m_isPlatformer = p->m_isPlatformer;
    g->m_isGoingLeft = p->m_isGoingLeft;
    g->m_isSideways = p->m_isSideways;
    g->m_holdingButtons = p->m_holdingButtons;
    g->m_isDashing = false;   // we never simulate while the real player dashes
    g->m_isDead = false;
    hideGhostVisuals(g);
}

static PathResult simulate(GJBaseGameLayer* layer, PlayerObject* ghost, PlayerObject* real,
                           SimInput input, int steps, bool record) {
    PathResult r;
    copyState(ghost, real);

    ghost->releaseButton(PlayerButton::Jump);
    if (input != SimInput::Release) ghost->pushButton(PlayerButton::Jump);

    g_simulating = true;
    g_ghostStopped = false;
    g_ghostDied = false;

    bool wasAirborne = !ghost->m_isOnGround;
    float dt = g_lastRealDt;
    if (record) {
        r.points.reserve(steps + 1);
        r.points.push_back(ghost->getPosition());
    }

    int i = 0;
    for (; i < steps; i++) {
        ghost->update(dt);
        layer->checkCollisions(ghost, dt, false);
        if (record) r.points.push_back(ghost->getPosition());

        if (i == 0 && input == SimInput::Tap) ghost->releaseButton(PlayerButton::Jump);

        if (wasAirborne && ghost->m_isOnGround && r.landingIndex < 0) r.landingIndex = i + 1;
        if (!ghost->m_isOnGround) wasAirborne = true;

        if (g_ghostStopped || ghost->m_isDead) break;
    }

    r.died = g_ghostDied || ghost->m_isDead;
    r.stopped = g_ghostStopped && !r.died;
    if (r.died) r.deathIndex = i + 1;
    g_simulating = false;
    g_ghostStopped = g_ghostDied = false;

    ghost->releaseButton(PlayerButton::Jump);
    hideGhostVisuals(ghost);
    return r;
}

// How many ticks the ghost lives if it uses `before` and switches to `after`
// at tick `at`. Returns steps + 1 if it survives the whole run.
static int survival(GJBaseGameLayer* layer, PlayerObject* ghost, PlayerObject* real,
                    SimInput before, SimInput after, int at, int steps) {
    copyState(ghost, real);
    auto apply = [&](SimInput in) {
        ghost->releaseButton(PlayerButton::Jump);
        if (in != SimInput::Release) ghost->pushButton(PlayerButton::Jump);
    };
    SimInput cur = at == 0 ? after : before;
    int curStart = 0;
    apply(cur);

    g_simulating = true;
    g_ghostStopped = g_ghostDied = false;
    float dt = g_lastRealDt;
    bool dead = false;
    int i = 0;
    for (; i < steps; i++) {
        if (i == at && at > 0) { cur = after; curStart = i; apply(cur); }
        ghost->update(dt);
        layer->checkCollisions(ghost, dt, false);
        if (cur == SimInput::Tap && i == curStart) ghost->releaseButton(PlayerButton::Jump);
        if (g_ghostDied || ghost->m_isDead) { dead = true; break; }
        if (g_ghostStopped) break; // orb/pad/portal ahead: can't see past it, treat as safe
    }
    g_simulating = false;
    g_ghostStopped = g_ghostDied = false;
    ghost->releaseButton(PlayerButton::Jump);
    hideGhostVisuals(ghost);
    return dead ? i + 1 : steps + 1;
}

// ------------------------------------------------------------
// PlayerObject hooks: record real dt, mute ghost effects/stats
// ------------------------------------------------------------
class $modify(IBPlayerObject, PlayerObject) {
    void update(float dt) {
        if (!g_simulating && !isGhost(this)) g_lastRealDt = dt;
        PlayerObject::update(dt);
    }
    void incrementJumps() {
        if (isGhost(this)) return;
        PlayerObject::incrementJumps();
    }
    void playerDestroyed(bool noEffects) {
        if (isGhost(this)) return;
        PlayerObject::playerDestroyed(noEffects);
    }
    void playSpawnEffect() {
        if (isGhost(this)) return;
        PlayerObject::playSpawnEffect();
    }
    void spawnCircle() {
        if (isGhost(this)) return;
        PlayerObject::spawnCircle();
    }
    void spawnDualCircle() {
        if (isGhost(this)) return;
        PlayerObject::spawnDualCircle();
    }
    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (isGhost(this)) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }
    void playDynamicSpiderRun() {
        if (isGhost(this)) return;
        PlayerObject::playDynamicSpiderRun();
    }
    void createFadeOutDartStreak() {
        if (isGhost(this)) return;
        PlayerObject::createFadeOutDartStreak();
    }
    void playBumpEffect(int type, GameObject* obj) {
        if (isGhost(this)) return;
        PlayerObject::playBumpEffect(type, obj);
    }
};

// ------------------------------------------------------------
// GJBaseGameLayer hooks: input, buffer, click shifting, blockers
// ------------------------------------------------------------
class $modify(IBGameLayer, GJBaseGameLayer) {
public:
    bool bufferActive() {
        return false; // wave-only build: the buffer only applies to other modes
        if (m_isEditor) return false;
        if (setting<bool>("platformer-only") && !m_isPlatformer) return false;
        return true;
    }

    void sendJump(bool down, bool isPlayer1) {
        g_injecting = true;
        GJBaseGameLayer::handleButton(down, JUMP, isPlayer1);
        g_injecting = false;
        if (isPlayer1) {
            g_smart.gdPressed = down;
            if (down) g_smart.pressTick = g_tick;
        }
    }

    bool smartActive() {
        bool shift = setting<bool>("smart-click");
        bool autoc = setting<bool>("auto-click");
        if (!shift && !autoc) return false;
        if (m_isEditor) return false;
        if (setting<bool>("smart-click-practice-only") && !m_isPracticeMode) return false;
        if (m_gameState.m_isDualMode) return false;
        auto pl = typeinfo_cast<PlayLayer*>(static_cast<GJBaseGameLayer*>(this));
        if (!pl || pl->m_hasCompletedLevel) return false;
        auto p = m_player1;
        if (!p || p->m_isDead) return false;
        if (!p->m_isDart) return false;                       // wave only
        if (isTouchingOrb(p) || p->m_isDashing) return false; // orbs and dashes: you're in control
        return true;
    }

    // Give GD the user's real input when smart features step aside.
    void handBack() {
        auto& s = g_smart;
        if (s.tapRequest) {
            if (!s.gdPressed) sendJump(true, true);
            s.tapRequest = false;
            if (!g_p1.held) s.tapRelease = true;
        } else if (s.gdPressed != (g_p1.held || false)) {
            sendJump(g_p1.held, true);
        }
        s.autoHeld = false;
    }

    // Scan what's in front of you and pick the tick to apply your input.
    //  0  = now, >0 = that many ticks later, -1 = don't (not doing it is safe and it never is)
    int planSwitch(PlayerObject* real, SimInput stayIn, SimInput switchIn) {
        int horizon = msToTicks(setting<int64_t>("smart-click-lookahead"));
        int maxK = msToTicks(setting<int64_t>("smart-max-delay"));
        int bigGap = msToTicks(setting<int64_t>("safe-gap"));

        int firstSafe = -1, runLen = 0, bestK = 0, bestLife = -1;
        for (int k = 0; k <= maxK; k++) {
            int steps = k + horizon;
            int life = survival(this, g_ghostB, real, stayIn, switchIn, k, steps);
            if (life > steps) {                       // safe if applied at tick k
                if (firstSafe < 0) firstSafe = k;
                runLen++;
                if (runLen >= bigGap) break;          // big safe area found
            } else {
                if (firstSafe >= 0) break;            // end of the safe window
                if (life > bestLife) { bestLife = life; bestK = k; }
            }
        }

        if (firstSafe >= 0) {
            if (runLen >= bigGap) {
                // big safe area: right now if possible, else just inside it
                return firstSafe == 0 ? 0 : firstSafe + 1;
            }
            return firstSafe + (runLen - 1) / 2;     // small window: its middle is safest
        }

        // no safe tick at all (frame-perfect or impossible): cancel the input
        (void)bestK; (void)bestLife;
        return -1;
    }

    // Runs at the start of every physics tick, and right when you click.
    void smartTick() {
        auto& s = g_smart;
        bool active = smartActive();
        if (!active) {
            if (s.wasActive) handBack();
            s.wasActive = false;
            return;
        }
        s.wasActive = true;

        auto real = m_player1;
        bool pressed = s.gdPressed;
        bool hold = isHoldMode(real);

        if (s.tapRequest && !g_p1.held && !s.planned &&
            nowMs() - s.requestTime > (double)setting<int64_t>("smart-max-delay")) {
            s.tapRequest = false; // never became safe in time: drop it
        }

        bool shiftOn = setting<bool>("smart-click");
        bool autoOn = setting<bool>("auto-click");
        bool desired = g_p1.held || s.tapRequest || s.autoHeld;
        bool wantSwitch = desired != pressed;

        if (wantSwitch && !shiftOn) {
            // shifting off: pass the input straight through
            sendJump(desired, true);
            if (desired) s.tapRequest = false;
            return;
        }
        if (!wantSwitch) s.planned = false;
        if (!wantSwitch && !autoOn && !s.autoHeld) return;
        if (pressed && g_tick <= s.pressTick) return; // a press lasts at least one tick
        if (!ensureGhosts(this)) {
            if (wantSwitch) { sendJump(desired, true); if (desired) s.tapRequest = false; }
            return;
        }

        SimInput stayIn = pressed ? SimInput::Hold : SimInput::Release;
        SimInput switchIn = pressed ? SimInput::Release : (hold ? SimInput::Hold : SimInput::Tap);
        auto doSwitch = [&] {
            sendJump(!pressed, true);
            if (!pressed) s.tapRequest = false;
            else s.autoHeld = false;
            s.planned = false;
        };

        if (wantSwitch) {
            if (s.planned) {
                if (g_tick >= s.planTick) doSwitch(); // the safest tick is here
                return;
            }
            if (g_tick < s.nextPlanTick) return;

            int k = planSwitch(real, stayIn, switchIn);
            if (k == 0) {
                doSwitch();
            } else if (k > 0) {
                s.planned = true;
                s.planTick = g_tick + (uint64_t)k;
            } else if (s.tapRequest && !g_p1.held) {
                s.tapRequest = false;          // tap never safe in time: dropped
            } else {
                s.nextPlanTick = g_tick + 4;   // still holding: look again shortly
            }
            return;
        }

        int horizon = msToTicks(setting<int64_t>("smart-click-lookahead"));
        auto stay = simulate(this, g_ghostA, real, stayIn, horizon, false);
        auto sw = simulate(this, g_ghostB, real, switchIn, horizon, false);

        // input already matches what you want
        if (s.autoHeld && pressed && !g_p1.held && !sw.died) {
            sendJump(false, true); // auto-hold no longer needed
            s.autoHeld = false;
            return;
        }
        if (autoOn && stay.died && !sw.died) {
            sendJump(!pressed, true);
            if (!pressed) {
                if (hold && !g_p1.held) s.autoHeld = true;
            } else {
                s.autoHeld = false;
            }
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_injecting || button != JUMP) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto& st = isPlayer1 ? g_p1 : g_p2;
        st.held = down;

        if (isPlayer1 && smartActive()) {
            if (down) {
                g_smart.tapRequest = true;
                g_smart.requestTime = nowMs();
            }
            bufferBookkeeping(down, isPlayer1);
            smartTick(); // decide right away: no added delay when it's safe
            return;
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (isPlayer1) {
            g_smart.gdPressed = down;
            if (down) g_smart.pressTick = g_tick;
        }
        bufferBookkeeping(down, isPlayer1);
    }

    void bufferBookkeeping(bool down, bool isPlayer1) {
        auto& st = isPlayer1 ? g_p1 : g_p2;
        if (!down || !bufferActive()) return;
        auto player = isPlayer1 ? m_player1 : m_player2;
        if (!player || player->m_isDead) return;

        if (!player->m_isOnGround && isGroundJumpMode(player) && !isTouchingOrb(player)) {
            st.pending = true;
            st.pressTime = nowMs();
        } else {
            st.pending = false;
        }
    }

    void tickBuffer(bool isPlayer1) {
        auto& st = isPlayer1 ? g_p1 : g_p2;
        auto player = isPlayer1 ? m_player1 : m_player2;

        if (st.releaseNextTick) {
            st.releaseNextTick = false;
            if (!st.held) sendJump(false, isPlayer1);
        }
        if (!st.pending) return;

        if (!player || player->m_isDead || !bufferActive() || !isGroundJumpMode(player)) {
            st.pending = false;
            return;
        }
        if (nowMs() - st.pressTime > (double)setting<int64_t>("buffer-window")) {
            st.pending = false;
            return;
        }
        if (player->m_isOnGround) {
            st.pending = false;
            if (st.held) return; // still holding: GD jumps on landing by itself
            if (isPlayer1 && smartActive()) {
                // let click shifting check the buffered jump too
                g_smart.tapRequest = true;
                g_smart.requestTime = nowMs();
                return;
            }
            sendJump(true, isPlayer1);
            st.releaseNextTick = true;
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!g_simulating) {
            ++g_tick;
            auto& s = g_smart;
            if (s.tapRelease) {
                if (!s.gdPressed || g_p1.held) s.tapRelease = false;
                else if (g_tick > s.pressTick) { s.tapRelease = false; sendJump(false, true); }
            }
            tickBuffer(true);
            if (m_gameState.m_isDualMode) tickBuffer(false);
            smartTick();
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void resetLevelVariables() {
        resetInputState();
        GJBaseGameLayer::resetLevelVariables();
    }

    // ---- ghost side-effect blockers ----
    void playerTouchedRing(PlayerObject* p, RingObject* o) {
        if (isGhost(p)) { g_ghostStopped = true; return; }
        GJBaseGameLayer::playerTouchedRing(p, o);
    }
    void bumpPlayer(PlayerObject* p, EffectGameObject* o) {
        if (isGhost(p)) { g_ghostStopped = true; return; }
        GJBaseGameLayer::bumpPlayer(p, o);
    }
    void playerWillSwitchMode(PlayerObject* p, GameObject* o) {
        if (isGhost(p)) { g_ghostStopped = true; return; }
        GJBaseGameLayer::playerWillSwitchMode(p, o);
    }
    void flipGravity(PlayerObject* p, bool flip, bool noEffects) {
        if (isGhost(p)) { if (g_simulating) g_ghostStopped = true; return; }
        GJBaseGameLayer::flipGravity(p, flip, noEffects);
    }
    void teleportPlayer(TeleportPortalObject* o, PlayerObject* p) {
        if (isGhost(p)) { g_ghostStopped = true; return; }
        GJBaseGameLayer::teleportPlayer(o, p);
    }
    void toggleDualMode(GameObject* o, bool dual, PlayerObject* p, bool noEffects) {
        if (isGhost(p)) { g_ghostStopped = true; return; }
        GJBaseGameLayer::toggleDualMode(o, dual, p, noEffects);
    }
    void playerTouchedTrigger(PlayerObject* p, EffectGameObject* o) {
        if (isGhost(p)) return;
        GJBaseGameLayer::playerTouchedTrigger(p, o);
    }
    void pickupItem(EffectGameObject* o) {
        if (g_simulating) return;
        GJBaseGameLayer::pickupItem(o);
    }
    void destroyObject(GameObject* o) {
        if (g_simulating) return;
        GJBaseGameLayer::destroyObject(o);
    }
    void gameEventTriggered(GJGameEvent e, int material, int playerID) {
        if (g_simulating) return;
        GJBaseGameLayer::gameEventTriggered(e, material, playerID);
    }
};

// ------------------------------------------------------------
// PlayLayer: drawing, label, cleanup
// ------------------------------------------------------------
class $modify(IBPlayLayer, PlayLayer) {
    struct Fields {
        Ref<CCDrawNode> draw;
        Ref<CCLabelBMFont> label;
        uint64_t lastTick = 0;
        double lastTime = 0.0;
    };

    void destroyPlayer(PlayerObject* p, GameObject* o) {
        if (isGhost(p)) {
            g_ghostDied = true;
            g_ghostStopped = true;
            return;
        }
        PlayLayer::destroyPlayer(p, o);
    }

    void onQuit() {
        releaseGhosts();
        resetInputState();
        PlayLayer::onQuit();
    }

    static void drawPath(CCDrawNode* d, PathResult const& r, ccColor4F color, int from, int to) {
        from = std::max(from, 0);
        to = std::min(to, (int)r.points.size() - 1);
        for (int i = from; i < to; i++) d->drawSegment(r.points[i], r.points[i + 1], 1.2f, color);
    }

    static void drawEnd(CCDrawNode* d, PathResult const& r) {
        if (r.points.empty()) return;
        auto end = r.points.back();
        if (r.died) {
            auto red = ccc4f(1.f, 0.15f, 0.15f, 1.f);
            d->drawSegment(end + ccp(-6, -6), end + ccp(6, 6), 1.5f, red);
            d->drawSegment(end + ccp(-6, 6), end + ccp(6, -6), 1.5f, red);
        } else if (r.stopped) {
            d->drawDot(end, 4.f, ccc4f(0.3f, 0.8f, 1.f, 1.f));
        }
    }

    void measureTickRate() {
        auto f = m_fields.self();
        double t = nowMs();
        uint64_t ticks = g_tick - f->lastTick;
        if (f->lastTime > 0 && ticks > 0) {
            double sample = (t - f->lastTime) / (double)ticks;
            if (sample > 0.5 && sample < 50.0) g_msPerTick = g_msPerTick * 0.9 + sample * 0.1;
        }
        f->lastTick = g_tick;
        f->lastTime = t;
    }

    void updateLabel() {
        auto f = m_fields.self();
        bool on = setting<bool>("smart-click") || setting<bool>("auto-click");
        if (setting<bool>("smart-click-practice-only") && !m_isPracticeMode) on = false;
        bool show = on && setting<bool>("smart-click-indicator") && m_player1 && m_player1->m_isDart;
        if (!f->label) {
            if (!show) return;
            f->label = CCLabelBMFont::create("Smart Click", "bigFont.fnt");
            f->label->setScale(0.3f);
            f->label->setOpacity(140);
            f->label->setAnchorPoint({0.f, 1.f});
            auto win = CCDirector::get()->getWinSize();
            f->label->setPosition({5.f, win.height - 5.f});
            this->addChild(f->label, 1000);
        }
        f->label->setVisible(show);
    }

    void updateTrajectory() {
        auto f = m_fields.self();
        if (f->draw) f->draw->clear();

        if (!setting<bool>("show-trajectory")) return;
        if (setting<bool>("trajectory-practice-only") && !m_isPracticeMode) return;
        auto real = m_player1;
        if (!real || real->m_isDead || m_hasCompletedLevel || real->m_isDashing) return;
        if (!real->m_isDart) return; // wave only
        if (!ensureGhosts(this)) return;

        if (!f->draw) {
            f->draw = CCDrawNode::create();
            f->draw->setZOrder(9999);
            m_objectLayer->addChild(f->draw);
        }

        int steps = msToTicks(setting<int64_t>("trajectory-length"));
        SimInput aIn = g_smart.gdPressed ? SimInput::Hold
                     : (isHoldMode(real) ? SimInput::Hold : SimInput::Tap);
        auto b = simulate(this, g_ghostB, real, SimInput::Release, steps, true);
        auto a = simulate(this, g_ghostA, real, aIn, steps, true);

        auto d = f->draw.data();
        drawPath(d, b, ccc4f(1.f, 0.3f, 0.3f, 0.85f), 0, (int)b.points.size());
        drawPath(d, a, ccc4f(0.3f, 1.f, 0.4f, 0.9f), 0, (int)a.points.size());

        if (false &&
            b.landingIndex > 0 && b.landingIndex < (int)b.points.size() &&
            isGroundJumpMode(real) && !real->m_isOnGround) {
            int start = b.landingIndex - msToTicks(setting<int64_t>("buffer-window"));
            auto yellow = ccc4f(1.f, 0.9f, 0.2f, 1.f);
            drawPath(d, b, yellow, start, b.landingIndex);
            d->drawDot(b.points[b.landingIndex], 3.f, yellow);
        }
        drawEnd(d, b);
        drawEnd(d, a);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        measureTickRate();
        updateLabel();
        updateTrajectory();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        if (f->draw) f->draw->clear();
        resetInputState();
    }
};
