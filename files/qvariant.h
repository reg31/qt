// Copyright (C) 2020 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:critical reason:data-parser

#ifndef QVARIANT_H
#define QVARIANT_H

#include <QtCore/qatomic.h>
#include <QtCore/qcompare.h>
#include <QtCore/qcontainerfwd.h>
#include <QtCore/qmetatype.h>
#ifndef QT_NO_DEBUG_STREAM
#include <QtCore/qdebug.h>
#endif

#include <memory>
#include <concepts>
#include <type_traits>
#include <QtCore/q20type_traits.h>
#include <QtCore/q23utility.h>
#include <variant>

#if !defined(QT_LEAN_HEADERS) || QT_LEAN_HEADERS < 1
#  include <QtCore/qlist.h>
#  include <QtCore/qstringlist.h>
#  include <QtCore/qbytearraylist.h>
#  include <QtCore/qhash.h>
#  include <QtCore/qmap.h>
#  include <QtCore/qobject.h>
#endif

QT_BEGIN_NAMESPACE

QT_ENABLE_P0846_SEMANTICS_FOR(get_if)
QT_ENABLE_P0846_SEMANTICS_FOR(get)

class QBitArray;
class QDataStream;
class QDate;
class QTime;
class QDateTime;
class QEasingCurve;
class QLine;
class QLineF;
class QLocale;
class QModelIndex;
class QPersistentModelIndex;
class QPoint;
class QPointF;
class QRect;
class QRectF;
class QRegularExpression;
class QSize;
class QSizeF;
class QTextFormat;
class QTextLength;
class QTransform;
class QUrl;
class QUuid;
class QJsonValue;
class QJsonObject;
class QJsonArray;
class QJsonDocument;
class QVariant;

template<typename T>
inline T qvariant_cast(const QVariant &);

namespace QtPrivate {
template<> constexpr inline bool qIsRelocatable<QVariant> = true;
}

class Q_CORE_EXPORT QVariant
{
    struct CborValueStandIn { qint64 n; void *c; int t; };
public:
    struct PrivateShared
    {
    private:
        inline PrivateShared() : ref(1) { }
    public:
        static int computeOffset(PrivateShared *ps, size_t align);
        static size_t computeAllocationSize(size_t size, size_t align);
        static PrivateShared *create(size_t size, size_t align);
        static void free(PrivateShared *p);

        alignas(8) QAtomicInt ref;
        int offset;

        const void *data() const { return reinterpret_cast<const uchar *>(this) + offset; }
        void *data() { return reinterpret_cast<uchar *>(this) + offset; }
    };

    struct Private
    {
        static constexpr size_t MaxInternalSize = 3 * sizeof(void *);
        template <size_t S> static constexpr bool FitsInInternalSize = S <= MaxInternalSize;
        template<typename T> static constexpr bool CanUseInternalSpace =
                (QTypeInfo<T>::isRelocatable && FitsInInternalSize<sizeof(T)> && alignof(T) <= alignof(double));
        static constexpr bool canUseInternalSpace(const QtPrivate::QMetaTypeInterface *type)
        {
            Q_ASSERT(type);
            return QMetaType::TypeFlags(type->flags) & QMetaType::RelocatableType &&
                   size_t(type->size) <= MaxInternalSize && size_t(type->alignment) <= alignof(double);
        }

        union
        {
            uchar data[MaxInternalSize] = {};
            PrivateShared *shared;
            double _forAlignment;
        } data;
        quintptr is_shared : 1;
        quintptr is_null : 1;
        quintptr packedType : sizeof(QMetaType) * 8 - 2;

        constexpr Private() noexcept : is_shared(false), is_null(true), packedType(0) {}
        explicit Private(const QtPrivate::QMetaTypeInterface *iface) noexcept;
        template <typename T> explicit Private(std::piecewise_construct_t, const T &t);

        const void *storage() const
        { return is_shared ? data.shared->data() : &data.data; }

        template<typename T> const T &get() const
        { return *static_cast<const T *>(storage()); }

        inline const QtPrivate::QMetaTypeInterface *typeInterface() const
        {
            return reinterpret_cast<const QtPrivate::QMetaTypeInterface *>(packedType << 2);
        }

        inline QMetaType type() const
        {
            return QMetaType(typeInterface());
        }
    };

    template<typename Indirect> class Reference;

    template<typename Indirect>
    class ConstReference
    {
    private:
        friend class Reference<Indirect>;
        const Indirect m_referred;
    public:
        explicit ConstReference(const Indirect &referred) noexcept(std::is_nothrow_copy_constructible_v<Indirect>) : m_referred(referred) {}
        explicit ConstReference(Indirect &&referred) noexcept(std::is_nothrow_move_constructible_v<Indirect>) : m_referred(std::move(referred)) {}
        ConstReference(const ConstReference &) noexcept(std::is_nothrow_copy_constructible_v<Indirect>) = default;
        ConstReference(ConstReference &&) = delete;
        ConstReference(const Reference<Indirect> &nonConst) noexcept(std::is_nothrow_copy_constructible_v<Indirect>) : m_referred(nonConst.m_referred) {}
        ~ConstReference() = default;
        ConstReference &operator=(const ConstReference &) = delete;
        operator QVariant() const noexcept(Indirect::CanNoexceptConvertToQVariant);
    };

    template<typename Indirect>
    class Reference
    {
    private:
        friend class ConstReference<Indirect>;
        Indirect m_referred;
        friend void swap(Reference a, Reference b) { a.swap(b); }
    public:
        explicit Reference(const Indirect &referred) noexcept(std::is_nothrow_copy_constructible_v<Indirect>) : m_referred(referred) {}
        explicit Reference(Indirect &&referred) noexcept(std::is_nothrow_move_constructible_v<Indirect>) : m_referred(std::move(referred)) {}
        Reference(const Reference &) = default;
        Reference(Reference &&) = delete;
        ~Reference() = default;
        Reference &operator=(const QVariant &value) noexcept(Indirect::CanNoexceptAssignQVariant);
        Reference &operator=(const Reference &value) noexcept(Indirect::CanNoexceptAssignQVariant) { return operator=(QVariant(value)); }
        operator QVariant() const noexcept(Indirect::CanNoexceptConvertToQVariant) { return ConstReference<Indirect>(m_referred); }
        void swap(Reference b) { QVariant tmp = *this; *this = std::move(b); b = std::move(tmp); }
    };

    template<typename Indirect>
    class ConstPointer
    {
    private:
        Indirect m_pointed;
    public:
        explicit ConstPointer(const Indirect &pointed) noexcept(std::is_nothrow_copy_constructible_v<Indirect>) : m_pointed(pointed) {}
        explicit ConstPointer(Indirect &&pointed) noexcept(std::is_nothrow_move_constructible_v<Indirect>) : m_pointed(std::move(pointed)) {}
        ConstReference<Indirect> operator*() const noexcept(std::is_nothrow_copy_constructible_v<Indirect>) { return ConstReference<Indirect>(m_pointed); }
    };

    template<typename Indirect>
    class Pointer
    {
    private:
        Indirect m_pointed;
    public:
        explicit Pointer(const Indirect &pointed) noexcept(std::is_nothrow_copy_constructible_v<Indirect>) : m_pointed(pointed) {}
        explicit Pointer(Indirect &&pointed) noexcept(std::is_nothrow_move_constructible_v<Indirect>) : m_pointed(std::move(pointed)) {}
        Reference<Indirect> operator*() const noexcept(std::is_nothrow_copy_constructible_v<Indirect>) { return Reference<Indirect>(m_pointed); }
        operator ConstPointer<Indirect>() const noexcept(std::is_nothrow_copy_constructible_v<Indirect>) { return ConstPointer<Indirect>(m_pointed); }
    };

#if QT_DEPRECATED_SINCE(6, 0)
    enum QT_DEPRECATED_VERSION_X_6_0("Use QMetaType::Type instead.") Type
    {
        Invalid = QMetaType::UnknownType, Bool = QMetaType::Bool, Int = QMetaType::Int, UInt = QMetaType::UInt,
        LongLong = QMetaType::LongLong, ULongLong = QMetaType::ULongLong, Double = QMetaType::Double, Char = QMetaType::QChar,
        Map = QMetaType::QVariantMap, List = QMetaType::QVariantList, String = QMetaType::QString, StringList = QMetaType::QStringList,
        ByteArray = QMetaType::QByteArray, BitArray = QMetaType::QBitArray, Date = QMetaType::QDate, Time = QMetaType::QTime,
        DateTime = QMetaType::QDateTime, Url = QMetaType::QUrl, Locale = QMetaType::QLocale, Rect = QMetaType::QRect,
        RectF = QMetaType::QRectF, Size = QMetaType::QSize, SizeF = QMetaType::QSizeF, Line = QMetaType::QLine,
        LineF = QMetaType::QLineF, Point = QMetaType::QPoint, PointF = QMetaType::QPointF, UserType = QMetaType::User, LastType = 0xffffffff
    };
#endif

    QVariant() noexcept : d() {}
    ~QVariant();
    explicit QVariant(QMetaType type, const void *copy = nullptr);
    QVariant(const QVariant &other);

    QVariant(int i) noexcept;
    QVariant(uint ui) noexcept;
    QVariant(qlonglong ll) noexcept;
    QVariant(qulonglong ull) noexcept;
    QVariant(bool b) noexcept;
    QVariant(double d) noexcept;
    QVariant(float f) noexcept;
    QVariant(long l) noexcept : QVariant(qlonglong(l)) {}
    QVariant(ulong ul) noexcept : QVariant(qulonglong(ul)) {}
    QVariant(short s) noexcept : QVariant(int(s)) {}
    QVariant(ushort us) noexcept : QVariant(uint(us)) {}
    QVariant(QChar qchar) noexcept;
    QVariant(const QString &string) noexcept;
    QVariant(const QByteArray &bytearray) noexcept;
    QVariant(QLatin1StringView string) noexcept(false);

#ifndef QT_NO_CAST_FROM_ASCII
    QT_ASCII_CAST_WARN QVariant(const char *str) noexcept(false) : QVariant(QString::fromUtf8(str)) {}
#endif

    template <typename T>
    requires (!std::is_same_v<std::remove_cvref_t<T>, QVariant> &&
              !std::convertible_to<T, QMetaType> &&
              !std::is_pointer_v<std::decay_t<T>> &&
              !std::is_member_pointer_v<std::decay_t<T>> &&
              !std::is_same_v<std::remove_cvref_t<T>, QLatin1StringView> &&
              !std::is_same_v<std::remove_cvref_t<T>, QString> &&
              !std::is_same_v<std::remove_cvref_t<T>, QByteArray> &&
              !std::is_same_v<std::remove_cvref_t<T>, QChar> &&
              !std::integral<std::remove_cvref_t<T>> &&
              !std::floating_point<std::remove_cvref_t<T>>)
    QVariant(T &&val) noexcept(std::is_nothrow_copy_constructible_v<std::remove_cvref_t<T>> && Private::CanUseInternalSpace<std::remove_cvref_t<T>>) : d()
    {
        auto tmp = fromValue(std::forward<T>(val));
        std::swap(d, tmp.d);
    }

    template <typename T, typename... Args>
    using if_constructible = std::enable_if_t<std::conjunction_v<std::is_copy_constructible<std::remove_cvref_t<T>>, std::is_destructible<std::remove_cvref_t<T>>, std::is_constructible<std::remove_cvref_t<T>, Args...>>, bool>;

    template <typename T, typename... Args, if_constructible<T, Args...> = true>
    explicit QVariant(std::in_place_type_t<T>, Args&&... args) : QVariant(std::in_place, QMetaType::fromType<std::remove_cvref_t<T>>())
    {
        void *ptr = const_cast<void *>(constData());
        new (ptr) std::remove_cvref_t<T>(std::forward<Args>(args)...);
    }

    QVariant& operator=(const QVariant &other);
    inline QVariant(QVariant &&other) noexcept : d(other.d) { other.d = Private(); }
    QT_MOVE_ASSIGNMENT_OPERATOR_IMPL_VIA_MOVE_AND_SWAP(QVariant)
    inline void swap(QVariant &other) noexcept { std::swap(d, other.d); }

    int userType() const { return typeId(); }
    int typeId() const { const auto *mt = metaType().iface(); if (!mt) return 0; int id = mt->typeId.loadRelaxed(); Q_PRESUME(id > 0); return id; }

    QT_CORE_INLINE_SINCE(6, 10) const char *typeName() const;
    QT_CORE_INLINE_SINCE(6, 10) QMetaType metaType() const;

    bool canConvert(QMetaType targetType) const { return QMetaType::canConvert(d.type(), targetType); }
    bool convert(QMetaType type);
    bool canView(QMetaType targetType) const { return QMetaType::canView(d.type(), targetType); }

    inline bool isValid() const;
    bool isNull() const;
    void clear();
    void detach();
    inline bool isDetached() const;

    int toInt(bool *ok = nullptr) const;
    uint toUInt(bool *ok = nullptr) const;
    qlonglong toLongLong(bool *ok = nullptr) const;
    qulonglong toULongLong(bool *ok = nullptr) const;
    bool toBool() const;
    double toDouble(bool *ok = nullptr) const;
    float toFloat(bool *ok = nullptr) const;
    qreal toReal(bool *ok = nullptr) const;
    QByteArray toByteArray() const;
    QBitArray toBitArray() const;
    QString toString() const;
    QStringList toStringList() const;
    QChar toChar() const;
    QDate toDate() const;
    QTime toTime() const;
    QDateTime toDateTime() const;
    QList<QVariant> toList() const;
    QMap<QString, QVariant> toMap() const;
    QHash<QString, QVariant> toHash() const;
    QPoint toPoint() const;
    QPointF toPointF() const;
    QRect toRect() const;
    QSize toSize() const;
    QSizeF toSizeF() const;
    QLine toLine() const;
    QLineF toLineF() const;
    QRectF toRectF() const;
    QLocale toLocale() const;
#if QT_CONFIG(regularexpression)
    QRegularExpression toRegularExpression() const;
#endif
#if QT_CONFIG(easingcurve)
    QEasingCurve toEasingCurve() const;
#endif
    QUuid toUuid() const;
    QUrl toUrl() const;
    QJsonValue toJsonValue() const;
    QJsonObject toJsonObject() const;
    QJsonArray toJsonArray() const;
    QJsonDocument toJsonDocument() const;
#if QT_CONFIG(itemmodel)
    QModelIndex toModelIndex() const;
    QPersistentModelIndex toPersistentModelIndex() const;
#endif

    void *data();
    const void *constData() const { return d.storage(); }
    inline const void *data() const { return constData(); }

    template <typename T, typename... Args, if_constructible<T, Args...> = true>
    T &emplace(Args&&... args)
    {
        void *ptr = prepareForEmplace(QMetaType::fromType<std::remove_cvref_t<T>>());
        return *new (ptr) std::remove_cvref_t<T>(std::forward<Args>(args)...);
    }

    template<typename T>
    void setValue(T &&avalue)
    {
        using VT = std::remove_cvref_t<T>;
        if constexpr (std::is_same_v<VT, QVariant>) {
            *this = std::forward<T>(avalue);
        } else {
            const QMetaType metaType = QMetaType::fromType<VT>();
            if (isDetached() && d.type() == metaType) {
                *reinterpret_cast<VT *>(const_cast<void *>(constData())) = std::forward<T>(avalue);
                d.is_null = false;
            } else {
                *this = QVariant::fromValue(std::forward<T>(avalue));
            }
        }
    }

    template<typename T> inline T value() const & { return qvariant_cast<T>(*this); }
    template<typename T> inline T value() && { return qvariant_cast<T>(std::move(*this)); }

    template<typename T>
    inline T view()
    {
        T t{};
        QMetaType::view(metaType(), data(), QMetaType::fromType<T>(), &t);
        return t;
    }

    template<typename T>
    bool canView() const { return canView(QMetaType::fromType<T>()); }

    template<typename T>
    static inline QVariant fromValue(T &&value) noexcept(std::is_nothrow_copy_constructible_v<std::remove_cvref_t<T>> && Private::CanUseInternalSpace<std::remove_cvref_t<T>>) requires (std::is_copy_constructible_v<std::remove_cvref_t<T>> && std::is_destructible_v<std::remove_cvref_t<T>>)
    {
        using VT = std::remove_cvref_t<T>;
        if constexpr (std::is_null_pointer_v<VT>) return QVariant::fromMetaType(QMetaType::fromType<std::nullptr_t>());
        else if constexpr (std::is_same_v<VT, QVariant>) return std::forward<T>(value);
        else if constexpr (std::is_same_v<VT, std::monostate>) return QVariant();
        else {
            QMetaType mt = QMetaType::fromType<VT>();
            mt.registerType();
            if constexpr (std::is_rvalue_reference_v<T&&> && std::is_move_constructible_v<VT> && !std::is_const_v<std::remove_reference_t<T>>)
                return moveConstruct(mt, std::addressof(value));
            return copyConstruct(mt, std::addressof(value));
        }
    }

    template<typename... Types>
    static inline QVariant fromStdVariant(const std::variant<Types...> &value)
    {
        if (value.valueless_by_exception()) return QVariant();
        return std::visit([](auto &&arg) { return QVariant::fromValue(arg); }, value);
    }

    static QVariant fromMetaType(QMetaType type, const void *copy = nullptr);
    static QPartialOrdering compare(const QVariant &lhs, const QVariant &rhs);

#ifndef QT_NO_DATASTREAM
    void load(QDataStream &ds);
    void save(QDataStream &ds) const;
#endif

#ifndef QT_NO_DEBUG_STREAM
    QDebug qdebugHelper(QDebug debug) const;
    friend QDebug operator<<(QDebug debug, const QVariant &variant) { return variant.qdebugHelper(debug); }
#endif

private:
    friend bool comparesEqual(const QVariant &a, const QVariant &b) { return a.equals(b); }
    Q_DECLARE_EQUALITY_COMPARABLE_NON_NOEXCEPT(QVariant)
    static QVariant moveConstruct(QMetaType type, void *data);
    static QVariant copyConstruct(QMetaType type, const void *data);
    void *prepareForEmplace(QMetaType type);

    template <typename T> friend T *get_if(QVariant *v) noexcept { if (!v || v->metaType() != QMetaType::fromType<T>()) return nullptr; return static_cast<T*>(v->data()); }
    template <typename T> friend const T *get_if(const QVariant *v) noexcept { if (!v || v->isNull() || v->metaType() != QMetaType::fromType<T>()) return nullptr; return static_cast<const T*>(v->constData()); }

#define Q_MK_GET(cvref) \
    template <typename T> friend T cvref get(QVariant cvref v) { \
        using VT = std::remove_cvref_t<T>; Q_ASSERT(v.metaType() == QMetaType::fromType<VT>()); \
        return static_cast<T cvref>(*get_if<VT>(&v)); \
    }
    Q_MK_GET(&) Q_MK_GET(const &) Q_MK_GET(&&) Q_MK_GET(const &&)
#undef Q_MK_GET

protected:
    Private d;
    void create(QMetaType type, const void *copy);
    bool equals(const QVariant &other) const;
    bool view(int type, void *ptr);
private:
    QVariant(std::in_place_t, QMetaType type);
    QVariant(void *) = delete;
    QVariant(QMetaType::Type) = delete;
    QVariant(Qt::GlobalColor) = delete;
public:
    typedef Private DataPtr;
    inline DataPtr &data_ptr() { return d; }
    inline const DataPtr &data_ptr() const { return d; }
};

inline bool QVariant::isValid() const { return d.type().isValid(QT6_CALL_NEW_OVERLOAD); }
#if QT_CORE_INLINE_IMPL_SINCE(6, 10)
QMetaType QVariant::metaType() const { return d.type(); }
const char *QVariant::typeName() const { return d.type().name(); }
#endif
inline bool QVariant::isDetached() const { return !d.is_shared || d.data.shared->ref.loadRelaxed() == 1; }
inline void swap(QVariant &v1, QVariant &v2) noexcept { v1.swap(v2); }
template<typename T> inline T qvariant_cast(const QVariant &v) {
    const QMetaType target = QMetaType::fromType<T>();
    if (v.metaType() == target) return *static_cast<const T *>(v.constData());
    T t{}; QMetaType::convert(v.metaType(), v.constData(), target, &t); return t;
}

#ifndef QT_NO_DATASTREAM
Q_CORE_EXPORT QDataStream &operator>>(QDataStream &s, QVariant &p);
Q_CORE_EXPORT QDataStream &operator<<(QDataStream &s, const QVariant &p);
#endif

namespace QtPrivate {
class Q_CORE_EXPORT QVariantTypeCoercer
{
public:
    const void *convert(const QVariant &value, const QMetaType &type);
    const void *coerce(const QVariant &value, const QMetaType &type);
private:
    QVariant converted;
};
}

#if QT_DEPRECATED_SINCE(6, 15)
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
template<typename Pointer> class QT_DEPRECATED_VERSION_X_6_15("Use QVariant::Reference instead.") QVariantRef {
private: const Pointer *m_pointer = nullptr;
public:
    explicit QVariantRef(const Pointer *r) : m_pointer(r) {}
    operator QVariant() const;
    QVariantRef &operator=(const QVariant &value);
};

class Q_CORE_EXPORT QT_DEPRECATED_VERSION_X_6_15("Use QVariant::ConstPointer instead.") QVariantConstPointer {
private: QVariant m_variant;
public:
    explicit QVariantConstPointer(QVariant variant);
    QVariant operator*() const;
    const QVariant *operator->() const;
};

template<typename Pointer> class QT_DEPRECATED_VERSION_X_6_15("Use QVariant::Pointer instead.") QVariantPointer {
private: const Pointer *m_pointer = nullptr;
public:
    explicit QVariantPointer(const Pointer *p) : m_pointer(p) {}
    QVariantRef<Pointer> operator*() const { return QVariantRef<Pointer>(m_pointer); }
};
QT_WARNING_POP
#endif

QT_END_NAMESPACE
#endif
