#include "platform/levilamina/bds/world_access.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <set>
#include <utility>
#include <vector>

#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/DimensionManager.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/ChunkState.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/dimension/VanillaDimensions.h"

#include "core/metadata/behavior_packs.h"
#include "core/util/world_region.h"

namespace spark::levilamina::bds {
namespace {

struct ActorObservation {
    std::int64_t id = 0;
    int dimension_id = 0;
    int chunk_x = 0;
    int chunk_z = 0;
    std::string type;
};

}  // namespace

std::string canonicalDimensionName(int dimension_id)
{
    if (dimension_id == static_cast<int>(::VanillaDimensions::Overworld())) {
        return "overworld";
    }
    if (dimension_id == static_cast<int>(::VanillaDimensions::Nether())) {
        return "nether";
    }
    if (dimension_id == static_cast<int>(::VanillaDimensions::TheEnd())) {
        return "the_end";
    }
    return "dimension_" + std::to_string(dimension_id);
}

DimensionRetention WorldAccess::retainDimension(::Dimension &dimension) noexcept
{
    if (level_ == nullptr) {
        return DimensionRetention::Failed;
    }
    try {
        if (&dimension.mLevel != static_cast<::ILevel *>(level_)) {
            return DimensionRetention::ForeignLevel;
        }
        const int dimension_id = static_cast<int>(dimension.getDimensionId());
        dimensions_[dimension_id] = dimension.weak_from_this();
        return DimensionRetention::Retained;
    }
    catch (...) {
        return DimensionRetention::Failed;
    }
}

bool WorldAccess::retainKnownDimensions() noexcept
{
    if (level_ == nullptr) {
        return false;
    }

    try {
        // Match Endstone's startup behavior so an unloaded vanilla dimension is
        // represented by a stable ID before the first snapshot.
        static_cast<void>(level_->getOrCreateDimension(::VanillaDimensions::Overworld()));
        static_cast<void>(level_->getOrCreateDimension(::VanillaDimensions::Nether()));
        static_cast<void>(level_->getOrCreateDimension(::VanillaDimensions::TheEnd()));

        bool found_dimension = false;
        bool failed = false;
        level_->forEachDimension([this, &found_dimension, &failed](::Dimension &dimension) {
            const auto result = retainDimension(dimension);
            if (result == DimensionRetention::Retained) {
                found_dimension = true;
            }
            else if (detail::dimensionCaptureInvalidates(result)) {
                failed = true;
            }
            return true;
        });
        if (!pruneExpiredDimensions()) {
            failed = true;
        }
        return !failed && found_dimension && !dimensions_.empty();
    }
    catch (...) {
        return false;
    }
}

bool WorldAccess::pruneExpiredDimensions() noexcept
{
    bool success = true;
    for (auto iterator = dimensions_.begin(); iterator != dimensions_.end();) {
        try {
            auto dimension = iterator->second.lock();
            if (!dimension || &dimension->mLevel != static_cast<::ILevel *>(level_)) {
                iterator = dimensions_.erase(iterator);
            }
            else {
                ++iterator;
            }
        }
        catch (...) {
            success = false;
            iterator = dimensions_.erase(iterator);
        }
    }
    return success;
}

ChunkKeyResult WorldAccess::chunkKeyStatus(::LevelChunk const &chunk, WorldGaugeChunkKey &key) const noexcept
{
    if (level_ == nullptr) {
        return ChunkKeyResult::Failed;
    }
    try {
        auto &dimension = chunk.mDimension;
        if (&dimension.mLevel != static_cast<::ILevel *>(level_)) {
            return ChunkKeyResult::ForeignLevel;
        }
        const auto &position = chunk.mPosition.get();
        key = {.dimension = canonicalDimensionName(static_cast<int>(dimension.getDimensionId())),
               .x = position.x,
               .z = position.z};
        return ChunkKeyResult::Valid;
    }
    catch (...) {
        return ChunkKeyResult::Failed;
    }
}

bool WorldAccess::scan(std::string_view level_name_hint, ScanResult &result) noexcept
{
    result = {};
    if (level_ == nullptr || !retainKnownDimensions()) {
        return false;
    }

    try {
        std::vector<ActorObservation> actors;
        std::set<std::int64_t> actor_ids;
        const auto &entities = level_->getEntities();
        actors.reserve(entities.size());
        for (const auto &entity : entities) {
            if (!entity) {
                continue;
            }
            auto *actor = ::Actor::tryGetFromEntity(const_cast<::EntityContext &>(*entity), false);
            if (actor == nullptr || &actor->getLevel() != level_ || !actor->hasUniqueID()) {
                continue;
            }

            const auto actor_id = actor->getOrCreateUniqueID().rawID;
            if (!actor_ids.insert(actor_id).second) {
                continue;
            }
            result.gauges.actor_ids.push_back(actor_id);
            const auto &position = actor->getPosition();
            int chunk_x = 0;
            int chunk_z = 0;
            if (!detail::floorChunkCoordinate(position.x, chunk_x) ||
                !detail::floorChunkCoordinate(position.z, chunk_z)) {
                continue;
            }
            actors.push_back({.id = actor_id,
                              .dimension_id = static_cast<int>(actor->getDimension().getDimensionId()),
                              .chunk_x = chunk_x,
                              .chunk_z = chunk_z,
                              .type = [actor] {
                                  const auto &identifier = actor->getActorIdentifier();
                                  const auto &canonical_name = identifier.mCanonicalName.get().getString();
                                  return canonical_name.empty()
                                           ? "actor_" + std::to_string(static_cast<int>(actor->getEntityTypeId()))
                                           : canonical_name;
                              }()});
        }

        level_->forEachPlayer([&result](::Player &player) {
            if (player.hasUniqueID()) {
                result.gauges.player_ids.push_back(player.getOrCreateUniqueID().rawID);
            }
            return true;
        });

        bool found_live_dimension = false;
        for (const auto &[dimension_id, weak_dimension] : dimensions_) {
            auto dimension = weak_dimension.lock();
            if (!dimension || &dimension->mLevel != static_cast<::ILevel *>(level_)) {
                continue;
            }
            found_live_dimension = true;
            const std::string dimension_name = canonicalDimensionName(dimension_id);
            std::map<std::pair<int, int>, WorldChunk> chunks;
            const auto &storage = dimension->getChunkSource().getStorage();
            for (const auto &[position, weak_chunk] : storage) {
                auto chunk = weak_chunk.lock();
                if (!chunk || !detail::isLoadedChunkState(chunk->mLoadState.get().load(std::memory_order_acquire))) {
                    continue;
                }
                const auto &actual_position = chunk->mPosition.get();
                chunks.try_emplace(std::pair{actual_position.x, actual_position.z},
                                   WorldChunk{.x = actual_position.x, .z = actual_position.z});
                result.gauges.chunks.push_back(
                    {.dimension = dimension_name, .x = actual_position.x, .z = actual_position.z});
                (void)position;
            }

            for (const auto &actor : actors) {
                if (actor.dimension_id != dimension_id) {
                    continue;
                }
                auto chunk = chunks.find({actor.chunk_x, actor.chunk_z});
                if (chunk == chunks.end()) {
                    continue;
                }
                ++chunk->second.total_entities;
                ++chunk->second.entity_counts[actor.type];
            }

            if (chunks.empty()) {
                continue;
            }
            WorldEntry entry;
            entry.name = dimension_name;
            for (const auto &region : groupChunksIntoRegions(chunks)) {
                entry.total_entities += region.total_entities;
                for (const auto &chunk : region.chunks) {
                    for (const auto &[type, count] : chunk.entity_counts) {
                        result.world.entity_counts[type] += count;
                    }
                }
                entry.regions.push_back(region);
            }
            result.world.total_entities += entry.total_entities;
            result.world.worlds.push_back(std::move(entry));
        }

        if (!pruneExpiredDimensions()) {
            return false;
        }
        if (!found_live_dimension) {
            return false;
        }

        const std::string world_name = level_name_hint.empty() ? level_->getLevelId() : std::string(level_name_hint);
        result.world.data_packs = discoverActiveBehaviorPacks(std::filesystem::current_path(), world_name);
        result.world.present = !result.world.worlds.empty() || !result.world.data_packs.empty();
        result.gauges.tile_entities_complete = false;
        return true;
    }
    catch (...) {
        result = {};
        return false;
    }
}

}  // namespace spark::levilamina::bds
