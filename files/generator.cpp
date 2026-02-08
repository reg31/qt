#include "generator.h"
#include "cbordevice.h"
#include "outputrevision.h"
#include "utils.h"
#include <QtCore/qmetatype.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qjsonvalue.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qplugin.h>
#include <QtCore/qstringview.h>
#include <QtCore/qtmocconstants.h>

#include <math.h>
#include <stdio.h>

#include <private/qmetaobject_p.h>
#include <private/qplugin_p.h>

QT_BEGIN_NAMESPACE

using namespace QtMiscUtils;

static int nameToBuiltinType(const QByteArray &name)
{
    if (name.isEmpty())
        return 0;

    uint tp = QMetaType::UnknownType;
    if (const QtPrivate::QMetaTypeInterface *iface = QMetaType::fromName(name).iface())
        tp = iface->typeId.loadRelaxed();

#ifndef QT_BOOTSTRAPPED
    if (tp >= uint(QMetaType::User))
        tp = QMetaType::UnknownType;
#endif

    return int(tp);
}

static bool isBuiltinType(const QByteArray &type)
{
    int id = nameToBuiltinType(type);
    return id != QMetaType::UnknownType;
}

constexpr const char *cxxTypeTag(TypeTags t)
{
    if (t & TypeTag::HasEnum) {
        if (t & TypeTag::HasClass)
            return "enum class ";
        if (t & TypeTag::HasStruct)
            return "enum struct ";
        return "enum ";
    }
    if (t & TypeTag::HasClass) return "class ";
    if (t & TypeTag::HasStruct) return "struct ";
    return "";
}

static const char *metaTypeEnumValueString(int type)
 {
#define RETURN_METATYPENAME_STRING(MetaTypeName, MetaTypeId, RealType) \
    case QMetaType::MetaTypeName: return #MetaTypeName;

    switch (type) {
QT_FOR_EACH_STATIC_TYPE(RETURN_METATYPENAME_STRING)
    }
#undef RETURN_METATYPENAME_STRING
    return nullptr;
 }

 Generator::Generator(Moc *moc, const ClassDef *classDef, const QList<QByteArray> &metaTypes,
                      const QHash<QByteArray, QByteArray> &knownQObjectClasses,
                      const QHash<QByteArray, QByteArray> &knownGadgets,
                      const QHash<QByteArray, QByteArray> &hashes,
                      FILE *outfile, bool requireCompleteTypes)
     : parser(moc),
       out(outfile),
       cdef(classDef),
       metaTypes(metaTypes),
       knownQObjectClasses(knownQObjectClasses),
       knownGadgets(knownGadgets),
       hashes(hashes),
       requireCompleteTypes(requireCompleteTypes)
 {
     if (cdef->superclassList.size())
         purestSuperClass = cdef->superclassList.constFirst().classname;
     
     stringCache.reserve(256);
}

static inline qsizetype lengthOfEscapeSequence(const QByteArray &s, qsizetype i)
{
    if (s.at(i) != '\\' || i >= s.size() - 1)
        return 1;
    const qsizetype startPos = i;
    ++i;
    char ch = s.at(i);
    if (ch == 'x') {
        ++i;
        while (i < s.size() && isxdigit(uchar(s.at(i))))
            ++i;
    } else if (isdigit(uchar(ch))) {
        while (i < startPos + 4 && i < s.size() && isdigit(uchar(s.at(i))))
            ++i;
    } else {
        ++i;
    }
    return i - startPos;
}

void Generator::strreg(const QByteArray &s)
{
    if (s.isEmpty())
        return;
    
    if (stringCache.find(s) != stringCache.end())
        return;
    
    int idx = strings.size();
    strings.append(s);
    stringCache[s] = idx;
}

int Generator::stridx(const QByteArray &s)
{
    auto it = stringCache.find(s);
    if (it != stringCache.end())
        return it->second;
    
    Q_ASSERT_X(false, Q_FUNC_INFO, "We forgot to register some strings");
    return -1;
}

QT_END_NAMESPACE
