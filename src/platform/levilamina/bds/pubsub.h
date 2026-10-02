// Copyright (c) 2024, The Endstone Project. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SPARK_PLATFORM_LEVILAMINA_BDS_PUBSUB_H
#define SPARK_PLATFORM_LEVILAMINA_BDS_PUBSUB_H

#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "mc/deps/core/utility/pub_sub/ConnectPosition.h"
#include "mc/deps/core/utility/pub_sub/Connector.h"
#include "mc/deps/core/utility/pub_sub/Subscription.h"
#include "mc/deps/core/utility/pub_sub/SubscriptionContext.h"
#include "mc/platform/brstd/move_only_function.h"

class ChunkSource;
class Dimension;
class LevelChunk;

namespace spark::levilamina::bds::pubsub {

// Private ABI view for the imported connector methods.
template <typename Signature>
class ConnectorView {
public:
    // The virtual signature must exactly match the 26.51 BDS connector ABI.
    using FunctionType = ::brstd::move_only_function<Signature>;
    using ContextType = ::Bedrock::PubSub::ContextType;

    template <typename Fn>
    [[nodiscard]] ::Bedrock::PubSub::Subscription connect(
        Fn &&function, ::Bedrock::PubSub::ConnectPosition position = ::Bedrock::PubSub::ConnectPosition::AtBack)
    {
        FunctionType callback(std::forward<Fn>(function));
        return _connectInternal(std::move(callback), position, ContextType{}, std::nullopt);
    }

protected:
    ConnectorView() = default;
    virtual ~ConnectorView() = default;

private:
    virtual ::Bedrock::PubSub::Subscription _connectInternal(FunctionType &&function,
                                                             ::Bedrock::PubSub::ConnectPosition position,
                                                             ContextType &&context, std::optional<int> group) = 0;
};

template <typename Signature>
[[nodiscard]] inline ConnectorView<Signature> &borrowConnectorView(
    ::Bedrock::PubSub::Connector<Signature> &connector) noexcept
{
    return reinterpret_cast<ConnectorView<Signature> &>(connector);
}

template <typename Signature, typename Fn>
[[nodiscard]] inline ::Bedrock::PubSub::Subscription connect(
    ::Bedrock::PubSub::Connector<Signature> &connector, Fn &&function,
    ::Bedrock::PubSub::ConnectPosition position = ::Bedrock::PubSub::ConnectPosition::AtBack)
{
    return borrowConnectorView(connector).connect(std::forward<Fn>(function), position);
}

using ChunkLoadedSignature = void(::ChunkSource &, ::LevelChunk &, int);
using ChunkDiscardedSignature = void(::LevelChunk &);
using DimensionCreatedSignature = void(::Dimension &);

inline void moveSubscriptionBody(::Bedrock::PubSub::SubscriptionBase &destination,
                                 ::Bedrock::PubSub::SubscriptionBase &source) noexcept
{
    destination.mBody = std::move(source.mBody);
    source.mBody.reset();
}

static_assert(static_cast<int>(::Bedrock::PubSub::ConnectPosition::AtBack) == 0);
static_assert(static_cast<int>(::Bedrock::PubSub::ConnectPosition::AtFront) == 1);
static_assert(sizeof(::Bedrock::PubSub::SubscriptionBase) == 16);
static_assert(alignof(::Bedrock::PubSub::SubscriptionBase) == 8);
static_assert(sizeof(::Bedrock::PubSub::Subscription) == 16);
static_assert(alignof(::Bedrock::PubSub::Subscription) == 8);
static_assert(sizeof(ConnectorView<ChunkLoadedSignature>) == sizeof(void *));
static_assert(sizeof(ConnectorView<ChunkDiscardedSignature>) == sizeof(void *));
static_assert(sizeof(ConnectorView<DimensionCreatedSignature>) == sizeof(void *));

}  // namespace spark::levilamina::bds::pubsub

#endif  // SPARK_PLATFORM_LEVILAMINA_BDS_PUBSUB_H
