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
#include <cmath>
#include <algorithm>
#include <utility>
#include <private/qmetaobject_p.h> 
#include <private/qplugin_p.h> 

QT_BEGIN_NAMESPACE

using namespace QtMiscUtils;

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
    strreg(""); 
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
        fprintf(out, "\n        \"%.*s\"", int(spanLen), s.constData() + idx);
        idx += spanLen;
    } while (idx < len);
}

void Generator::strreg(const QByteArray &s)
{
    if (!stringCache.contains(s)) {
        stringCache[s] = (int)strings.size();
        strings.append(s);
    }
}

int Generator::stridx(const QByteArray &s)
{
    auto it = stringCache.find(s);
    if (it != stringCache.end()) return it->second;
    return 0;
}

void Generator::addStrings(const QByteArrayList &strings)
{
    if (strings.isEmpty()) return;

    QByteArray blob;
    QList<uint32_t> offsets;
    for (const QByteArray &s : strings) {
        offsets.append(blob.size());
        blob.append(s);
        blob.append('\0');
    }

    fprintf(out, "        .data = ");
    printStringWithIndentation(out, blob);
    fprintf(out, ",\n        .offsets = { ");
    for (int i = 0; i < offsets.size(); ++i) {
        fprintf(out, "%u%s", offsets[i], (i == offsets.size() - 1) ? "" : ", ");
    }
    fprintf(out, " }");
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

    const bool requireCompleteness = requireCompleteTypes || cdef->requireCompleteMethodTypes;
    bool hasStaticMetaCall = (cdef->hasQObject || !cdef->methodList.isEmpty()
             || !cdef->propertyList.isEmpty() || !cdef->constructorList.isEmpty());
    if (parser->activeQtMode)
        hasStaticMetaCall = false;

    const QByteArray tagID = QByteArray::number(qHash(cdef->qualified), 16);
    const QByteArray qualifiedClassNameIdentifier = generateQualifiedClassNameIdentifier(cdef->qualified);
    const char *ownType = !cdef->hasQNamespace ? cdef->classname.data() : "void";

    fprintf(out, "namespace {\nstruct Q_DECL_HIDDEN qt_tag_%s {};\n} // unnamed namespace\n\n", 
            tagID.constData());

    fprintf(out, "template <> constexpr inline auto %s::qt_create_metaobjectdata<qt_tag_%s>()\n{\n"
                 "    namespace QMC = QtMocConstants;\n",
            cdef->qualified.constData(), tagID.constData());

    fprintf(out, "    static constexpr QtMocHelpers::StringRefStorage qt_stringData {");
    addStrings(strings);
    fprintf(out, "\n    };\n\n");

    fprintf(out, "    static constexpr QtMocHelpers::UintData qt_methods {\n");
    addFunctions(cdef->signalList, "Signal");
    addFunctions(cdef->slotList, "Slot");
    addFunctions(cdef->methodList, "Method");
    fprintf(out, "    };\n    static constexpr QtMocHelpers::UintData qt_properties {\n");
    addProperties();
    fprintf(out, "    };\n    static constexpr QtMocHelpers::UintData qt_enums {\n");
    addEnums();
    fprintf(out, "    };\n");

    fprintf(out, "    static constexpr auto qt_metaObjectHashIndex = %d;\n", stridx(hashes[cdef->qualified]));

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

    const char *metaObjectFlags = (cdef->hasQGadget || cdef->hasQNamespace) 
                                   ? "QMC::PropertyAccessInStaticMetaCall" 
                                   : "QMC::MetaObjectFlag{}";

    QByteArray tagType = QByteArrayLiteral("void");
    if (!requireCompleteness)
        tagType = "qt_tag_" + tagID;

    fprintf(out, "    return QtMocHelpers::metaObjectData<%s, %s>(%s, qt_stringData,\n"
                 "            qt_methods, qt_properties, qt_enums, qt_metaObjectHashIndex%s);\n}\n",
            ownType, tagType.constData(), metaObjectFlags, uintDataParams);

    QByteArray metaVarNameSuffix;
    if (cdef->hasQNamespace) {
        metaVarNameSuffix = '_' + qualifiedClassNameIdentifier;
        const char *n = metaVarNameSuffix.constData();
        fprintf(out, "\nstatic constexpr auto qt_staticMetaObjectContent%s ="
                     "\n    %s::qt_create_metaobjectdata<qt_tag_%s>();"
                     "\nstatic constexpr auto qt_staticMetaObjectStaticContent%s ="
                     "\n    qt_staticMetaObjectContent%s.staticData;"
                     "\nstatic constexpr auto qt_staticMetaObjectRelocatingContent%s ="
                     "\n    qt_staticMetaObjectContent%s.relocatingData;\n\n",
                n, cdef->qualified.constData(), tagID.constData(), n, n, n, n);
    } else {
        metaVarNameSuffix = "<qt_tag_" + tagID + ">";
    }

    QList<QByteArray> extraList;
    QMultiHash<QByteArray, QByteArray> knownExtraMetaObject(knownGadgets);
    knownExtraMetaObject.unite(knownQObjectClasses);

    for (const PropertyDef &p : std::as_const(cdef->propertyList)) {
        if (isBuiltinType(p.type) || p.type.contains('*') || p.type.contains('<')) continue;
        const qsizetype s = p.type.lastIndexOf("::");
        if (s <= 0) continue;
        QByteArray unqualifiedScope = p.type.left(s);
        QMultiHash<QByteArray, QByteArray>::ConstIterator scopeIt;
        QByteArray thisScope = cdef->qualified;
        do {
            const qsizetype s2 = thisScope.lastIndexOf("::");
            thisScope = thisScope.left(s2);
            QByteArray currentScope = thisScope.isEmpty() ? unqualifiedScope : thisScope + "::" + unqualifiedScope;
            scopeIt = knownExtraMetaObject.constFind(currentScope);
        } while (!thisScope.isEmpty() && scopeIt == knownExtraMetaObject.constEnd());

        if (scopeIt != knownExtraMetaObject.constEnd()) {
            const QByteArray &scope = *scopeIt;
            if (scope != "Qt" && !qualifiedNameEquals(cdef->qualified, scope) && !extraList.contains(scope))
                extraList += scope;
        }
    }

    for (auto it = cdef->enumDeclarations.keyBegin(); it != cdef->enumDeclarations.keyEnd(); ++it) {
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

    fprintf(out, "Q_CONSTINIT const QMetaObject %s::staticMetaObject = { {\n", cdef->qualified.constData());
    if (isQObject) fprintf(out, "    nullptr,\n");
    else if (cdef->superclassList.size() && !cdef->hasQGadget && !cdef->hasQNamespace)
        fprintf(out, "    QMetaObject::SuperData::link<%s::staticMetaObject>(),\n", purestSuperClass.constData());
    else if (cdef->superclassList.size())
        fprintf(out, "    QtPrivate::MetaObjectForType<%s>::value,\n", purestSuperClass.constData());
    else fprintf(out, "    nullptr,\n");

    fprintf(out, "    qt_staticMetaObjectStaticContent%s.stringdata,\n"
                 "    qt_staticMetaObjectStaticContent%s.data,\n",
            metaVarNameSuffix.constData(), metaVarNameSuffix.constData());

    if (hasStaticMetaCall) fprintf(out, "    qt_static_metacall,\n");
    else fprintf(out, "    nullptr,\n");

    if (extraList.isEmpty()) fprintf(out, "    nullptr,\n");
    else fprintf(out, "    qt_meta_extradata_%s,\n", qualifiedClassNameIdentifier.constData());

    fprintf(out, "    qt_staticMetaObjectRelocatingContent%s.metaTypes,\n    nullptr\n} };\n\n",
            metaVarNameSuffix.constData());

    if (hasStaticMetaCall) generateStaticMetacall();
    if (!cdef->hasQObject) return;

    fprintf(out, "\nconst QMetaObject *%s::metaObject() const\n{\n"
                 "    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;\n"
                 "}\n", cdef->qualified.constData());

    fprintf(out, "\nvoid *%s::qt_metacast(const char *_clname)\n{\n"
                 "    if (!_clname) return nullptr;\n"
                 "    if (!strcmp(_clname, qt_staticMetaObjectStaticContent%s.strings))\n"
                 "        return static_cast<void*>(this);\n",
            cdef->qualified.constData(), metaVarNameSuffix.constData());

    if (cdef->superclassList.size() > 1) {
        for (auto it = cdef->superclassList.cbegin() + 1; it != cdef->superclassList.cend(); ++it) {
            if (it->access == FunctionDef::Private) continue;
            fprintf(out, "    if (!strcmp(_clname, \"%s\"))\n        return static_cast< %s*>(this);\n",
                    it->classname.constData(), it->classname.constData());
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

    if (!purestSuperClass.isEmpty() && !isQObject)
        fprintf(out, "    return %s::qt_metacast(_clname);\n", purestSuperClass.constData());
    else fprintf(out, "    return nullptr;\n");
    fprintf(out, "}\n");

    if (parser->activeQtMode) return;

    generateMetacall();
    for (int signalindex = 0; signalindex < int(cdef->signalList.size()); ++signalindex)
        generateSignal(&cdef->signalList.at(signalindex), signalindex);
    generatePluginMetaData();
}

void Generator::generateStaticMetacall()
{
    fprintf(out, "void %s::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)\n{\n", cdef->qualified.constData());
    fprintf(out, "    auto *_t = static_cast<%s *>(_o);\n", cdef->classname.constData());

    QList<FunctionDef> methodList = cdef->signalList + cdef->slotList + cdef->methodList;
    if (!methodList.isEmpty()) {
        fprintf(out, "    if (_c == QMetaObject::InvokeMetaMethod) {\n        switch (_id) {\n");
        for (int i = 0; i < methodList.size(); ++i) {
            const auto &f = methodList.at(i);
            fprintf(out, "        case %d: ", i);
            if (f.normalizedType != "void") fprintf(out, "{ %s _r = ", disambiguatedTypeName(noRef(f.normalizedType)).constData());
            fprintf(out, "_t->%s(", f.name.constData());
            for (int j = 0; j < f.arguments.size(); ++j) {
                fprintf(out, "%s(*reinterpret_cast<%s>(_a[%d]))", (j == 0 ? "" : ","), 
                        disambiguatedTypeNameForCast(f.arguments.at(j).normalizedType).constData(), j + 1);
            }
            fprintf(out, ");");
            if (f.normalizedType != "void") fprintf(out, " if (_a[0]) *reinterpret_cast<%s*>(_a[0]) = std::move(_r); }", 
                                                    disambiguatedTypeName(noRef(f.normalizedType)).constData());
            fprintf(out, " break;\n");
        }
        fprintf(out, "        default: break;\n        }\n    }\n");
    }
    fprintf(out, "    (void)_t; (void)_c; (void)_id; (void)_a;\n}\n");
}

QByteArray Generator::disambiguatedTypeName(const QByteArray &name)
{
    if (cdef->allEnumNames.contains(name)) return "enum " + name;
    return name;
}

QByteArray Generator::disambiguatedTypeNameForCast(const QByteArray &name)
{
    return "std::add_pointer_t<" + disambiguatedTypeName(name) + ">";
}

QT_END_NAMESPACE
#include "cborencoder.c"
