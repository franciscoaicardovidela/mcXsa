// ============================================================================
//  Minecraftsa.cpp - Minecraft-style skin, items, elytra flight and melee for
//  GTA San Andreas (1.0 US by default, via plugin-sdk).  Built as MinecraftSA.asi by CMakeLists.txt.
//
//  IMPORTANT: written without access to the plugin-sdk headers.  Every call whose
//  exact name/signature I was not certain of lives in `namespace glue` below and is
//  tagged VERIFY.  Fix those against your checkout of gtamods/plugin-sdk; the game
//  logic outside `glue` only touches fields/functions that are very stable.
// ============================================================================
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "plugin.h"
#include "common.h"
#include "CCamera.h"
#include "CCutsceneMgr.h"
#include "CFont.h"
#include "CPad.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CPools.h"
#include "CSprite2d.h"
#include "CTimer.h"
#include "CWorld.h"
#include "extensions/ScriptCommands.h"

// windows.h and some SDK headers may leave min/max macros defined, which break std::min/std::max/std::clamp.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "Elytraphysics.h"

using namespace plugin;

// ----------------------------------------------------------------------------
//  Unit conversion.  Minecraft: blocks per 20 Hz tick.  GTA: metres per 1/50 s
//  (CTimer::ms_fTimeStep == 1.0 is 20 ms).   1 block/tick = 20 m/s = 0.4 GTA units.
// ----------------------------------------------------------------------------
static constexpr float kMcToGta        = 0.4f;
static constexpr float kGtaToMc        = 2.5f;
static constexpr float kTicksPerFrame  = 0.4f;    // ts (20 ms) as a fraction of a 50 ms tick
static constexpr float kPedGravity     = 0.008f;  // GTA ped gravity per timestep unit
static constexpr int   kWeaponKatana   = 8;

// ----------------------------------------------------------------------------
//  Configuration (MinecraftSA.ini next to the .asi)
// ----------------------------------------------------------------------------
struct Config {
    bool  skinEnabled = true;
    char  skinName[16] = "steve";
    int   skinSlot = 1;

    int   keyGlide = 'G', keyJump = VK_SHIFT, keyAttack = VK_LBUTTON, keyRocket = VK_RBUTTON;

    float hpScale = 5.0f;
    float swordDamage = 6.0f, swordReach = 3.0f, swordArcDeg = 60.0f;
    float swordAttackSpeed = 1.6f, swordKnockback = 0.4f;
    float sweepRadius = 1.0f, sweepDamage = 1.0f;

    float minStartAltitude = 2.0f;
    bool  safeLanding = true;
    float flareHeight = 3.0f;
    float gravityComp = 1.0f;
    int   rocketFlight = 1;

    char  glideAnim[32] = "FALL_SkyDive", glideBlock[32] = "PARACHUTE";
    char  swingBlock[32] = "SWORD";
    char  swing[3][32] = {"sword_1", "sword_2", "sword_3"};
};
static Config cfg;

static void IniPath(char* out, size_t n) {
    HMODULE hm = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&IniPath), &hm);
    GetModuleFileNameA(hm, out, static_cast<DWORD>(n));
    char* dot = std::strrchr(out, '.');
    if (dot && static_cast<size_t>(dot - out) + 5 <= n) std::strcpy(dot, ".ini");
}
static int   IniInt(const char* s, const char* k, int d, const char* f) { return static_cast<int>(GetPrivateProfileIntA(s, k, d, f)); }
static float IniFloat(const char* s, const char* k, float d, const char* f) {
    char def[32], buf[32];
    std::snprintf(def, sizeof def, "%g", d);
    GetPrivateProfileStringA(s, k, def, buf, sizeof buf, f);
    return static_cast<float>(std::atof(buf));
}
static void IniStr(const char* s, const char* k, char* inout, size_t n, const char* f) {
    char buf[64];
    GetPrivateProfileStringA(s, k, inout, buf, sizeof buf, f);
    std::strncpy(inout, buf, n - 1);
    inout[n - 1] = 0;
}

static void LoadConfig() {
    char f[MAX_PATH];
    IniPath(f, sizeof f);
    cfg.skinEnabled = IniInt("Skin", "Enabled", 1, f) != 0;
    IniStr("Skin", "ModelName", cfg.skinName, sizeof cfg.skinName, f);
    cfg.skinSlot = std::clamp(IniInt("Skin", "Slot", 1, f), 1, 10);

    cfg.keyGlide  = IniInt("Keys", "Glide", cfg.keyGlide, f);
    cfg.keyJump   = IniInt("Keys", "Jump", cfg.keyJump, f);
    cfg.keyAttack = IniInt("Keys", "Attack", cfg.keyAttack, f);
    cfg.keyRocket = IniInt("Keys", "Rocket", cfg.keyRocket, f);

    cfg.hpScale          = IniFloat("Combat", "HpScale", cfg.hpScale, f);
    cfg.swordDamage      = IniFloat("Combat", "SwordDamage", cfg.swordDamage, f);
    cfg.swordReach       = IniFloat("Combat", "SwordReach", cfg.swordReach, f);
    cfg.swordArcDeg      = IniFloat("Combat", "SwordArcDegrees", cfg.swordArcDeg, f);
    cfg.swordAttackSpeed = std::max(0.1f, IniFloat("Combat", "SwordAttackSpeed", cfg.swordAttackSpeed, f));
    cfg.swordKnockback   = IniFloat("Combat", "SwordKnockback", cfg.swordKnockback, f);
    cfg.sweepRadius      = IniFloat("Combat", "SweepRadius", cfg.sweepRadius, f);
    cfg.sweepDamage      = IniFloat("Combat", "SweepDamage", cfg.sweepDamage, f);

    cfg.minStartAltitude = IniFloat("Elytra", "MinStartAltitude", cfg.minStartAltitude, f);
    cfg.safeLanding      = IniInt("Elytra", "SafeLanding", 1, f) != 0;
    cfg.flareHeight      = IniFloat("Elytra", "FlareHeight", cfg.flareHeight, f);
    cfg.gravityComp      = IniFloat("Elytra", "GravityCompensation", cfg.gravityComp, f);
    cfg.rocketFlight     = IniInt("Elytra", "RocketFlightDuration", cfg.rocketFlight, f);

    IniStr("Animations", "GlideName", cfg.glideAnim, sizeof cfg.glideAnim, f);
    IniStr("Animations", "GlideBlock", cfg.glideBlock, sizeof cfg.glideBlock, f);
    IniStr("Animations", "SwingBlock", cfg.swingBlock, sizeof cfg.swingBlock, f);
    IniStr("Animations", "Swing1", cfg.swing[0], sizeof cfg.swing[0], f);
    IniStr("Animations", "Swing2", cfg.swing[1], sizeof cfg.swing[1], f);
    IniStr("Animations", "Swing3", cfg.swing[2], sizeof cfg.swing[2], f);
}

// ----------------------------------------------------------------------------
//  SDK glue.  Everything marked VERIFY is from memory; adjust to your SDK checkout.
//  Script commands are called as plugin::Command<plugin::Commands::NAME>(...).
//  If your SDK spells them COMMAND_NAME, change them here and only here.
// ----------------------------------------------------------------------------
namespace glue {

static int Ref(CPed* p) { return CPools::GetPedRef(p); }

// VERIFY: bit-field name (m_nPedFlags.bIsStanding in current SDK; older ones use m_pedFlags).
static bool IsStanding(CPed* p) { return p->m_nPedFlags.bIsStanding; }

static bool InVehicle(CPed* p) { return p->m_pVehicle != nullptr; }

static bool InWater(CPed* p) { return Command<Commands::IS_CHAR_IN_WATER>(Ref(p)); }

// VERIFY: camera member names.  Needs the view direction of the active camera.
static mcphys::V3 LookDir() {
    const CVector& f = TheCamera.m_aCams[TheCamera.m_nActiveCam].m_vecFront;
    return {f.x, f.y, f.z};
}

// VERIFY: signature (x, y, z, bool* found, CEntity** entity).  Height above whatever is below us.
static float Altitude(const CVector& pos) {
    bool found = false;
    const float gz = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z, &found, nullptr);
    return found ? pos.z - gz : 9999.0f;
}

static void SetHeadingDeg(CPed* p, float deg) { Command<Commands::SET_CHAR_HEADING>(Ref(p), deg); }

static bool AnimReady(const char* block) {
    Command<Commands::REQUEST_ANIMATION>(block);
    return Command<Commands::HAS_ANIMATION_LOADED>(block);
}
static void PlayAnim(CPed* p, const char* name, const char* block, bool loop, float blend) {
    if (!AnimReady(block)) return;
    Command<Commands::TASK_PLAY_ANIM>(Ref(p), name, block, blend, loop ? 1 : 0, 0, 0, 0, -1);
}
static void ClearTasks(CPed* p) { Command<Commands::CLEAR_CHAR_TASKS>(Ref(p)); }

// The katana is the visual carrier for the sword (replace its DFF/TXD in gta3.img).
static void SetSwordEquipped(CPed* p, bool on) {
    const int h = Ref(p);
    if (on) {
        Command<Commands::GIVE_WEAPON_TO_CHAR>(h, kWeaponKatana, 1);
        Command<Commands::SET_CURRENT_CHAR_WEAPON>(h, kWeaponKatana);
    } else {
        Command<Commands::SET_CURRENT_CHAR_WEAPON>(h, 0);
        Command<Commands::REMOVE_WEAPON_FROM_CHAR>(h, kWeaponKatana);
    }
}

// VERIFY: stop GTA's own punch/swing from firing while the sword is selected.
static void SuppressNativeAttack() { CPad::GetPad(0)->NewState.ButtonCircle = 0; }

// Hook for rocket exhaust / sparks (particles, sound). Left empty on purpose.
static void RocketFx(const CVector& /*pos*/) {}

}  // namespace glue

// ----------------------------------------------------------------------------
//  Input: raw keyboard/mouse with edge detection, only while the game has focus.
// ----------------------------------------------------------------------------
namespace Input {
static bool cur[256], prev[256];

static bool GameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
static void Update() {
    const bool focus = GameFocused();
    std::memcpy(prev, cur, sizeof cur);
    for (int k = 1; k < 256; ++k) cur[k] = focus && ((GetAsyncKeyState(k) & 0x8000) != 0);
}
static bool Down(int vk) { return vk > 0 && vk < 256 && cur[vk]; }
static bool Pressed(int vk) { return Down(vk) && !prev[vk]; }
}  // namespace Input

// ----------------------------------------------------------------------------
//  Items and hotbar
// ----------------------------------------------------------------------------
enum ItemType : unsigned char { ITEM_NONE, ITEM_SWORD, ITEM_ELYTRA, ITEM_ROCKET, ITEM_COUNT };

struct ItemDef {
    const char* name;
    const char* tag;
    unsigned char r, g, b;
    int maxStack;
    int maxDurability;
};
static const ItemDef kDefs[ITEM_COUNT] = {
    {"", "", 0, 0, 0, 0, 0},
    {"Iron Sword", "SW", 190, 195, 205, 1, 250},
    {"Elytra", "EL", 140, 120, 175, 1, 432},
    {"Firework Rocket", "RK", 205, 70, 60, 64, 0},
};

struct Slot {
    ItemType type = ITEM_NONE;
    int count = 0;
    int durability = 0;
};

static constexpr int kSlots = 9;
static Slot g_slots[kSlots];
static int g_selected = 0;

static void ResetHotbar() {
    for (auto& s : g_slots) s = Slot{};
    g_slots[0] = {ITEM_SWORD, 1, kDefs[ITEM_SWORD].maxDurability};
    g_slots[1] = {ITEM_ELYTRA, 1, kDefs[ITEM_ELYTRA].maxDurability};
    g_slots[2] = {ITEM_ROCKET, 64, 0};
    g_selected = 0;
}
static Slot* FindItem(ItemType t) {
    for (auto& s : g_slots)
        if (s.type == t) return &s;
    return nullptr;
}
static Slot& Selected() { return g_slots[g_selected]; }
static void ClearIfEmpty(Slot& s) {
    if (s.type == ITEM_ROCKET && s.count <= 0) s = Slot{};
}

// ----------------------------------------------------------------------------
//  Status line shown by the HUD (e.g. when the Steve model is missing)
// ----------------------------------------------------------------------------
static char g_status[96];
static unsigned g_statusUntil;
static void SetStatus(const char* text, unsigned now, unsigned ms = 8000) {
    std::strncpy(g_status, text, sizeof g_status - 1);
    g_status[sizeof g_status - 1] = 0;
    g_statusUntil = now + ms;
}

// ----------------------------------------------------------------------------
//  Damage helper (HP in GTA units)
// ----------------------------------------------------------------------------
static void HurtPed(CPed* t, float gtaDamage) {
    if (gtaDamage <= 0.0f) return;
    if (t->m_fArmour > 0.0f) {
        const float a = std::min(t->m_fArmour, gtaDamage);
        t->m_fArmour -= a;
        gtaDamage -= a;
    }
    t->m_fHealth = std::max(0.0f, t->m_fHealth - gtaDamage);
}

// ----------------------------------------------------------------------------
//  1. Steve skin  (special model slot, loaded without blocking the game)
// ----------------------------------------------------------------------------
namespace Skin {
enum State { Idle, Loading, Applied };
static State state = Idle;
static unsigned loadStart = 0, retryAt = 0;

static int ModelId() { return 289 + cfg.skinSlot; }  // slot 1 -> 290 (SPECIAL01)

static void Reset() { state = Idle; retryAt = 0; }

static void Update(CPlayerPed* p, unsigned now) {
    if (!cfg.skinEnabled || CCutsceneMgr::ms_running || glue::InVehicle(p)) return;

    if (static_cast<int>(p->m_nModelIndex) == ModelId()) { state = Applied; return; }
    if (state == Applied) state = Idle;  // the game swapped CJ back (mission end, save load...)
    if (now < retryAt) return;

    if (state == Idle) {
        Command<Commands::LOAD_SPECIAL_CHARACTER>(cfg.skinSlot, cfg.skinName);
        state = Loading;
        loadStart = now;
    } else if (state == Loading) {
        if (Command<Commands::HAS_SPECIAL_CHARACTER_LOADED>(cfg.skinSlot)) {
            p->SetModelIndex(ModelId());
            state = Applied;
        } else if (now - loadStart > 8000) {
            SetStatus("MinecraftSA: steve.dff/.txd not found in gta3.img", now, 12000);
            state = Idle;
            retryAt = now + 20000;
        }
    }
}
}  // namespace Skin

// ----------------------------------------------------------------------------
//  2. Sword: Minecraft-style melee (cooldown, reach, crit, sweep, knockback)
// ----------------------------------------------------------------------------
namespace Sword {
static unsigned lastAttack = 0;
static bool equipped = false;
static int swingIndex = 0;

static void Reset() { lastAttack = 0; equipped = false; swingIndex = 0; }

static float Charge(unsigned now) {
    const float rechargeMs = 1000.0f / cfg.swordAttackSpeed;
    return std::clamp((static_cast<float>(now - lastAttack) + 25.0f) / rechargeMs, 0.0f, 1.0f);
}

static bool Valid(CPed* t, CPed* self) {
    return t && t != self && t->m_fHealth > 0.0f && !glue::InVehicle(t);
}

static CPed* FindTarget(CPlayerPed* p) {
    const CVector pp = p->GetPosition();
    const float h = p->GetHeading();
    const CVector fwd(-std::sin(h), std::cos(h), 0.0f);
    const float minDot = std::cos(cfg.swordArcDeg * 3.14159265f / 180.0f);

    CPed* best = nullptr;
    float bestScore = 1e9f;
    auto* pool = CPools::ms_pPedPool;
    for (int i = 0; i < pool->m_nSize; ++i) {
        CPed* t = pool->GetAt(i);
        if (!Valid(t, p)) continue;
        const CVector d = t->GetPosition() - pp;
        if (std::fabs(d.z) > 2.0f) continue;
        const float dist = std::hypot(d.x, d.y);
        if (dist < 0.01f || dist > cfg.swordReach + 0.5f) continue;  // +0.5 ~ body radius
        const float dot = (d.x * fwd.x + d.y * fwd.y) / dist;
        if (dot < minDot) continue;
        const float score = dist * (2.0f - dot);
        if (score < bestScore) { bestScore = score; best = t; }
    }
    return best;
}

static void Attack(CPlayerPed* p, Slot& sword, unsigned now) {
    const float charge = Charge(now);
    lastAttack = now;  // swinging at air still resets the cooldown, like Minecraft

    glue::PlayAnim(p, cfg.swing[swingIndex % 3], cfg.swingBlock, false, 8.0f);
    ++swingIndex;

    CPed* target = FindTarget(p);
    if (!target) return;

    float dmg = cfg.swordDamage * (0.2f + charge * charge * 0.8f);
    const bool falling = !glue::IsStanding(p) && p->m_vecMoveSpeed.z < -0.01f;
    const bool crit = charge > 0.9f && falling;
    if (crit) dmg *= 1.5f;
    HurtPed(target, dmg * cfg.hpScale);

    // Knockback away from the attacker.
    const CVector pp = p->GetPosition();
    const CVector tp = target->GetPosition();
    const float dx = tp.x - pp.x, dy = tp.y - pp.y;
    const float dist = std::max(0.01f, std::hypot(dx, dy));
    const float kb = cfg.swordKnockback * kMcToGta;
    target->m_vecMoveSpeed.x += dx / dist * kb;
    target->m_vecMoveSpeed.y += dy / dist * kb;
    target->m_vecMoveSpeed.z += 0.4f * kMcToGta;

    // Sweeping edge: full-charge, non-crit hits splash nearby targets.
    if (charge > 0.9f && !crit) {
        auto* pool = CPools::ms_pPedPool;
        for (int i = 0; i < pool->m_nSize; ++i) {
            CPed* t = pool->GetAt(i);
            if (!Valid(t, p) || t == target) continue;
            const CVector d = t->GetPosition() - tp;
            if (std::hypot(d.x, d.y) <= cfg.sweepRadius && std::fabs(d.z) <= 1.0f)
                HurtPed(t, cfg.sweepDamage * cfg.hpScale);
        }
    }

    if (--sword.durability <= 0) { sword.durability = 0; }  // broken sword stays in the slot
}

static void Update(CPlayerPed* p, unsigned now, bool gliding) {
    Slot& sel = Selected();
    const bool holding = sel.type == ITEM_SWORD && sel.durability > 0;
    if (holding != equipped) { glue::SetSwordEquipped(p, holding); equipped = holding; }
    if (!holding || gliding) return;

    glue::SuppressNativeAttack();
    if (Input::Pressed(cfg.keyAttack)) Attack(p, sel, now);
}
}  // namespace Sword

// ----------------------------------------------------------------------------
//  3. Elytra + firework rockets
// ----------------------------------------------------------------------------
namespace Elytra {
static bool gliding = false;
static mcphys::V3 vel;                 // blocks / tick, Z up
static std::vector<float> rockets;     // remaining ticks per active rocket
static float durAccum = 0.0f;
static unsigned lastJumpPress = 0;
static bool haveLast = false;
static CVector lastPos;
static float lastSetSpeedGta = 0.0f, lastTs = 1.0f;

static void Reset() { gliding = false; rockets.clear(); durAccum = 0; lastJumpPress = 0; haveLast = false; }

static void End(CPed* p) {
    if (!gliding) return;
    gliding = false;
    rockets.clear();
    haveLast = false;
    glue::ClearTasks(p);
}

static void Start(CPlayerPed* p) {
    gliding = true;
    rockets.clear();
    durAccum = 0.0f;
    haveLast = false;
    vel = {p->m_vecMoveSpeed.x * kGtaToMc, p->m_vecMoveSpeed.y * kGtaToMc, p->m_vecMoveSpeed.z * kGtaToMc};
    glue::PlayAnim(p, cfg.glideAnim, cfg.glideBlock, true, 4.0f);
}

static void Update(CPlayerPed* p, unsigned now) {
    const float ts = std::min(CTimer::ms_fTimeStep, 5.0f);
    const bool jumpPressed = Input::Pressed(cfg.keyJump);
    bool doubleTap = false;
    if (jumpPressed) {
        doubleTap = (now - lastJumpPress) < 350;
        lastJumpPress = now;
    }
    const bool glideKey = Input::Pressed(cfg.keyGlide);

    Slot* elytra = FindItem(ITEM_ELYTRA);
    const bool usable = elytra && elytra->durability > 0 && !glue::InVehicle(p) && !CCutsceneMgr::ms_running;
    const CVector pos = p->GetPosition();

    if (!gliding) {
        if (usable && (doubleTap || glideKey) && !glue::IsStanding(p) && glue::Altitude(pos) > cfg.minStartAltitude)
            Start(p);
        return;
    }

    // ---- end conditions ----
    if (!usable || p->m_fHealth <= 0.0f || glideKey || glue::IsStanding(p) || glue::InWater(p)) {
        End(p);
        return;
    }

    // ---- crash detection: compare where we should have moved with where we did ----
    if (haveLast && lastTs > 0.0f && lastSetSpeedGta > 1e-4f) {
        const float expected = lastSetSpeedGta * lastTs;
        const float actual = std::hypot(pos.x - lastPos.x, pos.y - lastPos.y);
        const float lossMc = ((expected - actual) / lastTs) * kGtaToMc;
        const float hBefore = static_cast<float>(mcphys::HorizontalLength(vel));
        if (lossMc > 0.04f + 0.08f * hBefore) {
            const float hAfter = std::max(0.0f, hBefore - lossMc);
            const double dmg = mcphys::KineticImpactDamage(hBefore, hAfter);
            if (dmg > 0.0) HurtPed(p, static_cast<float>(dmg) * cfg.hpScale);
            const float k = hBefore > 1e-4f ? hAfter / hBefore : 0.0f;
            vel.x *= k;
            vel.y *= k;
        }
    }

    // ---- rocket use ----
    Slot& sel = Selected();
    if (Input::Pressed(cfg.keyRocket) && sel.type == ITEM_ROCKET && sel.count > 0) {
        --sel.count;
        ClearIfEmpty(sel);
        rockets.push_back(static_cast<float>(
            mcphys::RocketLifetimeTicks(cfg.rocketFlight, std::rand() % 6, std::rand() % 7)));
    }

    // ---- physics ----
    const double s = ts * kTicksPerFrame;
    const mcphys::V3 look = glue::LookDir();
    vel = mcphys::ElytraStep(vel, look, s);
    for (float& left : rockets) {
        vel = mcphys::RocketStep(vel, look, s);
        left -= static_cast<float>(s);
        glue::RocketFx(pos);
    }
    rockets.erase(std::remove_if(rockets.begin(), rockets.end(), [](float t) { return t <= 0.0f; }), rockets.end());

    // Optional flare: keep the touchdown speed under GTA's fall-damage threshold.
    if (cfg.safeLanding && glue::Altitude(pos) < cfg.flareHeight && vel.z < -0.3)
        vel.z = -0.3;

    // ---- durability: 1 point per second of flight ----
    durAccum += static_cast<float>(s);
    while (durAccum >= 20.0f) { durAccum -= 20.0f; if (elytra->durability > 0) --elytra->durability; }

    // ---- hand the velocity to the game ----
    p->m_vecMoveSpeed.x = static_cast<float>(vel.x) * kMcToGta;
    p->m_vecMoveSpeed.y = static_cast<float>(vel.y) * kMcToGta;
    p->m_vecMoveSpeed.z = static_cast<float>(vel.z) * kMcToGta + kPedGravity * ts * cfg.gravityComp;

    if (mcphys::HorizontalLength(vel) > 0.02)
        glue::SetHeadingDeg(p, std::atan2(-static_cast<float>(vel.x), static_cast<float>(vel.y)) * 57.2957795f);

    lastPos = pos;
    lastTs = ts;
    lastSetSpeedGta = std::hypot(p->m_vecMoveSpeed.x, p->m_vecMoveSpeed.y);
    haveLast = true;
}

static float SpeedKmh() { return static_cast<float>(mcphys::Length(vel)) * 20.0f * 3.6f; }
}  // namespace Elytra

// ----------------------------------------------------------------------------
//  HUD
// ----------------------------------------------------------------------------
namespace Hud {
static void Rect(float x, float y, float w, float h, const CRGBA& c) {
    CSprite2d::DrawRect(CRect(x, y + h, x + w, y), c);  // VERIFY: CRect(left, bottom, right, top)
}

static void Text(float x, float y, float sx, float sy, const CRGBA& col, const char* s) {
    char buf[96];
    std::strncpy(buf, s, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    CFont::SetFontStyle(FONT_SUBTITLES);
    CFont::SetProportional(true);
    CFont::SetBackground(false, false);
    CFont::SetOrientation(ALIGN_LEFT);
    CFont::SetScale(sx, sy);
    CFont::SetColor(col);
    CFont::SetEdge(1);
    CFont::PrintString(x, y, buf);
}

static void Draw() {
    CPlayerPed* p = FindPlayerPed(-1);
    if (!p || CCutsceneMgr::ms_running) return;

    const float W = static_cast<float>(RsGlobal.maximumWidth);   // VERIFY: RsGlobal field names
    const float H = static_cast<float>(RsGlobal.maximumHeight);
    const float k = H / 448.0f;
    const unsigned now = CTimer::m_snTimeInMilliseconds;

    const float slot = 40.0f * k, gap = 3.0f * k;
    const float total = kSlots * slot + (kSlots - 1) * gap;
    const float x0 = (W - total) * 0.5f, y0 = H - slot - 16.0f * k;

    for (int i = 0; i < kSlots; ++i) {
        const float x = x0 + i * (slot + gap);
        const bool sel = i == g_selected;
        Rect(x - 2 * k, y0 - 2 * k, slot + 4 * k, slot + 4 * k, sel ? CRGBA(255, 255, 255, 230) : CRGBA(20, 20, 20, 150));
        Rect(x, y0, slot, slot, CRGBA(45, 45, 45, 200));

        char num[4];
        std::snprintf(num, sizeof num, "%d", i + 1);
        Text(x + 3 * k, y0 + 1 * k, 0.28f * k, 0.7f * k, CRGBA(190, 190, 190, 255), num);

        const Slot& s = g_slots[i];
        if (s.type == ITEM_NONE) continue;
        const ItemDef& d = kDefs[s.type];
        const bool broken = d.maxDurability > 0 && s.durability <= 0;
        Rect(x + 8 * k, y0 + 8 * k, slot - 16 * k, slot - 16 * k, broken ? CRGBA(110, 40, 40, 255) : CRGBA(d.r, d.g, d.b, 255));
        Text(x + 11 * k, y0 + 12 * k, 0.34f * k, 0.9f * k, CRGBA(20, 20, 20, 255), d.tag);

        if (s.count > 1) {
            char c[8];
            std::snprintf(c, sizeof c, "%d", s.count);
            Text(x + slot - (6.0f + 6.0f * std::strlen(c)) * k, y0 + slot - 14 * k, 0.32f * k, 0.8f * k, CRGBA(255, 255, 255, 255), c);
        }
        if (d.maxDurability > 0 && s.durability < d.maxDurability) {
            const float f = std::clamp(static_cast<float>(s.durability) / d.maxDurability, 0.0f, 1.0f);
            Rect(x + 3 * k, y0 + slot - 5 * k, slot - 6 * k, 2 * k, CRGBA(0, 0, 0, 220));
            Rect(x + 3 * k, y0 + slot - 5 * k, (slot - 6 * k) * f, 2 * k, CRGBA(static_cast<int>(255 * (1 - f)), static_cast<int>(255 * f), 0, 255));
        }
    }

    // Selected item name.
    const Slot& sel = Selected();
    if (sel.type != ITEM_NONE) Text(x0, y0 - 20 * k, 0.4f * k, 1.0f * k, CRGBA(255, 255, 255, 255), kDefs[sel.type].name);

    // Attack-charge indicator under the crosshair while the sword is out.
    if (sel.type == ITEM_SWORD && sel.durability > 0) {
        const float c = Sword::Charge(now);
        if (c < 1.0f) {
            const float w = 36.0f * k;
            Rect(W * 0.5f - w * 0.5f, H * 0.5f + 24 * k, w, 3 * k, CRGBA(0, 0, 0, 160));
            Rect(W * 0.5f - w * 0.5f, H * 0.5f + 24 * k, w * c, 3 * k, CRGBA(255, 255, 255, 220));
        }
    }

    // Flight read-out.
    if (Elytra::gliding) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.0f km/h   alt %.0f m", Elytra::SpeedKmh(), p->GetPosition().z);
        Text(W * 0.5f - 60 * k, y0 - 44 * k, 0.4f * k, 1.0f * k, CRGBA(255, 255, 255, 255), buf);
    }

    if (now < g_statusUntil) Text(12 * k, 12 * k, 0.4f * k, 1.0f * k, CRGBA(255, 120, 120, 255), g_status);
}
}  // namespace Hud

// ----------------------------------------------------------------------------
//  Per-frame driver
// ----------------------------------------------------------------------------
static void ResetRuntime() {
    ResetHotbar();
    Skin::Reset();
    Sword::Reset();
    Elytra::Reset();
}

static void Process() {
    CPlayerPed* p = FindPlayerPed(-1);
    if (!p || CTimer::m_UserPause || CTimer::m_CodePause) return;

    static CPlayerPed* lastPlayer = nullptr;
    if (p != lastPlayer) { lastPlayer = p; ResetRuntime(); }

    Input::Update();
    const unsigned now = CTimer::m_snTimeInMilliseconds;

    Skin::Update(p, now);

    if (p->m_fHealth <= 0.0f) { Elytra::End(p); return; }

    for (int i = 0; i < kSlots; ++i)
        if (Input::Pressed('1' + i)) g_selected = i;

    Sword::Update(p, now, Elytra::gliding);
    Elytra::Update(p, now);
}

class MinecraftSA {
public:
    MinecraftSA() {
        LoadConfig();
        ResetHotbar();
        Events::gameProcessEvent += Process;
        Events::drawHudEvent += Hud::Draw;
    }
} g_minecraftSA;
