#ifndef M_OBJECT_H
#define M_OBJECT_H

#include <QtCore/QObject>
#include <QtCore/QMetaObject>
#include <QtCore/QThread>
#include <QtCore/QPointer>
#include <array>
#include <memory>
#include <atomic>
#include <functional>
#include <mutex>
#include <algorithm>
#include <concepts>
#include <type_traits>
#include <utility>
#include <typeinfo>
#include <cstring>

namespace melo {

enum class ConnectionType {
    Auto = 0,
    Direct = 1,
    Queued = 2,
    SingleShot = 0x80
};

struct Connection {
    std::size_t id = 0;
    explicit constexpr operator bool() const noexcept { return id != 0; }
    constexpr bool operator==(const Connection&) const = default;
};

class ScopedConnection;

class SignalBase {
    friend class ScopedConnection;
protected:
    struct Slot {
        virtual ~Slot() = default;
        QPointer<QObject> context;
        std::size_t id;
        std::atomic<bool> connected{true};
        bool single_shot = false;
    };

    static constexpr size_t INLINE_CAPACITY = 2;

    struct InlineStorage {
        std::array<std::shared_ptr<Slot>, INLINE_CAPACITY> slots{};
        uint8_t count = 0;
    };

    struct HeapStorage {
        std::vector<std::shared_ptr<Slot>> slots;
        std::mutex mutex;
        std::atomic<int> ref_count{1};
    };

    union Storage {
        InlineStorage inline_data;
        HeapStorage* heap_ptr;

        constexpr Storage() noexcept : inline_data{} {}
        ~Storage() {}
    };

    Storage m_storage;
    std::atomic<std::size_t> m_sequence{0};
    std::atomic<uint8_t> m_flags{0};

    static constexpr uint8_t FLAG_HEAP = 0x01;
    static constexpr uint8_t FLAG_HAS_SLOTS = 0x02;

    [[nodiscard]] bool uses_heap() const noexcept {
        return m_flags.load(std::memory_order_relaxed) & FLAG_HEAP;
    }

    [[nodiscard]] bool has_slots() const noexcept {
        return m_flags.load(std::memory_order_relaxed) & FLAG_HAS_SLOTS;
    }

    void set_flag(uint8_t flag) noexcept {
        m_flags.fetch_or(flag, std::memory_order_release);
    }

    void clear_flag(uint8_t flag) noexcept {
        m_flags.fetch_and(~flag, std::memory_order_release);
    }

    void move_to_heap();

public:
    SignalBase() noexcept = default;
    SignalBase(const SignalBase& other) noexcept;
    SignalBase(SignalBase&& other) noexcept;
    SignalBase& operator=(const SignalBase& other) noexcept;
    SignalBase& operator=(SignalBase&& other) noexcept;
    ~SignalBase();

    void disconnect(Connection connection = {});
};

class ScopedConnection {
    SignalBase* m_signal = nullptr;
    Connection m_connection;

public:
    ScopedConnection() = default;
    ScopedConnection(SignalBase& signal, Connection connection)
        : m_signal(&signal), m_connection(connection) {}

    ScopedConnection(ScopedConnection&& other) noexcept
        : m_signal(std::exchange(other.m_signal, nullptr))
        , m_connection(std::exchange(other.m_connection, {})) {}

    ScopedConnection& operator=(ScopedConnection&& other) noexcept {
        if (this != &other) {
            disconnect();
            m_signal = std::exchange(other.m_signal, nullptr);
            m_connection = std::exchange(other.m_connection, {});
        }
        return *this;
    }

    ~ScopedConnection() { disconnect(); }

    void disconnect() {
        if (m_signal && m_connection) {
            m_signal->disconnect(m_connection);
            m_signal = nullptr;
            m_connection = {};
        }
    }

    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;
};

template <typename... Args>
class signal : public SignalBase {
    static_assert((std::is_copy_constructible_v<Args> && ...),
                  "Signal args must be copyable");

    struct SlotInterface : Slot {
        virtual ~SlotInterface() = default;
        virtual void invoke(const Args&... args) = 0;
    };

    template <typename Callback>
    struct SlotT final : SlotInterface {
        [[no_unique_address]] Callback callback;

        SlotT(Callback&& cb, QObject* ctx, std::size_t id, bool ss)
            : callback(std::move(cb)) {
            this->context = ctx;
            this->id = id;
            this->single_shot = ss;
        }

        void invoke(const Args&... args) override {
            std::invoke(callback, args...);
        }
    };

    template <typename Callback>
    Connection add(QObject* context, Callback&& callback, ConnectionType type);

    [[nodiscard]] bool try_disconnect_single_shot(const std::shared_ptr<Slot>& slot) const noexcept;
    void invoke_direct(const std::shared_ptr<Slot>& slot, const Args&... args, bool& cleanup) const;
    void invoke_queued(const std::shared_ptr<Slot>& slot, const Args&... args) const;

public:
    signal() = default;
    signal(const signal&) noexcept = default;
    signal(signal&&) noexcept = default;
    signal& operator=(const signal&) noexcept = default;
    signal& operator=(signal&&) noexcept = default;
    ~signal() = default;

    template <std::invocable<Args...> Callback>
    Connection connect(Callback&& callback, ConnectionType type = ConnectionType::Auto);

    template <std::invocable<Args...> Callback>
    Connection connect(QObject* context, Callback&& callback,
                       ConnectionType type = ConnectionType::Auto);

    template <typename T, typename Callback>
        requires std::invocable<Callback, T*, Args...>
    Connection connect(T* instance, Callback&& callback,
                       ConnectionType type = ConnectionType::Auto);

    ScopedConnection scoped_connect(auto&& callback,
                                    ConnectionType type = ConnectionType::Auto);

    ScopedConnection scoped_connect(QObject* context, auto&& callback,
                                    ConnectionType type = ConnectionType::Auto);

    template <typename T>
    ScopedConnection scoped_connect(T* instance, auto&& callback,
                                    ConnectionType type = ConnectionType::Auto);

    void operator()(const Args&... args) const;
};

template<typename T>
struct has_signals_impl {
    template<typename U>
    static auto test(int) -> decltype(
        std::declval<U>().*(static_cast<melo::SignalBase U::*>(nullptr)),
        std::true_type{});

    template<typename>
    static std::false_type test(...);

    static constexpr bool value = decltype(test<T>(0))::value;
};

template<typename T>
concept has_signals = has_signals_impl<T>::value;

template<typename Self>
class MetaObjectBuilder {
private:
    static void qt_static_metacall(QObject*, QMetaObject::Call, int, void**) {}

    static QMetaObject* get_meta_full(const QMetaObject* superClass) {
        static QMetaObject metaObject = {
            .d = {
                .superdata = superClass,
                .stringdata = nullptr,
                .data = nullptr,
                .static_metacall = &qt_static_metacall,
                .relatedMetaObjects = nullptr,
                .extradata = nullptr
            }
        };
        return &metaObject;
    }

    static QMetaObject* get_meta_minimal(const QMetaObject* superClass) {
        static QMetaObject metaObject = {
            .d = {
                .superdata = superClass,
                .stringdata = nullptr,
                .data = nullptr,
                .static_metacall = nullptr,
                .relatedMetaObjects = nullptr,
                .extradata = nullptr
            }
        };
        return &metaObject;
    }

public:
    static const QMetaObject* build(const char* className, const QMetaObject* superClass) {
        if constexpr (has_signals<Self>) {
            return get_meta_full(superClass);
        } else {
            return get_meta_minimal(superClass);
        }
    }
};

}

#define M_OBJECT(CLASS) \
private: \
    using M_Self = CLASS; \
    friend class melo::MetaObjectBuilder<CLASS>; \
    public: \
    static inline const QMetaObject staticMetaObject = \
      *melo::MetaObjectBuilder<CLASS>::build(#CLASS, &QObject::staticMetaObject); \
    \
    const QMetaObject* metaObject() const override { \
        return &staticMetaObject; \
} \
    \
    void* qt_metacast(const char* className) override { \
        if (!className) [[unlikely]] return nullptr; \
        if (std::strcmp(className, #CLASS) == 0) \
        return static_cast<void*>(this); \
        return QObject::qt_metacast(className); \
} \
    \
    int qt_metacall(QMetaObject::Call call, int id, void** args) override { \
        id = QObject::qt_metacall(call, id, args); \
        if (id < 0) return id; \
        return id; \
} \
    private:

#define M_SIGNAL(NAME, ...) \
              melo::signal<__VA_ARGS__> NAME

#define M_SIGNAL_ONLY(NAME, ...) \
                      melo::signal<__VA_ARGS__> NAME

              namespace melo {

    inline void SignalBase::move_to_heap() {
        auto* heap = new HeapStorage();
        for (size_t i = 0; i < m_storage.inline_data.count; ++i) {
            heap->slots.push_back(std::move(m_storage.inline_data.slots[i]));
        }
        m_storage.inline_data.~InlineStorage();
        m_storage.heap_ptr = heap;
        set_flag(FLAG_HEAP);
    }

    inline SignalBase::SignalBase(const SignalBase& other) noexcept {
        if (other.uses_heap()) {
            m_storage.heap_ptr = other.m_storage.heap_ptr;
            m_storage.heap_ptr->ref_count.fetch_add(1, std::memory_order_relaxed);
            set_flag(FLAG_HEAP);
        } else {
            new (&m_storage.inline_data) InlineStorage(other.m_storage.inline_data);
        }
        if (other.has_slots()) set_flag(FLAG_HAS_SLOTS);
        m_sequence.store(other.m_sequence.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
    }

    inline SignalBase::SignalBase(SignalBase&& other) noexcept {
        if (other.uses_heap()) {
            m_storage.heap_ptr = std::exchange(other.m_storage.heap_ptr, nullptr);
            set_flag(FLAG_HEAP);
        } else {
            new (&m_storage.inline_data) InlineStorage(std::move(other.m_storage.inline_data));
            other.m_storage.inline_data.count = 0;
        }
        if (other.has_slots()) {
            set_flag(FLAG_HAS_SLOTS);
            other.clear_flag(FLAG_HAS_SLOTS);
        }
        m_sequence.store(other.m_sequence.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
    }

    inline SignalBase& SignalBase::operator=(const SignalBase& other) noexcept {
        if (this != &other) {
            this->~SignalBase();
            new (this) SignalBase(other);
        }
        return *this;
    }

    inline SignalBase& SignalBase::operator=(SignalBase&& other) noexcept {
        if (this != &other) {
            this->~SignalBase();
            new (this) SignalBase(std::move(other));
        }
        return *this;
    }

    inline SignalBase::~SignalBase() {
        disconnect();
        if (uses_heap()) {
            if (m_storage.heap_ptr &&
                m_storage.heap_ptr->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                delete m_storage.heap_ptr;
            }
        } else {
            m_storage.inline_data.~InlineStorage();
        }
    }

    inline void SignalBase::disconnect(Connection connection) {
        if (!connection.id) {
            if (uses_heap()) {
                std::unique_lock lock(m_storage.heap_ptr->mutex);
                for (auto& slot : m_storage.heap_ptr->slots) {
                    if (slot) slot->connected.store(false, std::memory_order_release);
                }
                m_storage.heap_ptr->slots.clear();
            } else {
                for (size_t i = 0; i < m_storage.inline_data.count; ++i) {
                    if (m_storage.inline_data.slots[i]) {
                        m_storage.inline_data.slots[i]->connected.store(
                            false, std::memory_order_release);
                    }
                }
                m_storage.inline_data.count = 0;
            }
            clear_flag(FLAG_HAS_SLOTS);
        } else {
            if (uses_heap()) {
                std::unique_lock lock(m_storage.heap_ptr->mutex);
                auto& slots = m_storage.heap_ptr->slots;
                auto it = std::ranges::find_if(slots,
                                               [id = connection.id](const auto& s) { return s && s->id == id; });
                if (it != slots.end()) {
                    (*it)->connected.store(false, std::memory_order_release);
                    slots.erase(it);
                }
                if (slots.empty()) clear_flag(FLAG_HAS_SLOTS);
            } else {
                for (size_t i = 0; i < m_storage.inline_data.count; ++i) {
                    if (m_storage.inline_data.slots[i] &&
                        m_storage.inline_data.slots[i]->id == connection.id) {
                        m_storage.inline_data.slots[i]->connected.store(
                            false, std::memory_order_release);
                        for (size_t j = i; j < m_storage.inline_data.count - 1; ++j) {
                            m_storage.inline_data.slots[j] =
                                std::move(m_storage.inline_data.slots[j + 1]);
                        }
                        --m_storage.inline_data.count;
                        if (m_storage.inline_data.count == 0)
                            clear_flag(FLAG_HAS_SLOTS);
                        break;
                    }
                }
            }
        }
    }

    template <typename... Args>
    template <typename Callback>
    Connection signal<Args...>::add(QObject* context, Callback&& callback, ConnectionType type) {
        auto id = m_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        auto slot = std::make_shared<SlotT<std::decay_t<Callback>>>(
            std::forward<Callback>(callback),
            context,
            id,
            static_cast<int>(type) & 0x80
            );

        if (!uses_heap()) {
            if (m_storage.inline_data.count < INLINE_CAPACITY) {
                m_storage.inline_data.slots[m_storage.inline_data.count++] = std::move(slot);
                set_flag(FLAG_HAS_SLOTS);
                return {id};
            }
            move_to_heap();
        }

        std::unique_lock lock(m_storage.heap_ptr->mutex);
        m_storage.heap_ptr->slots.push_back(std::move(slot));
        set_flag(FLAG_HAS_SLOTS);
        return {id};
    }

    template <typename... Args>
    bool signal<Args...>::try_disconnect_single_shot(const std::shared_ptr<Slot>& slot) const noexcept {
        bool expected = true;
        return slot->connected.compare_exchange_strong(
            expected, false, std::memory_order_acq_rel);
    }

    template <typename... Args>
    void signal<Args...>::invoke_direct(const std::shared_ptr<Slot>& slot, const Args&... args,
                                        bool& cleanup) const {
        if (slot->single_shot) [[unlikely]] {
            if (!try_disconnect_single_shot(slot)) return;
            cleanup = true;
        }
        static_cast<SlotInterface*>(slot.get())->invoke(args...);
    }

    template <typename... Args>
    void signal<Args...>::invoke_queued(const std::shared_ptr<Slot>& slot, const Args&... args) const {
        auto keeper = slot;
        auto slot_interface = static_cast<SlotInterface*>(slot.get());

        QMetaObject::invokeMethod(slot->context,
                                  [keeper, slot_interface, ...args = args]() {
                                      if (!keeper->connected.load(std::memory_order_acquire)) [[unlikely]]
                                          return;

                                      if (keeper->single_shot) [[unlikely]] {
                                          bool expected = true;
                                          if (!keeper->connected.compare_exchange_strong(
                                                  expected, false, std::memory_order_acq_rel))
                                              return;
                                      }

                                      slot_interface->invoke(args...);
                                  }, Qt::QueuedConnection);
    }

    template <typename... Args>
    template <std::invocable<Args...> Callback>
    Connection signal<Args...>::connect(Callback&& callback, ConnectionType type) {
        return add(nullptr, std::forward<Callback>(callback), type);
    }

    template <typename... Args>
    template <std::invocable<Args...> Callback>
    Connection signal<Args...>::connect(QObject* context, Callback&& callback,
                                        ConnectionType type) {
        return add(context, std::forward<Callback>(callback), type);
    }

    template <typename... Args>
    template <typename T, typename Callback>
        requires std::invocable<Callback, T*, Args...>
    Connection signal<Args...>::connect(T* instance, Callback&& callback,
                                        ConnectionType type) {
        if (!instance) [[unlikely]] return {};

        QObject* context = nullptr;
        if constexpr (std::is_convertible_v<T*, QObject*>) {
            context = static_cast<QObject*>(instance);
        }

        return add(context, [instance, fn = std::forward<Callback>(callback)]
                   (const Args&... args) {
                       std::invoke(fn, instance, args...);
                   }, type);
    }

    template <typename... Args>
    ScopedConnection signal<Args...>::scoped_connect(auto&& callback,
                                                     ConnectionType type) {
        return ScopedConnection(*this,
                                connect(std::forward<decltype(callback)>(callback), type));
    }

    template <typename... Args>
    ScopedConnection signal<Args...>::scoped_connect(QObject* context, auto&& callback,
                                                     ConnectionType type) {
        return ScopedConnection(*this,
                                connect(context, std::forward<decltype(callback)>(callback), type));
    }

    template <typename... Args>
    template <typename T>
    ScopedConnection signal<Args...>::scoped_connect(T* instance, auto&& callback,
                                                     ConnectionType type) {
        return ScopedConnection(*this,
                                connect(instance, std::forward<decltype(callback)>(callback), type));
    }

    template <typename... Args>
    void signal<Args...>::operator()(const Args&... args) const {
        if (!has_slots()) [[unlikely]] return;

        if (!uses_heap()) [[likely]] {
            QThread* current = QThread::currentThread();
            bool cleanup = false;

            for (size_t i = 0; i < m_storage.inline_data.count; ++i) {
                const auto& slot = m_storage.inline_data.slots[i];
                if (!slot || !slot->connected.load(std::memory_order_acquire)) {
                    cleanup = true;
                    continue;
                }

                if (slot->context && slot->context.isNull()) [[unlikely]] {
                    slot->connected.store(false, std::memory_order_release);
                    cleanup = true;
                    continue;
                }

                QThread* target = slot->context ? slot->context->thread() : current;
                if (!target) [[unlikely]] {
                    slot->connected.store(false, std::memory_order_release);
                    cleanup = true;
                    continue;
                }

                if (target == current) [[likely]] {
                    invoke_direct(slot, args..., cleanup);
                } else {
                    invoke_queued(slot, args...);
                }
            }

            return;
        }

        std::vector<std::shared_ptr<Slot>> snapshot;
        {
            std::unique_lock lock(m_storage.heap_ptr->mutex);
            if (m_storage.heap_ptr->slots.empty()) return;
            snapshot = m_storage.heap_ptr->slots;
        }

        QThread* current = QThread::currentThread();
        bool cleanup = false;

        for (const auto& slot : snapshot) {
            if (!slot->connected.load(std::memory_order_acquire)) {
                cleanup = true;
                continue;
            }

            if (slot->context && slot->context.isNull()) [[unlikely]] {
                slot->connected.store(false, std::memory_order_release);
                cleanup = true;
                continue;
            }

            QThread* target = slot->context ? slot->context->thread() : current;
            if (!target) [[unlikely]] {
                slot->connected.store(false, std::memory_order_release);
                cleanup = true;
                continue;
            }

            if (target == current) [[likely]] {
                invoke_direct(slot, args..., cleanup);
            } else {
                invoke_queued(slot, args...);
            }
        }

        if (cleanup) [[unlikely]] {
            std::unique_lock lock(m_storage.heap_ptr->mutex);
            std::erase_if(m_storage.heap_ptr->slots, [](const auto& s) {
                return !s->connected.load(std::memory_order_relaxed);
            });
        }
    }

}

#ifndef QT_NO_EMIT
# ifndef emit
#  define emit
# endif
#endif

#endif
