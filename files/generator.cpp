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

int Generator::stridx(const QByteArray &s)
{
    int i = 0;
    for (const StringDef &str : std::as_const(strings)) {
        if (str.str == s)
            return i;
        ++i;
    }
    strings.append({s, ""});
    return strings.size() - 1;
}

void Generator::strreg(const QByteArray &s)
{
    if (s.isEmpty())
        return;
    for (const StringDef &str : std::as_const(strings))
        if (str.str == s)
            return;
    strings.append({s, ""});
}

void Generator::registerClassInfoStrings()
{
    for (const ClassInfoDef &c : std::as_const(cdef->classInfoList)) {
        strreg(c.name);
        strreg(c.value);
    }
}

void Generator::registerFunctionStrings(const QList<FunctionDef>& list)
{
    for (const FunctionDef &f : std::as_const(list)) {
        strreg(f.name);
        if (!isBuiltinType(f.normalizedType))
            strreg(f.normalizedType);
        for (const ArgumentDef &a : std::as_const(f.arguments)) {
            if (!isBuiltinType(a.normalizedType))
                strreg(a.normalizedType);
            strreg(a.name);
        }
    }
}

void Generator::registerByteArrayVector(const QList<QByteArray> &list)
{
    for (const QByteArray &ba : std::as_const(list))
        strreg(ba);
}

void Generator::registerPropertyStrings()
{
    for (const PropertyDef &p : std::as_const(cdef->propertyList)) {
        strreg(p.name);
        if (!isBuiltinType(p.type))
            strreg(p.type);
    }
}

void Generator::registerEnumStrings()
{
    for (const EnumDef &e : std::as_const(cdef->enumList)) {
        strreg(e.name);
        if (!e.enumName.isNull())
            strreg(e.enumName);
        for (const QByteArray &val : std::as_const(e.values))
            strreg(val);
    }
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
                 "} \n\n",
            qualifiedClassNameIdentifier.constData());

    fprintf(out, "template <> constexpr inline auto %s::qt_create_metaobjectdata<qt_meta_tag_%s_t>()\n"
                 "{\n"
                 "    namespace QMC = QtMocConstants;\n",
            cdef->qualified.constData(), qualifiedClassNameIdentifier.constData());

    fprintf(out, "    QtMocHelpers::StringRefStorage qt_stringData {");
    addStrings(strings);
    fprintf(out, "\n    };\n\n");

    fprintf(out, "    QtMocHelpers::UintData qt_methods {\n");

    addFunctions(cdef->signalList, "Signal");
    addFunctions(cdef->slotList, "Slot");
    addFunctions(cdef->methodList, "Method");
    fprintf(out, "    };\n"
                 "    QtMocHelpers::UintData qt_properties {\n");
    addProperties();
    fprintf(out, "    };\n"
                 "    QtMocHelpers::UintData qt_enums {\n");
    addEnums();
    fprintf(out, "    };\n");

    fprintf(out, "    constexpr int qt_metaObjectHashIndex = %d;\n", stridx(hashes[cdef->qualified]));

    const char *uintDataParams = "";
    if (isConstructible || !cdef->classInfoList.isEmpty()) {
        if (isConstructible) {
            fprintf(out, "    using Constructor = QtMocHelpers::NoType;\n"
                         "    QtMocHelpers::UintData qt_constructors {\n");
            addFunctions(cdef->constructorList, "Constructor");
            fprintf(out, "    };\n");
        } else {
            fputs("    QtMocHelpers::UintData qt_constructors {};\n", out);
        }

        uintDataParams = ", qt_constructors";
        if (!cdef->classInfoList.isEmpty()) {
            fprintf(out, "    QtMocHelpers::ClassInfos qt_classinfo({\n");
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

    if (hasStaticMetaCall)
        generateStaticMetacall();

    if (!isQObject)
        generateMetacall();

    if (!cdef->hasQObject)
        return;

    fprintf(out, "\nconst QMetaObject *%s::metaObject() const\n{\n    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;\n}\n",
            cdef->qualified.constData());

    fprintf(out, "\nvoid *%s::qt_metacast(const char *_clname)\n{\n"
                 "    if (!_clname) return nullptr;\n"
                 "    if (!strcmp(_clname, qt_staticMetaObjectStaticContent%s.stringdata0))\n"
                 "        return static_cast<void*>(this);\n",
            cdef->qualified.constData(), metaVarNameSuffix.constData());

    for (const BaseDef &cl : std::as_const(cdef->superclassList)) {
        if (cl.classname.isEmpty())
            continue;
        fprintf(out, "    if (!strcmp(_clname, \"%s\"))\n        return static_cast< %s*>(this);\n",
                cl.classname.constData(), cl.classname.constData());
    }

    for (int i = 0; i < int(cdef->interfaceList.size()); ++i) {
        const QByteArray &iface = cdef->interfaceList.at(i);
        fprintf(out, "    if (!strcmp(_clname, %s_iid))\n        return ", iface.constData());
        for (int j = 0; j < int(cdef->interfaceList.size()); ++j)
            if (j == cdef->interfaceList.indexOf(iface))
                fprintf(out, "static_cast< %s*>(this)", iface.constData());
        fprintf(out, ";\n");
    }

    if (!purestSuperClass.isEmpty() && !isQObject) {
        fprintf(out, "    return %s::qt_metacast(_clname);\n", purestSuperClass.constData());
    } else {
        fprintf(out, "    return nullptr;\n");
    }
    fprintf(out, "}\n");

    QList<FunctionDef> methodList;
    methodList += cdef->signalList;
    methodList += cdef->slotList;
    methodList += cdef->methodList;

    bool needConnect = false;
    for (int i = 0; i < int(cdef->signalList.size()); ++i) {
        const FunctionDef &f = cdef->signalList.at(i);
        if (f.wasCloned || !f.inPrivateClass.isEmpty() || f.isStatic)
            continue;
        needConnect = true;
        break;
    }

    if (needConnect) {
        fprintf(out, "\nint %s::qt_metacast(QMetaObject::Call _c, int _id, void **_a)\n{\n",
                cdef->qualified.constData());

        if (!purestSuperClass.isEmpty() && !isQObject) {
            fprintf(out, "    _id = %s::qt_metacall(_c, _id, _a);\n", purestSuperClass.constData());
        }

        if (methodList.size() || cdef->propertyList.size()) {
            fprintf(out, "    if (_id < 0)\n        return _id;\n");
        }

        if (methodList.size()) {
            fprintf(out, "    if (_c == QMetaObject::InvokeMetaMethod) {\n");
            fprintf(out, "        if (_id < %d)\n", int(methodList.size()));
            fprintf(out, "            qt_static_metacall(this, _c, _id, _a);\n");
            fprintf(out, "        _id -= %d;\n    }\n", int(methodList.size()));

            fprintf(out, "    else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {\n");
            fprintf(out, "        if (_id < %d)\n", int(methodList.size()));

            if (methodsWithAutomaticTypesHelper(methodList).isEmpty())
                fprintf(out, "            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();\n");
            else
                fprintf(out, "            qt_static_metacall(this, _c, _id, _a);\n");
            fprintf(out, "        _id -= %d;\n    }\n", int(methodList.size()));
        }

        if (cdef->propertyList.size()) {
            fprintf(out,
                "    else if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty\n"
                "            || _c == QMetaObject::ResetProperty || _c == QMetaObject::BindableProperty\n"
                "            || _c == QMetaObject::RegisterPropertyMetaType) {\n"
                "        qt_static_metacall(this, _c, _id, _a);\n"
                "        _id -= %d;\n    }\n", int(cdef->propertyList.size()));
        }
        fprintf(out,"    return _id;\n}\n");
    }
}

void Generator::addStrings(const QList<StringDef>& list)
{
    bool first = true;
    for (const StringDef &s : std::as_const(list)) {
        if (!first)
            fprintf(out, ",");
        first = false;
        fprintf(out, "\n        \"");
        qsizetype len = s.str.size();
        for (qsizetype i = 0; i < len; ) {
            qsizetype escLen = lengthOfEscapeSequence(s.str, i);
            if (escLen == 1) {
                char c = s.str.at(i);
                if (c == '\\' || c == '\"')
                    fprintf(out, "\\%c", c);
                else if (c >= 0x20 && c < 0x7F)
                    fprintf(out, "%c", c);
                else
                    fprintf(out, "\\x%02x", uchar(c));
            } else {
                fprintf(out, "%.*s", int(escLen), s.str.constData() + i);
            }
            i += escLen;
        }
        fprintf(out, "\"");
    }
}

void Generator::addClassInfos()
{
    for (const ClassInfoDef &c : std::as_const(cdef->classInfoList)) {
        fprintf(out, "        {%d, %d},\n", stridx(c.name), stridx(c.value));
    }
}

void Generator::addFunctions(const QList<FunctionDef>& list, const char *functype)
{
    for (const FunctionDef &f : std::as_const(list)) {
        fprintf(out, "        QtMocHelpers::%sData(%d, %d, %d, %d, ",
                functype,
                stridx(f.name),
                int(f.arguments.size()),
                f.arguments.isEmpty() ? 0 : stridx(f.arguments.first().name),
                isBuiltinType(f.normalizedType) ? 0 : stridx(f.normalizedType));

        uint flags = 0;
        if (f.access == FunctionDef::Private)
            flags |= 0x00000002;
        else if (f.access == FunctionDef::Public)
            flags |= 0x00000001;
        else if (f.access == FunctionDef::Protected)
            flags |= 0x00000000;

        if (f.isConst)
            flags |= 0x00000004;

        fprintf(out, "0x%08x),\n", flags);
    }
}

void Generator::addProperties()
{
    for (int i = 0; i < int(cdef->propertyList.size()); ++i) {
        const PropertyDef &p = cdef->propertyList.at(i);
        uint flags = 0;
        if (!p.read.isEmpty())
            flags |= 0x00000001;
        if (!p.write.isEmpty())
            flags |= 0x00000002;
        if (!p.reset.isEmpty())
            flags |= 0x00000004;
        if (p.constant)
            flags |= 0x00000400;
        if (p.final)
            flags |= 0x00000800;
        if (p.required)
            flags |= 0x00001000;

        fprintf(out, "        QtMocHelpers::PropertyData(%d, %d, 0x%08x),\n",
                stridx(p.name),
                isBuiltinType(p.type) ? 0 : stridx(p.type),
                flags);
    }
}

void Generator::addEnums()
{
    for (const EnumDef &e : std::as_const(cdef->enumList)) {
        const QByteArray &typeName = e.enumName.isNull() ? e.name : e.enumName;
        fprintf(out, "        QtMocHelpers::EnumData<%s>(%d, %d,",
                disambiguatedTypeName(e.name).constData(), stridx(e.name), stridx(typeName));

        if (e.flags) {
            const char *separator = "";
            auto addFlag = [this, &separator](const char *text) {
                fprintf(out, "%s QMC::%s", separator, text);
                separator = " |";
            };
            if (e.flags & EnumIsFlag)
                addFlag("EnumIsFlag");
            if (e.flags & EnumIsScoped)
                addFlag("EnumIsScoped");
        } else {
            fprintf(out, " QMC::EnumFlags{}");
        }

        if (e.values.isEmpty()) {
            fprintf(out, "),\n");
            continue;
        }

        fprintf(out, ").add({\n");
        QByteArray prefix = (e.enumName.isNull() ? e.name : e.enumName);
        for (const QByteArray &val : std::as_const(e.values)) {
            fprintf(out, "            { %4d, %s::%s },\n", stridx(val),
                    prefix.constData(), val.constData());
        }

        fprintf(out, "        }),\n");
    }
}

void Generator::generateMetacall()
{
    bool isQObject = (cdef->classname == "QObject");

    fprintf(out, "\nint %s::qt_metacall(QMetaObject::Call _c, int _id, void **_a)\n{\n",
             cdef->qualified.constData());

    if (!purestSuperClass.isEmpty() && !isQObject) {
        fprintf(out, "    _id = %s::qt_metacall(_c, _id, _a);\n", purestSuperClass.constData());
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

        fprintf(out, "    else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {\n");
        fprintf(out, "        if (_id < %d)\n", int(methodList.size()));

        if (methodsWithAutomaticTypesHelper(methodList).isEmpty())
            fprintf(out, "            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();\n");
        else
            fprintf(out, "            qt_static_metacall(this, _c, _id, _a);\n");
        fprintf(out, "        _id -= %d;\n    }\n", int(methodList.size()));
    }

    if (cdef->propertyList.size()) {
        fprintf(out,
            "    else if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty\n"
            "            || _c == QMetaObject::ResetProperty || _c == QMetaObject::BindableProperty\n"
            "            || _c == QMetaObject::RegisterPropertyMetaType) {\n"
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
    fprintf(out, "void %s::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)\n{\n",
            cdef->qualified.constData());

    enum UsedArgs {
        UsedT = 1,
        UsedC = 2,
        UsedId = 4,
        UsedA = 8,
    };
    uint usedArgs = 0;

    if (cdef->hasQObject) {
#ifndef QT_NO_DEBUG
        fprintf(out, "    Q_ASSERT(_o == nullptr || staticMetaObject.cast(_o));\n");
#endif
        fprintf(out, "    auto *_t = static_cast<%s *>(_o);\n", cdef->classname.constData());
    } else {
        fprintf(out, "    auto *_t = reinterpret_cast<%s *>(_o);\n", cdef->classname.constData());
    }

    const auto generateCtorArguments = [&](int ctorindex) {
        const FunctionDef &f = cdef->constructorList.at(ctorindex);
        Q_ASSERT(!f.isPrivateSignal);
        int offset = 1;

        const auto begin = f.arguments.cbegin();
        const auto end = f.arguments.cend();
        for (auto it = begin; it != end; ++it) {
            const ArgumentDef &a = *it;
            if (it != begin)
                fprintf(out, ",");
            fprintf(out, "(*reinterpret_cast<%s>(_a[%d]))",
                    disambiguatedTypeNameForCast(a.normalizedType).constData(), offset++);
        }
    };

    if (!cdef->constructorList.isEmpty()) {
        fprintf(out, "    if (_c == QMetaObject::CreateInstance) {\n");
        fprintf(out, "        switch (_id) {\n");
        const int ctorend = int(cdef->constructorList.size());
        for (int ctorindex = 0; ctorindex < ctorend; ++ctorindex) {
            fprintf(out, "        case %d: { auto *_r = new %s(", ctorindex,
                    cdef->classname.constData());
            generateCtorArguments(ctorindex);
            fprintf(out, ");\n");
            fprintf(out, "            if (_a[0]) *reinterpret_cast<%s**>(_a[0]) = _r; } break;\n",
                    (cdef->hasQGadget || cdef->hasQNamespace) ? "void" : "QObject");
        }
        fprintf(out, "        default: break;\n");
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        fprintf(out, "    else if (_c == QMetaObject::ConstructInPlace) {\n");
        fprintf(out, "        switch (_id) {\n");
        for (int ctorindex = 0; ctorindex < ctorend; ++ctorindex) {
            fprintf(out, "        case %d: { new (_a[0]) %s(",
                    ctorindex, cdef->classname.constData());
            generateCtorArguments(ctorindex);
            fprintf(out, "); } break;\n");
        }
        fprintf(out, "        default: break;\n");
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        usedArgs |= UsedC | UsedId | UsedA;
    }

    QList<FunctionDef> methodList;
    methodList += cdef->signalList;
    methodList += cdef->slotList;
    methodList += cdef->methodList;

    if (!methodList.isEmpty()) {
        usedArgs |= UsedT | UsedC | UsedId;
        fprintf(out, "    else if (_c == QMetaObject::InvokeMetaMethod) {\n");
        fprintf(out, "        Q_ASSERT(_id >= 0 && _id < %d);\n", int(methodList.size()));
        fprintf(out, "        using FP = void (*)(%s*, void**);\n", cdef->classname.constData());
        fprintf(out, "        static constexpr FP vtable[] = {\n");
        
        for (int methodindex = 0; methodindex < methodList.size(); ++methodindex) {
            const FunctionDef &f = methodList.at(methodindex);
            fprintf(out, "            [](%s *_t, void **_a) { ", cdef->classname.constData());
            
            if (f.normalizedType != "void")
                fprintf(out, "auto _r = ");
            
            fprintf(out, "_t->");
            if (f.inPrivateClass.size())
                fprintf(out, "%s->", f.inPrivateClass.constData());
            fprintf(out, "%s(", f.name.constData());
            
            int offset = 1;
            if (f.isRawSlot) {
                fprintf(out, "QMethodRawArguments{_a}");
                usedArgs |= UsedA;
            } else {
                const auto begin = f.arguments.cbegin();
                const auto end = f.arguments.cend();
                for (auto it = begin; it != end; ++it) {
                    const ArgumentDef &a = *it;
                    if (it != begin)
                        fprintf(out, ",");
                    fprintf(out, "(*reinterpret_cast<%s>(_a[%d]))",
                            disambiguatedTypeNameForCast(a.normalizedType).constData(), offset++);
                    usedArgs |= UsedA;
                }
                if (f.isPrivateSignal) {
                    if (!f.arguments.isEmpty())
                        fprintf(out, ", ");
                    fprintf(out, "QPrivateSignal()");
                }
            }
            fprintf(out, ");");
            
            if (f.normalizedType != "void") {
                fprintf(out, " if (_a[0]) *reinterpret_cast<%s*>(_a[0]) = std::move(_r);",
                        disambiguatedTypeName(noRef(f.normalizedType)).constData());
                usedArgs |= UsedA;
            }
            fprintf(out, " },\n");
        }
        
        fprintf(out, "        };\n");
        fprintf(out, "        vtable[_id](_t, _a);\n");
        fprintf(out, "    }\n");

        QMap<int, QMultiMap<QByteArray, int> > methodsWithAutomaticTypes = methodsWithAutomaticTypesHelper(methodList);

        if (!methodsWithAutomaticTypes.isEmpty()) {
            fprintf(out, "    else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {\n");
            fprintf(out, "        switch (_id) {\n");
            fprintf(out, "        default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;\n");
            QMap<int, QMultiMap<QByteArray, int> >::const_iterator it = methodsWithAutomaticTypes.constBegin();
            const QMap<int, QMultiMap<QByteArray, int> >::const_iterator end = methodsWithAutomaticTypes.constEnd();
            for ( ; it != end; ++it) {
                fprintf(out, "        case %d:\n", it.key());
                fprintf(out, "            switch (*reinterpret_cast<int*>(_a[1])) {\n");
                fprintf(out, "            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;\n");
                auto jt = it->begin();
                const auto jend = it->end();
                while (jt != jend) {
                    fprintf(out, "            case %d:\n", jt.value());
                    const QByteArray &lastKey = jt.key();
                    ++jt;
                    if (jt == jend || jt.key() != lastKey)
                        fprintf(out, "                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< %s >(); break;\n", lastKey.constData());
                }
                fprintf(out, "            }\n");
                fprintf(out, "            break;\n");
            }
            fprintf(out, "        }\n");
            fprintf(out, "    }\n");
            usedArgs |= UsedC | UsedId | UsedA;
        }
    }
    
    if (!cdef->signalList.isEmpty()) {
        usedArgs |= UsedC | UsedA;
        fprintf(out, "    else if (_c == QMetaObject::IndexOfMethod) {\n");
        for (int methodindex = 0; methodindex < int(cdef->signalList.size()); ++methodindex) {
            const FunctionDef &f = cdef->signalList.at(methodindex);
            if (f.wasCloned || !f.inPrivateClass.isEmpty() || f.isStatic)
                continue;
            fprintf(out, "        if (QtMocHelpers::indexOfMethod<%s (%s::*)(",
                    f.type.rawName.constData() , cdef->classname.constData());

            const auto begin = f.arguments.cbegin();
            const auto end = f.arguments.cend();
            for (auto it = begin; it != end; ++it) {
                const ArgumentDef &a = *it;
                if (it != begin)
                    fprintf(out, ", ");
                fprintf(out, "%s", QByteArray(a.type.name + ' ' + a.rightType).constData());
            }
            if (f.isPrivateSignal) {
                if (!f.arguments.isEmpty())
                    fprintf(out, ", ");
                fprintf(out, "%s", "QPrivateSignal");
            }
            fprintf(out, ")%s>(_a, &%s::%s, %d))\n",
                    f.isConst ? " const" : "",
                    cdef->classname.constData(), f.name.constData(), methodindex);
            fprintf(out, "            return;\n");
        }
        fprintf(out, "    }\n");
    }

    const QMultiMap<QByteArray, int> automaticPropertyMetaTypes = automaticPropertyMetaTypesHelper();

    if (!automaticPropertyMetaTypes.isEmpty()) {
        fprintf(out, "    else if (_c == QMetaObject::RegisterPropertyMetaType) {\n");
        fprintf(out, "        switch (_id) {\n");
        fprintf(out, "        default: *reinterpret_cast<int*>(_a[0]) = -1; break;\n");
        auto it = automaticPropertyMetaTypes.begin();
        const auto end = automaticPropertyMetaTypes.end();
        while (it != end) {
            fprintf(out, "        case %d:\n", it.value());
            const QByteArray &lastKey = it.key();
            ++it;
            if (it == end || it.key() != lastKey)
                fprintf(out, "            *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< %s >(); break;\n", lastKey.constData());
        }
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        usedArgs |= UsedC | UsedId | UsedA;
    }

    if (!cdef->propertyList.empty()) {
        bool needGet = false;
        bool needTempVarForGet = false;
        bool needSet = false;
        bool needReset = false;
        bool hasBindableProperties = false;
        for (const PropertyDef &p : std::as_const(cdef->propertyList)) {
            needGet |= !p.read.isEmpty() || !p.member.isEmpty();
            if (!p.read.isEmpty() || !p.member.isEmpty())
                needTempVarForGet |= (p.gspec != PropertyDef::PointerSpec
                                      && p.gspec != PropertyDef::ReferenceSpec);

            needSet |= !p.write.isEmpty() || (!p.member.isEmpty() && !p.constant);
            needReset |= !p.reset.isEmpty();
            hasBindableProperties |= !p.bind.isEmpty();
        }
        if (needGet || needSet || hasBindableProperties || needReset)
            usedArgs |= UsedT | UsedC | UsedId;
        if (needGet || needSet || hasBindableProperties)
            usedArgs |= UsedA;

        if (needGet) {
            fprintf(out, "    else if (_c == QMetaObject::ReadProperty) {\n");
            if (needTempVarForGet)
                fprintf(out, "        void *_v = _a[0];\n");
            fprintf(out, "        switch (_id) {\n");
            for (int propindex = 0; propindex < int(cdef->propertyList.size()); ++propindex) {
                const PropertyDef &p = cdef->propertyList.at(propindex);
                if (p.read.isEmpty() && p.member.isEmpty())
                    continue;
                QByteArray prefix = "_t->";
                if (p.inPrivateClass.size()) {
                    prefix += p.inPrivateClass + "->";
                }

                if (p.gspec == PropertyDef::PointerSpec)
                    fprintf(out, "        case %d: _a[0] = const_cast<void*>(reinterpret_cast<const void*>(%s%s())); break;\n",
                            propindex, prefix.constData(), p.read.constData());
                else if (p.gspec == PropertyDef::ReferenceSpec)
                    fprintf(out, "        case %d: _a[0] = const_cast<void*>(reinterpret_cast<const void*>(&%s%s())); break;\n",
                            propindex, prefix.constData(), p.read.constData());
#if QT_VERSION <= QT_VERSION_CHECK(7, 0, 0)
                else if (auto eflags = cdef->enumDeclarations.value(p.type); eflags & EnumIsFlag)
                    fprintf(out, "        case %d: QtMocHelpers::assignFlags<%s>(_v, %s%s()); break;\n",
                            propindex, disambiguatedTypeName(p.type, p.typeTag).constData(), prefix.constData(), p.read.constData());
#endif
                else if (p.read == "default")
                    fprintf(out, "        case %d: *reinterpret_cast<%s%s*>(_v) = %s%s().value(); break;\n",
                            propindex, cxxTypeTag(p.typeTag), disambiguatedTypeName(p.type, p.typeTag).constData(),
                            prefix.constData(), p.bind.constData());
                else if (!p.read.isEmpty())
                    fprintf(out, "        case %d: *reinterpret_cast<%s%s*>(_v) = %s%s(); break;\n",
                            propindex, cxxTypeTag(p.typeTag), disambiguatedTypeName(p.type, p.typeTag).constData(),
                            prefix.constData(), p.read.constData());
                else
                    fprintf(out, "        case %d: *reinterpret_cast<%s%s*>(_v) = %s%s; break;\n",
                            propindex, cxxTypeTag(p.typeTag), disambiguatedTypeName(p.type, p.typeTag).constData(),
                            prefix.constData(), p.member.constData());
            }
            fprintf(out, "        default: break;\n");
            fprintf(out, "        }\n");
            fprintf(out, "    }\n");
        }

        if (needSet) {
            fprintf(out, "    else if (_c == QMetaObject::WriteProperty) {\n");
            fprintf(out, "        void *_v = _a[0];\n");
            fprintf(out, "        switch (_id) {\n");
            for (int propindex = 0; propindex < int(cdef->propertyList.size()); ++propindex) {
                const PropertyDef &p = cdef->propertyList.at(propindex);
                if (p.constant)
                    continue;
                if (p.write.isEmpty() && p.member.isEmpty())
                    continue;
                QByteArray prefix = "_t->";
                if (p.inPrivateClass.size()) {
                    prefix += p.inPrivateClass + "->";
                }
                if (p.write == "default") {
                    fprintf(out, "        case %d: %s%s().setValue(*reinterpret_cast<%s%s*>(_v)); break;\n",
                            propindex, prefix.constData(), p.bind.constData(),
                            cxxTypeTag(p.typeTag), disambiguatedTypeName(p.type, p.typeTag).constData());
                } else if (!p.write.isEmpty()) {
                    fprintf(out, "        case %d: %s%s(*reinterpret_cast<%s%s*>(_v)); break;\n",
                            propindex, prefix.constData(), p.write.constData(),
                            cxxTypeTag(p.typeTag), disambiguatedTypeName(p.type, p.typeTag).constData());
                } else if (!p.member.isEmpty()) {
                    fprintf(out, "        case %d: %s%s = *reinterpret_cast<%s%s*>(_v); break;\n",
                            propindex, prefix.constData(), p.member.constData(),
                            cxxTypeTag(p.typeTag), disambiguatedTypeName(p.type, p.typeTag).constData());
                }
            }
            fprintf(out, "        default: break;\n");
            fprintf(out, "        }\n");
            fprintf(out, "    }\n");
        }

        if (needReset) {
            fprintf(out, "    else if (_c == QMetaObject::ResetProperty) {\n");
            fprintf(out, "        switch (_id) {\n");
            for (int propindex = 0; propindex < int(cdef->propertyList.size()); ++propindex) {
                const PropertyDef &p = cdef->propertyList.at(propindex);
                if (p.reset.isEmpty())
                    continue;
                QByteArray prefix = "_t->";
                if (p.inPrivateClass.size()) {
                    prefix += p.inPrivateClass + "->";
                }
                fprintf(out, "        case %d: %s%s(); break;\n",
                        propindex, prefix.constData(), p.reset.constData());
            }
            fprintf(out, "        default: break;\n");
            fprintf(out, "        }\n");
            fprintf(out, "    }\n");
        }

        if (hasBindableProperties) {
            fprintf(out, "    else if (_c == QMetaObject::BindableProperty) {\n");
            fprintf(out, "        switch (_id) {\n");
            for (int propindex = 0; propindex < int(cdef->propertyList.size()); ++propindex) {
                const PropertyDef &p = cdef->propertyList.at(propindex);
                if (p.bind.isEmpty())
                    continue;
                QByteArray prefix = "_t->";
                if (p.inPrivateClass.size()) {
                    prefix += p.inPrivateClass + "->";
                }
                fprintf(out,
                        "        case %d: *static_cast<QUntypedBindable *>(_a[0]) = %s%s(); break;\n",
                        propindex, prefix.constData(), p.bind.constData());
            }
            fprintf(out, "        default: break;\n");
            fprintf(out, "        }\n");
            fprintf(out, "    }\n");
        }
    }

    if (!(usedArgs & UsedT))
        fprintf(out, "    Q_UNUSED(_t);\n");
    if (!(usedArgs & UsedC))
        fprintf(out, "    Q_UNUSED(_c);\n");
    if (!(usedArgs & UsedId))
        fprintf(out, "    Q_UNUSED(_id);\n");
    if (!(usedArgs & UsedA))
        fprintf(out, "    Q_UNUSED(_a);\n");
    fprintf(out, "}\n");
}

QT_END_NAMESPACE
