// Copyright (C) 2022 The Qt Company Ltd.
// Copyright (C) 2021 Intel Corporation.
// Copyright (C) 2015 Olivier Goffart <ogoffart@woboq.com>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:critical reason:data-parser


#include "qvariant_p.h"
#include "private/qlocale_p.h"
#include "qmetatype_p.h"
#if QT_CONFIG(itemmodel)
#include "qabstractitemmodel.h"
#endif
#include "qbitarray.h"
#include "qbytearray.h"
#include "qbytearraylist.h"
#include "qcborarray.h"
#include "qcborcommon.h"
#include "qcbormap.h"
#include "qcborvalue.h"
#include "qdatastream.h"
#include "qdatetime.h"
#include "qdebug.h"
#if QT_CONFIG(easingcurve)
#include "qeasingcurve.h"
#endif
#include "qhash.h"
#include "qjsonarray.h"
#include "qjsondocument.h"
#include "qjsonobject.h"
#include "qjsonvalue.h"
#include "qline.h"
#include "qlist.h"
#include "qlocale.h"
#include "qmap.h"
#include "qpoint.h"
#include "qrect.h"
#if QT_CONFIG(regularexpression)
#include "qregularexpression.h"
#endif
#include "qsize.h"
#include "qstring.h"
#include "qstringlist.h"
#include "qurl.h"
#include "quuid.h"
#include <memory>
#include <cmath>
#include <cstring>
#include <utility>

QT_BEGIN_NAMESPACE

using namespace Qt::StringLiterals;

namespace {

static qlonglong qMetaTypeNumberBySize(const QVariant::Private *d)
{
    switch (d->typeInterface()->size) {
    case 1: return d->get<signed char>();
    case 2: return d->get<short>();
    case 4: return d->get<int>();
    case 8: return d->get<qlonglong>();
    }
    Q_UNREACHABLE_RETURN(0);
}

static qlonglong qMetaTypeNumber(const QVariant::Private *d)
{
    using enum QMetaType::Type;
    switch (static_cast<QMetaType::Type>(d->typeInterface()->typeId.loadRelaxed())) {
    case Int: case LongLong: case Char: case SChar: case Short: case Long:
        return qMetaTypeNumberBySize(d);
    case Float: return qRound64(d->get<float>());
    case Double: return qRound64(d->get<double>());
    case QJsonValue: return d->get<QJsonValue>().toDouble();
    case QCborValue: return d->get<QCborValue>().toInteger();
    default: Q_UNREACHABLE_RETURN(0);
    }
}

static qulonglong qMetaTypeUNumber(const QVariant::Private *d)
{
    switch (d->typeInterface()->size) {
    case 1: return d->get<unsigned char>();
    case 2: return d->get<unsigned short>();
    case 4: return d->get<unsigned int>();
    case 8: return d->get<qulonglong>();
    }
    Q_UNREACHABLE_RETURN(0);
}

static std::optional<qlonglong> qConvertToNumber(const QVariant::Private *d, bool allowStringToBool = false)
{
    bool ok;
    const auto id = static_cast<QMetaType::Type>(d->typeInterface()->typeId.loadRelaxed());
    switch (id) {
    case QMetaType::QString: {
        const auto &s = d->get<QString>();
        if (qlonglong l = s.toLongLong(&ok); ok) return l;
        if (allowStringToBool) {
            if (s == "false"_L1 || s == "0"_L1) return 0;
            if (s == "true"_L1 || s == "1"_L1) return 1;
        }
        return std::nullopt;
    }
    case QMetaType::QChar: return d->get<QChar>().unicode();
    case QMetaType::QByteArray:
        if (qlonglong l = d->get<QByteArray>().toLongLong(&ok); ok) return l;
        return std::nullopt;
    case QMetaType::Bool: return qlonglong(d->get<bool>());
    case QMetaType::Double: case QMetaType::Int: case QMetaType::Char:
    case QMetaType::SChar: case QMetaType::Short: case QMetaType::Long:
    case QMetaType::Float: case QMetaType::LongLong:
        return qMetaTypeNumber(d);
    case QMetaType::ULongLong: case QMetaType::UInt: case QMetaType::UChar:
    case QMetaType::Char16: case QMetaType::Char32: case QMetaType::UShort:
    case QMetaType::ULong:
        return qlonglong(qMetaTypeUNumber(d));
    case QMetaType::QCborValue:
        if (auto v = d->get<QCborValue>(); v.isInteger() || v.isDouble()) return qMetaTypeNumber(d);
        break;
    default:
        if (d->typeInterface()->flags & QMetaType::IsEnumeration || id == QMetaType::QCborSimpleType)
            return qMetaTypeNumberBySize(d);
    }
    return std::nullopt;
}

static std::optional<double> qConvertToRealNumber(const QVariant::Private *d)
{
    bool ok;
    const auto id = static_cast<QMetaType::Type>(d->typeInterface()->typeId.loadRelaxed());
    switch (id) {
    case QMetaType::QString:
        if (double val = d->get<QString>().toDouble(&ok); ok) return val;
        return std::nullopt;
    case QMetaType::Double: return d->get<double>();
    case QMetaType::Float: return static_cast<double>(d->get<float>());
    case QMetaType::Float16: return static_cast<double>(d->get<qfloat16>());
    case QMetaType::QCborValue: return d->get<QCborValue>().toDouble();
    case QMetaType::QJsonValue: return d->get<QJsonValue>().toDouble();
    case QMetaType::ULongLong: case QMetaType::UInt: case QMetaType::UChar:
    case QMetaType::Char16: case QMetaType::Char32: case QMetaType::UShort:
    case QMetaType::ULong:
        return static_cast<double>(qMetaTypeUNumber(d));
    default:
        return qConvertToNumber(d).transform([](qlonglong l) { return static_cast<double>(l); });
    }
}

static bool qIsNumericType(uint tp)
{
    static constexpr uint64_t mask = []() {
        uint64_t m = 0;
        m |= (1ULL << QMetaType::QString) | (1ULL << QMetaType::Bool) | (1ULL << QMetaType::Double) |
             (1ULL << QMetaType::Float16) | (1ULL << QMetaType::Float) | (1ULL << QMetaType::Char) |
             (1ULL << QMetaType::Char16) | (1ULL << QMetaType::Char32) | (1ULL << QMetaType::SChar) |
             (1ULL << QMetaType::UChar) | (1ULL << QMetaType::Short) | (1ULL << QMetaType::UShort) |
             (1ULL << QMetaType::Int) | (1ULL << QMetaType::UInt) | (1ULL << QMetaType::Long) |
             (1ULL << QMetaType::ULong) | (1ULL << QMetaType::LongLong) | (1ULL << QMetaType::ULongLong);
        return m;
    }();
    return tp < 64 && (mask & (1ULL << tp));
}

static bool qIsFloatingPoint(uint tp)
{ return tp == QMetaType::Double || tp == QMetaType::Float || tp == QMetaType::Float16; }

static bool canBeNumericallyCompared(const QtPrivate::QMetaTypeInterface *i1, const QtPrivate::QMetaTypeInterface *i2)
{
    if (!i1 || !i2) return false;
    bool n1 = qIsNumericType(i1->typeId.loadRelaxed());
    bool n2 = qIsNumericType(i2->typeId.loadRelaxed());
    if (n1 && n2) return true;
    bool e1 = i1->flags & QMetaType::IsEnumeration;
    bool e2 = i2->flags & QMetaType::IsEnumeration;
    if (e1 && e2) return QMetaType(i1) == QMetaType(i2);
    return (e1 && n2) || (n1 && e2);
}

static int numericTypePromotion(const QtPrivate::QMetaTypeInterface *i1, const QtPrivate::QMetaTypeInterface *i2)
{
    if (qIsFloatingPoint(i1->typeId.loadRelaxed()) || qIsFloatingPoint(i2->typeId.loadRelaxed())) return QMetaType::QReal;
    auto isU = [](uint tp) { return tp == QMetaType::ULongLong || tp == QMetaType::ULong || tp == QMetaType::UInt || tp == QMetaType::Char32; };
    if ((isU(i1->typeId.loadRelaxed()) && i1->size > 4) || (isU(i2->typeId.loadRelaxed()) && i2->size > 4)) return QMetaType::ULongLong;
    if (i1->size > 4 || i2->size > 4) return QMetaType::LongLong;
    if (isU(i1->typeId.loadRelaxed()) || isU(i2->typeId.loadRelaxed())) return QMetaType::UInt;
    return QMetaType::Int;
}

static QPartialOrdering integralCompare(uint pt, const QVariant::Private *d1, const QVariant::Private *d2)
{
    auto l1 = qConvertToNumber(d1, pt == QMetaType::Bool);
    auto l2 = qConvertToNumber(d2, pt == QMetaType::Bool);
    if (!l1 || !l2) return QPartialOrdering::Unordered;
    if (pt == QMetaType::UInt) return Qt::compareThreeWay(uint(*l1), uint(*l2));
    if (pt == QMetaType::LongLong) return Qt::compareThreeWay(qlonglong(*l1), qlonglong(*l2));
    if (pt == QMetaType::ULongLong) return Qt::compareThreeWay(qulonglong(*l1), qulonglong(*l2));
    return Qt::compareThreeWay(int(*l1), int(*l2));
}

static QPartialOrdering numericCompare(const QVariant::Private *d1, const QVariant::Private *d2)
{
    uint pt = numericTypePromotion(d1->typeInterface(), d2->typeInterface());
    if (pt != QMetaType::QReal) return integralCompare(pt, d1, d2);
    auto r1 = qConvertToRealNumber(d1);
    auto r2 = qConvertToRealNumber(d2);
    return (r1 && r2) ? Qt::compareThreeWay(*r1, *r2) : QPartialOrdering::Unordered;
}

static bool qvCanConvertMetaObject(QMetaType f, QMetaType t)
{
    if ((f.flags() & QMetaType::PointerToQObject) && (t.flags() & QMetaType::PointerToQObject)) {
        const QMetaObject *fm = f.metaObject(), *tm = t.metaObject();
        return fm && tm && (fm->inherits(tm) || tm->inherits(fm));
    }
    return false;
}

static QPartialOrdering pointerCompare(const QVariant::Private *d1, const QVariant::Private *d2)
{ return Qt::compareThreeWay(Qt::totally_ordered_wrapper(d1->get<QObject *>()), Qt::totally_ordered_wrapper(d2->get<QObject *>())); }

template<typename Comp>
static QPartialOrdering genericCompare(const QVariant::Private *lhs, const QVariant::Private *rhs, Comp comparator)
{
    const auto lt = lhs->type();
    const auto rt = rhs->type();
    if (lt != rt) {
        if (canBeNumericallyCompared(lt.iface(), rt.iface())) return numericCompare(lhs, rhs);
        if (qvCanConvertMetaObject(lt, rt)) return pointerCompare(lhs, rhs);
        return QPartialOrdering::Unordered;
    }
    if (!lt.isValid()) return QPartialOrdering::Equivalent;
    return comparator(lt, lhs->storage(), rhs->storage());
}

static bool isValidMetaTypeForVariant(const QtPrivate::QMetaTypeInterface *iface, const void *copy)
{
    using namespace QtMetaTypePrivate;
    if (!iface || iface->size == 0) return false;
    if (!isCopyConstructible(iface) || !isDestructible(iface)) return false;
    if (!copy && !isDefaultConstructible(iface)) return false;
    return true;
}

enum CustomConstructMoveOptions { UseCopy, ForceMove };
enum CustomConstructNullabilityOption { MaybeNull, NonNull };

template <CustomConstructMoveOptions moveOption = UseCopy, CustomConstructNullabilityOption nullability = MaybeNull>
static void customConstruct(const QtPrivate::QMetaTypeInterface *iface, QVariant::Private *d,
                            std::conditional_t<moveOption == ForceMove, void *, const void *> copy)
{
    using namespace QtMetaTypePrivate;
    d->is_null = !copy || isInterfaceFor<std::nullptr_t>(iface);
    auto op = [=](void *where) {
        if constexpr (moveOption == ForceMove && nullability == NonNull) moveConstruct(iface, where, copy);
        else construct(iface, where, copy);
    };
    if (QVariant::Private::canUseInternalSpace(iface)) {
        d->is_shared = false;
        if (copy || iface->defaultCtr) op(d->data.data);
    } else {
        d->data.shared = customConstructShared(iface->size, iface->alignment, op);
        d->is_shared = true;
    }
}

static void customClear(QVariant::Private *d)
{
    const auto *iface = d->typeInterface();
    if (!iface) return;
    if (!d->is_shared) QtMetaTypePrivate::destruct(iface, d->data.data);
    else { QtMetaTypePrivate::destruct(iface, d->data.shared->data()); QVariant::PrivateShared::free(d->data.shared); }
}

static QVariant::Private clonePrivate(const QVariant::Private &other)
{
    QVariant::Private d = other;
    if (d.is_shared) d.data.shared->ref.ref();
    else if (const auto *iface = d.typeInterface()) {
        if (Q_LIKELY(d.canUseInternalSpace(iface))) {
            if (iface->copyCtr) QtMetaTypePrivate::copyConstruct(iface, d.data.data, other.data.data);
        } else {
            d.data.shared = QVariant::PrivateShared::create(iface->size, iface->alignment);
            QtMetaTypePrivate::copyConstruct(iface, d.data.shared->data(), other.data.data);
            d.is_shared = true;
        }
    }
    return d;
}
}

QVariant::~QVariant() { if (!d.is_shared || !d.data.shared->ref.deref()) customClear(&d); }
QVariant::QVariant(const QVariant &p) : d(clonePrivate(p.d)) {}
QVariant::QVariant(QMetaType type, const void *copy) : QVariant(fromMetaType(type, copy)) {}
QVariant::QVariant(int val) noexcept : d(std::piecewise_construct_t{}, val) {}
QVariant::QVariant(uint val) noexcept : d(std::piecewise_construct_t{}, val) {}
QVariant::QVariant(qlonglong val) noexcept : d(std::piecewise_construct_t{}, val) {}
QVariant::QVariant(qulonglong val) noexcept : d(std::piecewise_construct_t{}, val) {}
QVariant::QVariant(bool val) noexcept : d(std::piecewise_construct_t{}, val) {}
QVariant::QVariant(double val) noexcept : d(std::piecewise_construct_t{}, val) {}
QVariant::QVariant(float val) noexcept : d(std::piecewise_construct_t{}, val) {}
QVariant::QVariant(QChar val) noexcept : d(std::piecewise_construct_t{}, val) {}
QVariant::QVariant(const QString &val) noexcept : d(std::piecewise_construct_t{}, val) {}
QVariant::QVariant(const QByteArray &val) noexcept : d(std::piecewise_construct_t{}, val) {}
QVariant::QVariant(QLatin1StringView val) : QVariant(QString(val)) {}

QVariant &QVariant::operator=(const QVariant &variant)
{ if (this != &variant) { clear(); d = clonePrivate(variant.d); } return *this; }

void QVariant::detach()
{
    if (!d.is_shared || d.data.shared->ref.loadRelaxed() == 1) return;
    Private dd(d.typeInterface());
    customConstruct<UseCopy, NonNull>(d.typeInterface(), &dd, constData());
    if (!d.data.shared->ref.deref()) customClear(&d);
    d.data.shared = dd.data.shared;
}

void QVariant::clear()
{ if (!d.is_shared || !d.data.shared->ref.deref()) customClear(&d); d = {}; }

bool QVariant::convert(QMetaType targetType)
{
    if (d.type() == targetType) return targetType.isValid();
    QVariant old = std::move(*this);
    create(targetType, nullptr);
    if (!old.canConvert(targetType)) return false;
    if (old.d.is_null && old.d.type().id() != QMetaType::Nullptr) return false;
    bool ok = QMetaType::convert(old.d.type(), old.constData(), targetType, data());
    d.is_null = !ok;
    return ok;
}

bool QVariant::equals(const QVariant &v) const
{ return genericCompare(&d, &v.d, [](QMetaType t, const void *a, const void *b) { return t.equals(a, b) ? QPartialOrdering::Equivalent : QPartialOrdering::Unordered; }) == QPartialOrdering::Equivalent; }

QPartialOrdering QVariant::compare(const QVariant &lhs, const QVariant &rhs)
{ return genericCompare(&lhs.d, &rhs.d, [](QMetaType t, const void *a, const void *b) { return t.compare(a, b); }); }

void *QVariant::data() { detach(); d.is_null = false; return const_cast<void *>(constData()); }

bool QVariant::isNull() const
{
    if (d.is_null || !metaType().isValid()) return true;
    if (metaType().flags() & QMetaType::IsPointer) return d.get<void *>() == nullptr;
    return false;
}

QVariant QVariant::fromMetaType(QMetaType type, const void *copy)
{
    QVariant res;
    type.registerType();
    if (const auto *iface = type.iface(); isValidMetaTypeForVariant(iface, copy)) {
        res.d = Private(iface);
        customConstruct(iface, &res.d, copy);
    }
    return res;
}

QVariant QVariant::moveConstruct(QMetaType type, void *data)
{
    QVariant v; v.d = Private(type.d_ptr);
    customConstruct<ForceMove, NonNull>(type.d_ptr, &v.d, data);
    return v;
}

QVariant QVariant::copyConstruct(QMetaType type, const void *data)
{
    QVariant v; v.d = Private(type.d_ptr);
    customConstruct<UseCopy, NonNull>(type.d_ptr, &v.d, data);
    return v;
}

void *QVariant::prepareForEmplace(QMetaType type)
{
    if (Private::canUseInternalSpace(type.iface())) { clear(); d.packedType = quintptr(type.iface()) >> 2; return d.data.data; }
    QVariant next(std::in_place, type); std::swap(d, next.d); return const_cast<void *>(d.storage());
}

bool QVariant::view(int type, void *ptr)
{ return QMetaType::view(d.type(), data(), QMetaType(type), ptr); }

#ifndef QT_NO_DEBUG_STREAM
QDebug QVariant::qdebugHelper(QDebug dbg) const
{
    QDebugStateSaver saver(dbg);
    dbg.nospace() << "QVariant(";
    if (d.type().id() != QMetaType::UnknownType) {
        dbg << d.type().name() << ", ";
        if (!d.type().debugStream(dbg, storage()) && canConvert<QString>()) dbg << toString();
    } else dbg << "Invalid";
    dbg << ')';
    return dbg;
}
#endif

template <typename T>
inline T qNumVariantToHelper(const QVariant::Private &d, bool *ok)
{
    const auto t = QMetaType::fromType<T>();
    if (d.type() == t) { if (ok) *ok = true; return d.get<T>(); }
    T ret = 0;
    bool s = QMetaType::convert(d.type(), d.storage(), t, &ret);
    if (ok) *ok = s;
    return ret;
}

int QVariant::toInt(bool *ok) const { return qNumVariantToHelper<int>(d, ok); }
uint QVariant::toUInt(bool *ok) const { retur
