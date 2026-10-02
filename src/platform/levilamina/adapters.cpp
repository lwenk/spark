#include "platform/levilamina/adapters.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ll/api/Versions.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/ModManagerRegistry.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/utils/SystemUtils.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "platform/levilamina/bds/player_ping.h"
#include "platform/levilamina/callback_state.h"
#include "platform/levilamina/world_gauge_provider.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace spark::levilamina {

namespace {

static_assert(std::is_polymorphic_v<ll::mod::ModManager>);

std::optional<std::uintptr_t> validatedModuleBase(ll::sys_utils::HandleT handle) noexcept
{
#ifdef _WIN32
    if (handle == nullptr) {
        return std::nullopt;
    }

    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(handle, &info, sizeof(info)) != sizeof(info) || info.Type != MEM_IMAGE ||
        info.AllocationBase != handle) {
        return std::nullopt;
    }
    return reinterpret_cast<std::uintptr_t>(info.AllocationBase);
#else
    (void)handle;
    return std::nullopt;
#endif
}

std::string modulePath(ll::sys_utils::HandleT handle)
{
    const auto path = ll::sys_utils::getModulePath(handle);
    return path.has_value() ? path->string() : std::string{};
}

std::shared_ptr<ll::mod::ModManager> nativeModManager()
{
    return ll::mod::ModManagerRegistry::getInstance().getManager(ll::mod::NativeModManagerName);
}

}  // namespace

bool StartupClock::initializeProcessStart()
{
#ifdef _WIN32
    FILETIME creation_time{};
    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    FILETIME utc_now{};
    if (::GetProcessTimes(::GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time) == FALSE) {
        return false;
    }
    ::GetSystemTimeAsFileTime(&utc_now);

    const auto ticks = [](FILETIME value) noexcept {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
               static_cast<std::uint64_t>(value.dwLowDateTime);
    };
    const auto creation_ticks = ticks(creation_time);
    const auto now_ticks = ticks(utc_now);
    if (now_ticks < creation_ticks) {
        return false;
    }
    const auto elapsed_ticks = now_ticks - creation_ticks;
    constexpr auto max_nanoseconds = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    if (elapsed_ticks > max_nanoseconds / 100U) {
        return false;
    }

    const auto elapsed = std::chrono::duration_cast<Clock::duration>(
        std::chrono::nanoseconds{static_cast<std::int64_t>(elapsed_ticks * 100U)});
    const auto now = Clock::now();
    if (now.time_since_epoch() < elapsed) {
        return false;
    }

    std::lock_guard lock(mutex_);
    if (start_time_.has_value()) {
        return true;
    }
    start_time_ = now - elapsed;
    return true;
#else
    return false;
#endif
}

bool StartupClock::recordStart()
{
    return initializeProcessStart();
}

std::optional<StartupClock::TimePoint> StartupClock::startTime() const
{
    std::lock_guard lock(mutex_);
    return start_time_;
}

LeviLaminaDispatcher::LeviLaminaDispatcher(std::shared_ptr<CallbackState> callback_state)
    : callback_state_(std::move(callback_state))
{
    if (!callback_state_) {
        throw std::invalid_argument{"LeviLamina dispatcher requires callback state"};
    }
}

void LeviLaminaDispatcher::runOnMainThread(std::function<void()> task)
{
    static_cast<void>(callback_state_->post(std::move(task)));
}

LeviLaminaMetadataProvider::LeviLaminaMetadataProvider(std::shared_ptr<const StartupClock> startup_clock,
                                                       std::shared_ptr<CallbackState> callback_state)
    : startup_clock_(std::move(startup_clock)), callback_state_(std::move(callback_state))
{
    if (!startup_clock_ || !startup_clock_->startTime().has_value()) {
        throw std::invalid_argument{"LeviLamina metadata provider requires a captured server start time"};
    }
    if (!callback_state_) {
        throw std::invalid_argument{"LeviLamina metadata provider requires callback state"};
    }
}

LeviLaminaMetadataProvider::~LeviLaminaMetadataProvider() = default;

PlatformIdentity LeviLaminaMetadataProvider::platformIdentity() const
{
    return {.platform_name = "LeviLamina", .platform_brand = "LeviLamina"};
}

void LeviLaminaMetadataProvider::gatherServerMetadata(ServerMetadata &metadata, std::int64_t /*now_ms*/)
{
    metadata.endstone_version = ll::getLoaderVersion().to_string();
    metadata.minecraft_version = ll::getGameVersion().to_string();
    metadata.player_count = playerCount();
    metadata.online_mode = 0;
    metadata.uptime_ms = uptimeMilliseconds();
    metadata.plugins.clear();
    if (const auto manager = nativeModManager()) {
        for (ll::mod::Mod &mod : manager->mods()) {
            const auto manifest = mod.getManifest();
            metadata.plugins.push_back(
                {.name = manifest.name,
                 .version = manifest.version.has_value() ? manifest.version->to_string() : std::string{},
                 .author = manifest.author.value_or(std::string{}),
                 .description = manifest.description.value_or(std::string{})});
        }
    }
    metadata.server_configurations.clear();
    metadata.platform_name = "LeviLamina";
    metadata.platform_brand = "LeviLamina";
}

void LeviLaminaMetadataProvider::gatherWorldMetadata(WorldInfo &world, std::string_view level_name_hint)
{
    if (auto *provider = ensureWorldGauges(); provider != nullptr) {
        provider->gatherWorldMetadata(world, level_name_hint);
        return;
    }
    world = WorldInfo{};
}

std::vector<NativePluginSource> LeviLaminaMetadataProvider::nativePluginSources()
{
    const auto manager = nativeModManager();
    if (!manager) {
        return {};
    }

    std::vector<NativePluginSource> sources;
    for (ll::mod::Mod &mod : manager->mods()) {
        auto &native_mod = static_cast<ll::mod::NativeMod &>(mod);
        const auto handle = native_mod.getHandle();
        const auto module_base = validatedModuleBase(handle);
        if (!module_base.has_value()) {
            continue;
        }

        const auto manifest = native_mod.getManifest();
        sources.push_back({.module_base = *module_base, .module_path = modulePath(handle), .source_id = manifest.name});
    }
    return sources;
}

std::int64_t LeviLaminaMetadataProvider::serverUptimeSeconds()
{
    return uptimeMilliseconds() / 1000;
}

std::int64_t LeviLaminaMetadataProvider::playerCount()
{
    const auto level = ll::service::getLevel();
    if (!level) {
        return -1;
    }
    return static_cast<std::int64_t>(level->getActivePlayerCount());
}

bool LeviLaminaMetadataProvider::worldGaugesAvailable()
{
    return ensureWorldGauges() != nullptr && world_gauges_->available();
}

WorldGaugeValues LeviLaminaMetadataProvider::worldGauges()
{
    if (auto *provider = ensureWorldGauges(); provider != nullptr && provider->available()) {
        return provider->worldGauges();
    }
    return {};
}

bool LeviLaminaMetadataProvider::closeWorldGauges(std::chrono::steady_clock::time_point deadline) noexcept
{
    if (!world_gauges_) {
        return true;
    }
    if (!world_gauges_->close(deadline)) {
        return false;
    }
    world_gauges_.reset();
    return true;
}

PlayerPingProvider *LeviLaminaMetadataProvider::playerPingProvider()
{
    if (!ping_provider_) {
        ping_provider_ = std::make_unique<LeviLaminaPlayerPingProvider>();
    }
    return ping_provider_.get();
}

LeviLaminaWorldGaugeProvider *LeviLaminaMetadataProvider::ensureWorldGauges()
{
    const auto level = ll::service::getLevel();
    if (!level) {
        return nullptr;
    }
    if (!world_gauges_) {
        world_gauges_ = std::make_unique<LeviLaminaWorldGaugeProvider>(callback_state_);
    }
    if (!world_gauges_->initialized() && !world_gauges_->initialize(*level)) {
        return nullptr;
    }
    return world_gauges_.get();
}

std::map<std::string, int> LeviLaminaPlayerPingProvider::poll()
{
    std::map<std::string, int> result;
    const auto level = ll::service::getLevel();
    if (!level) {
        return result;
    }

    level->forEachPlayer([&result](::Player &player) {
        const auto name = player.getRealName();
        if (name.empty()) {
            return true;
        }

        const auto ping = bds::readPlayerAveragePingMilliseconds(player);
        if (!ping.has_value() || *ping < 0 || *ping > std::numeric_limits<int>::max()) {
            return true;
        }
        result.emplace(name, static_cast<int>(*ping));
        return true;
    });
    return result;
}

std::int64_t LeviLaminaMetadataProvider::uptimeMilliseconds() const
{
    const auto start = startup_clock_->startTime();
    if (!start.has_value()) {
        throw std::logic_error{"LeviLamina server start time was not captured"};
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(StartupClock::Clock::now() - *start);
    return elapsed.count();
}

LeviLaminaNotifier::LeviLaminaNotifier(std::shared_ptr<CallbackState> callback_state,
                                       std::weak_ptr<ll::io::Logger> logger)
    : callback_state_(std::move(callback_state)), logger_(std::move(logger))
{
    if (!callback_state_) {
        throw std::invalid_argument{"LeviLamina notifier requires callback state"};
    }
}

void LeviLaminaNotifier::notify(const std::string &sender_name, const std::string &text)
{
    const auto weak_self = weak_from_this();
    static_cast<void>(callback_state_->post([weak_self, sender_name, text] {
        if (const auto self = weak_self.lock()) {
            self->notifyOnMainThread(sender_name, text);
        }
    }));
}

void LeviLaminaNotifier::notifyOnMainThread(const std::string &sender_name, const std::string &text)
{
    if (const auto logger = logger_.lock()) {
        logger->info("{}", text);
    }

    const auto level = ll::service::getLevel();
    if (!level) {
        return;
    }
    level->forEachPlayer([&](::Player &player) {
        if (player.getRealName() == sender_name) {
            player.sendMessage(text);
            return false;
        }
        return true;
    });
}

BorrowedCommandSender::BorrowedCommandSender(::CommandOrigin const &origin, ::CommandOutput &output)
    : origin_(origin), output_(output)
{
}

std::string BorrowedCommandSender::getName() const
{
    if (const auto *player = resolvePlayer()) {
        return player->getRealName();
    }
    return origin_.getName();
}

bool BorrowedCommandSender::isPlayer() const
{
    return resolvePlayer() != nullptr;
}

std::string BorrowedCommandSender::getUniqueId() const
{
    if (const auto *player = resolvePlayer()) {
        return player->getUuid().asString();
    }
    return {};
}

bool BorrowedCommandSender::hasPermission(const std::string & /*name*/) const
{
    return static_cast<int>(origin_.getPermissionsLevel()) >= static_cast<int>(::CommandPermissionLevel::GameDirectors);
}

void BorrowedCommandSender::sendImpl(const std::string &message)
{
    output_.success(message);
}

void BorrowedCommandSender::errorImpl(const std::string &message)
{
    output_.error(message);
}

::Player const *BorrowedCommandSender::resolvePlayer() const
{
    auto *level = origin_.getLevel();
    auto *entity = origin_.getEntity();
    if (level == nullptr || entity == nullptr) {
        return nullptr;
    }

    ::Player const *result = nullptr;
    level->forEachPlayer([&](::Player &player) {
        if (static_cast<::Actor *>(&player) == entity) {
            result = &player;
            return false;
        }
        return true;
    });
    return result;
}

}  // namespace spark::levilamina
