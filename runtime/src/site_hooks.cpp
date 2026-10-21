#include "galaxy/site_hooks.h"

#include "galaxy/cloud90_game.h"
#include "galaxy/experimental_ultrawide_aspect.h"
#include "galaxy/layout_aspect.h"
#include "galaxy/layout_aspect_cinema.h"
#include "galaxy/layout_aspect_fits.h"
#include "galaxy/layout_aspect_home.h"
#include "galaxy/runtime_settings.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <stdexcept>
#include <string>

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

// Cloud90 Game Link phase 5: Mario's state through the game's own functions
// (addresses from the CC0 Petari RMGE01 symbols), called from the
// Cloud90Player hook on the game thread. Every call leaves the registers as it
// found them.
class GuestPlayer final : public cloud90::PlayerAccess {
public:
    GuestPlayer(const NativeServicesV1* services, PpcContext* context, GuestMemoryV1* memory,
                std::uint32_t actor, std::uint32_t pc) noexcept
        : services_(services), context_(context), memory_(memory), actor_(actor), pc_(pc) {}

    // MarioActor::mHealth / mMaxHealth.
    std::uint32_t health() override { return guest_load_u32(memory_, actor_ + 0x380u, services_, pc_); }
    std::uint32_t max_health() override { return guest_load_u32(memory_, actor_ + 0x3E0u, services_, pc_); }
    // GameDataFunction::getPlayerLeft / getStarPieceNum.
    std::int32_t lives() override { return static_cast<std::int32_t>(call(0x803B01F0u, 0u)); }
    std::int32_t star_bits() override { return static_cast<std::int32_t>(call(0x803B02DCu, 0u)); }
    // MarioActor::incLife(u32): plays the game's own recovery sound.
    void heal(std::uint32_t amount) override { call(0x802B1CE4u, actor_, amount); }
    // MarioActor::decLife(0): one point, as a hit takes it.
    void damage() override { call(0x802B1BECu, actor_, 0u); }
    // As a 1-Up mushroom does it: MR::requestOneUp (the "1UP" board) and
    // MR::incPlayerLeft. A life taken is GameDataFunction::addPlayerLeft(-1),
    // not MR::decPlayerLeft, which also counts a miss in the save. The lives
    // counter on screen only counts up, so a taken life shows on the next
    // scene.
    void add_lives(std::int32_t delta) override {
        for (; delta > 0; --delta) {
            call(0x803F8170u, 0u);
            call(0x803CC3D0u, 0u);
        }
        if (delta < 0) call(0x803B021Cu, static_cast<std::uint32_t>(delta));
    }
    // GameDataFunction::addStarPiece.
    void add_star_bits(std::int32_t delta) override { call(0x803B02ECu, static_cast<std::uint32_t>(delta)); }

private:
    std::uint32_t call(std::uint32_t function, std::uint32_t r3, std::uint32_t r4 = 0u) {
        std::uint32_t gpr[32];
        std::copy(std::begin(context_->gpr), std::end(context_->gpr), gpr);
        const std::uint32_t cr = context_->cr, lr = context_->lr, ctr = context_->ctr, xer = context_->xer;
        context_->gpr[3] = r3;
        context_->gpr[4] = r4;
        services_->call_guest(services_->user, function, context_, memory_);
        const std::uint32_t result = context_->gpr[3];
        std::copy(std::begin(gpr), std::end(gpr), context_->gpr);
        context_->cr = cr;
        context_->lr = lr;
        context_->ctr = ctr;
        context_->xer = xer;
        return result;
    }

    const NativeServicesV1* services_;
    PpcContext* context_;
    GuestMemoryV1* memory_;
    std::uint32_t actor_;
    std::uint32_t pc_;
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
