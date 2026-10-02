#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "application/command/command_sender.h"
#include "ll/api/Global.h"
#include "ll/api/Versions.h"
#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/command/OverloadData.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/Listener.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/service/GamingStatus.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "mc/server/commands/Command.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/CommandRawText.h"
#include "platform/levilamina/adapters.h"
#include "platform/levilamina/application_bridge.h"
#include "platform/levilamina/bds/tick_duration.h"
#include "platform/levilamina/callback_state.h"
#include "platform/levilamina/cleanup_deadline_guard.h"
#include "platform/levilamina/command_lifecycle.h"
#include "platform/levilamina/host_command_parameter.h"
#include "core/spark_constants.h"

struct SparkRawParameters {
    ::CommandRawText raw;
};

namespace {

using TickEvent = ll::event::ServerLevelTickEvent;

template <typename Deleter>
class ProbeMemory final {
public:
    ProbeMemory(void *pointer, Deleter deleter) : pointer_(pointer), deleter_(deleter) {}
    ProbeMemory(ProbeMemory const &) = delete;
    ProbeMemory &operator=(ProbeMemory const &) = delete;
    ~ProbeMemory()
    {
        if (pointer_ != nullptr) {
            deleter_(pointer_);
        }
    }

    [[nodiscard]] void *get() const noexcept { return pointer_; }

private:
    void *pointer_;
    Deleter deleter_;
};

struct AlignedProbeDeleter {
    using DeleteFn = void (*)(void *, std::align_val_t) noexcept;

    DeleteFn function;
    std::align_val_t alignment;

    void operator()(void *pointer) const noexcept { function(pointer, alignment); }
};

struct HostSession final {
    std::unique_ptr<spark::levilamina::LeviLaminaDispatcher> dispatcher;
    std::unique_ptr<spark::levilamina::LeviLaminaMetadataProvider> metadata;
    std::shared_ptr<spark::levilamina::LeviLaminaNotifier> notifier;
    std::unique_ptr<spark::levilamina::ApplicationBridge> bridge;
    std::atomic_bool tick_started{false};
};

using CommandLifetimeGuard = spark::levilamina::CommandLifetimeGuard;
using PublicationBoundary = spark::levilamina::PublicationBoundary;

struct CommandContext final {
    std::shared_ptr<spark::levilamina::CallbackState> callback_state;
    std::weak_ptr<HostSession> session;
    std::shared_ptr<CommandLifetimeGuard> lifetime;

    void dispatch(::CommandOrigin const &origin, ::CommandOutput &output, std::string_view raw_text) const
    {
        bool handled = false;
        const bool admitted = callback_state->invokeInline([&] {
            auto current_session = session.lock();
            if (!current_session || !current_session->bridge) {
                return;
            }
            spark::levilamina::BorrowedCommandSender sender(origin, output);
            handled = current_session->bridge->dispatch(sender, raw_text);
        });
        if (!admitted) {
            output.error("Spark is disabled");
        }
        else if (!handled) {
            output.error("Spark command was not handled");
        }
    }
};

class SparkNoArgumentCommand final : public ::Command {
public:
    SparkNoArgumentCommand(CommandLifetimeGuard::Lease lease, std::shared_ptr<CommandContext> context)
        : lease_(std::move(lease)), context_(std::move(context))
    {
    }

    SparkNoArgumentCommand(SparkNoArgumentCommand const &) = delete;
    SparkNoArgumentCommand &operator=(SparkNoArgumentCommand const &) = delete;
    SparkNoArgumentCommand(SparkNoArgumentCommand &&) = delete;
    SparkNoArgumentCommand &operator=(SparkNoArgumentCommand &&) = delete;

    ~SparkNoArgumentCommand() override = default;

    void execute(::CommandOrigin const &origin, ::CommandOutput &output) const override
    {
        if (!lease_.admitExecution()) {
            output.error("Spark command was rejected on an unobserved server thread");
            return;
        }
        context_->dispatch(origin, output, {});
    }

private:
    mutable CommandLifetimeGuard::Lease lease_;
    std::shared_ptr<CommandContext> context_;
};

class SparkRawCommand final : public ::Command {
public:
    std::uint64_t placeholder = 0;
    SparkRawParameters parameters;

    SparkRawCommand(CommandLifetimeGuard::Lease lease, std::shared_ptr<CommandContext> context)
        : lease_(std::move(lease)), context_(std::move(context))
    {
    }

    SparkRawCommand(SparkRawCommand const &) = delete;
    SparkRawCommand &operator=(SparkRawCommand const &) = delete;
    SparkRawCommand(SparkRawCommand &&) = delete;
    SparkRawCommand &operator=(SparkRawCommand &&) = delete;

    ~SparkRawCommand() override = default;

    void execute(::CommandOrigin const &origin, ::CommandOutput &output) const override
    {
        if (!lease_.admitExecution()) {
            output.error("Spark command was rejected on an unobserved server thread");
            return;
        }
        context_->dispatch(origin, output, parameters.raw.mText);
    }

private:
    mutable CommandLifetimeGuard::Lease lease_;
    std::shared_ptr<CommandContext> context_;
};

struct SparkRawLayoutProbe final : ::Command {
    std::uint64_t placeholder = 0;
    SparkRawParameters parameters;

    void execute(::CommandOrigin const &, ::CommandOutput &) const override {}
};

static_assert(offsetof(SparkRawCommand, parameters) == offsetof(SparkRawLayoutProbe, parameters));
constexpr int kSparkRawParameterOffset =
    static_cast<int>(offsetof(SparkRawLayoutProbe, parameters) + offsetof(SparkRawParameters, raw));

bool validateActiveSparkCommand(::CommandRegistry::Signature const &signature,
                                spark::levilamina::HostRawParameterTemplate const &expected,
                                ll::sys_utils::HandleT current_module,
                                spark::levilamina::HostRawParameterTemplate &actual, std::string &error)
{
    if (signature.overloads.size() != 2) {
        error = "active spark signature does not contain exactly two overloads";
        return false;
    }

    ::CommandParameterData const *raw = nullptr;
    bool empty = false;
    for (auto const &overload : signature.overloads) {
        if (overload.params.empty()) {
            if (empty) {
                error = "active spark signature contains duplicate empty overloads";
                return false;
            }
            empty = true;
            continue;
        }
        if (overload.params.size() == 1 && overload.params.front().mName == "raw") {
            if (raw != nullptr) {
                error = "active spark signature contains duplicate raw overloads";
                return false;
            }
            raw = &overload.params.front();
            continue;
        }
        error = "active spark signature contains an unexpected overload";
        return false;
    }
    if (!empty || raw == nullptr) {
        error = "active spark signature is missing the expected empty/raw overload pair";
        return false;
    }

    if (raw->mTypeIndex != expected.type_index || raw->mParseOverride != expected.parse_override ||
        raw->mParseRule != expected.parse_rule) {
        error = "active raw overload does not retain the host type/parser/rule pointers";
        return false;
    }
    if (raw->mParamType != ::CommandParameterDataType::Basic || raw->mOffset != kSparkRawParameterOffset ||
        raw->mSetOffset != -1 || raw->mIsOptional || raw->mOptions != ::CommandParameterOption::None ||
        raw->mEnumNameOrPostfix != nullptr || raw->mChainedSubcommand != nullptr) {
        error = "active raw overload changed the frozen parameter layout or required semantics";
        return false;
    }

    actual = spark::levilamina::copyHostRawParameterFields(*raw, expected.rule_module, expected.parser_module);
    return spark::levilamina::validateHostRawParameterTemplate(actual, current_module, error);
}

class SparkEmptyOverload final : public ll::command::OverloadData {
public:
    SparkEmptyOverload(::ll::command::CommandHandle &handle, std::weak_ptr<ll::mod::Mod> mod)
        : OverloadData(handle, std::move(mod))
    {
    }

    void setFactory(::CommandRegistry::Overload::AllocFunction factory)
    {
        OverloadData::setFactory(std::move(factory));
    }
};

class SparkRawOverload final : public ll::command::OverloadData {
public:
    SparkRawOverload(::ll::command::CommandHandle &handle, std::weak_ptr<ll::mod::Mod> mod)
        : OverloadData(handle, std::move(mod))
    {
    }

    void requiredRaw(spark::levilamina::HostRawParameterTemplate const &host)
    {
        auto &data =
            addParamImpl(host.type_index, host.parse_override, "raw", ::CommandParameterDataType::Basic, {}, {},
                         kSparkRawParameterOffset, -1, false, ::CommandParameterOption::None, host.parse_rule);
        static_cast<void>(data);
    }

    void setFactory(::CommandRegistry::Overload::AllocFunction factory)
    {
        OverloadData::setFactory(std::move(factory));
    }
};

#include "spark_mod_decl.inc"

ll::GamingStatus SparkMod::gamingStatus()
{
    return ll::getGamingStatus();
}

#include "spark_mod_disable.inc"

ll::io::Logger &SparkMod::logger() const noexcept
{
    if (auto logger = logger_.lock()) {
        return *logger;
    }
    std::terminate();
}

void SparkMod::reportException(char const *operation, std::exception_ptr exception) const noexcept
{
    auto logger = logger_.lock();
    if (!logger) {
        return;
    }

    try {
        if (exception) {
            std::rethrow_exception(exception);
        }
    }
    catch (std::exception const &error) {
        try {
            logger->error("spark {} failed: {}", operation, error.what());
        }
        catch (...) {
        }
        return;
    }
    catch (...) {
        try {
            logger->error("spark {} failed with an unknown exception", operation);
        }
        catch (...) {
        }
        return;
    }
    try {
        logger->error("spark {} failed", operation);
    }
    catch (...) {
    }
}

bool SparkMod::runAllocatorProbe()
{
    using NewFn = void *(*)(std::size_t);
    using DeleteFn = void (*)(void *) noexcept;
    using AlignedNewFn = void *(*)(std::size_t, std::align_val_t);
    using AlignedDeleteFn = void (*)(void *, std::align_val_t) noexcept;

    const volatile NewFn scalar_new = static_cast<NewFn>(&::operator new);
    const volatile DeleteFn scalar_delete = static_cast<DeleteFn>(&::operator delete);
    ProbeMemory scalar{scalar_new(32), scalar_delete};
    if (scalar.get() == nullptr) {
        throw std::runtime_error{"scalar allocator returned null"};
    }
    auto *scalar_bytes = static_cast<volatile std::uint8_t *>(scalar.get());
    scalar_bytes[0] = 0x5A;
    if (scalar_bytes[0] != 0x5A) {
        throw std::runtime_error{"scalar allocator readback failed"};
    }

    const volatile NewFn array_new = static_cast<NewFn>(&::operator new[]);
    const volatile DeleteFn array_delete = static_cast<DeleteFn>(&::operator delete[]);
    ProbeMemory array{array_new(48), array_delete};
    if (array.get() == nullptr) {
        throw std::runtime_error{"array allocator returned null"};
    }
    auto *array_bytes = static_cast<volatile std::uint8_t *>(array.get());
    array_bytes[17] = 0xA5;
    if (array_bytes[17] != 0xA5) {
        throw std::runtime_error{"array allocator readback failed"};
    }

    constexpr std::align_val_t alignment{64};
    const volatile AlignedNewFn aligned_new = static_cast<AlignedNewFn>(&::operator new);
    const volatile AlignedDeleteFn aligned_delete = static_cast<AlignedDeleteFn>(&::operator delete);
    ProbeMemory aligned{
        aligned_new(96, alignment),
        AlignedProbeDeleter{aligned_delete, alignment},
    };
    if (aligned.get() == nullptr ||
        reinterpret_cast<std::uintptr_t>(aligned.get()) % static_cast<std::size_t>(alignment) != 0) {
        throw std::runtime_error{"aligned allocator returned an invalid address"};
    }
    auto *aligned_bytes = static_cast<volatile std::uint8_t *>(aligned.get());
    aligned_bytes[31] = 0x3C;
    if (aligned_bytes[31] != 0x3C) {
        throw std::runtime_error{"aligned allocator readback failed"};
    }

    const volatile AlignedNewFn aligned_array_new = static_cast<AlignedNewFn>(&::operator new[]);
    const volatile AlignedDeleteFn aligned_array_delete = static_cast<AlignedDeleteFn>(&::operator delete[]);
    ProbeMemory aligned_array{
        aligned_array_new(128, alignment),
        AlignedProbeDeleter{aligned_array_delete, alignment},
    };
    if (aligned_array.get() == nullptr ||
        reinterpret_cast<std::uintptr_t>(aligned_array.get()) % static_cast<std::size_t>(alignment) != 0) {
        throw std::runtime_error{"aligned array allocator returned an invalid address"};
    }
    auto *aligned_array_bytes = static_cast<volatile std::uint8_t *>(aligned_array.get());
    aligned_array_bytes[63] = 0xC3;
    if (aligned_array_bytes[63] != 0xC3) {
        throw std::runtime_error{"aligned array readback failed"};
    }

    logger().info("Spark allocator probe passed: scalar, array, aligned scalar, and aligned array paths");
    return true;
}

bool SparkMod::load()
{
    if (loaded_) {
        return true;
    }

    try {
        owner_ = ll::mod::NativeMod::current();
        auto owner = owner_.lock();
        if (!owner) {
            return false;
        }

        logger_ = owner->getLogger().weak_from_this();
        if (logger_.expired()) {
            logger_.reset();
            return false;
        }

        std::filesystem::create_directories(owner->getDataDir());
        std::filesystem::create_directories(owner->getConfigDir());

        startup_clock_ = std::make_shared<spark::levilamina::StartupClock>();
        if (!startup_clock_->initializeProcessStart()) {
            startup_clock_.reset();
            logger().error("spark load failed: BDS process creation time could not be anchored");
            return false;
        }

        loaded_ = true;
        logger().info("Spark {} loaded for LeviLamina {}", spark::kVersion, ll::getLoaderVersion().to_string());
        logger().info("Spark data directory: {}", owner->getDataDir().string());
        logger().info("Spark config directory: {}", owner->getConfigDir().string());
        return true;
    }
    catch (...) {
        auto exception = std::current_exception();
        resetSessionState();
        startup_clock_.reset();
        reportException("load", exception);
        return false;
    }
}

bool SparkMod::confirmCommandRegistryCleanup()
{
    if (!publication_boundary_.published()) {
        return true;
    }

    auto registry = ll::service::getCommandRegistry();
    if (!registry) {
        if (auto logger = logger_.lock()) {
            try {
                logger->warn("Spark lifecycle could not prove command registry cleanup");
            }
            catch (...) {
            }
        }
        return false;
    }
    auto *command = registry->findCommand("spark");
    const bool registry_empty = command == nullptr || command->overloads.empty();
    if (!registry_empty) {
        if (auto logger = logger_.lock()) {
            try {
                logger->warn("Spark lifecycle found retained command overloads after disable");
            }
            catch (...) {
            }
        }
        return false;
    }
    return publication_boundary_.confirmHostRegistryCleanup(true);
}

bool SparkMod::registerCommand()
{
    auto registry = ll::service::getCommandRegistry();
    if (!registry) {
        throw std::runtime_error{"spark command registration failed: server command registry is unavailable"};
    }

    if (auto *existing = registry->findCommand("spark"); existing != nullptr) {
        if (existing->permissionLevel != ::CommandPermissionLevel::GameDirectors || !existing->overloads.empty()) {
            throw std::runtime_error{"spark command registration rejected a conflicting pre-existing signature"};
        }
    }

    auto &registrar = ll::command::CommandRegistrar::getServerInstance();
    auto context = command_context_;
    if (!context || !context->callback_state || !context->lifetime) {
        throw std::runtime_error{"spark command registration has no live command context"};
    }
    publication_boundary_.begin();
    auto &command = registrar.getOrCreateCommand("spark", "Spark profiler", ::CommandPermissionLevel::GameDirectors,
                                                 ::CommandFlagValue::NotCheat, owner_);

    auto native_owner = owner_.lock();
    if (!native_owner) {
        throw std::runtime_error{"spark raw command registration lost its owning native mod"};
    }
    std::string host_parameter_error;
    auto host_raw = spark::levilamina::captureHostRawParameterTemplate(command, owner_, native_owner->getHandle(),
                                                                       host_parameter_error);
    if (!host_raw.has_value()) {
        throw std::runtime_error{"spark raw command host parameter validation failed: " + host_parameter_error};
    }

    SparkEmptyOverload empty{command, owner_};
    empty.setFactory(::CommandRegistry::Overload::AllocFunction{[context]() -> std::unique_ptr<::Command> {
        auto lease = context->lifetime->acquireForConstruction();
        if (!lease.has_value()) {
            return {};
        }
        try {
            return std::make_unique<SparkNoArgumentCommand>(std::move(*lease), context);
        }
        catch (...) {
            return {};
        }
    }});

    SparkRawOverload raw{command, owner_};
    raw.requiredRaw(*host_raw);
    raw.setFactory(::CommandRegistry::Overload::AllocFunction{[context]() -> std::unique_ptr<::Command> {
        auto lease = context->lifetime->acquireForConstruction();
        if (!lease.has_value()) {
            return {};
        }
        try {
            return std::make_unique<SparkRawCommand>(std::move(*lease), context);
        }
        catch (...) {
            return {};
        }
    }});
    auto *active_signature = registry->findCommand("spark");
    if (active_signature == nullptr) {
        throw std::runtime_error{"spark command registration lost its active signature"};
    }
    spark::levilamina::HostRawParameterTemplate actual_raw;
    std::string active_parameter_error;
    if (!validateActiveSparkCommand(*active_signature, *host_raw, native_owner->getHandle(), actual_raw,
                                    active_parameter_error)) {
        throw std::runtime_error{"spark active raw parameter validation failed: " + active_parameter_error};
    }
    command_registered_ = true;
    logger().debug("Spark raw command host rule accepted: rule_module={}, parser_module={}, symbol=RawText",
                   actual_raw.rule_module, actual_raw.parser_module);
    logger().info("Registered /spark with GameDirectors permission");
    return true;
}

bool SparkMod::enable()
{
    try {
        if (enabled_.load(std::memory_order_acquire)) {
            return true;
        }
        if (!loaded_ || !startup_clock_ || callback_state_ || cleanup_guard_ || host_session_ || executor_ ||
            tick_listener_ || command_lifetime_ || command_context_) {
            throw std::runtime_error{"spark enable requires a completed load"};
        }

        auto owner = owner_.lock();
        if (!owner) {
            throw std::runtime_error{"spark enable could not resolve its owning native mod"};
        }
        if (!confirmCommandRegistryCleanup()) {
            CleanupDeadlineGuard::terminateOnTimeout();
        }
        if (!runAllocatorProbe()) {
            throw std::runtime_error{"spark allocator probe failed"};
        }

        callback_state_ = std::make_shared<CallbackState>();
        cleanup_guard_ = std::make_unique<CleanupDeadlineGuard>();
        command_lifetime_ = std::make_shared<CommandLifetimeGuard>();
        tick_warning_once_ = std::make_shared<std::atomic_bool>(false);

        const auto weak_logger = logger_;
        auto state = callback_state_;
        state->setInfoCallback([weak_logger](std::string const &message) {
            if (auto logger = weak_logger.lock()) {
                logger->info("{}", message);
            }
        });
        state->setErrorCallback([weak_logger](std::string const &message) {
            if (auto logger = weak_logger.lock()) {
                logger->error("{}", message);
            }
        });
        state->setFatalHandler([](CallbackState::FatalReason) { CleanupDeadlineGuard::terminateOnTimeout(); });

        executor_ = std::make_shared<Executor>("spark_server_thread", std::chrono::milliseconds{30}, 16);
        const std::weak_ptr<Executor> weak_executor = executor_;
        state->setSubmitter([state, weak_executor](std::function<void()> wrapper) {
            auto executor = weak_executor.lock();
            if (!executor) {
                throw std::runtime_error{"server executor is unavailable"};
            }
            executor->execute(std::move(wrapper));
        });

        auto session = std::make_shared<HostSession>();
        host_session_ = session;
        session->dispatcher = std::make_unique<spark::levilamina::LeviLaminaDispatcher>(state);
        session->metadata = std::make_unique<spark::levilamina::LeviLaminaMetadataProvider>(startup_clock_, state);
        session->notifier = std::make_shared<spark::levilamina::LeviLaminaNotifier>(state, weak_logger);
        session->bridge = std::make_unique<spark::levilamina::ApplicationBridge>(
            owner->getDataDir(), owner->getConfigDir(), *session->dispatcher, *session->metadata, *session->notifier);
        session->bridge->enable();

        command_context_ = std::make_shared<CommandContext>(CommandContext{
            .callback_state = state,
            .session = session,
            .lifetime = command_lifetime_,
        });

        const std::weak_ptr<HostSession> weak_session = session;
        const auto warning_once = tick_warning_once_;
        const auto lifetime = command_lifetime_;
        tick_listener_ = ll::event::EventBus::getInstance().emplaceListener<TickEvent>(
            [state, weak_session, warning_once, lifetime](TickEvent &) {
                static_cast<void>(state->invokeInline([state, weak_session, warning_once, lifetime] {
                    state->observeTick();
                    if (lifetime) {
                        lifetime->observeServerThread(std::this_thread::get_id());
                    }
                    const auto measured_ms = spark::levilamina::bds::readServerTickMilliseconds();
                    auto session = weak_session.lock();
                    if (!session) {
                        return;
                    }

                    const bool first_event = !session->tick_started.exchange(true, std::memory_order_acq_rel);
                    if (!measured_ms.has_value()) {
                        if (warning_once && !warning_once->exchange(true, std::memory_order_acq_rel)) {
                            state->reportError("Spark skipped an invalid BDS server tick duration");
                        }
                    }
                    else if (!first_event && session->bridge) {
                        session->bridge->onTick(*measured_ms, spark::levilamina::bds::currentThreadId());
                    }
                }));
            },
            ll::event::EventPriority::Normal, owner_);
        if (!tick_listener_) {
            throw std::runtime_error{"spark enable failed: could not register ServerLevelTickEvent listener"};
        }

        // Keep command registration last so every earlier resource can unwind on failure.
        if (!registerCommand()) {
            throw std::runtime_error{"spark command registration failed"};
        }

        enabled_.store(true, std::memory_order_release);
        logger().info("Spark {} enabled; SparkApplication forwarding is active", spark::kVersion);
        return true;
    }
    catch (...) {
        auto exception = std::current_exception();
        return spark::levilamina::runPublicationFailurePath(
            publication_boundary_, exception, [this] { static_cast<void>(closeResources(false)); },
            [this](std::exception_ptr error) { reportException("enable", error); },
            [] { CleanupDeadlineGuard::terminateOnTimeout(); });
    }
}

void SparkMod::resetSessionState() noexcept
{
    command_context_.reset();
    command_lifetime_.reset();
    host_session_.reset();
    executor_.reset();
    tick_listener_.reset();
    tick_warning_once_.reset();
    callback_state_.reset();
    enabled_.store(false, std::memory_order_release);
    command_registered_ = false;
}

bool SparkMod::admitRuntimeClose() const
{
    auto state = callback_state_;
    auto session = host_session_;
    auto lifetime = command_lifetime_;
    return state && session && session->bridge && tick_listener_ && tick_listener_.use_count() == 2 &&
           session->tick_started.load(std::memory_order_acquire) && state->phase() == CallbackState::Phase::Open &&
           !state->isInBodyOnCurrentThread() && lifetime && lifetime->hasObservedServerThread() &&
           lifetime->isServerThread() && lifetime->activeCommands() == 0 && !lifetime->unsafeViolation();
}

bool SparkMod::closeResources(bool require_server_thread)
{
    try {
        auto state = callback_state_;
        if (!state) {
            if (cleanup_guard_) {
                if (!cleanup_guard_->cancelDormantAndJoin()) {
                    CleanupDeadlineGuard::terminateOnTimeout();
                }
                cleanup_guard_.reset();
            }
            resetSessionState();
            return true;
        }

        if (require_server_thread && !admitRuntimeClose()) {
            logger().warn("Spark cleanup refused: server tick thread or command lifetime admission is not proven");
            return false;
        }
        if (!cleanup_guard_) {
            state->failFatal(CallbackState::FatalReason::Deadline);
            CleanupDeadlineGuard::terminateOnTimeout();
        }
        if (require_server_thread && (!command_lifetime_ || !command_lifetime_->beginCleanup())) {
            logger().warn("Spark cleanup refused: a command or lifecycle transition became active");
            return false;
        }

        constexpr auto cleanup_timeout = std::chrono::seconds{5};
        cleanup_guard_->arm(cleanup_timeout);
        const auto deadline = cleanup_guard_->deadline();

        std::shared_ptr<Executor> executor;
        ll::event::ListenerPtr tick_listener;
        const auto claim = state->beginClosing([&] {
            tick_listener = std::move(tick_listener_);
            executor = std::move(executor_);
        });
        if (claim != CallbackState::CloseClaim::Owner) {
            state->failFatal(claim == CallbackState::CloseClaim::SelfWaitRejected
                                 ? CallbackState::FatalReason::SelfWait
                                 : CallbackState::FatalReason::Deadline);
            CleanupDeadlineGuard::terminateOnTimeout();
        }

        {
            auto cleanup_scope = state->enterCleanupScope();
            if (!state->waitProducer(deadline)) {
                state->failFatal(CallbackState::FatalReason::Deadline);
                CleanupDeadlineGuard::terminateOnTimeout();
            }

            auto pending_payloads = state->takePendingPayloads();
            state->destroyPendingPayloads(pending_payloads);

            if (tick_listener) {
                ll::event::EventBus::getInstance().removeListener<TickEvent>(tick_listener);
                if (!spark::levilamina::waitForSoleSharedOwner(tick_listener, deadline)) {
                    state->failFatal(CallbackState::FatalReason::Deadline);
                    CleanupDeadlineGuard::terminateOnTimeout();
                }
            }
            tick_listener.reset();

            auto diagnostic_callbacks = state->takeDiagnosticCallbacks();
            state->destroyDiagnosticCallbacks(diagnostic_callbacks);
            if (!state->waitQuiescent(deadline)) {
                state->failFatal(CallbackState::FatalReason::Deadline);
                CleanupDeadlineGuard::terminateOnTimeout();
            }

            auto session = std::move(host_session_);
            if (session && session->bridge) {
                std::string error;
                if (!session->bridge->shutdown(error)) {
                    state->reportError(error.empty() ? "Spark application shutdown failed" : error);
                    state->failFatal(CallbackState::FatalReason::Deadline);
                    CleanupDeadlineGuard::terminateOnTimeout();
                }
                session->bridge.reset();
            }
            if (session) {
                if (session->metadata && !session->metadata->closeWorldGauges(deadline)) {
                    state->failFatal(CallbackState::FatalReason::Deadline);
                    CleanupDeadlineGuard::terminateOnTimeout();
                }
                session->notifier.reset();
                session->metadata.reset();
                session->dispatcher.reset();
            }
            session.reset();

            executor.reset();
            if (state->activeBodies() != 0 || state->pendingWorkSlots() != 0 || executor) {
                state->failFatal(CallbackState::FatalReason::Deadline);
                CleanupDeadlineGuard::terminateOnTimeout();
            }

            const auto ticks = state->rawTickObservations();
            logger().info(
                "Spark cleanup quiescent: ticks={}, active_bodies={}, pending_payloads={}, tick_listener_released={}, "
                "executor_released={}",
                ticks, state->activeBodies(), state->pendingWorkSlots(), tick_listener == nullptr, executor == nullptr);

            command_context_.reset();
            if (require_server_thread && (!command_lifetime_ || !command_lifetime_->completeCleanup())) {
                state->failFatal(CallbackState::FatalReason::Deadline);
                CleanupDeadlineGuard::terminateOnTimeout();
            }

            enabled_.store(false, std::memory_order_release);
            command_registered_ = false;
            cleanup_guard_->completeAndJoin();
            state->markClosed();
        }
        callback_state_.reset();
        command_lifetime_.reset();
        tick_warning_once_.reset();
        cleanup_guard_.reset();
        return true;
    }
    catch (...) {
        CleanupDeadlineGuard::terminateOnTimeout();
    }
}

bool SparkMod::unload()
{
    if (!loaded_) {
        return true;
    }
    if (enabled_.load(std::memory_order_acquire) || callback_state_ || cleanup_guard_ || command_lifetime_ ||
        command_context_ || host_session_ || executor_ || tick_listener_) {
        if (auto logger = logger_.lock()) {
            logger->warn("Spark unload refused while the active session is not fully disabled");
        }
        return false;
    }
    if (!confirmCommandRegistryCleanup()) {
        return false;
    }
    if (command_registered_) {
        if (auto logger = logger_.lock()) {
            logger->warn("Spark unload refused before LeviLamina command registry cleanup");
        }
        return false;
    }
    if (auto logger = logger_.lock()) {
        logger->info("Spark unloaded after a quiescent session");
    }
    startup_clock_.reset();
    loaded_ = false;
    owner_.reset();
    logger_.reset();
    return true;
}

SparkMod &getSparkMod()
{
    static SparkMod mod;
    return mod;
}

}  // namespace

LL_REGISTER_MOD(SparkMod, getSparkMod());
