#include "galaxy/site_hooks.h"

#include "galaxy/cloud90_game.h"
#include "galaxy/cloud90_link.h"
#include "galaxy/experimental_ultrawide_aspect.h"
#include "galaxy/layout_aspect.h"
#include "galaxy/layout_aspect_cinema.h"
#include "galaxy/layout_aspect_fits.h"
#include "galaxy/layout_aspect_home.h"
#include "galaxy/runtime_settings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace galaxy {
namespace {

// The star-select sky (MultiSceneActor named by the DOL literal at 0x805C4411)
// is one quad at z = -1000 in model space, sized for a 16:9 view. Wider views
// scale it about its center so it still covers the screen.
void fit_rmge01_scenario_sky(PpcContext* context, GuestMemoryV1* memory,
                             const NativeServicesV1* services, std::uint32_t pc) {
    if (guest_load_u32(memory, context->gpr[31] + 4u, services, pc) != 0x805C4411u) return;
    const auto policy = experimental_effective_guest_aspect(services);
    if (!policy || native_four_three(*policy)) return;
    const float k = experimental_nw4r_canvas_width(*policy) / 608.0f;
    if (!(k > 1.0f)) return;
    const std::uint32_t model = context->gpr[3];
    const auto f32 = [&](std::uint32_t address) {
        return std::bit_cast<float>(guest_load_u32(memory, address, services, pc));
    };
    const auto store = [&](std::uint32_t address, float value) {
        guest_store_u32(memory, address, std::bit_cast<std::uint32_t>(value), services, pc);
    };
    constexpr float kCenter[3] = {0.0f, 0.0f, -1000.0f};
    float offset[3];
    for (std::uint32_t i = 0; i < 3u; ++i) {
        const float scale = f32(model + 0x18u + i * 4u);
        offset[i] = scale * (1.0f - k) * kCenter[i];
        store(model + 0x18u + i * 4u, scale * k);
    }
    // J3DModel base matrix (+0x24) is rotation and translation only.
    for (std::uint32_t row = 0; row < 3u; ++row) {
        const std::uint32_t base = model + 0x24u + row * 0x10u;
        float shift = 0.0f;
        for (std::uint32_t column = 0; column < 3u; ++column) shift += f32(base + column * 4u) * offset[column];
        store(base + 0xCu, f32(base + 0xCu) + shift);
    }
}

bool rmge01_guest_object(std::uint32_t address) noexcept {
    return ((address >= 0x80000000u && address < 0x81800000u) ||
            (address >= 0x90000000u && address < 0x94000000u)) &&
           (address & 3u) == 0u;
}

// ScenarioSelectScene builds the perspective of its own CameraContext once, at
// boot, with the aspect in force then; a later aspect never reaches it, and the
// star select drew 16:9 into every other aspect. Each frame its stars and sky
// animate, the projection's x scale is set from its y scale and the current
// aspect. GameSystem (0x806A1228) +0x24 scene controller, +0xBC
// ScenarioSelectScene, +0x30 CameraContext, projection (row-major 4x4) at +0x6C.
void fit_rmge01_scenario_camera(GuestMemoryV1* memory, const NativeServicesV1* services,
                                std::uint32_t pc) {
    const auto policy = experimental_effective_guest_aspect(services);
    if (policy && native_four_three(*policy)) return;
    const double aspect = policy ? policy->content_aspect : kWiiContentAspect;
    if (!(aspect > 0.0)) return;
    std::uint32_t object = guest_load_u32(memory, 0x806A1228u, services, pc);
    for (const std::uint32_t offset : {0x24u, 0xBCu, 0x30u}) {
        if (!rmge01_guest_object(object)) return;
        object = guest_load_u32(memory, object + offset, services, pc);
    }
    if (!rmge01_guest_object(object)) return;
    const std::uint32_t projection = object + 0x6Cu;
    const auto f32 = [&](std::uint32_t address) {
        return std::bit_cast<float>(guest_load_u32(memory, address, services, pc));
    };
    const float y_scale = f32(projection + 0x14u);
    if (!(y_scale > 0.0f) || f32(projection + 0x38u) != -1.0f) return;
    const float x_scale = static_cast<float>(y_scale / aspect);
    if (f32(projection) != x_scale) {
        guest_store_u32(memory, projection, std::bit_cast<std::uint32_t>(x_scale), services, pc);
    }
}

// Calls into the game's own functions (addresses from the CC0 Petari RMGE01
// symbols) from a Cloud90 hook on the game thread. Every call leaves the
// registers as it found them; the hooks sit right after a call returns, so
// the volatile float registers hold nothing then.
class GuestCalls {
protected:
    GuestCalls(const NativeServicesV1* services, PpcContext* context, GuestMemoryV1* memory,
               std::uint32_t pc) noexcept
        : services_(services), context_(context), memory_(memory), pc_(pc) {}

    std::uint32_t call(std::uint32_t function, std::uint32_t r3, std::uint32_t r4 = 0u, std::uint32_t r5 = 0u) {
        std::uint32_t gpr[32];
        std::copy(std::begin(context_->gpr), std::end(context_->gpr), gpr);
        const std::uint32_t cr = context_->cr, lr = context_->lr, ctr = context_->ctr, xer = context_->xer;
        context_->gpr[3] = r3;
        context_->gpr[4] = r4;
        context_->gpr[5] = r5;
        services_->call_guest(services_->user, function, context_, memory_);
        const std::uint32_t result = context_->gpr[3];
        std::copy(std::begin(gpr), std::end(gpr), context_->gpr);
        context_->cr = cr;
        context_->lr = lr;
        context_->ctr = ctr;
        context_->xer = xer;
        return result;
    }
    // A C++ virtual of a game object: its vtable is at +0.
    std::uint32_t call_virtual(std::uint32_t object, std::uint32_t slot) {
        const std::uint32_t vtable = guest_load_u32(memory_, object, services_, pc_);
        if (!rmge01_guest_object(vtable)) return 0u;
        const std::uint32_t function = guest_load_u32(memory_, vtable + slot, services_, pc_);
        return rmge01_guest_object(function) ? call(function, object) : 0u;
    }
    // Runs `body(buffer)` with `size` bytes of guest memory in a stack frame
    // of its own under the stack pointer (back chain at +0, the buffer at
    // +0x10), for the game's functions that take a pointer.
    template <typename Body>
    void with_stack(std::uint32_t size, Body body) {
        const std::uint32_t sp = context_->gpr[1];
        const std::uint32_t frame = (sp - (size + 0x10u + 0xFu)) & ~0xFu;
        guest_store_u32(memory_, frame, sp, services_, pc_);
        context_->gpr[1] = frame;
        body(frame + 0x10u);
        context_->gpr[1] = sp;
    }
    void store_text(std::uint32_t address, const char* text) {
        for (std::uint32_t i = 0;; ++i) {
            guest_store_u8(memory_, address + i, static_cast<std::uint8_t>(text[i]), services_, pc_);
            if (text[i] == '\0') break;
        }
    }
    float load_f32(std::uint32_t address) {
        return std::bit_cast<float>(guest_load_u32(memory_, address, services_, pc_));
    }
    void store_f32(std::uint32_t address, float value) {
        guest_store_u32(memory_, address, std::bit_cast<std::uint32_t>(value), services_, pc_);
    }

    const NativeServicesV1* services_;
    PpcContext* context_;
    GuestMemoryV1* memory_;
    std::uint32_t pc_;
};

// LiveActor's virtuals (vtable offsets).
constexpr std::uint32_t kVtAppear = 0x24u;
constexpr std::uint32_t kVtMakeActorDead = 0x30u;

// Cloud90 Game Link enemies (PLAN-GAME-TROLL.md): with every galaxy a few of
// each are created dead, as the game makes its own pools (BossStinkBugBomb…):
// their archive asked for with the stage's objects, then the class's factory
// function and NameObj::initWithoutIter, which these classes take (each
// checks MR::isValidInfo before reading placement data).
struct EnemyDef {
    const char* archive;  // ObjectData/<archive>.arc, also the object's name
    std::uint32_t creator;  // NameObjFactory's createNameObj<Class>(const char*)
    std::uint32_t nerve;    // its first state again when it comes out (0: appear() does it)
    std::uint32_t count;
};
constexpr std::array<EnemyDef, static_cast<std::size_t>(cloud90::EnemyKind::Count)> kEnemies{{
    {"Kuribo", 0x80264E38u, 0x8069F9B0u, 3u},   // KuriboNrvWander
    {"BombHei", 0x80264D38u, 0u, 2u},           // BombHei::appear sets its own
}};
// The scene's heaps must keep this much free after our enemies (bytes,
// MR::getSceneHeapNapa + MR::getSceneHeapGDDR3).
constexpr std::uint32_t kEnemyHeapReserve = 1u << 20;

// The current scene's enemies (guest addresses). Game thread only: filled by
// the Cloud90Scene hook, read by Mario's.
std::array<std::vector<std::uint32_t>, static_cast<std::size_t>(cloud90::EnemyKind::Count)> g_enemy_pool;
bool g_enemy_archives_asked = false;

class GuestScene final : GuestCalls {
public:
    GuestScene(const NativeServicesV1* services, PpcContext* context, GuestMemoryV1* memory,
               std::uint32_t pc) noexcept
        : GuestCalls(services, context, memory, pc) {}

    // After GameScene::init asked for the scenario's object archives: ours
    // too, outside the Observatory and with room in the scene's heaps.
    void ask_archives() {
        for (auto& pool : g_enemy_pool) pool.clear();
        g_enemy_archives_asked = false;
        const std::string stage = stage_name();
        const std::uint32_t free = heap_free();
        cloud90::log_event("escena " + stage + ": memoria libre " + std::to_string(free / 1024u) + " KB");
        if (stage.empty() || stage.rfind("Astro", 0) == 0 || free < 2u * kEnemyHeapReserve) return;
        for (const EnemyDef& def : kEnemies) {
            with_stack(32u, [&](std::uint32_t name) {
                store_text(name, def.archive);
                if ((call(0x803CE2E4u, name) & 0xFFu) == 0u)  // MR::isLoadedObjectOrLayoutArchive
                    call(0x803CDE94u, name, 0u);              // MR::mountAsyncArchiveByObjectOrLayoutName(name, null)
            });
        }
        g_enemy_archives_asked = true;
    }

    // After the stage's objects were placed: the enemies, dead, while the
    // scene still adds objects (initAfterPlacement reaches them too).
    void create_enemies() {
        if (!g_enemy_archives_asked) return;
        std::string made;
        for (std::size_t kind = 0; kind < kEnemies.size(); ++kind) {
            const EnemyDef& def = kEnemies[kind];
            std::uint32_t count = 0;
            for (; count < def.count && heap_free() >= kEnemyHeapReserve; ++count) {
                // The name stays with the object: on the scene's heap
                // (operator new, freed with the scene).
                const std::uint32_t name = call(0x80409AF8u, 16u);
                if (!rmge01_guest_object(name)) break;
                store_text(name, def.archive);
                const std::uint32_t enemy = call(def.creator, name);
                if (!rmge01_guest_object(enemy)) break;
                call(0x8026120Cu, enemy);  // NameObj::initWithoutIter
                call_virtual(enemy, kVtMakeActorDead);
                g_enemy_pool[kind].push_back(enemy);
            }
            made += std::string(made.empty() ? "" : ", ") + def.archive + " x" + std::to_string(count);
        }
        cloud90::log_event("enemigos listos: " + made + "; memoria libre " + std::to_string(heap_free() / 1024u) +
                           " KB");
    }

private:
    // MR::getCurrentStageName.
    std::string stage_name() {
        const std::uint32_t name = call(0x803F5AB8u, 0u);
        std::string out;
        if (!rmge01_guest_object(name & ~3u)) return out;
        for (std::uint32_t i = 0; i < 64u; ++i) {
            const auto byte = static_cast<char>(guest_load_u8(memory_, name + i, services_, pc_));
            if (byte == '\0') break;
            out += byte;
        }
        return out;
    }
    // JKRHeap::getFreeSize of MR::getSceneHeapNapa and MR::getSceneHeapGDDR3.
    std::uint32_t heap_free() {
        std::uint32_t total = 0;
        for (const std::uint32_t getter : {0x803E7444u, 0x803E7450u}) {
            const std::uint32_t heap = call(getter, 0u);
            if (rmge01_guest_object(heap)) total += call(0x80409794u, heap);
        }
        return total;
    }
};

void run_cloud90_scene(const NativeServicesV1* services, PpcContext* context, GuestMemoryV1* memory,
                       std::uint32_t pc, std::uint32_t argument) {
    if (services->call_guest == nullptr) return;
    GuestScene scene(services, context, memory, pc);
    if (argument == 0u) scene.ask_archives();
    else scene.create_enemies();
}

// Cloud90 Game Link phase 5: Mario's state through the game's own
// functions, from the Cloud90Player hook.
class GuestPlayer final : public cloud90::PlayerAccess, GuestCalls {
public:
    GuestPlayer(const NativeServicesV1* services, PpcContext* context, GuestMemoryV1* memory,
                std::uint32_t actor, std::uint32_t pc) noexcept
        : GuestCalls(services, context, memory, pc), actor_(actor) {}

    // MarioActor::mHealth / mMaxHealth.
    std::uint32_t health() override { return guest_load_u32(memory_, actor_ + 0x380u, services_, pc_); }
    std::uint32_t max_health() override { return guest_load_u32(memory_, actor_ + 0x3E0u, services_, pc_); }
    // GameDataFunction::getPlayerLeft. Star bits as the counter on screen
    // reads them, MR::getStarPieceNum: the galaxy's ScenePlayingResult, the
    // stocked ones in the Observatory.
    std::int32_t lives() override { return static_cast<std::int32_t>(call(0x803B01F0u, 0u)); }
    std::int32_t star_bits() override { return static_cast<std::int32_t>(call(0x803CC4ECu, 0u)); }
    // MarioActor::incLife(u32): plays the game's own recovery sound.
    void heal(std::uint32_t amount) override { call(0x802B1CE4u, actor_, amount); }
    // MarioActor::decLife(0): one point, as a hit takes it.
    void damage() override { call(0x802B1BECu, actor_, 0u); }
    // As a 1-Up mushroom does it: MR::requestOneUp (the "1UP" board) and
    // MR::incPlayerLeft. A life taken is GameDataFunction::addPlayerLeft(-1),
    // not MR::decPlayerLeft, which also counts a miss in the save. The lives
    // counter on screen keeps its own number and only counts it up, so that
    // number is set too: MR::getGameSceneLayoutHolder() +0x34
    // CounterLayoutController, +0x30 PlayerLeft, +0x20 its count.
    void add_lives(std::int32_t delta) override {
        for (; delta > 0; --delta) {
            call(0x803F8170u, 0u);
            call(0x803CC3D0u, 0u);
        }
        if (delta >= 0) return;
        call(0x803B021Cu, static_cast<std::uint32_t>(delta));
        const std::uint32_t counters = layout_counters();
        if (!counters) return;
        const std::uint32_t player_left = guest_load_u32(memory_, counters + 0x30u, services_, pc_);
        if (!rmge01_guest_object(player_left)) return;
        guest_store_u32(memory_, player_left + 0x20u, static_cast<std::uint32_t>(lives()), services_, pc_);
    }
    // MR::addStarPiece, as picking one up does (same two places).
    void add_star_bits(std::int32_t delta) override { call(0x803CC52Cu, static_cast<std::uint32_t>(delta)); }
    // MR::getCoinNum (the scene's ScenePlayingResult).
    std::int32_t coins() override { return static_cast<std::int32_t>(call(0x803F670Cu, 0u)); }
    // Coins given: MR::incCoin, as picking them up (each heals one point,
    // every 50 a 1-up). Taken: ScenePlayingResult::incCoin straight (scene
    // object 0x24), since MR::incCoin would heal by the negative count; and
    // the counter on screen, which only counts up, shows the new number
    // (GameSceneLayoutHolder +0x34, CounterLayoutController +0x28
    // CoinCounter, +0x24 the number shown).
    void add_coins(std::int32_t delta) override {
        if (delta > 0) {
            call(0x803F669Cu, static_cast<std::uint32_t>(delta));
            return;
        }
        if (delta == 0 || (call(0x80344AB4u, 0x24u) & 0xFFu) == 0u) return;
        const std::uint32_t result = call(0x80344A54u, call(0x80344AA8u, 0u), 0x24u);
        if (!rmge01_guest_object(result)) return;
        call(0x80345858u, result, static_cast<std::uint32_t>(delta));
        const std::uint32_t counters = layout_counters();
        if (!counters) return;
        const std::uint32_t coin_counter = guest_load_u32(memory_, counters + 0x28u, services_, pc_);
        if (rmge01_guest_object(coin_counter))
            guest_store_u32(memory_, coin_counter + 0x24u, static_cast<std::uint32_t>(coins()), services_, pc_);
    }

    // MR::changePlayerItemStatus -> MarioAccess::changeItemStatus.
    void item_status(std::int32_t status) override { call(0x803F2868u, static_cast<std::uint32_t>(status)); }
    // MR::shakeCameraVeryStrong.
    void shake_camera() override { call(0x803F1384u, 0u); }
    // The models MarioActor::setPlayerMode uses for each item, which only
    // galaxies with that item load (garbage elsewhere, and the game
    // crashes): +0x9C8 the body's effect model (every power-up), the rainbow
    // hands +0xA50/+0xA54, the bee's +0x9E4/+0x9E8, the boo's +0x9A4.
    bool item_ready(std::int32_t status) override {
        const auto loaded = [this](std::initializer_list<std::uint32_t> fields) {
            for (const std::uint32_t field : fields)
                if (!rmge01_guest_object(guest_load_u32(memory_, actor_ + field, services_, pc_))) return false;
            return true;
        };
        switch (status) {
        case cloud90::kItemRainbowStar: return loaded({0x9C8u, 0xA50u, 0xA54u});
        case cloud90::kItemBee: return loaded({0x9C8u, 0x9E4u, 0x9E8u});
        case cloud90::kItemBoo: return loaded({0x9C8u, 0x9A4u});
        case cloud90::kItemSpring:
        case cloud90::kItemIce:
        case cloud90::kItemFire:
        case cloud90::kItemFlying: return loaded({0x9C8u});
        default: return false;
        }
    }
    // MarioActor::forceKill(3): the ordinary death (rumble, health 0, game over nerve).
    void kill() override { call(0x802B8834u, actor_, 3u); }
    // LiveActor::mScale (+0x24), x of the three.
    float scale() override { return read_f32(actor_ + 0x24u); }
    void set_scale(float scale) override {
        for (std::uint32_t axis = 0; axis < 3u; ++axis) write_f32(actor_ + 0x24u + axis * 4u, scale);
    }
    // MarioActor::mConst (+0x23C) -> MarioConst: mTable[2], mCurrentTable (+8).
    std::uint32_t const_table() override {
        const std::uint32_t mario_const = guest_load_u32(memory_, actor_ + 0x23Cu, services_, pc_);
        if (!rmge01_guest_object(mario_const)) return 0u;
        const std::uint32_t index = guest_load_u32(memory_, mario_const + 8u, services_, pc_);
        if (index > 1u) return 0u;
        const std::uint32_t table = guest_load_u32(memory_, mario_const + index * 4u, services_, pc_);
        return rmge01_guest_object(table) ? table : 0u;
    }
    float read_f32(std::uint32_t address) override {
        return std::bit_cast<float>(guest_load_u32(memory_, address, services_, pc_));
    }
    void write_f32(std::uint32_t address, float value) override {
        guest_store_u32(memory_, address, std::bit_cast<std::uint32_t>(value), services_, pc_);
    }
    // LiveActor::mPosition (+0x0C).
    cloud90::Vec3 position() override {
        return {read_f32(actor_ + 0x0Cu), read_f32(actor_ + 0x10u), read_f32(actor_ + 0x14u)};
    }
    // MR::isOnGroundPlayer, MR::isPlayerInRush (bound to an object: a launch
    // star's flight, a bubble, a shell ride…).
    bool on_ground() override { return (call(0x803F1E64u, 0u) & 0xFFu) != 0u; }
    bool in_rush() override { return (call(0x803F26D0u, 0u) & 0xFFu) != 0u; }
    // MR::setPlayerPosAndWait(const TVec3f&): the move, then standing still.
    void teleport(const cloud90::Vec3& position) override { call_with_vec(0x803F2528u, position); }
    // MR::forceJumpPlayer(const TVec3f& velocity), against
    // MarioActor::getGravityVec() (a unit vector).
    void launch(float speed) override {
        const std::uint32_t gravity = call(0x802B902Cu, actor_);
        if (!rmge01_guest_object(gravity)) return;
        cloud90::Vec3 velocity{};
        for (std::uint32_t axis = 0; axis < 3u; ++axis) velocity[axis] = -read_f32(gravity + axis * 4u) * speed;
        call_with_vec(0x803F240Cu, velocity);
    }
    // MarioActor::mPlayerMode, a u16 at +0x3D4.
    std::uint32_t power_mode() override { return guest_load_u32(memory_, actor_ + 0x3D4u, services_, pc_) >> 16; }
    // MR::getCurrentStageName (FNV-1a of up to 64 bytes) with
    // MR::getCurrentScenarioNo.
    std::uint32_t scene_id() override {
        std::uint32_t hash = 2166136261u ^ call(0x803F5A7Cu, 0u);
        const std::uint32_t name = call(0x803F5AB8u, 0u);
        if (!rmge01_guest_object(name & ~3u)) return hash;
        for (std::uint32_t i = 0; i < 64u; ++i) {
            const std::uint32_t byte = guest_load_u8(memory_, name + i, services_, pc_);
            if (byte == 0u) break;
            hash = (hash ^ byte) * 16777619u;
        }
        return hash;
    }

    bool enemy_ready(cloud90::EnemyKind kind) override { return waiting_enemy(kind) != 0u; }
    // The enemy, set down kEnemyAhead in front of Mario and kEnemyAbove
    // over him (it falls to the ground), facing nowhere in particular, back
    // to its first state, alive.
    void spawn_enemy(cloud90::EnemyKind kind) override {
        const std::uint32_t enemy = waiting_enemy(kind);
        const std::uint32_t gravity = call(0x802B902Cu, actor_);
        if (!enemy || !rmge01_guest_object(gravity)) return;
        cloud90::Vec3 front{};
        with_stack(16u, [&](std::uint32_t buffer) {
            call(0x803F2128u, buffer);  // MR::getPlayerFrontVec(TVec3f*)
            for (std::uint32_t axis = 0; axis < 3u; ++axis) front[axis] = load_f32(buffer + axis * 4u);
        });
        const cloud90::Vec3 at = position();
        cloud90::Vec3 spot{};
        for (std::uint32_t axis = 0; axis < 3u; ++axis)
            spot[axis] = at[axis] + front[axis] * kEnemyAhead - load_f32(gravity + axis * 4u) * kEnemyAbove;
        with_stack(16u, [&](std::uint32_t buffer) {
            for (std::uint32_t axis = 0; axis < 3u; ++axis) store_f32(buffer + axis * 4u, spot[axis]);
            call(0x803BED4Cu, enemy, buffer);  // MR::resetPosition(LiveActor*, const TVec3f&)
        });
        call_virtual(enemy, kVtAppear);
        if (const std::uint32_t nerve = kEnemies[static_cast<std::size_t>(kind)].nerve)
            call(0x80165824u, enemy, nerve);  // LiveActor::setNerve
    }

private:
    static constexpr float kEnemyAhead = 600.0f;
    static constexpr float kEnemyAbove = 150.0f;

    // A dead one of this galaxy's pool, or 0. MR::isDead(const LiveActor*).
    std::uint32_t waiting_enemy(cloud90::EnemyKind kind) {
        for (const std::uint32_t enemy : g_enemy_pool[static_cast<std::size_t>(kind)])
            if ((call(0x803DD748u, enemy) & 0xFFu) != 0u) return enemy;
        return 0u;
    }

    // Calls `function(const TVec3f&)`.
    void call_with_vec(std::uint32_t function, const cloud90::Vec3& vec) {
        with_stack(16u, [&](std::uint32_t buffer) {
            for (std::uint32_t axis = 0; axis < 3u; ++axis) store_f32(buffer + axis * 4u, vec[axis]);
            call(function, buffer);
        });
    }

    // MR::getGameSceneLayoutHolder() +0x34: the CounterLayoutController, or 0.
    std::uint32_t layout_counters() {
        const std::uint32_t holder = call(0x80361738u, 0u);
        if (!rmge01_guest_object(holder)) return 0u;
        const std::uint32_t counters = guest_load_u32(memory_, holder + 0x34u, services_, pc_);
        return rmge01_guest_object(counters) ? counters : 0u;
    }

    std::uint32_t actor_;
};

void run_cloud90_player(const NativeServicesV1* services, PpcContext* context, GuestMemoryV1* memory,
                        std::uint32_t pc) {
    const std::uint32_t actor = context->gpr[30];
    if (!rmge01_guest_object(actor) || services->call_guest == nullptr) return;
    GuestPlayer player(services, context, memory, actor, pc);
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    cloud90::on_player_frame(
        player, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()));
}

}  // namespace

void dispatch_site_hook(
    const NativeServicesV1* services,
    std::uint32_t guest_pc,
    std::uint32_t kind,
    std::uint32_t argument,
    PpcContext* context,
    GuestMemoryV1* memory) {
    switch (static_cast<SiteHookKind>(kind)) {
    case SiteHookKind::CinemaAndHud:
        apply_rmge01_cinema_vertical(context, memory, services);
        apply_rmge01_layout_fits(context, memory, services);
        return;
    case SiteHookKind::LayoutBackings:
        apply_rmge01_layout_backings(context, memory, services);
        return;
    case SiteHookKind::HomeBackings:
        apply_rmge01_home_backings(context, memory, services, guest_pc);
        return;
    case SiteHookKind::HomeVertical:
        apply_rmge01_home_vertical(context, memory, services, guest_pc);
        return;
    case SiteHookKind::NativeScreenWidthEnter:
        enter_experimental_rmge01_native_screen_width();
        return;
    case SiteHookKind::NativeScreenWidthExit:
        exit_experimental_rmge01_native_screen_width();
        return;
    case SiteHookKind::FourThreeAspectRead:
        apply_experimental_rmge01_native_four_three_aspect_read(context, memory, services);
        return;
    case SiteHookKind::LayoutConstant:
        apply_experimental_rmge01_layout_constant(
            context, services, argument & 0xFFu, ((argument >> 8u) & 0xFFu) != 0u);
        return;
    case SiteHookKind::Nw4rCanvasWidth:
        apply_experimental_rmge01_nw4r_canvas_width(context, services);
        return;
    case SiteHookKind::CameraAspect:
        apply_experimental_rmge01_camera_aspect(context, services);
        return;
    case SiteHookKind::UltrawideScreenWidth:
        apply_experimental_rmge01_ultrawide_screen_width(context, services);
        return;
    case SiteHookKind::EfbScreenWidth:
        apply_experimental_rmge01_efb_screen_width(context, services);
        return;
    case SiteHookKind::LayoutVerticalFit:
        apply_experimental_rmge01_layout_vertical_fit(
            context, services, argument & 0xFFu, ((argument >> 8u) & 0xFFu) != 0u);
        return;
    case SiteHookKind::HomeGeometry:
        apply_experimental_rmge01_home_geometry(
            context, services, argument & 0xFFu, (argument >> 8u) & 0xFFu);
        return;
    case SiteHookKind::MultiSceneModel:
        fit_rmge01_scenario_sky(context, memory, services, guest_pc);
        fit_rmge01_scenario_camera(memory, services, guest_pc);
        return;
    case SiteHookKind::TextStep: {
        // Text speed 2x/3x: the same step again while text is still appearing
        // (TalkTextFormer::isTextAppearedAll 0x802A368C, updateTalking 0x802A367C).
        const unsigned extra = std::clamp(get_runtime_settings().text_speed, 1u, 3u) - 1u;
        if (extra == 0u) return;
        const std::uint32_t former = argument == 0u
            ? guest_load_u32(memory, context->gpr[31] + 0x24u, services, guest_pc)
            : context->gpr[30] + 0x24u;
        for (unsigned i = 0; i < extra; ++i) {
            context->gpr[3] = former;
            services->call_guest(services->user, 0x802A368Cu, context, memory);
            if ((context->gpr[3] & 0xFFu) != 0u) return;
            context->gpr[3] = former;
            services->call_guest(services->user, 0x802A367Cu, context, memory);
        }
        return;
    }
    case SiteHookKind::Language:
        // The game, its HOME Menu and its message files all follow SCGetLanguage. The answer is
        // fixed at the first question of the session (game_language_for_boot).
        context->gpr[3] = static_cast<std::uint32_t>(game_language_for_boot());
        return;
    case SiteHookKind::Cloud90Scene:
        run_cloud90_scene(services, context, memory, guest_pc, argument);
        break;
    case SiteHookKind::Cloud90Player:
        run_cloud90_player(services, context, memory, guest_pc);
        return;
    case SiteHookKind::LodDistance:
        // LodCtrl::update keeps the game's distances: which model shows is
        // also game state (MR::isHiddenModel).
        return;
    }
    throw std::runtime_error("unknown site hook kind " + std::to_string(kind));
}

}  // namespace galaxy
