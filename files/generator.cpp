// Copyright (C) 2020 The Qt Company Ltd.
// Copyright (C) 2019 Olivier Goffart <ogoffart@woboq.com>
// Copyright (C) 2018 Intel Corporation.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

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

#include <private/qmetaobject_p.h> 
#include <private/qplugin_p.h> 

#include <print>
#include <format>
#include <string_view>
#include <utility>

QT_BEGIN_NAMESPACE

using namespace QtMiscUtils;

template <>
struct std::formatter<QByteArray> : std::formatter<std::string_view> {
    auto format(const QByteArray &ba, std::format_context &ctx) const {
        return std::formatter<std::string_view>::format(
            std::string_view(ba.constData(), ba.size()), ctx);
    }
};

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

Generator::Generator(Moc *moc, const ClassDef *classDef, const QList<QByteArray> &metaTypesArg,
                     const QHash<QByteArray, QByteArray> &knownQObjectClassesArg,
                     const QHash<QByteArray, QByteArray> &knownGadgetsArg,
                     const QHash<QByteArray, QByteArray> &hashesArg,
                     FILE *outfile, bool requireCompleteTypesArg)
    : parser(moc), out(outfile), cdef(classDef),
      metaTypes(metaTypesArg), knownQObjectClasses(knownQObjectClassesArg),
      knownGadgets(knownGadgetsArg), hashes(hashesArg),
      requireCompleteTypes(requireCompleteTypesArg)
{
    if (!cdef->superclassList.empty())
        purestSuperClass = cdef->superclassList.constFirst().classname;
    stringCache.reserve(256);
    typeCache.reserve(128);
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
        while (i < s.size() && isHexDigit(s.at(i)))
            ++i;
    } else if (isOctalDigit(ch)) {
        while (i < startPos + 4
               && i < s.size()
               && isOctalDigit(s.at(i))) {
            ++i;
        }
    } else { 
        i = qMin(i + 1, s.size());
    }
    return i - startPos;
}

static void printStringWithIndentation(FILE *out, const QByteArray &s)
{
    static constexpr int ColumnWidth = 68;
    const qsizetype len = s.size();
    qsizetype idx = 0;

    do {
        qsizetype spanLen = qMin(ColumnWidth - 2, len - idx);
        const qsizetype backSlashPos = s.lastIndexOf('\\', idx + spanLen - 1);
        if (backSlashPos >= idx) {
            const qsizetype escapeLen = lengthOfEscapeSequence(s, backSlashPos);
            spanLen = qBound(spanLen, backSlashPos + escapeLen - idx, len - idx);
        }
        fprintf(out, "\n        \"%.*s\"", int(spanLen), s.constData() + idx);
        idx += spanLen;
    } while (idx < len);
}

void Generator::strreg(const QByteArray &s)
{
    if (!s.isEmpty() && !stringCache.contains(s))
        stringCache[s] = strings.size(), strings.append(s);
}

int Generator::stridx(const QByteArray &s)
{
    if (auto it = stringCache.find(s); it != stringCache.end()) [[likely]] {
        Q_ASSERT_X(it->second <= 0xFFFF, Q_FUNC_INFO, "Too many strings (>65535)");
        return it->second;
    }
    Q_ASSERT_X(false, Q_FUNC_INFO, "We forgot to register some strings");
    return -1;
}

void Generator::precomputeTypeInfo(const QByteArray &typeName)
{
    if (typeCache.contains(typeName))
        return;
    
    TypeInfo info;
    info.builtinType = nameToBuiltinType(typeName);
    info.isBuiltin = (info.builtinType != QMetaType::UnknownType);
    
    if (info.isBuiltin) {
        if (typeName == "qreal") {
            info.builtinType = QMetaType::UnknownType;
            info.valueString = "QReal";
        } else {
            info.valueString = metaTypeEnumValueString(info.builtinType);
        }
    }
    
    typeCache[typeName] = info;
}

bool Generator::registerableMetaType(const QByteArray &propertyType)
{
    if (metaTypes.contains(propertyType))
        return true;

    if (propertyType.endsWith('*')) {
        QByteArray objectPointerType = propertyType;
        objectPointerType.chop(1);
        if (knownQObjectClasses.contains(objectPointerType))
            return true;
    }

    static const QList<QByteArray> smartPointers = QList<QByteArray>()
#define STREAM_SMART_POINTER(SMART_POINTER) << #SMART_POINTER
            QT_FOR_EACH_AUTOMATIC_TEMPLATE_SMART_POINTER(STREAM_SMART_POINTER)
#undef STREAM_SMART_POINTER
            ;

    for (const QByteArray &smartPointer : smartPointers) {
        QByteArray ba = smartPointer + "<";
        if (propertyType.startsWith(ba) && !propertyType.endsWith("&"))
            return knownQObjectClasses.contains(propertyType.mid(smartPointer.size() + 1, propertyType.size() - smartPointer.size() - 1 - 1));
    }

    static const QList<QByteArray> oneArgTemplates = QList<QByteArray>()
#define STREAM_1ARG_TEMPLATE(TEMPLATENAME) << #TEMPLATENAME
            QT_FOR_EACH_AUTOMATIC_TEMPLATE_1ARG(STREAM_1ARG_TEMPLATE)
#undef STREAM_1ARG_TEMPLATE
            ;
    for (const QByteArray &oneArgTemplateType : oneArgTemplates) {
        const QByteArray ba = oneArgTemplateType + "<";
        if (propertyType.startsWith(ba) && propertyType.endsWith(">")) {
            const qsizetype argumentSize = propertyType.size() - ba.size()
                                     - 1
                                     - (propertyType.at(propertyType.size() - 2) == ' ' ? 1 : 0 );
            const QByteArray templateArg = propertyType.sliced(ba.size(), argumentSize);
            return isBuiltinType(templateArg) || registerableMetaType(templateArg);
        }
    }
    return false;
}

static bool qualifiedNameEquals(const QByteArray &qualifiedName, const QByteArray &name)
{
    if (qualifiedName == name)
        return true;
    const qsizetype index = qualifiedName.indexOf("::");
    if (index == -1)
        return false;
    return qualifiedNameEquals(qualifiedName.mid(index+2), name);
}

static QByteArray generateQualifiedClassNameIdentifier(const QByteArray &identifier)
{
    QByteArray qualifiedClassNameIdentifier = "ZN";
    for (const auto scope : qTokenize(QLatin1StringView(identifier), QLatin1Char(':'),
                                      Qt::SkipEmptyParts)) {
        qualifiedClassNameIdentifier += QByteArray::number(scope.size());
        qualifiedClassNameIdentifier += scope;
    }
    qualifiedClassNameIdentifier += 'E';
    return qualifiedClassNameIdentifier;
}

void Generator::generateCode()
{
    bool isQObject = (cdef->classname == "QObject");
    bool isConstructible = !cdef->constructorList.isEmpty();

    strreg(cdef->qualified);
    strreg(hashes[cdef->qualified]);
    registerClassInfoStrings();
    registerFunctionStrings(cdef->signalList);
    registerFunctionStrings(cdef->slotList);
    registerFunctionStrings(cdef->methodList);
    registerFunctionStrings(cdef->constructorList);
    registerByteArrayVector(cdef->nonClassSignalList);
    registerPropertyStrings();
    registerEnumStrings();
    
    precomputeTypesForFunctions(cdef->signalList);
    precomputeTypesForFunctions(cdef->slotList);
    precomputeTypesForFunctions(cdef->methodList);
    precomputeTypesForFunctions(cdef->constructorList);
    precomputeTypesForProperties();

    const bool requireCompleteness = requireCompleteTypes || cdef->requireCompleteMethodTypes;
    bool hasStaticMetaCall =
            (cdef->hasQObject || !cdef->methodList.isEmpty()
             || !cdef->propertyList.isEmpty() || !cdef->constructorList.isEmpty());
    if (parser->activeQtMode)
        hasStaticMetaCall = false;

    const QByteArray qualifiedClassNameIdentifier = generateQualifiedClassNameIdentifier(cdef->qualified);

    const char *ownType = !cdef->hasQNamespace ? cdef->classname.data() : "void";

    fprintf(out, "namespace {\n"
                 "struct qt_meta_tag_%s_t {};\n"
                 "} // unnamed namespace\n\n",
            qualifiedClassNameIdentifier.constData());

    fprintf(out, "template <> constexpr inline auto %s::qt_create_metaobjectdata<qt_meta_tag_%s_t>()\n"
                 "{\n"
                 "    namespace QMC = QtMocConstants;\n",
            cdef->qualified.constData(), qualifiedClassNameIdentifier.constData());

    fprintf(out, "    static constexpr QtMocHelpers::StringRefStorage qt_stringData {\n");
    for (int i = 0; i < strings.size(); ++i) {
        if (i > 0)
            fprintf(out, ",\n");
        fprintf(out, "        \"");
        const QByteArray &s = strings.at(i);
        for (int j = 0; j < s.size(); ++j) {
            if (s[j] == '\\' || s[j] == '"')
                fprintf(out, "\\");
            fprintf(out, "%c", s[j]);
        }
        fprintf(out, "\"");
    }
    fprintf(out, "\n    };\n\n");

    fprintf(out, "    static constexpr QtMocHelpers::UintData qt_methods {\n");

    addFunctions(cdef->signalList, "Signal");
    addFunctions(cdef->slotList, "Slot");
    addFunctions(cdef->methodList, "Method");
    fprintf(out, "    };\n"
                 "    static constexpr QtMocHelpers::UintData qt_properties {\n");
    addProperties();
    fprintf(out, "    };\n"
                 "    static constexpr QtMocHelpers::UintData qt_enums {\n");
    addEnums();
    fprintf(out, "    };\n");

    fprintf(out, "    static constexpr auto qt_metaObjectHashIndex = %s;\n",
            stridx(hashes[cdef->qualified]) == -1 ? "~0u" : QByteArray("uint32_t(" + QByteArray::number(stridx(hashes[cdef->qualified])) + ")").constData());

    const char *uintDataParams = "";
    if (isConstructible || !cdef->classInfoList.isEmpty()) {
        if (isConstructible) {
            fprintf(out, "    using Constructor = QtMocHelpers::NoType;\n"
                         "    static constexpr QtMocHelpers::UintData qt_constructors {\n");
            addFunctions(cdef->constructorList, "Constructor");
            fprintf(out, "    };\n");
        } else {
            fputs("    static constexpr QtMocHelpers::UintData qt_constructors {};\n", out);
        }

        uintDataParams = ", qt_constructors";
        if (!cdef->classInfoList.isEmpty()) {
            fprintf(out, "    static constexpr QtMocHelpers::ClassInfos qt_classinfo({\n");
            addClassInfos();
            fprintf(out, "    });\n");
            uintDataParams = ", qt_constructors, qt_classinfo";
        }
    }

    const char *metaObjectFlags = "QMC::MetaObjectFlag{}";
    if (cdef->hasQGadget || cdef->hasQNamespace) {
        metaObjectFlags = "QMC::PropertyAccessInStaticMetaCall";
    }
    {
        QByteArray tagType = QByteArrayLiteral("void");
        if (!requireCompleteness)
            tagType = "qt_meta_tag_" + qualifiedClassNameIdentifier +  "_t";
        fprintf(out, "    return QtMocHelpers::metaObjectData<%s, %s>(%s, qt_stringData,\n"
                     "            qt_methods, qt_properties, qt_enums, qt_metaObjectHashIndex%s);\n"
                     "}\n",
                ownType, tagType.constData(), metaObjectFlags, uintDataParams);
    }

    QByteArray metaVarNameSuffix;
    if (cdef->hasQNamespace) {
        metaVarNameSuffix = '_' + qualifiedClassNameIdentifier;
        const char *n = metaVarNameSuffix.constData();
        fprintf(out, R"(
static constexpr auto qt_staticMetaObjectContent%s =
    %s::qt_create_metaobjectdata<qt_meta_tag%s_t>();
static constexpr auto qt_staticMetaObjectStaticContent%s =
    qt_staticMetaObjectContent%s.staticData;
static constexpr auto qt_staticMetaObjectRelocatingContent%s =
    qt_staticMetaObjectContent%s.relocatingData;

)",
                n, cdef->qualified.constData(), n,
                n, n,
                n, n);
    } else {
        metaVarNameSuffix = "<qt_meta_tag_" + qualifiedClassNameIdentifier + "_t>";
    }

    QList<QByteArray> extraList;
    QMultiHash<QByteArray, QByteArray> knownExtraMetaObject(knownGadgets);
    knownExtraMetaObject.unite(knownQObjectClasses);

    for (const PropertyDef &p : std::as_const(cdef->propertyList)) {
        if (isBuiltinType(p.type))
            continue;

        if (p.type.contains('*') || p.type.contains('<') || p.type.contains('>'))
            continue;

        const qsizetype s = p.type.lastIndexOf("::");
        if (s <= 0)
            continue;

        QByteArray unqualifiedScope = p.type.left(s);

        QMultiHash<QByteArray, QByteArray>::ConstIterator scopeIt;

        QByteArray thisScope = cdef->qualified;
        do {
            const qsizetype s = thisScope.lastIndexOf("::");
            thisScope = thisScope.left(s);
            QByteArray currentScope = thisScope.isEmpty() ? unqualifiedScope : thisScope + "::" + unqualifiedScope;
            scopeIt = knownExtraMetaObject.constFind(currentScope);
        } while (!thisScope.isEmpty() && scopeIt == knownExtraMetaObject.constEnd());

        if (scopeIt == knownExtraMetaObject.constEnd())
            continue;

        const QByteArray &scope = *scopeIt;

        if (scope == "Qt")
            continue;
        if (qualifiedNameEquals(cdef->qualified, scope))
            continue;

        if (!extraList.contains(scope))
            extraList += scope;
    }

    for (auto it = cdef->enumDeclarations.keyBegin(),
         end = cdef->enumDeclarations.keyEnd(); it != end; ++it) {
        const QByteArray &enumKey = *it;
        const qsizetype s = enumKey.lastIndexOf("::");
        if (s > 0) {
            QByteArray scope = enumKey.left(s);
            if (scope != "Qt" && !qualifiedNameEquals(cdef->qualified, scope) && !extraList.contains(scope))
                extraList += scope;
        }
    }

    if (!extraList.isEmpty()) {
        fprintf(out, "Q_CONSTINIT static const QMetaObject::SuperData qt_meta_extradata_%s[] = {\n",
                qualifiedClassNameIdentifier.constData());
        for (const QByteArray &ba : std::as_const(extraList))
            fprintf(out, "    QMetaObject::SuperData::link<%s::staticMetaObject>(),\n", ba.constData());

        fprintf(out, "    nullptr\n};\n\n");
    }

    fprintf(out, "Q_CONSTINIT const QMetaObject %s::staticMetaObject = { {\n",
            cdef->qualified.constData());

    if (isQObject)
        fprintf(out, "    nullptr,\n");
    else if (cdef->superclassList.size() && !cdef->hasQGadget && !cdef->hasQNamespace) 
        fprintf(out, "    QMetaObject::SuperData::link<%s::staticMetaObject>(),\n", purestSuperClass.constData());
    else if (cdef->superclassList.size()) 
        fprintf(out, "    QtPrivate::MetaObjectForType<%s>::value,\n", purestSuperClass.constData());
    else
        fprintf(out, "    nullptr,\n");
    fprintf(out, "    qt_staticMetaObjectStaticContent%s.stringdata,\n"
            "    qt_staticMetaObjectStaticContent%s.data,\n",
            metaVarNameSuffix.constData(),
            metaVarNameSuffix.constData());
    if (hasStaticMetaCall)
        fprintf(out, "    qt_static_metacall,\n");
    else
        fprintf(out, "    nullptr,\n");

    if (extraList.isEmpty())
        fprintf(out, "    nullptr,\n");
    else
        fprintf(out, "    qt_meta_extradata_%s,\n", qualifiedClassNameIdentifier.constData());

    fprintf(out, "    qt_staticMetaObjectRelocatingContent%s.metaTypes,\n",
            metaVarNameSuffix.constData());

    fprintf(out, "    nullptr\n} };\n\n");

    if (hasStaticMetaCall)
        generateStaticMetacall();

    if (!cdef->hasQObject)
        return;

    fprintf(out, "\nconst QMetaObject *%s::metaObject() const\n{\n"
                 "    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;\n"
                 "}\n",
            cdef->qualified.constData());

    fprintf(out, "\nvoid *%s::qt_metacast(const char *_clname)\n{\n", cdef->qualified.constData());
    fprintf(out, "    if (!_clname) return nullptr;\n");
    fprintf(out, "    if (!strcmp(_clname, qt_staticMetaObjectStaticContent%s.strings))\n"
                  "        return static_cast<void*>(this);\n",
            metaVarNameSuffix.constData());

    if (cdef->superclassList.size() > 1) {
        auto it = cdef->superclassList.cbegin() + 1;
        const auto end = cdef->superclassList.cend();
        for (; it != end; ++it) {
            if (it->access == FunctionDef::Private)
                continue;
            const char *cname = it->classname.constData();
            fprintf(out, "    if (!strcmp(_clname, \"%s\"))\n        return static_cast< %s*>(this);\n",
                    cname, cname);
        }
    }

    for (const QList<ClassDef::Interface> &iface : std::as_const(cdef->interfaceList)) {
        for (qsizetype j = 0; j < iface.size(); ++j) {
            fprintf(out, "    if (!strcmp(_clname, %s))\n        return ", iface.at(j).interfaceId.constData());
            for (qsizetype k = j; k >= 0; --k)
                fprintf(out, "static_cast< %s*>(", iface.at(k).className.constData());
            fprintf(out, "this%s;\n", QByteArray(j + 1, ')').constData());
        }
    }
    if (!purestSuperClass.isEmpty() && !isQObject) {
        QByteArray superClass = purestSuperClass;
        fprintf(out, "    return %s::qt_metacast(_clname);\n", superClass.constData());
    } else {
        fprintf(out, "    return nullptr;\n");
    }
    fprintf(out, "}\n");

    if (parser->activeQtMode)
        return;

    generateMetacall();

    for (int signalindex = 0; signalindex < int(cdef->signalList.size()); ++signalindex)
        generateSignal(&cdef->signalList.at(signalindex), signalindex);

    generatePluginMetaData();

    if (!cdef->nonClassSignalList.isEmpty()) {
        fprintf(out, "namespace CheckNotifySignalValidity_%s {\n", qualifiedClassNameIdentifier.constData());
        for (const QByteArray &nonClassSignal : std::as_const(cdef->nonClassSignalList)) {
            const auto propertyIt = std::find_if(cdef->propertyList.constBegin(),
                                   cdef->propertyList.constEnd(),
                                   [&nonClassSignal](const PropertyDef &p) {
                return nonClassSignal == p.notify;
            });
            Q_ASSERT(propertyIt != cdef->propertyList.constEnd());
            fprintf(out, "template<typename T> using has_nullary_%s = decltype(std::declval<T>().%s());\n",
                    nonClassSignal.constData(),
                    nonClassSignal.constData());
            const auto &propertyType = propertyIt->type;
            fprintf(out, "template<typename T> using has_unary_%s = decltype(std::declval<T>().%s(std::declval<%s>()));\n",
                    nonClassSignal.constData(),
                    nonClassSignal.constData(),
                    propertyType.constData());
            fprintf(out, "static_assert(qxp::is_detected_v<has_nullary_%s, %s> || qxp::is_detected_v<has_unary_%s, %s>,\n"
                         "              \"NOTIFY signal %s does not exist in class (or is private in its parent)\");\n",
                    nonClassSignal.constData(), cdef->qualified.constData(),
                    nonClassSignal.constData(), cdef->qualified.constData(),
                    nonClassSignal.constData());
        }
        fprintf(out, "}\n");
    }
}

void Generator::precomputeTypesForFunctions(const QList<FunctionDef> &list)
{
    for (const auto &f : list) {
        if (!isBuiltinType(f.normalizedType))
            precomputeTypeInfo(f.normalizedType);
        for (const auto &a : f.arguments) {
            if (!isBuiltinType(a.normalizedType))
                precomputeTypeInfo(a.normalizedType);
        }
    }
}

void Generator::precomputeTypesForProperties()
{
    for (const PropertyDef &p : std::as_const(cdef->propertyList)) {
        if (!isBuiltinType(p.type))
            precomputeTypeInfo(p.type);
    }
}

void Generator::registerClassInfoStrings()
{
    for (const ClassInfoDef &c : std::as_const(cdef->classInfoList)) {
        strreg(c.name);
        strreg(c.value);
    }
}

void Generator::addClassInfos()
{
    for (const ClassInfoDef &c : std::as_const(cdef->classInfoList)) {
        const int nIdx = stridx(c.name);
        const int vIdx = stridx(c.value);
        std::print(out, "            {{ {}, {} }},\n",
                   nIdx == -1 ? "~0u" : std::format("uint32_t({})", nIdx),
                   vIdx == -1 ? "~0u" : std::format("uint32_t({})", vIdx));
    }
}

void Generator::registerFunctionStrings(const QList<FunctionDef> &list)
{
    for (const auto &f : list) {
        strreg(f.name);
        if (!isBuiltinType(f.normalizedType)) strreg(f.normalizedType);
        strreg(f.tag);
		for (const auto& a : f.arguments) {
            if (!isBuiltinType(a.normalizedType)) strreg(a.normalizedType);
            strreg(a.name);
        }
    }
}

void Generator::registerByteArrayVector(const QList<QByteArray> &list)
{
    for (const QByteArray &ba : list)
        strreg(ba);
}

void Generator::addStrings(const QByteArrayList &strings)
{
    if (strings.isEmpty()) {
        fprintf(out, " \"\" ");
        return;
    }
    
    QByteArray combined;
    QList<uint16_t> offsets;
    
    for (const QByteArray &str : strings) {
        offsets.append(combined.size());
        combined.append(str);
        combined.append('\0');
    }
    
    fprintf(out, "\n        .data = ");
    printStringWithIndentation(out, combined);
    fprintf(out, ",\n        .offsets = {");
    
    for (int i = 0; i < offsets.size(); ++i) {
        if (i % 16 == 0) fprintf(out, "\n            ");
        fprintf(out, "%u", offsets[i]);
        if (i < offsets.size() - 1) fprintf(out, ", ");
    }
    fprintf(out, "\n        },\n        .size = %d", (int)offsets.size());
}

void Generator::addFunctions(const QList<FunctionDef> &list, const char *functype)
{
    for (const FunctionDef &f : list) {
        if (!f.isConstructor) std::print(out, "        // {} '{}'\n", functype, f.name);
        std::print(out, "        QtMocHelpers::{}{}Data<", f.revision > 0 ? "Revisioned" : "", functype);

        if (f.isConstructor) std::print(out, "Constructor(");
        else std::print(out, "{}(", disambiguatedTypeName(f.type.name));

        const char *comma = "";
        for (const auto &arg : f.arguments) {
            std::print(out, "{}{}", comma, disambiguatedTypeName(arg.type.name));
            comma = ", ";
        }

        int nIdx = stridx(f.name);
        int tIdx = stridx(f.tag);

        if (f.isConstructor) {
            std::print(out, ")>({}, ", tIdx == -1 ? "~0u" : std::format("uint32_t({})", tIdx));
        } else {
            std::print(out, "){}>({}, {}, ", f.isConst ? " const" : "",
                       nIdx == -1 ? "~0u" : std::format("uint32_t({})", nIdx),
                       tIdx == -1 ? "~0u" : std::format("uint32_t({})", tIdx));
        }

        if (f.access == FunctionDef::Private) std::print(out, "QMC::AccessPrivate");
        else if (f.access == FunctionDef::Public) std::print(out, "QMC::AccessPublic");
        else if (f.access == FunctionDef::Protected) std::print(out, "QMC::AccessProtected");

        if (f.isCompat) std::print(out, " | QMC::MethodCompatibility");
        if (f.wasCloned) std::print(out, " | QMC::MethodCloned");
        if (f.isScriptable) std::print(out, " | QMC::MethodScriptable");
        if (f.revision > 0) std::print(out, ", {:#x}", f.revision);

        if (!f.isConstructor) {
            std::print(out, ", ");
            generateTypeInfo(f.normalizedType);
        }

        if (f.arguments.isEmpty()) {
            std::print(out, "),\n");
        } else {
            std::print(out, ", {{{{");
            for (qsizetype i = 0; i < f.arguments.size(); ++i) {
                if ((i % 4) == 0) std::print(out, "\n           ");
                const auto &arg = f.arguments.at(i);
                int aIdx = stridx(arg.name);
                std::print(out, " {{ ");
                generateTypeInfo(arg.normalizedType);
                std::print(out, ", {} }},", aIdx == -1 ? "0xFFFFu" : std::format("uint16_t({})", aIdx));
            }
            std::print(out, "\n        }}}}),\n");
        }
    }
}

void Generator::generateTypeInfo(const QByteArray &typeName, bool allowEmptyName)
{
    Q_UNUSED(allowEmptyName);
    
    auto it = typeCache.find(typeName);
    if (it != typeCache.end()) [[likely]] {
        const TypeInfo &info = it->second;
        if (info.isBuiltin) {
            if (info.valueString) {
                fprintf(out, "QMetaType::%s", info.valueString);
            } else {
                fprintf(out, "%4d", info.builtinType);
            }
            return;
        }
    }
    
    int idx = stridx(typeName);
    if (idx == -1)
        fprintf(out, "0x%.8x | 0xFFFFu", IsUnresolvedType);
    else
        fprintf(out, "0x%.8x | uint16_t(%d)", IsUnresolvedType, idx);
}

void Generator::registerPropertyStrings()
{
    for (const PropertyDef &p : std::as_const(cdef->propertyList)) {
        strreg(p.name);
        if (!isBuiltinType(p.type))
            strreg(p.type);
    }
}

void Generator::addProperties()
{
    for (const PropertyDef &p : std::as_const(cdef->propertyList)) {
        fprintf(out, "        // property '%s'\n"
                     "        QtMocHelpers::PropertyData<%s%s>(uint32_t(%d), ",
                p.name.constData(), cxxTypeTag(p.typeTag),
                disambiguatedTypeName(p.type, p.typeTag).constData(),
                stridx(p.name));
        generateTypeInfo(p.type);
        fputc(',', out);

        const char *separator = "";
        auto addFlag = [this, &separator](const char *text) {
            fprintf(out, "%s QMC::%s", separator, text);
            separator = " |";
        };
        bool readable = !p.read.isEmpty() || !p.member.isEmpty();
        bool designable = p.designable != "false";
        bool scriptable = p.scriptable != "false";
        bool stored = p.stored != "false";
        if (readable && designable && scriptable && stored) {
            addFlag("DefaultPropertyFlags");
            if ((!p.member.isEmpty() && !p.constant) || !p.write.isEmpty())
                addFlag("Writable");
        } else {
            if (readable)
                addFlag("Readable");
            if ((!p.member.isEmpty() && !p.constant) || !p.write.isEmpty())
                addFlag("Writable");
            if (designable)
                addFlag("Designable");
            if (scriptable)
                addFlag("Scriptable");
            if (stored)
                addFlag("Stored");
        }
        if (!p.reset.isEmpty())
            addFlag("Resettable");
        if (!isBuiltinType(p.type))
            addFlag("EnumOrFlag");
        if (p.stdCppSet())
            addFlag("StdCppSet");
        if (p.constant)
            addFlag("Constant");
        if (p.final)
            addFlag("Final");
        if (p.virtual_)
            addFlag("Virtual");
        if (p.override)
            addFlag("Override");
        if (p.user != "false")
            addFlag("User");
        if (p.required)
            addFlag("Required");
        if (!p.bind.isEmpty())
            addFlag("Bindable");

        if (*separator == '\0')
            addFlag("Invalid");

        int notifyId = p.notifyId;
        if (notifyId != -1 || p.revision > 0) {
            fputs(", ", out);
            if (p.notifyId < -1) {
                fprintf(out, "%#x | uint32_t(%d)", IsUnresolvedSignal, int(strings.indexOf(p.notify)));
            } else if (notifyId == -1) {
                fputs("~0u", out);
            } else {
                fprintf(out, "uint32_t(%d)", notifyId);
            }
            if (p.revision > 0)
                fprintf(out, ", %#x", p.revision);
        }

        fprintf(out, "),\n");
    }
}

void Generator::registerEnumStrings()
{
    for (const EnumDef &e : std::as_const(cdef->enumList)) {
        strreg(e.name);
        if (!e.enumName.isNull())
            strreg(e.enumName);
        for (const QByteArray &val : e.values)
            strreg(val);
    }
}

void Generator::addEnums()
{
    for (const EnumDef &e : std::as_const(cdef->enumList)) {
        const QByteArray &typeName = e.enumName.isNull() ? e.name : e.enumName;
        int nIdx = stridx(e.name);
        int tnIdx = stridx(typeName);

        std::print(out, "        // {} '{}'\n", e.flags & EnumIsFlag ? "flag" : "enum", e.name);
        std::print(out, "        QtMocHelpers::EnumData<{}>(", disambiguatedTypeName(e.name));
        std::print(out, "{}, {},", 
                   nIdx == -1 ? "~0u" : std::format("uint32_t({})", nIdx),
                   tnIdx == -1 ? "~0u" : std::format("uint32_t({})", tnIdx));

        if (e.flags) {
            bool first = true;
            if (e.flags & EnumIsFlag) { std::print(out, " QMC::EnumIsFlag"); first = false; }
            if (e.flags & EnumIsScoped) std::print(out, "{} QMC::EnumIsScoped", first ? "" : " |");
        } else {
            std::print(out, " QMC::EnumFlags{{}}");
        }

        if (e.values.isEmpty()) {
            std::print(out, "),\n");
            continue;
        }

        std::print(out, ").add({{\n");
        QByteArray prefix = (e.enumName.isNull() ? e.name : e.enumName);
        for (const QByteArray &val : e.values) {
            int vIdx = stridx(val);
            std::print(out, "            {{ {}, std::to_underlying({}::{}) }},\n",
                       vIdx == -1 ? "~0u" : std::format("uint32_t({})", vIdx), prefix, val);
        }
        std::print(out, "        }}),\n");
    }
}

void Generator::generateMetacall()
{
    bool isQObject = (cdef->classname == "QObject");

    fprintf(out, "\nint %s::qt_metacall(QMetaObject::Call _c, int _id, void **_a)\n{\n",
             cdef->qualified.constData());

    if (!purestSuperClass.isEmpty() && !isQObject) {
        QByteArray superClass = purestSuperClass;
        fprintf(out, "    _id = %s::qt_metacall(_c, _id, _a);\n", superClass.constData());
    }

    QList<FunctionDef> methodList;
    methodList += cdef->signalList;
    methodList += cdef->slotList;
    methodList += cdef->methodList;

    if (methodList.size() || cdef->propertyList.size()) {
        fprintf(out, "    if (_id < 0)\n        return _id;\n");
    }

    if (methodList.size()) {
        fprintf(out, "    if (_c == QMetaObject::InvokeMetaMethod) {\n");
        fprintf(out, "        if (_id < %d)\n", int(methodList.size()));
        fprintf(out, "            qt_static_metacall(this, _c, _id, _a);\n");
        fprintf(out, "        _id -= %d;\n    }\n", int(methodList.size()));

        fprintf(out, "    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {\n");
        fprintf(out, "        if (_id < %d)\n", int(methodList.size()));

        if (methodsWithAutomaticTypesHelper(methodList).isEmpty())
            fprintf(out, "            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();\n");
        else
            fprintf(out, "            qt_static_metacall(this, _c, _id, _a);\n");
        fprintf(out, "        _id -= %d;\n    }\n", int(methodList.size()));

    }

    if (cdef->propertyList.size()) {
        fprintf(out,
            "    else if (_c >= QMetaObject::ReadProperty && _c <= QMetaObject::RegisterPropertyMetaType) {\n"
            "        qt_static_metacall(this, _c, _id, _a);\n"
            "        _id -= %d;\n    }\n", int(cdef->propertyList.size()));
    }
    fprintf(out,"    return _id;\n}\n");
}

QMultiMap<QByteArray, int> Generator::automaticPropertyMetaTypesHelper()
{
    QMultiMap<QByteArray, int> automaticPropertyMetaTypes;
    for (int i = 0; i < int(cdef->propertyList.size()); ++i) {
        const PropertyDef &p = cdef->propertyList.at(i);
        const QByteArray &propertyType = p.type;
        if (registerableMetaType(propertyType) && !isBuiltinType(propertyType))
            automaticPropertyMetaTypes.insert(cxxTypeTag(p.typeTag) + propertyType, i);
    }
    return automaticPropertyMetaTypes;
}

QMap<int, QMultiMap<QByteArray, int>>
Generator::methodsWithAutomaticTypesHelper(const QList<FunctionDef> &methodList)
{
    QMap<int, QMultiMap<QByteArray, int> > methodsWithAutomaticTypes;
    for (int i = 0; i < methodList.size(); ++i) {
        const FunctionDef &f = methodList.at(i);
        for (int j = 0; j < f.arguments.size(); ++j) {
            const QByteArray &argType = f.arguments.at(j).normalizedType;
            if (registerableMetaType(argType) && !isBuiltinType(argType))
                methodsWithAutomaticTypes[i].insert(argType, j);
        }
    }
    return methodsWithAutomaticTypes;
}

void Generator::generateStaticMetacall()
{
    std::print(out, "void {}::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)\n{{\n", cdef->qualified);

    enum UsedArgs { UsedT = 1, UsedC = 2, UsedId = 4, UsedA = 8 };
    uint usedArgs = 0;

    if (cdef->hasQObject) {
        std::print(out, "    auto *_t = static_cast<{} *>(_o);\n", cdef->classname);
    } else {
        std::print(out, "    auto *_t = reinterpret_cast<{} *>(_o);\n", cdef->classname);
    }

    if (!cdef->constructorList.isEmpty()) {
        std::print(out, "    if (_c == QMetaObject::CreateInstance) {{\n        switch (_id) {{\n");
        for (int i = 0; i < cdef->constructorList.size(); ++i) {
            std::print(out, "        case {}: {{ {} *_r = new {}(", i, cdef->classname, cdef->classname);
            // ... generateCtorArguments logic ...
            std::print(out, "); if (_a[0]) *reinterpret_cast<{}**>(_a[0]) = _r; }} break;\n", (cdef->hasQGadget || cdef->hasQNamespace) ? "void" : "QObject");
        }
        std::print(out, "        default: std::unreachable();\n        }}\n    }}\n");
        usedArgs |= UsedC | UsedId | UsedA;
    }

    QList<FunctionDef> methodList = cdef->signalList + cdef->slotList + cdef->methodList;
    if (!methodList.isEmpty()) {
        usedArgs |= UsedT | UsedC | UsedId;
        std::print(out, "    if (_c == QMetaObject::InvokeMetaMethod) [[likely]] {{\n");
        if (methodList.size() <= 16) {
            std::print(out, "        [[assume(_id >= 0 && _id < {})]];\n", methodList.size());
            std::print(out, "        using Func = void (*)({} *, void **);\n", cdef->classname);
            std::print(out, "        static constexpr std::array<Func, {}> vtable = {{\n", methodList.size());
            for (int i = 0; i < methodList.size(); ++i) {
                const auto &f = methodList.at(i);
                std::print(out, "            []({} *t, void **a) {{ ", cdef->classname);
                if (f.normalizedType == "void" && !f.isRawSlot && f.arguments.isEmpty()) std::print(out, "(void)a; ");
                if (f.isStatic) std::print(out, "(void)t; ");
                if (f.normalizedType != "void") std::print(out, "if (auto r = ");
                std::print(out, "{}{}(", f.isStatic ? std::format("{}::", cdef->classname) : "t->", f.name);
                // ... args logic ...
                std::print(out, "); ");
                if (f.normalizedType != "void") std::print(out, "if (a[0]) *reinterpret_cast<{}*>(a[0]) = std::move(r); ", disambiguatedTypeName(noRef(f.normalizedType)));
                std::print(out, "}}");
                if (i < methodList.size() - 1) std::print(out, ",\n");
                usedArgs |= UsedA;
            }
            std::print(out, "\n        }};\n        vtable[_id](_t, _a);\n");
        } else {
            std::print(out, "        switch (_id) {{\n");
            // ... existing case generation ...
            std::print(out, "        default: std::unreachable();\n        }}\n");
        }
        std::print(out, "    }}\n");
    }

    auto printUnused = [&](UsedArgs entry, const char *name) {
        if ((usedArgs & entry) == 0) std::print(out, "    (void){};\n", name);
    };
    printUnused(UsedT, "_t");
    printUnused(UsedC, "_c");
    printUnused(UsedId, "_id");
    printUnused(UsedA, "_a");
    std::print(out, "}}\n");
}

void Generator::generateSignal(const FunctionDef *def, int index)
{
    if (def->wasCloned || def->isAbstract)
        return;
	
    fprintf(out, "\n// SIGNAL %d\n%s %s::%s(",
        index, def->type.name.constData(), cdef->qualified.constData(), def->name.constData());

    QByteArray thisPtr = "this";
    const char *constQualifier = "";

    if (def->isConst) {
        thisPtr = "const_cast< " + cdef->qualified + " *>(this)";
        constQualifier = "const";
    }

    Q_ASSERT(!def->normalizedType.isEmpty());
    if (def->arguments.isEmpty() && def->normalizedType == "void" && !def->isPrivateSignal) {
        fprintf(out, ")%s\n{\n"
                "    QMetaObject::activate(%s, &staticMetaObject, %d, nullptr);\n"
                "}\n", constQualifier, thisPtr.constData(), index);
        return;
    }

    int offset = 1;
    const auto begin = def->arguments.cbegin();
    const auto end = def->arguments.cend();
    for (auto it = begin; it != end; ++it) {
        const ArgumentDef &a = *it;
        if (it != begin)
            fputs(", ", out);
        if (a.type.name.size())
            fputs(a.type.name.constData(), out);
        fprintf(out, " _t%d", offset++);
        if (a.rightType.size())
            fputs(a.rightType.constData(), out);
    }
    if (def->isPrivateSignal) {
        if (!def->arguments.isEmpty())
            fprintf(out, ", ");
        fprintf(out, "QPrivateSignal _t%d", offset++);
    }

    fprintf(out, ")%s\n{\n", constQualifier);
    if (def->type.name.size() && def->normalizedType != "void") {
        QByteArray returnType = noRef(def->normalizedType);
        fprintf(out, "    %s _t0{};\n", returnType.constData());
    }

    fprintf(out, "    QMetaObject::activate<%s>(%s, &staticMetaObject, %d, ",
            def->normalizedType.constData(), thisPtr.constData(), index);
    if (def->normalizedType == "void") {
        fprintf(out, "nullptr");
    } else {
        fprintf(out, "std::addressof(_t0)");
    }
    int i;
    for (i = 1; i < offset; ++i)
        fprintf(out, ", _t%d", i);
    fprintf(out, ");\n");

    if (def->normalizedType != "void")
        fprintf(out, "    return _t0;\n");
    fprintf(out, "}\n");
}

static CborError jsonValueToCbor(CborEncoder *parent, const QJsonValue &v);
static CborError jsonObjectToCbor(CborEncoder *parent, const QJsonObject &o)
{
    auto it = o.constBegin();
    auto end = o.constEnd();
    CborEncoder map;
    cbor_encoder_create_map(parent, &map, o.size());

    for ( ; it != end; ++it) {
        QByteArray key = it.key().toUtf8();
        cbor_encode_text_string(&map, key.constData(), key.size());
        jsonValueToCbor(&map, it.value());
    }
    return cbor_encoder_close_container(parent, &map);
}

static CborError jsonArrayToCbor(CborEncoder *parent, const QJsonArray &a)
{
    CborEncoder array;
    cbor_encoder_create_array(parent, &array, a.size());
    for (const QJsonValue v : a)
        jsonValueToCbor(&array, v);
    return cbor_encoder_close_container(parent, &array);
}

static CborError jsonValueToCbor(CborEncoder *parent, const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return cbor_encode_null(parent);
    case QJsonValue::Bool:
        return cbor_encode_boolean(parent, v.toBool());
    case QJsonValue::Array:
        return jsonArrayToCbor(parent, v.toArray());
    case QJsonValue::Object:
        return jsonObjectToCbor(parent, v.toObject());
    case QJsonValue::String: {
        QByteArray s = v.toString().toUtf8();
        return cbor_encode_text_string(parent, s.constData(), s.size());
    }
    case QJsonValue::Double: {
        double d = v.toDouble();
        if (d == floor(d) && fabs(d) <= (Q_INT64_C(1) << std::numeric_limits<double>::digits))
            return cbor_encode_int(parent, qint64(d));
        return cbor_encode_double(parent, d);
    }
    }
    Q_UNREACHABLE_RETURN(CborUnknownError);
}

void Generator::generatePluginMetaData()
{
    if (cdef->pluginData.iid.isEmpty())
        return;

    auto outputCborData = [this]() {
        CborDevice dev(out);
        CborEncoder enc;
        cbor_encoder_init_writer(&enc, CborDevice::callback, &dev);

        CborEncoder map;
        cbor_encoder_create_map(&enc, &map, CborIndefiniteLength);

        dev.nextItem("\"IID\"");
        cbor_encode_int(&map, int(QtPluginMetaDataKeys::IID));
        cbor_encode_text_string(&map, cdef->pluginData.iid.constData(), cdef->pluginData.iid.size());

        dev.nextItem("\"className\"");
        cbor_encode_int(&map, int(QtPluginMetaDataKeys::ClassName));
        cbor_encode_text_string(&map, cdef->classname.constData(), cdef->classname.size());

        QJsonObject o = cdef->pluginData.metaData.object();
        if (!o.isEmpty()) {
            dev.nextItem("\"MetaData\"");
            cbor_encode_int(&map, int(QtPluginMetaDataKeys::MetaData));
            jsonObjectToCbor(&map, o);
        }

        if (!cdef->pluginData.uri.isEmpty()) {
            dev.nextItem("\"URI\"");
            cbor_encode_int(&map, int(QtPluginMetaDataKeys::URI));
            cbor_encode_text_string(&map, cdef->pluginData.uri.constData(), cdef->pluginData.uri.size());
        }

        for (auto it = cdef->pluginData.metaArgs.cbegin(), end = cdef->pluginData.metaArgs.cend(); it != end; ++it) {
            const QJsonArray &a = it.value();
            QByteArray key = it.key().toUtf8();
            dev.nextItem(QByteArray("command-line \"" + key + "\"").constData());
            cbor_encode_text_string(&map, key.constData(), key.size());
            jsonArrayToCbor(&map, a);
        }

        dev.nextItem();
        cbor_encoder_close_container(&enc, &map);
    };

    qsizetype pos = cdef->qualified.indexOf("::");
    for ( ; pos != -1 ; pos = cdef->qualified.indexOf("::", pos + 2) )
        fprintf(out, "using namespace %s;\n", cdef->qualified.left(pos).constData());

    fputs("\n#ifdef QT_MOC_EXPORT_PLUGIN_V2", out);

    fprintf(out, "\nstatic constexpr unsigned char qt_pluginMetaDataV2_%s[] = {",
          cdef->classname.constData());
    outputCborData();
    fprintf(out, "\n};\nQT_MOC_EXPORT_PLUGIN_V2(%s, %s, qt_pluginMetaDataV2_%s)\n",
            cdef->qualified.constData(), cdef->classname.constData(), cdef->classname.constData());

    fprintf(out, "#else\nQT_PLUGIN_METADATA_SECTION\n"
          "Q_CONSTINIT static constexpr unsigned char qt_pluginMetaData_%s[] = {\n"
          "    'Q', 'T', 'M', 'E', 'T', 'A', 'D', 'A', 'T', 'A', ' ', '!',\n"
          "    // metadata version, Qt version, architectural requirements\n"
          "    0, QT_VERSION_MAJOR, QT_VERSION_MINOR, qPluginArchRequirements(),",
          cdef->classname.constData());
    outputCborData();
    fprintf(out, "\n};\nQT_MOC_EXPORT_PLUGIN(%s, %s)\n"
                 "#endif  // QT_MOC_EXPORT_PLUGIN_V2\n",
            cdef->qualified.constData(), cdef->classname.constData());

    fputs("\n", out);
}

QByteArray Generator::disambiguatedTypeName(const QByteArray &name)
{
    if (cdef->allEnumNames.contains(name))
        return "enum " + name;
    return name;
}

QByteArray Generator::disambiguatedTypeName(const QByteArray &name, TypeTags tag)
{
    if (tag == TypeTag::None)
        return disambiguatedTypeName(name);
    return name;
}

QByteArray Generator::disambiguatedTypeNameForCast(const QByteArray &name)
{
    return QByteArray("std::add_pointer_t<"+ disambiguatedTypeName(name) +">");
}

QT_WARNING_DISABLE_GCC("-Wunused-function")
QT_WARNING_DISABLE_CLANG("-Wunused-function")
QT_WARNING_DISABLE_CLANG("-Wundefined-internal")
QT_WARNING_DISABLE_MSVC(4334) 

#define CBOR_NO_HALF_FLOAT_TYPE         1
#define CBOR_ENCODER_WRITER_CONTROL     1
#define CBOR_ENCODER_WRITE_FUNCTION     CborDevice::callback

QT_END_NAMESPACE

#include "cborencoder.c"
