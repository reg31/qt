// Copyright (C) 2020 The Qt Company Ltd.
// Copyright (C) 2019 Olivier Goffart ogoffart@woboq.com
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
#include <private/qmetaobject_p.h> 
#include <private/qplugin_p.h> 
#include <cmath>
#include <algorithm>
#include <array>
#include <limits>
#include <cstdio>
#include <format>
#include <string_view>
#include <utility>

template<>
struct std::formatter<QByteArray, char> : std::formatter<std::string_view, char>
{
    auto format(const QByteArray& ba, std::format_context& ctx) const
    {
        return std::formatter<std::string_view, char>::format(
            std::string_view(ba.constData(), static_cast<size_t>(ba.size())), ctx);
    }
};

QT_BEGIN_NAMESPACE

using namespace QtMiscUtils;

template<typename... Args>
void moc_print(FILE *out, std::format_string<Args...> fmt, Args&&... args) {
    std::fputs(std::format(fmt, std::forward<Args>(args)...).c_str(), out);
}

static int nameToBuiltinType(const QByteArray &name)
{
    if (name.isEmpty()) return 0;
    uint tp = QMetaType::UnknownType;
    if (const QtPrivate::QMetaTypeInterface *iface = QMetaType::fromName(name).iface())
        tp = iface->typeId.loadRelaxed(); 
#ifndef QT_BOOTSTRAPPED
    if (tp >= uint(QMetaType::User)) tp = QMetaType::UnknownType;
#endif
    return int(tp);
}

static bool isBuiltinType(const QByteArray &type)
{
    return nameToBuiltinType(type) != QMetaType::UnknownType;
}

constexpr const char *cxxTypeTag(TypeTags t)
{
    if (t & TypeTag::HasEnum) {
        if (t & TypeTag::HasClass) return "enum class ";
        if (t & TypeTag::HasStruct) return "enum struct ";
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
    if (s.at(i) != '\\' || i >= s.size() - 1) return 1;
    const qsizetype startPos = i;
    ++i;
    char ch = s.at(i);
    if (ch == 'x') {
        ++i;
        while (i < s.size() && isHexDigit(s.at(i))) ++i;
    } else if (isOctalDigit(ch)) {
        while (i < startPos + 4 && i < s.size() && isOctalDigit(s.at(i))) ++i;
    } else { 
        i = std::min(i + 1, s.size());
    }
    return i - startPos;
}

static void printStringWithIndentation(FILE *out, const QByteArray &s)
{
    static constexpr int ColumnWidth = 68;
    const qsizetype len = s.size();
    qsizetype idx = 0;
    do {
        qsizetype spanLen = std::min(qsizetype(ColumnWidth - 2), len - idx);
        const qsizetype backSlashPos = s.lastIndexOf('\\', idx + spanLen - 1);
        if (backSlashPos >= idx) {
            const qsizetype escapeLen = lengthOfEscapeSequence(s, backSlashPos);
            spanLen = std::clamp(spanLen, backSlashPos + escapeLen - idx, len - idx);
        }
        moc_print(out, "\n        \"{:.*s}\"", int(spanLen), s.constData() + idx);
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
    if (auto it = stringCache.find(s); it != stringCache.end()) [[likely]] return it->second;
    return -1;
}

void Generator::precomputeTypeInfo(const QByteArray &typeName)
{
    if (typeCache.contains(typeName)) return;
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
    if (metaTypes.contains(propertyType)) return true;
    if (propertyType.endsWith('*')) {
        QByteArray objectPointerType = propertyType;
        objectPointerType.chop(1);
        if (knownQObjectClasses.contains(objectPointerType)) return true;
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
            const qsizetype argumentSize = propertyType.size() - ba.size() - 1 - (propertyType.at(propertyType.size() - 2) == ' ' ? 1 : 0 );
            const QByteArray templateArg = propertyType.sliced(ba.size(), argumentSize);
            return isBuiltinType(templateArg) || registerableMetaType(templateArg);
        }
    }
    return false;
}

static bool qualifiedNameEquals(const QByteArray &qualifiedName, const QByteArray &name)
{
    if (qualifiedName == name) return true;
    const qsizetype index = qualifiedName.indexOf("::");
    if (index == -1) return false;
    return qualifiedNameEquals(qualifiedName.mid(index+2), name);
}

static QByteArray generateQualifiedClassNameIdentifier(const QByteArray &identifier)
{
    QByteArray qualifiedClassNameIdentifier = "ZN";
    for (const auto scope : qTokenize(QLatin1StringView(identifier), QLatin1Char(':'), Qt::SkipEmptyParts)) {
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
    bool hasStaticMetaCall = (cdef->hasQObject || !cdef->methodList.isEmpty() || !cdef->propertyList.isEmpty() || !cdef->constructorList.isEmpty());
    if (parser->activeQtMode) hasStaticMetaCall = false;
    const QByteArray qualifiedClassNameIdentifier = generateQualifiedClassNameIdentifier(cdef->qualified);
    const char *ownType = !cdef->hasQNamespace ? cdef->classname.data() : "void";
    moc_print(out, "namespace {{\nstruct qt_meta_tag_{}_t {{}};\n}} \n\n", qualifiedClassNameIdentifier);
    moc_print(out, "template <> constexpr inline auto {}::qt_create_metaobjectdata<qt_meta_tag_{}_t>()\n{{\n    namespace QMC = QtMocConstants;\n", cdef->qualified, qualifiedClassNameIdentifier);
    moc_print(out, "    static constexpr QtMocHelpers::StringRefStorage qt_stringData {{\n");
    for (int i = 0; i < strings.size(); ++i) {
        if (i > 0) moc_print(out, ",\n");
        moc_print(out, "        \"");
        const QByteArray &s = strings.at(i);
        for (int j = 0; j < s.size(); ++j) {
            if (s[j] == '\\' || s[j] == '"') moc_print(out, "\\");
            moc_print(out, "{}", s[j]);
        }
        moc_print(out, "\"");
    }
    moc_print(out, "\n    }};\n\n    static constexpr QtMocHelpers::UintData qt_methods {{\n");
    addFunctions(cdef->signalList, "Signal");
    addFunctions(cdef->slotList, "Slot");
    addFunctions(cdef->methodList, "Method");
    moc_print(out, "    }};\n    static constexpr QtMocHelpers::UintData qt_properties {{\n");
    addProperties();
    moc_print(out, "    }};\n    static constexpr QtMocHelpers::UintData qt_enums {{\n");
    addEnums();
    moc_print(out, "    }};\n");
    int hashIdx = stridx(hashes[cdef->qualified]);
    if (hashIdx == -1) moc_print(out, "    static constexpr auto qt_metaObjectHashIndex = ~0u;\n");
    else moc_print(out, "    static constexpr auto qt_metaObjectHashIndex = uint32_t({});\n", hashIdx);
    const char *uintDataParams = "";
    if (isConstructible || !cdef->classInfoList.isEmpty()) {
        if (isConstructible) {
            moc_print(out, "    using Constructor = QtMocHelpers::NoType;\n    static constexpr QtMocHelpers::UintData qt_constructors {{\n");
            addFunctions(cdef->constructorList, "Constructor");
            moc_print(out, "    }};\n");
        } else {
            moc_print(out, "    static constexpr QtMocHelpers::UintData qt_constructors {{}};\n");
        }
        uintDataParams = ", qt_constructors";
        if (!cdef->classInfoList.isEmpty()) {
            moc_print(out, "    static constexpr QtMocHelpers::ClassInfos qt_classinfo({{\n");
            addClassInfos();
            moc_print(out, "    }});\n");
            uintDataParams = ", qt_constructors, qt_classinfo";
        }
    }
    const char *metaObjectFlags = "QMC::MetaObjectFlag{}";
    if (cdef->hasQGadget || cdef->hasQNamespace) metaObjectFlags = "QMC::PropertyAccessInStaticMetaCall";
    QByteArray tagType = QByteArrayLiteral("void");
    if (!requireCompleteness) tagType = "qt_meta_tag_" + qualifiedClassNameIdentifier +  "_t";
    moc_print(out, "    return QtMocHelpers::metaObjectData<{}, {}>( {}, qt_stringData,\n            qt_methods, qt_properties, qt_enums, qt_metaObjectHashIndex{});\n}}\n", ownType, tagType, metaObjectFlags, uintDataParams);
    QByteArray metaVarNameSuffix;
    if (cdef->hasQNamespace) {
        metaVarNameSuffix = '_' + qualifiedClassNameIdentifier;
        moc_print(out, R"(
static constexpr auto qt_staticMetaObjectContent{0} =
    {1}::qt_create_metaobjectdata<qt_meta_tag{0}_t>();
static constexpr auto qt_staticMetaObjectStaticContent{0} =
    qt_staticMetaObjectContent{0}.staticData;
static constexpr auto qt_staticMetaObjectRelocatingContent{0} =
    qt_staticMetaObjectContent{0}.relocatingData;
)", metaVarNameSuffix, cdef->qualified);
    } else {
        metaVarNameSuffix = "<qt_meta_tag_" + qualifiedClassNameIdentifier + "_t>";
    }
    if (hasStaticMetaCall) generateStaticMetacall();
    moc_print(out, "Q_CONSTINIT const QMetaObject {}::staticMetaObject = {{ {{\n", cdef->qualified);
    if (isQObject) moc_print(out, "    nullptr,\n");
    else if (cdef->superclassList.size() && !cdef->hasQGadget && !cdef->hasQNamespace) moc_print(out, "    QMetaObject::SuperData::link<{}::staticMetaObject>(),\n", purestSuperClass);
    else if (cdef->superclassList.size()) moc_print(out, "    QtPrivate::MetaObjectForType<{}>::value,\n", purestSuperClass);
    else moc_print(out, "    nullptr,\n");
    moc_print(out, "    qt_staticMetaObjectStaticContent{0}.stringdata,\n    qt_staticMetaObjectStaticContent{0}.data,\n", metaVarNameSuffix);
    moc_print(out, "    {}, ", hasStaticMetaCall ? "qt_static_metacall" : "nullptr");
    QList<QByteArray> extraList;
    QMultiHash<QByteArray, QByteArray> knownExtraMetaObject(knownGadgets);
    knownExtraMetaObject.unite(knownQObjectClasses);
    for (const PropertyDef &p : std::as_const(cdef->propertyList)) {
        if (isBuiltinType(p.type)) continue;
        if (p.type.contains('*') || p.type.contains('<') || p.type.contains('>')) continue;
        const qsizetype s = p.type.lastIndexOf("::");
        if (s <= 0) continue;
        QByteArray unqualifiedScope = p.type.left(s);
        QMultiHash<QByteArray, QByteArray>::ConstIterator scopeIt;
        QByteArray thisScope = cdef->qualified;
        do {
            const qsizetype s = thisScope.lastIndexOf("::");
            thisScope = thisScope.left(s);
            QByteArray currentScope = thisScope.isEmpty() ? unqualifiedScope : thisScope + "::" + unqualifiedScope;
            scopeIt = knownExtraMetaObject.constFind(currentScope);
        } while (!thisScope.isEmpty() && scopeIt == knownExtraMetaObject.constEnd());
        if (scopeIt != knownExtraMetaObject.constEnd() && *scopeIt != "Qt" && !qualifiedNameEquals(cdef->qualified, *scopeIt) && !extraList.contains(*scopeIt))
            extraList += *scopeIt;
    }
    for (auto it = cdef->enumDeclarations.keyBegin(); it != cdef->enumDeclarations.keyEnd(); ++it) {
        const qsizetype s = it->lastIndexOf("::");
        if (s > 0) {
            QByteArray scope = it->left(s);
            if (scope != "Qt" && !qualifiedNameEquals(cdef->qualified, scope) && !extraList.contains(scope)) extraList += scope;
        }
    }
    if (extraList.isEmpty()) moc_print(out, "    nullptr,\n");
    else {
        moc_print(out, "    []() {{\n        static const QMetaObject::SuperData extradata[] = {{\n");
        for (const auto &ba : extraList) moc_print(out, "            QMetaObject::SuperData::link<{}::staticMetaObject>(),\n", ba);
        moc_print(out, "            nullptr\n        }};\n        return extradata;\n    }}(),\n");
    }
    moc_print(out, "    qt_staticMetaObjectRelocatingContent{}.metaTypes,\n    nullptr\n}} }};\n\n", metaVarNameSuffix);
    if (!cdef->hasQObject) return;
    moc_print(out, "const QMetaObject *{}::metaObject() const\n{{\n    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;\n}}\n", cdef->qualified);
    moc_print(out, "\nvoid *{}::qt_metacast(const char *_clname)\n{{\n    if (!_clname) return nullptr;\n    if (!strcmp(_clname, qt_staticMetaObjectStaticContent{}.strings))\n        return static_cast<void*>(this);\n", cdef->qualified, metaVarNameSuffix);
    if (cdef->superclassList.size() > 1) {
        for (auto it = cdef->superclassList.cbegin() + 1; it != cdef->superclassList.cend(); ++it) {
            if (it->access != FunctionDef::Private)
                moc_print(out, "    if (!strcmp(_clname, \"{}\"))\n        return static_cast< {}*>(this);\n", it->classname, it->classname);
        }
    }
    for (const auto &iface : std::as_const(cdef->interfaceList)) {
        for (qsizetype j = 0; j < iface.size(); ++j) {
            moc_print(out, "    if (!strcmp(_clname, {}))\n        return ", iface.at(j).interfaceId);
            for (qsizetype k = j; k >= 0; --k) moc_print(out, "static_cast< {}*>(", iface.at(k).className);
            moc_print(out, "this{});\n", QByteArray(j + 1, ')'));
        }
    }
    if (!purestSuperClass.isEmpty() && !isQObject) moc_print(out, "    return {}::qt_metacast(_clname);\n", purestSuperClass);
    else moc_print(out, "    return nullptr;\n");
    moc_print(out, "}}\n");
    if (parser->activeQtMode) return;
    generateMetacall();
    for (int i = 0; i < int(cdef->signalList.size()); ++i) generateSignal(&cdef->signalList.at(i), i);
    generatePluginMetaData();
    if (!cdef->nonClassSignalList.isEmpty()) {
        moc_print(out, "namespace CheckNotifySignalValidity_{} {{\n", qualifiedClassNameIdentifier);
        for (const auto &nonClassSignal : std::as_const(cdef->nonClassSignalList)) {
            const auto propertyIt = std::find_if(cdef->propertyList.constBegin(), cdef->propertyList.constEnd(), [&nonClassSignal](const PropertyDef &p) { return nonClassSignal == p.notify; });
            Q_ASSERT(propertyIt != cdef->propertyList.constEnd());
            moc_print(out, "template<typename T> using has_nullary_{0} = decltype(std::declval<T>().{0}());\ntemplate<typename T> using has_unary_{0} = decltype(std::declval<T>().{0}(std::declval<{1}>()));\nstatic_assert(qxp::is_detected_v<has_nullary_{0}, {2}> || qxp::is_detected_v<has_unary_{0}, {2}>, \"NOTIFY signal {0} does not exist\");\n", nonClassSignal, propertyIt->type, cdef->qualified);
        }
        moc_print(out, "}}\n");
    }
}

void Generator::precomputeTypesForFunctions(const QList<FunctionDef> &list)
{
    for (const auto &f : list) {
        precomputeTypeInfo(f.normalizedType);
        for (const auto &a : f.arguments) precomputeTypeInfo(a.normalizedType);
    }
}

void Generator::precomputeTypesForProperties()
{
    for (const PropertyDef &p : std::as_const(cdef->propertyList)) precomputeTypeInfo(p.type);
}

void Generator::registerClassInfoStrings()
{
    for (const auto &c : std::as_const(cdef->classInfoList)) { strreg(c.name); strreg(c.value); }
}

void Generator::addClassInfos()
{
    for (const auto &c : std::as_const(cdef->classInfoList)) {
        int nIdx = stridx(c.name), vIdx = stridx(c.value);
        moc_print(out, "            {{ {}, {} }},\n", nIdx == -1 ? "~0u" : std::format("uint32_t({})", nIdx), vIdx == -1 ? "~0u" : std::format("uint32_t({})", vIdx));
    }
}

void Generator::registerFunctionStrings(const QList<FunctionDef> &list)
{
    for (const auto &f : list) {
        strreg(f.name); strreg(f.tag);
        if (!isBuiltinType(f.normalizedType)) strreg(f.normalizedType);
        for (const auto& a : f.arguments) {
            strreg(a.name);
            if (!isBuiltinType(a.normalizedType)) strreg(a.normalizedType);
        }
    }
}

void Generator::registerByteArrayVector(const QList<QByteArray> &list) { for (const auto &ba : list) strreg(ba); }

void Generator::addStrings(const QByteArrayList &strings)
{
    if (strings.isEmpty()) { moc_print(out, " \"\" "); return; }
    QByteArray combined; QList<uint16_t> offsets;
    for (const auto &str : strings) { offsets.append(combined.size()); combined.append(str); combined.append('\0'); }
    moc_print(out, "\n        .data = "); printStringWithIndentation(out, combined);
    moc_print(out, ",\n        .offsets = {{");
    for (int i = 0; i < offsets.size(); ++i) {
        if (i % 16 == 0) moc_print(out, "\n            ");
        moc_print(out, "{}", offsets[i]);
        if (i < offsets.size() - 1) moc_print(out, ", ");
    }
    moc_print(out, "\n        }},\n        .size = {}", offsets.size());
}

void Generator::addFunctions(const QList<FunctionDef> &list, const char *functype)
{
    for (const auto &f : list) {
        if (!f.isConstructor) moc_print(out, "        // {} '{}'\n", functype, f.name);
        moc_print(out, "        QtMocHelpers::{}{}Data<", f.revision > 0 ? "Revisioned" : "", functype);
        if (f.isConstructor) moc_print(out, "Constructor(");
        else moc_print(out, "{}(", disambiguatedTypeName(f.type.name));
        for (int i = 0; i < f.arguments.size(); ++i) moc_print(out, "{}{}", i == 0 ? "" : ", ", disambiguatedTypeName(f.arguments.at(i).type.name));
        int nIdx = stridx(f.name), tIdx = stridx(f.tag);
        if (f.isConstructor) moc_print(out, ")>({}, ", tIdx == -1 ? "~0u" : std::format("uint32_t({})", tIdx));
        else moc_print(out, "){}>({}, {}, ", f.isConst ? " const" : "", nIdx == -1 ? "~0u" : std::format("uint32_t({})", nIdx), tIdx == -1 ? "~0u" : std::format("uint32_t({})", tIdx));
        if (f.access == FunctionDef::Private) moc_print(out, "QMC::AccessPrivate");
        else if (f.access == FunctionDef::Public) moc_print(out, "QMC::AccessPublic");
        else moc_print(out, "QMC::AccessProtected");
        if (f.isCompat) moc_print(out, " | QMC::MethodCompatibility");
        if (f.wasCloned) moc_print(out, " | QMC::MethodCloned");
        if (f.isScriptable) moc_print(out, " | QMC::MethodScriptable");
        if (f.revision > 0) moc_print(out, ", {:#x}", f.revision);
        if (!f.isConstructor) { moc_print(out, ", "); generateTypeInfo(f.normalizedType); }
        if (f.arguments.isEmpty()) moc_print(out, "),\n");
        else {
            moc_print(out, ", {{{{");
            for (int i = 0; i < f.arguments.size(); ++i) {
                if ((i % 4) == 0) moc_print(out, "\n           ");
                int aIdx = stridx(f.arguments.at(i).name);
                moc_print(out, " {{ "); generateTypeInfo(f.arguments.at(i).normalizedType);
                moc_print(out, ", {} }},", aIdx == -1 ? "0xFFFFu" : std::format("uint16_t({})", aIdx));
            }
            moc_print(out, "\n        }}}}),\n");
        }
    }
}

void Generator::generateTypeInfo(const QByteArray &typeName, bool allowEmptyName)
{
    Q_UNUSED(allowEmptyName);
    if (auto it = typeCache.find(typeName); it != typeCache.end()) {
        if (it->second.isBuiltin) {
            if (it->second.valueString) moc_print(out, "QMetaType::{}", it->second.valueString);
            else moc_print(out, "{:4d}", it->second.builtinType);
            return;
        }
    }
    int idx = stridx(typeName);
    uint32_t unresolved = static_cast<uint32_t>(IsUnresolvedType);
    if (idx == -1) moc_print(out, "0x{:08x} | 0xFFFFu", unresolved);
    else moc_print(out, "0x{:08x} | uint16_t({})", unresolved, idx);
}

void Generator::registerPropertyStrings() { for (const auto &p : std::as_const(cdef->propertyList)) { strreg(p.name); if (!isBuiltinType(p.type)) strreg(p.type); } }

void Generator::addProperties()
{
    for (const auto &p : std::as_const(cdef->propertyList)) {
        moc_print(out, "        // property '{}'\n        QtMocHelpers::PropertyData<{}{}>(uint32_t({}), ", p.name, cxxTypeTag(p.typeTag), disambiguatedTypeName(p.type, p.typeTag), stridx(p.name));
        generateTypeInfo(p.type);
        moc_print(out, ",");
        const char *sep = "";
        auto addFlag = [&](const char *f) { moc_print(out, "{} QMC::{}", sep, f); sep = " |"; };
        if (p.read.isEmpty() && p.member.isEmpty()) addFlag("Invalid");
        else {
            bool d = p.designable != "false", s = p.scriptable != "false", st = p.stored != "false";
            if (d && s && st) { addFlag("DefaultPropertyFlags"); if ((!p.member.isEmpty() && !p.constant) || !p.write.isEmpty()) addFlag("Writable"); }
            else { addFlag("Readable"); if ((!p.member.isEmpty() && !p.constant) || !p.write.isEmpty()) addFlag("Writable"); if (d) addFlag("Designable"); if (s) addFlag("Scriptable"); if (st) addFlag("Stored"); }
        }
        if (!p.reset.isEmpty()) addFlag("Resettable");
        if (!isBuiltinType(p.type)) addFlag("EnumOrFlag");
        if (p.constant) addFlag("Constant");
        if (p.final) addFlag("Final");
        if (p.required) addFlag("Required");
        if (!p.bind.isEmpty()) addFlag("Bindable");
        if (p.notifyId != -1 || p.revision > 0) {
            moc_print(out, ", ");
            if (p.notifyId < -1) moc_print(out, "0x{:08x} | uint32_t({})", static_cast<uint32_t>(IsUnresolvedSignal), int(strings.indexOf(p.notify)));
            else if (p.notifyId == -1) moc_print(out, "~0u");
            else moc_print(out, "uint32_t({})", p.notifyId);
            if (p.revision > 0) moc_print(out, ", {:#x}", p.revision);
        }
        moc_print(out, "),\n");
    }
}

void Generator::registerEnumStrings() { for (const auto &e : std::as_const(cdef->enumList)) { strreg(e.name); if (!e.enumName.isNull()) strreg(e.enumName); for (const auto &v : e.values) strreg(v); } }

void Generator::addEnums()
{
    for (const auto &e : std::as_const(cdef->enumList)) {
        int nIdx = stridx(e.name), tnIdx = stridx(e.enumName.isNull() ? e.name : e.enumName);
        moc_print(out, "        // {} '{}'\n        QtMocHelpers::EnumData<{}>(", e.flags & EnumIsFlag ? "flag" : "enum", e.name, disambiguatedTypeName(e.name));
        moc_print(out, "{}, {},", nIdx == -1 ? "~0u" : std::format("uint32_t({})", nIdx), tnIdx == -1 ? "~0u" : std::format("uint32_t({})", tnIdx));
        if (e.flags) { const char *s = ""; if (e.flags & EnumIsFlag) { moc_print(out, " QMC::EnumIsFlag"); s = " |"; } if (e.flags & EnumIsScoped) moc_print(out, "{} QMC::EnumIsScoped", s); }
        else moc_print(out, " QMC::EnumFlags{{}}");
        if (e.values.isEmpty()) { moc_print(out, "),\n"); continue; }
        moc_print(out, ").add({{\n");
        QByteArray pfx = (e.enumName.isNull() ? e.name : e.enumName);
        for (const auto &v : e.values) {
            int vIdx = stridx(v);
            moc_print(out, "            {{ {}, std::to_underlying({}::{}) }},\n", vIdx == -1 ? "~0u" : std::format("uint32_t({})", vIdx), pfx, v);
        }
        moc_print(out, "        }}),\n");
    }
}

void Generator::generateMetacall()
{
    moc_print(out, "\nint {}::qt_metacall(QMetaObject::Call _c, int _id, void **_a)\n{{\n", cdef->qualified);
    if (!purestSuperClass.isEmpty() && cdef->classname != "QObject") moc_print(out, "    _id = {}::qt_metacall(_c, _id, _a);\n", purestSuperClass);
    QList<FunctionDef> mList = cdef->signalList + cdef->slotList + cdef->methodList;
    if (mList.size() || cdef->propertyList.size()) moc_print(out, "    if (_id < 0) return _id;\n");
    if (mList.size()) {
        moc_print(out, "    if (_c == QMetaObject::InvokeMetaMethod) {{\n        if (_id < {0}) qt_static_metacall(this, _c, _id, _a);\n        _id -= {0};\n    }}\n", mList.size());
        moc_print(out, "    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {{\n        if (_id < {0}) {{\n", mList.size());
        if (methodsWithAutomaticTypesHelper(mList).isEmpty()) moc_print(out, "            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();\n");
        else moc_print(out, "            qt_static_metacall(this, _c, _id, _a);\n");
        moc_print(out, "        }}\n        _id -= {0};\n    }}\n", mList.size());
    }
    if (!cdef->propertyList.isEmpty()) moc_print(out, "    else if (_c >= QMetaObject::ReadProperty && _c <= QMetaObject::RegisterPropertyMetaType) {{\n        qt_static_metacall(this, _c, _id, _a);\n        _id -= {};\n    }}\n", cdef->propertyList.size());
    moc_print(out, "    return _id;\n}}\n");
}

QMultiMap<QByteArray, int> Generator::automaticPropertyMetaTypesHelper()
{
    QMultiMap<QByteArray, int> map;
    for (int i = 0; i < int(cdef->propertyList.size()); ++i) {
        const auto &p = cdef->propertyList.at(i);
        if (registerableMetaType(p.type) && !isBuiltinType(p.type)) map.insert(cxxTypeTag(p.typeTag) + p.type, i);
    }
    return map;
}

QMap<int, QMultiMap<QByteArray, int>> Generator::methodsWithAutomaticTypesHelper(const QList<FunctionDef> &mList)
{
    QMap<int, QMultiMap<QByteArray, int>> map;
    for (int i = 0; i < mList.size(); ++i) {
        for (int j = 0; j < mList.at(i).arguments.size(); ++j) {
            const auto &at = mList.at(i).arguments.at(j).normalizedType;
            if (registerableMetaType(at) && !isBuiltinType(at)) map[i].insert(at, j);
        }
    }
    return map;
}

void Generator::generateStaticMetacall()
{
    moc_print(out, "void {}::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)\n{{\n", cdef->qualified);
    enum UsedArgs { UsedT = 1, UsedC = 2, UsedId = 4, UsedA = 8 }; uint usedArgs = 0;
    if (cdef->hasQObject) moc_print(out, "    auto *_t = static_cast<{} *>(_o);\n", cdef->classname);
    else moc_print(out, "    auto *_t = reinterpret_cast<{} *>(_o);\n", cdef->classname);
    if (!cdef->constructorList.isEmpty()) {
        moc_print(out, "    if (_c == QMetaObject::CreateInstance) {{\n        switch (_id) {{\n");
        for (int i = 0; i < cdef->constructorList.size(); ++i) {
            const auto &f = cdef->constructorList.at(i);
            moc_print(out, "        case {}: {{ {} *_r = new {}(", i, cdef->classname, cdef->classname);
            for (int j = 0; j < f.arguments.size(); ++j) moc_print(out, "{}*reinterpret_cast<{}*>(_a[{}])", j == 0 ? "" : ",", disambiguatedTypeNameForCast(f.arguments.at(j).normalizedType), j + 1);
            moc_print(out, "); if (_a[0]) *reinterpret_cast<{}**>(_a[0]) = _r; }} break;\n", (cdef->hasQGadget || cdef->hasQNamespace) ? "void" : "QObject");
        }
        moc_print(out, "        default: std::unreachable();\n        }}\n    }}\n    if (_c == QMetaObject::ConstructInPlace) {{\n        switch (_id) {{\n");
        for (int i = 0; i < cdef->constructorList.size(); ++i) {
            const auto &f = cdef->constructorList.at(i);
            moc_print(out, "        case {}: new (_a[0]) {}(", i, cdef->classname);
            for (int j = 0; j < f.arguments.size(); ++j) moc_print(out, "{}*reinterpret_cast<{}*>(_a[{}])", j == 0 ? "" : ",", disambiguatedTypeNameForCast(f.arguments.at(j).normalizedType), j + 1);
            moc_print(out, "); break;\n");
        }
        moc_print(out, "        default: std::unreachable();\n        }}\n    }}\n");
        usedArgs |= UsedC | UsedId | UsedA;
    }
    QList<FunctionDef> mList = cdef->signalList + cdef->slotList + cdef->methodList;
    if (!mList.isEmpty()) {
        usedArgs |= UsedT | UsedC | UsedId;
        moc_print(out, "    if (_c == QMetaObject::InvokeMetaMethod) [[likely]] {{\n");
        if (mList.size() <= 16) {
            moc_print(out, "        [[assume(_id >= 0 && _id < {})]];\n        using Func = void (*)({} *, void **);\n        static constexpr std::array<Func, {}> vtable = {{\n", mList.size(), cdef->classname, mList.size());
            for (int i = 0; i < mList.size(); ++i) {
                const auto &f = mList.at(i);
                moc_print(out, "            []({} *t, void **a) {{ ", cdef->classname);
                if (f.normalizedType == "void" && !f.isRawSlot && f.arguments.isEmpty()) moc_print(out, "(void)a; ");
                if (f.isStatic) moc_print(out, "(void)t; ");
                if (f.normalizedType != "void") moc_print(out, "if (auto r = ");
                moc_print(out, "{}{}(", f.isStatic ? std::format("{}::", cdef->classname) : "t->", f.name);
                if (f.isRawSlot) { moc_print(out, "QMethodRawArguments{{a}}"); usedArgs |= UsedA; }
                else { for (int j = 0; j < f.arguments.size(); ++j) { moc_print(out, "{}*reinterpret_cast<{}*>(a[{}])", j == 0 ? "" : ",", disambiguatedTypeNameForCast(f.arguments.at(j).normalizedType), j + 1); usedArgs |= UsedA; } }
                moc_print(out, "); ");
                if (f.normalizedType != "void") moc_print(out, "if (a[0]) *reinterpret_cast<{}*>(a[0]) = std::move(r); ", disambiguatedTypeName(noRef(f.normalizedType)));
                moc_print(out, "}}{}", i < mList.size() - 1 ? ",\n" : "");
            }
            moc_print(out, "\n        }};\n        vtable[_id](_t, _a);\n");
        } else {
            moc_print(out, "        switch (_id) {{\n");
            for (int i = 0; i < mList.size(); ++i) {
                const auto &f = mList.at(i);
                moc_print(out, "        case {}: ", i);
                if (f.normalizedType != "void") moc_print(out, "{{ {} _r = ", disambiguatedTypeName(noRef(f.normalizedType)));
                moc_print(out, "{}{}(", f.isStatic ? std::format("{}::", cdef->classname) : "_t->", f.name);
                if (f.isRawSlot) { moc_print(out, "QMethodRawArguments{{_a}}"); usedArgs |= UsedA; }
                else { for (int j = 0; j < f.arguments.size(); ++j) { moc_print(out, "{}*reinterpret_cast<{}*>(_a[{}])", j == 0 ? "" : ",", disambiguatedTypeNameForCast(f.arguments.at(j).normalizedType), j + 1); usedArgs |= UsedA; } }
                moc_print(out, "); ");
                if (f.normalizedType != "void") moc_print(out, "if (_a[0]) *reinterpret_cast<{}*>(_a[0]) = std::move(_r); }} ", disambiguatedTypeName(noRef(f.normalizedType)));
                moc_print(out, "break;\n");
            }
            moc_print(out, "        default: std::unreachable();\n        }}\n");
        }
        moc_print(out, "    }}\n");
    }
    auto printUnused = [&](UsedArgs entry, const char *name) { if ((usedArgs & entry) == 0) moc_print(out, "    (void){};\n", name); };
    printUnused(UsedT, "_t"); printUnused(UsedC, "_c"); printUnused(UsedId, "_id"); printUnused(UsedA, "_a");
    moc_print(out, "}}\n");
}

void Generator::generateSignal(const FunctionDef *def, int index)
{
    if (def->wasCloned || def->isAbstract) return;
    moc_print(out, "\n// SIGNAL {}\n{} {}::{}(", index, def->type.name, cdef->qualified, def->name);
    QByteArray thisPtr = def->isConst ? std::format("const_cast<{} *>(this)", cdef->qualified) : "this";
    for (int i = 0; i < def->arguments.size(); ++i) moc_print(out, "{}{} _t{}{}", i == 0 ? "" : ", ", def->arguments.at(i).type.name, i + 1, def->arguments.at(i).rightType);
    moc_print(out, "){}\n{{\n", def->isConst ? " const" : "");
    if (def->type.name != "void") moc_print(out, "    {} _t0{{}};\n", noRef(def->normalizedType));
    moc_print(out, "    QMetaObject::activate<{}>( {}, &staticMetaObject, {}, {}", def->normalizedType, thisPtr, index, def->normalizedType == "void" ? "nullptr" : "std::addressof(_t0)");
    for (int i = 0; i < def->arguments.size(); ++i) moc_print(out, ", _t{}", i + 1);
    moc_print(out, ");\n");
    if (def->type.name != "void") moc_print(out, "    return _t0;\n");
    moc_print(out, "}}\n");
}

static CborError jsonValueToCbor(CborEncoder *parent, const QJsonValue &v);
static CborError jsonObjectToCbor(CborEncoder *parent, const QJsonObject &o)
{
    CborEncoder map; cbor_encoder_create_map(parent, &map, o.size());
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) { QByteArray k = it.key().toUtf8(); cbor_encode_text_string(&map, k.constData(), k.size()); jsonValueToCbor(&map, it.value()); }
    return cbor_encoder_close_container(parent, &map);
}
static CborError jsonArrayToCbor(CborEncoder *parent, const QJsonArray &a)
{
    CborEncoder array; cbor_encoder_create_array(parent, &array, a.size());
    for (const auto &v : a) jsonValueToCbor(&array, v);
    return cbor_encoder_close_container(parent, &array);
}
static CborError jsonValueToCbor(CborEncoder *parent, const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::Null: case QJsonValue::Undefined: return cbor_encode_null(parent);
    case QJsonValue::Bool: return cbor_encode_boolean(parent, v.toBool());
    case QJsonValue::Array: return jsonArrayToCbor(parent, v.toArray());
    case QJsonValue::Object: return jsonObjectToCbor(parent, v.toObject());
    case QJsonValue::String: { QByteArray s = v.toString().toUtf8(); return cbor_encode_text_string(parent, s.constData(), s.size()); }
    case QJsonValue::Double: { double d = v.toDouble(); if (d == std::floor(d) && std::abs(d) <= (Q_INT64_C(1) << std::numeric_limits<double>::digits)) return cbor_encode_int(parent, qint64(d)); return cbor_encode_double(parent, d); }
    }
    return CborUnknownError;
}

void Generator::generatePluginMetaData()
{
    if (cdef->pluginData.iid.isEmpty()) return;
    auto outputCborData = [this]() {
        CborDevice dev(out); CborEncoder enc; cbor_encoder_init_writer(&enc, CborDevice::callback, &dev);
        CborEncoder map; cbor_encoder_create_map(&enc, &map, CborIndefiniteLength);
        dev.nextItem("\"IID\""); cbor_encode_int(&map, int(QtPluginMetaDataKeys::IID)); cbor_encode_text_string(&map, cdef->pluginData.iid.constData(), cdef->pluginData.iid.size());
        dev.nextItem("\"className\""); cbor_encode_int(&map, int(QtPluginMetaDataKeys::ClassName)); cbor_encode_text_string(&map, cdef->classname.constData(), cdef->classname.size());
        if (!cdef->pluginData.metaData.object().isEmpty()) { dev.nextItem("\"MetaData\""); cbor_encode_int(&map, int(QtPluginMetaDataKeys::MetaData)); jsonObjectToCbor(&map, cdef->pluginData.metaData.object()); }
        if (!cdef->pluginData.uri.isEmpty()) { dev.nextItem("\"URI\""); cbor_encode_int(&map, int(QtPluginMetaDataKeys::URI)); cbor_encode_text_string(&map, cdef->pluginData.uri.constData(), cdef->pluginData.uri.size()); }
        for (auto it = cdef->pluginData.metaArgs.cbegin(); it != cdef->pluginData.metaArgs.cend(); ++it) { dev.nextItem(std::format("command-line \"{}\"", it.key()).c_str()); cbor_encode_text_string(&map, it.key().toUtf8().constData(), it.key().toUtf8().size()); jsonArrayToCbor(&map, it.value()); }
        dev.nextItem(); cbor_encoder_close_container(&enc, &map);
    };
    for (qsizetype pos = cdef->qualified.indexOf("::"); pos != -1; pos = cdef->qualified.indexOf("::", pos + 2)) moc_print(out, "using namespace {};\n", cdef->qualified.left(pos));
    moc_print(out, "\n#ifdef QT_MOC_EXPORT_PLUGIN_V2\nstatic constexpr unsigned char qt_pluginMetaDataV2_{}[] = {{", cdef->classname);
    outputCborData();
    moc_print(out, "\n}};\nQT_MOC_EXPORT_PLUGIN_V2({}, {}, qt_pluginMetaDataV2_{})\n#else\nQT_PLUGIN_METADATA_SECTION\nQ_CONSTINIT static constexpr unsigned char qt_pluginMetaData_{}[] = {{\n    'Q', 'T', 'M', 'E', 'T', 'A', 'D', 'A', 'T', 'A', ' ', '!',\n    0, QT_VERSION_MAJOR, QT_VERSION_MINOR, qPluginArchRequirements(),", cdef->qualified, cdef->classname, cdef->classname, cdef->classname);
    outputCborData();
    moc_print(out, "\n}};\nQT_MOC_EXPORT_PLUGIN({}, {})\n#endif\n\n", cdef->qualified, cdef->classname);
}

QByteArray Generator::disambiguatedTypeName(const QByteArray &name) { return cdef->allEnumNames.contains(name) ? "enum " + name : name; }
QByteArray Generator::disambiguatedTypeName(const QByteArray &name, TypeTags tag) { return tag == TypeTag::None ? disambiguatedTypeName(name) : name; }
QByteArray Generator::disambiguatedTypeNameForCast(const QByteArray &name) { return QByteArray("std::add_pointer_t<"+ disambiguatedTypeName(name) +">"); }

QT_WARNING_DISABLE_GCC("-Wunused-function")
QT_WARNING_DISABLE_CLANG("-Wunused-function")
QT_WARNING_DISABLE_CLANG("-Wundefined-internal")
QT_WARNING_DISABLE_MSVC(4334) 
#define CBOR_NO_HALF_FLOAT_TYPE 1
#define CBOR_ENCODER_WRITER_CONTROL 1
#define CBOR_ENCODER_WRITE_FUNCTION CborDevice::callback
QT_END_NAMESPACE
#include "cborencoder.c"
