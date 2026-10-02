#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "platform/levilamina/bds/pubsub.h"

namespace {

using spark::levilamina::bds::pubsub::ChunkLoadedSignature;
using spark::levilamina::bds::pubsub::ConnectorView;

class MockConnector final : public ConnectorView<ChunkLoadedSignature> {
public:
private:
    ::Bedrock::PubSub::Subscription _connectInternal(FunctionType &&, ::Bedrock::PubSub::ConnectPosition,
                                                     ContextType &&, std::optional<int>) override
    {
        throw std::runtime_error{"the ABI mock must not invoke the imported subscription destructor"};
    }
};

static_assert(std::is_polymorphic_v<MockConnector>);

}  // namespace

int main()
{
    static_assert(sizeof(ConnectorView<ChunkLoadedSignature>) == sizeof(void *));
    static_assert(sizeof(::Bedrock::PubSub::SubscriptionBase) == 16);
    static_assert(sizeof(::Bedrock::PubSub::Subscription) == 16);
    static_assert(alignof(::Bedrock::PubSub::SubscriptionBase) == 8);
    static_assert(alignof(::Bedrock::PubSub::Subscription) == 8);
    static_assert(static_cast<int>(::Bedrock::PubSub::ConnectPosition::AtBack) == 0);
    static_assert(static_cast<int>(::Bedrock::PubSub::ConnectPosition::AtFront) == 1);
    static_assert(!std::is_copy_constructible_v<ConnectorView<ChunkLoadedSignature>::FunctionType>);
    static_assert(std::is_move_constructible_v<ConnectorView<ChunkLoadedSignature>::FunctionType>);
    static_cast<void>(sizeof(MockConnector));
    return 0;
}
