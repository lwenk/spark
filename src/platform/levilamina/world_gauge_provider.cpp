#include "platform/levilamina/world_gauge_provider.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

#include "mc/world/level/DimensionManager.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/chunk/ChunkState.h"
#include "mc/world/level/chunk/ILevelChunkEventManagerProxy.h"
#include "mc/world/level/chunk/LevelChunkEventManager.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "platform/levilamina/callback_state.h"

namespace spark::levilamina {
namespace {

constexpr std::int64_t KReconcileIntervalMs = 30000;

void dimensionCallbackBody(LeviLaminaWorldGaugeProvider &provider, ::Dimension &dimension)
{
    provider.onDimensionCreated(dimension);
}

void chunkLoadedCallbackBody(LeviLaminaWorldGaugeProvider &provider, ::ChunkSource &source, ::LevelChunk &chunk,
                             int distance)
{
    provider.onChunkLoaded(source, chunk, distance);
}

void chunkDiscardedCallbackBody(LeviLaminaWorldGaugeProvider &provider, ::LevelChunk &chunk)
{
    provider.onChunkDiscarded(chunk);
}

std::int64_t steadyNowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

LeviLaminaWorldGaugeProvider::LeviLaminaWorldGaugeProvider(std::shared_ptr<CallbackState> callback_state)
    : callback_state_(std::move(callback_state))
{
    if (!callback_state_) {
        throw std::invalid_argument{"LeviLamina world gauge provider requires callback state"};
    }
    control_ = std::make_shared<WorldCallbackControl>(this, callback_state_);
}

LeviLaminaWorldGaugeProvider::~LeviLaminaWorldGaugeProvider()
{
    if (!closed_ && !close(std::chrono::steady_clock::now())) {
        std::terminate();
    }
}

bool LeviLaminaWorldGaugeProvider::initialize(::Level &level)
{
    if (closed_ || initialized_) {
        return initialized_ && access_ != nullptr && access_->level() == &level;
    }

    try {
        access_ = std::make_unique<bds::WorldAccess>(level);
        auto &dimension_connector = level.getDimensionManager().getOnNewDimensionCreatedConnector();
        dimension_subscription_.assign(bds::pubsub::connect<bds::pubsub::DimensionCreatedSignature>(
            dimension_connector, WorldCallbackFunction<::Dimension &>{control_, &dimensionCallbackBody}));

        auto event_manager = level.getLevelChunkEventManager();
        auto &loaded_connector = event_manager->getOnChunkLoadedConnector();
        chunk_loaded_subscription_.assign(bds::pubsub::connect<bds::pubsub::ChunkLoadedSignature>(
            loaded_connector,
            WorldCallbackFunction<::ChunkSource &, ::LevelChunk &, int>{control_, &chunkLoadedCallbackBody}));
        auto &discarded_connector = event_manager->getOnChunkDiscardedConnector();
        chunk_discarded_subscription_.assign(bds::pubsub::connect<bds::pubsub::ChunkDiscardedSignature>(
            discarded_connector, WorldCallbackFunction<::LevelChunk &>{control_, &chunkDiscardedCallbackBody}));

        initialized_ = true;
        closed_ = false;
        if (!reconcile({})) {
            static_cast<void>(close(std::chrono::steady_clock::now()));
            return false;
        }
        return true;
    }
    catch (...) {
        initialized_ = true;
        static_cast<void>(close(std::chrono::steady_clock::now()));
        return false;
    }
}

bool LeviLaminaWorldGaugeProvider::close(std::chrono::steady_clock::time_point deadline) noexcept
{
    if (closed_) {
        return true;
    }
    if (control_ == nullptr || control_->isActiveOnCurrentThread()) {
        return false;
    }

    control_->closeAdmission();
    bool disconnected = true;
    const auto disconnect = [&disconnected](ImportedSubscriptionSlot &subscription) noexcept {
        try {
            subscription.disconnect();
        }
        catch (...) {
            disconnected = false;
        }
    };
    disconnect(dimension_subscription_);
    disconnect(chunk_loaded_subscription_);
    disconnect(chunk_discarded_subscription_);

    if (!disconnected || !control_->waitQuiescent(deadline)) {
        return false;
    }

    control_->detachProvider();
    access_.reset();
    initialized_ = false;
    available_ = false;
    closed_ = true;
    return true;
}

WorldGaugeValues LeviLaminaWorldGaugeProvider::worldGauges()
{
    if (!available_) {
        return {};
    }
    const auto now = steadyNowMs();
    if (now - last_reconcile_steady_ms_ >= KReconcileIntervalMs && !reconcile({})) {
        return {};
    }

    const auto counts = event_adapter_.counts();
    return {.entities = counts.entities,
            .tile_entities = counts.tile_entities,
            .chunks = counts.chunks,
            .tile_entities_present = counts.tile_entities_present};
}

void LeviLaminaWorldGaugeProvider::gatherWorldMetadata(WorldInfo &world, std::string_view level_name_hint)
{
    world = {};
    if (!initialized_ || access_ == nullptr) {
        markUnavailable();
        return;
    }

    bds::WorldAccess::ScanResult result;
    if (!access_->scan(level_name_hint, result)) {
        markUnavailable();
        return;
    }
    event_adapter_.reconcile(result.gauges);
    world = std::move(result.world);
    available_ = true;
    last_reconcile_steady_ms_ = steadyNowMs();
}

bool LeviLaminaWorldGaugeProvider::reconcile(std::string_view level_name_hint)
{
    if (!initialized_ || access_ == nullptr) {
        markUnavailable();
        return false;
    }

    bds::WorldAccess::ScanResult result;
    if (!access_->scan(level_name_hint, result)) {
        markUnavailable();
        return false;
    }
    event_adapter_.reconcile(result.gauges);
    available_ = true;
    last_reconcile_steady_ms_ = steadyNowMs();
    return true;
}

void LeviLaminaWorldGaugeProvider::onDimensionCreated(::Dimension &dimension) noexcept
{
    try {
        if (access_ != nullptr) {
            if (bds::detail::dimensionCaptureInvalidates(access_->retainDimension(dimension))) {
                markUnavailable();
            }
        }
    }
    catch (...) {
        markUnavailable();
    }
}

void LeviLaminaWorldGaugeProvider::onChunkLoaded(::ChunkSource &, ::LevelChunk &chunk, int) noexcept
{
    try {
        if (!bds::detail::isLoadedChunkState(chunk.mLoadState.get().load(std::memory_order_acquire))) {
            return;
        }
        if (access_ == nullptr) {
            return;
        }
        bds::WorldGaugeChunkKey key;
        const auto result = access_->chunkKeyStatus(chunk, key);
        if (result == bds::ChunkKeyResult::Valid) {
            event_adapter_.chunkLoaded(std::move(key));
        }
        else if (result == bds::ChunkKeyResult::Failed) {
            markUnavailable();
        }
    }
    catch (...) {
        markUnavailable();
    }
}

void LeviLaminaWorldGaugeProvider::onChunkDiscarded(::LevelChunk &chunk) noexcept
{
    try {
        if (access_ == nullptr) {
            return;
        }
        bds::WorldGaugeChunkKey key;
        const auto result = access_->chunkKeyStatus(chunk, key);
        if (result == bds::ChunkKeyResult::Valid) {
            event_adapter_.chunkUnloaded(key);
        }
        else if (result == bds::ChunkKeyResult::Failed) {
            markUnavailable();
        }
    }
    catch (...) {
        markUnavailable();
    }
}

void LeviLaminaWorldGaugeProvider::markUnavailable() noexcept
{
    available_ = false;
}

}  // namespace spark::levilamina
