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
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <algorithm>
#include <span>

#include <private/qmetaobject_p.h>
#include <private/qplugin_p.h>

QT_BEGIN_NAMESPACE

using namespace QtMiscUtils;

static int nameToBuiltinType(std::string_view name) noexcept
{
    if (name.empty())
        return 0;

    uint tp = QMetaType::UnknownType;
    if (const QtPrivate::QMetaTypeInterface *iface = QMetaType::fromName(QByteArray(name.data(), name.size())).iface())
        tp = iface->typeId.loadRelaxed();

#ifndef QT_BOOTSTRAPPED
    if (tp >= uint(QMetaType::User))
        tp = QMetaType::UnknownType;
#endif

    return int(tp);
}

static bool isBuiltinType(const QByteArray &type) noexcept
{
    return nameToBuiltinType(std::string_view(type.data(), type.size())) != QMetaType::UnknownType;
}

constexpr const char *cxxTypeTag(TypeTags t) noexcept
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

static const char *metaTypeEnumValueString(int type) noexcept
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

static inline qsizetype lengthOfEscapeSequence(std::string_view s, qsizetype i) noexcept
{
    if (s[i] != '\\' || i >= static_cast<qsizetype>(s.size()) - 1)
        return 1;
    const qsizetype startPos = i;
    ++i;
    char ch = s[i];
    if (ch == 'x') {
        ++i;
        while (i < static_cast<qsizetype>(s.size()) && std::isxdigit(static_cast<unsigned char>(s[i])))
            ++i;
    } else if (std::isdigit(static_cast<unsigned char>(ch))) {
        while (i < startPos + 4 && i < static_cast<qsizetype>(s.size()) && std::isdigit(static_cast<unsigned char>(s[i])))
            ++i;
    } else {
        ++i;
    }
    return i - startPos;
}

int Generator::stridx(const QByteArray &s)
{
    auto it = stringCache.find(s);
    if (it != stringCache.end())
        return it->second;
    
    int idx = strings.size();
    strings.push_back({s, ""});
    stringCache[s] = idx;
    return idx;
}

void Generator::strreg(const QByteArray &s)
{
    if (s.isEmpty() || stringCache.contains(s))
        return;
    
    int idx = strings.size();
    strings.push_back({s, ""});
    stringCache[s] = idx;
}

void Generator::registerClassInfoStrings()
{
    for (const ClassInfoDef &c : std::as_const(cdef->classInfoList)) {
        strreg(c.name);
        strreg(c.value);
    }
}

void Generator::registerFunctionStrings(std::span<const FunctionDef> list)
{
    for (const FunctionDef &f : list) {
        strreg(f.name);
        if (!isBuiltinType(f.normalizedType))
            strreg(f.normalizedType);
        for (const ArgumentDef &a : f.arguments) {
            if (!isBuiltinType(a.normalizedType))
                strreg(a.normalizedType);
            strreg(a.name);
        }
    }
}

void Generator::registerByteArrayVector(std::span<const QByteArray> list)
{
    for (const QByteArray &ba : list)
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
        for (const QByteArray &val : e.values)
            strreg(val);
    }
}

static bool qualifiedNameEquals(std::string_view qualifiedName, std::string_view name) noexcept
{
    if (qualifiedName == name)
        return true;
    
    size_t index = qualifiedName.find("::");
    if (index == std::string_view::npos)
        return false;
    
    return qualifiedNameEquals(qualifiedName.substr(index + 2), name);
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
    registerFunctionStrings(std::span(cdef->signalList.data(), cdef->signalList.size()));
    registerFunctionStrings(std::span(cdef->slotList.data(), cdef->slotList.size()));
    registerFunctionStrings(std::span(cdef->methodList.data(), cdef->methodList.size()));
    registerFunctionStrings(std::span(cdef->constructorList.data(), cdef->constructorList.size()));
    registerByteArrayVector(std::span(cdef->nonClassSignalList.data(), cdef->nonClassSignalList.size()));
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

    addFunctions(std::span(cdef->signalList.data(), cdef->signalList.size()), "Signal");
    addFunctions(std::span(cdef->slotList.data(), cdef->slotList.size()), "Slot");
    addFunctions(std::span(cdef->methodList.data(), cdef->methodList.size()), "Method");
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
            addFunctions(std::span(cdef->constructorList.data(), cdef->constructorList.size()), "Constructor");
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

    std::vector<QByteArray> extraList;
    std::unordered_set<std::string_view> extraSet;
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
        if (qualifiedNameEquals(std::string_view(cdef->qualified.data(), cdef->qualified.size()), 
                                std::string_view(scope.data(), scope.size())))
            continue;

        std::string_view sv(scope.data(), scope.size());
        if (!extraSet.contains(sv)) {
            extraList.push_back(scope);
            extraSet.insert(sv);
        }
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

    std::vector<FunctionDef> methodList;
    methodList.reserve(cdef->signalList.size() + cdef->slotList.size() + cdef->methodList.size());
    methodList.insert(methodList.end(), cdef->signalList.begin(), cdef->signalList.end());
    methodList.insert(methodList.end(), cdef->slotList.begin(), cdef->slotList.end());
    methodList.insert(methodList.end(), cdef->methodList.begin(), cdef->methodList.end());

    bool needConnect = std::any_of(cdef->signalList.begin(), cdef->signalList.end(),
        [](const FunctionDef &f) { return !f.wasCloned && f.inPrivateClass.isEmpty() && !f.isStatic; });

    if (needConnect) {
        fprintf(out, "\nint %s::qt_metacast(QMetaObject::Call _c, int _id, void **_a)\n{\n",
                cdef->qualified.constData());

        if (!purestSuperClass.isEmpty() && !isQObject) {
            fprintf(out, "    _id = %s::qt_metacall(_c, _id, _a);\n", purestSuperClass.constData());
        }

        if (!methodList.empty() || !cdef->propertyList.isEmpty()) {
            fprintf(out, "    if (_id < 0)\n        return _id;\n");
        }

        if (!methodList.empty()) {
            fprintf(out, "    if (_c == QMetaObject::InvokeMetaMethod) {\n");
            fprintf(out, "        if (_id < %zu)\n", methodList.size());
            fprintf(out, "            qt_static_metacall(this, _c, _id, _a);\n");
            fprintf(out, "        _id -= %zu;\n    }\n", methodList.size());

            fprintf(out, "    else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {\n");
            fprintf(out, "        if (_id < %zu)\n", methodList.size());

            if (methodsWithAutomaticTypesHelper(methodList).empty())
                fprintf(out, "            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();\n");
            else
                fprintf(out, "            qt_static_metacall(this, _c, _id, _a);\n");
            fprintf(out, "        _id -= %zu;\n    }\n", methodList.size());
        }

        if (!cdef->propertyList.isEmpty()) {
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

void Generator::addStrings(const std::vector<StringDef>& list)
{
    bool first = true;
    for (const StringDef &s : list) {
        if (!first)
            fprintf(out, ",");
        first = false;
        fprintf(out, "\n        \"");
        std::string_view sv(s.str.data(), s.str.size());
        for (size_t i = 0; i < sv.size(); ) {
            qsizetype escLen = lengthOfEscapeSequence(sv, i);
            if (escLen == 1) {
                char c = sv[i];
                if (c == '\\' || c == '\"')
                    fprintf(out, "\\%c", c);
                else if (c >= 0x20 && c < 0x7F)
                    fprintf(out, "%c", c);
                else
                    fprintf(out, "\\x%02x", static_cast<unsigned char>(c));
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

void Generator::addFunctions(std::span<const FunctionDef> list, const char *functype)
{
    for (const FunctionDef &f : list) {
        fprintf(out, "        QtMocHelpers::%sData(%d, %zu, %d, %d, ",
                functype,
                stridx(f.name),
                f.arguments.size(),
                f.arguments.isEmpty() ? 0 : stridx(f.arguments.first().name),
                isBuiltinType(f.normalizedType) ? 0 : stridx(f.normalizedType));

        uint flags = 0;
        if (f.access == FunctionDef::Private)
            flags |= 0x00000002;
        else if (f.access == FunctionDef::Public)
            flags |= 0x00000001;

        if (f.isConst)
            flags |= 0x00000004;

        fprintf(out, "0x%08x),\n", flags);
    }
}

void Generator::addProperties()
{
    for (size_t i = 0; i < cdef->propertyList.size(); ++i) {
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
        for (const QByteArray &val : e.values) {
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

    std::vector<FunctionDef> methodList;
    methodList.reserve(cdef->signalList.size() + cdef->slotList.size() + cdef->methodList.size());
    methodList.insert(methodList.end(), cdef->signalList.begin(), cdef->signalList.end());
    methodList.insert(methodList.end(), cdef->slotList.begin(), cdef->slotList.end());
    methodList.insert(methodList.end(), cdef->methodList.begin(), cdef->methodList.end());

    if (!methodList.empty() || !cdef->propertyList.isEmpty()) {
        fprintf(out, "    if (_id < 0)\n        return _id;\n");
    }

    if (!methodList.empty()) {
        fprintf(out, "    if (_c == QMetaObject::InvokeMetaMethod) {\n");
        fprintf(out, "        if (_id < %zu)\n", methodList.size());
        fprintf(out, "            qt_static_metacall(this, _c, _id, _a);\n");
        fprintf(out, "        _id -= %zu;\n    }\n", methodList.size());

        fprintf(out, "    else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {\n");
        fprintf(out, "        if (_id < %zu)\n", methodList.size());

        if (methodsWithAutomaticTypesHelper(methodList).empty())
            fprintf(out, "            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();\n");
        else
            fprintf(out, "            qt_static_metacall(this, _c, _id, _a);\n");
        fprintf(out, "        _id -= %zu;\n    }\n", methodList.size());
    }

    if (!cdef->propertyList.isEmpty()) {
        fprintf(out,
            "    else if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty\n"
            "            || _c == QMetaObject::ResetProperty || _c == QMetaObject::BindableProperty\n"
            "            || _c == QMetaObject::RegisterPropertyMetaType) {\n"
            "        qt_static_metacall(this, _c, _id, _a);\n"
            "        _id -= %d;\n    }\n", int(cdef->propertyList.size()));
    }
    fprintf(out,"    return _id;\n}\n");
}

std::unordered_map<QByteArray, int> Generator::automaticPropertyMetaTypesHelper()
{
    std::unordered_map<QByteArray, int> result;
    for (size_t i = 0; i < cdef->propertyList.size(); ++i) {
        const PropertyDef &p = cdef->propertyList.at(i);
        if (registerableMetaType(p.type) && !isBuiltinType(p.type))
            result[cxxTypeTag(p.typeTag) + p.type] = i;
    }
    return result;
}

std::unordered_map<int, std::unordered_map<QByteArray, int>>
Generator::methodsWithAutomaticTypesHelper(const std::vector<FunctionDef> &methodList)
{
    std::unordered_map<int, std::unordered_map<QByteArray, int>> result;
    for (size_t i = 0; i < methodList.size(); ++i) {
        const FunctionDef &f = methodList[i];
        for (size_t j = 0; j < f.arguments.size(); ++j) {
            const QByteArray &argType = f.arguments.at(j).normalizedType;
            if (registerableMetaType(argType) && !isBuiltinType(argType))
                result[i][argType] = j;
        }
    }
    return result;
}

void Generator::generateStaticMetacall()
{
    fprintf(out, "void %s::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)\n{\n",
            cdef->qualified.constData());

    if (cdef->hasQObject) {
#ifndef QT_NO_DEBUG
        fprintf(out, "    Q_ASSERT(_o == nullptr || staticMetaObject.cast(_o));\n");
#endif
        fprintf(out, "    auto *_t = static_cast<%s *>(_o);\n", cdef->classname.constData());
    } else {
        fprintf(out, "    auto *_t = reinterpret_cast<%s *>(_o);\n", cdef->classname.constData());
    }

    const auto generateCtorArguments = [&](size_t ctorindex) {
        const FunctionDef &f = cdef->constructorList.at(ctorindex);
        size_t offset = 1;
        for (size_t i = 0; i < f.arguments.size(); ++i) {
            const ArgumentDef &a = f.arguments.at(i);
            if (i > 0)
                fprintf(out, ",");
            fprintf(out, "(*reinterpret_cast<%s>(_a[%zu]))",
                    disambiguatedTypeNameForCast(a.normalizedType).constData(), offset++);
        }
    };

    if (!cdef->constructorList.isEmpty()) {
        fprintf(out, "    if (_c == QMetaObject::CreateInstance) {\n");
        fprintf(out, "        switch (_id) {\n");
        for (size_t ctorindex = 0; ctorindex < cdef->constructorList.size(); ++ctorindex) {
            fprintf(out, "        case %zu: { auto *_r = new %s(", ctorindex,
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
        for (size_t ctorindex = 0; ctorindex < cdef->constructorList.size(); ++ctorindex) {
            fprintf(out, "        case %zu: { new (_a[0]) %s(",
                    ctorindex, cdef->classname.constData());
            generateCtorArguments(ctorindex);
            fprintf(out, "); } break;\n");
        }
        fprintf(out, "        default: break;\n");
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
    }

    std::vector<FunctionDef> methodList;
    methodList.reserve(cdef->signalList.size() + cdef->slotList.size() + cdef->methodList.size());
    methodList.insert(methodList.end(), cdef->signalList.begin(), cdef->signalList.end());
    methodList.insert(methodList.end(), cdef->slotList.begin(), cdef->slotList.end());
    methodList.insert(methodList.end(), cdef->methodList.begin(), cdef->methodList.end());

    if (!methodList.empty()) {
        fprintf(out, "    else if (_c == QMetaObject::InvokeMetaMethod) {\n");
        fprintf(out, "        Q_ASSERT(_id >= 0 && _id < %zu);\n", methodList.size());
        fprintf(out, "        using FP = void (*)(%s*, void**);\n", cdef->classname.constData());
        fprintf(out, "        static constexpr FP vtable[] = {\n");
        
        for (const FunctionDef &f : methodList) {
            fprintf(out, "            [](%s *_t, void **_a) { ", cdef->classname.constData());
            
            if (f.normalizedType != "void")
                fprintf(out, "auto _r = ");
            
            fprintf(out, "_t->");
            if (!f.inPrivateClass.isEmpty())
                fprintf(out, "%s->", f.inPrivateClass.constData());
            fprintf(out, "%s(", f.name.constData());
            
            if (f.isRawSlot) {
                fprintf(out, "QMethodRawArguments{_a}");
            } else {
                for (size_t i = 0; i < f.arguments.size(); ++i) {
                    const ArgumentDef &a = f.arguments.at(i);
                    if (i > 0)
                        fprintf(out, ",");
                    fprintf(out, "(*reinterpret_cast<%s>(_a[%zu]))",
                            disambiguatedTypeNameForCast(a.normalizedType).constData(), i + 1);
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
            }
            fprintf(out, " },\n");
        }
        
        fprintf(out, "        };\n");
        fprintf(out, "        vtable[_id](_t, _a);\n");
        fprintf(out, "    }\n");
    }

    fprintf(out, "    Q_UNUSED(_o);\n    Q_UNUSED(_id);\n    Q_UNUSED(_c);\n    Q_UNUSED(_a);\n}\n");
}

QT_END_NAMESPACE