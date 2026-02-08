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
#include <cstdio>
#include <ranges>
#include <algorithm>
#include <string_view>
#include <print>
#include <utility>

QT_BEGIN_NAMESPACE

using namespace QtMiscUtils;

namespace {
struct QByteArrayHash {
std::size_t operator()(const QByteArray &b) const noexcept {
return qHash(b);
}
};
}

static int nameToBuiltinType(const QByteArray &name)
{
if (name.isEmpty())
return 0;


uint tp = QMetaType::UnknownType;
if (const auto *iface = QMetaType::fromName(name).iface())
    tp = iface->typeId.loadRelaxed();

#ifndef QT_BOOTSTRAPPED
if (tp >= uint(QMetaType::User))
tp = QMetaType::UnknownType;
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

Generator::Generator(Moc *moc, const ClassDef *classDef, const QList<QByteArray> &metaTypes,
const QHash<QByteArray, QByteArray> &knownQObjectClasses,
const QHash<QByteArray, QByteArray> &knownGadgets,
const QHash<QByteArray, QByteArray> &hashes,
FILE *outfile, bool requireCompleteTypes)
: parser(moc),
out(outfile),
cdef(classDef),
requireCompleteTypes(requireCompleteTypes)
{
this->metaTypes.reserve(metaTypes.size());
for (const auto &mt : metaTypes)
this->metaTypes.push_back(mt);


for (auto it = knownQObjectClasses.begin(); it != knownQObjectClasses.end(); ++it)
    this->knownQObjectClasses.emplace(it.key(), it.value());
for (auto it = knownGadgets.begin(); it != knownGadgets.end(); ++it)
    this->knownGadgets.emplace(it.key(), it.value());
for (auto it = hashes.begin(); it != hashes.end(); ++it)
    this->hashes.emplace(it.key(), it.value());

if (!cdef->superclassList.empty())
    purestSuperClass = cdef->superclassList.first().classname;

}

static inline std::size_t lengthOfEscapeSequence(const QByteArray &s, std::size_t i)
{
if (s.at(i) != '\' || i >= static_caststd::size_t(s.size()) - 1)
return 1;
const auto startPos = i;
++i;
char ch = s.at(i);
if (ch == 'x') {
++i;
while (i < static_caststd::size_t(s.size()) && std::isxdigit(static_cast<unsigned char>(s.at(i))))
++i;
} else if (std::isdigit(static_cast<unsigned char>(ch))) {
while (i < startPos + 4 && i < static_caststd::size_t(s.size()) && std::isdigit(static_cast<unsigned char>(s.at(i))))
++i;
} else {
++i;
}
return i - startPos;
}

int Generator::stridx(const QByteArray &s)
{
if (auto it = stringCache.find(s); it != stringCache.end())
return it->second;


int idx = static_cast<int>(strings.size());
strings.push_back({s, ""});
stringCache.emplace(s, idx);
return idx;

}

void Generator::strreg(const QByteArray &s)
{
if (s.isEmpty()) return;
if (!stringCache.contains(s)) {
stringCache.emplace(s, static_cast<int>(strings.size()));
strings.push_back({s, ""});
}
}

void Generator::registerClassInfoStrings()
{
for (const auto &c : cdef->classInfoList) {
strreg(c.name);
strreg(c.value);
}
}

void Generator::registerFunctionStrings(std::span<const FunctionDef> list)
{
for (const auto &f : list) {
strreg(f.name);
if (!isBuiltinType(f.normalizedType))
strreg(f.normalizedType);
for (const auto &a : f.arguments) {
if (!isBuiltinType(a.normalizedType))
strreg(a.normalizedType);
strreg(a.name);
}
}
}

void Generator::registerByteArrayVector(std::span<const QByteArray> list)
{
for (const auto &ba : list)
strreg(ba);
}

void Generator::registerPropertyStrings()
{
for (const auto &p : cdef->propertyList) {
strreg(p.name);
if (!isBuiltinType(p.type))
strreg(p.type);
}
}

void Generator::registerEnumStrings()
{
for (const auto &e : cdef->enumList) {
strreg(e.name);
if (!e.enumName.isNull())
strreg(e.enumName);
for (const auto &val : e.values)
strreg(val);
}
}

static QByteArray generateQualifiedClassNameIdentifier(const QByteArray &identifier)
{
QByteArray result = "ZN";
auto view = std::string_view(identifier.data(), identifier.size());
for (auto part : view | std::views::split(std::string_view("::"))) {
std::string_view p(part.begin(), part.end());
if (p.empty()) continue;
result += QByteArray::number(p.size());
result += QByteArray::fromRawData(p.data(), p.size());
}
result += 'E';
return result;
}

void Generator::generateCode()
{
const bool isQObject = (cdef->classname == "QObject");
const bool isConstructible = !cdef->constructorList.isEmpty();


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
bool hasStaticMetaCall = (cdef->hasQObject || !cdef->methodList.isEmpty() || 
                          !cdef->propertyList.isEmpty() || !cdef->constructorList.isEmpty());
if (parser->activeQtMode) hasStaticMetaCall = false;

const auto qualifiedClassNameIdentifier = generateQualifiedClassNameIdentifier(cdef->qualified);
const char *ownType = !cdef->hasQNamespace ? cdef->classname.data() : "void";

std::println(out, "namespace {{\nstruct qt_meta_tag_{}_t {{}};\n}} \n", 
             qualifiedClassNameIdentifier.constData());

std::println(out, "template <> constexpr inline auto {}::qt_create_metaobjectdata<qt_meta_tag_{}_t>()\n{{\n    namespace QMC = QtMocConstants;",
             cdef->qualified.constData(), qualifiedClassNameIdentifier.constData());

std::print(out, "\n    QtMocHelpers::StringRefStorage qt_stringData {{");
addStrings(strings);
std::print(out, "\n    }};\n\n");

std::print(out, "    QtMocHelpers::UintData qt_methods {{\n");
addFunctions(cdef->signalList, "Signal");
addFunctions(cdef->slotList, "Slot");
addFunctions(cdef->methodList, "Method");
std::print(out, "    }};\n    QtMocHelpers::UintData qt_properties {{\n");
addProperties();
std::print(out, "    }};\n    QtMocHelpers::UintData qt_enums {{\n");
addEnums();
std::print(out, "    }};\n");

std::println(out, "    constexpr int qt_metaObjectHashIndex = {};", stridx(hashes[cdef->qualified]));

std::string uintDataParams = "";
if (isConstructible || !cdef->classInfoList.isEmpty()) {
    if (isConstructible) {
        std::print(out, "    using Constructor = QtMocHelpers::NoType;\n    QtMocHelpers::UintData qt_constructors {{\n");
        addFunctions(cdef->constructorList, "Constructor");
        std::print(out, "    }};\n");
    } else {
        std::print(out, "    QtMocHelpers::UintData qt_constructors {{}};\n");
    }
    uintDataParams = ", qt_constructors";
    if (!cdef->classInfoList.isEmpty()) {
        std::print(out, "    QtMocHelpers::ClassInfos qt_classinfo({{\n");
        addClassInfos();
        std::print(out, "    }});\n");
        uintDataParams = ", qt_constructors, qt_classinfo";
    }
}

const char *metaObjectFlags = (cdef->hasQGadget || cdef->hasQNamespace) ? "QMC::PropertyAccessInStaticMetaCall" : "QMC::MetaObjectFlag{}";
QByteArray tagType = requireCompleteness ? QByteArrayLiteral("void") : "qt_meta_tag_" + qualifiedClassNameIdentifier + "_t";

std::println(out, "    return QtMocHelpers::metaObjectData<{}, {}>( {}, qt_stringData,\n"
             "            qt_methods, qt_properties, qt_enums, qt_metaObjectHashIndex{});\n}}",
             ownType, tagType.constData(), metaObjectFlags, uintDataParams);

if (cdef->hasQNamespace) {
    auto n = "_" + qualifiedClassNameIdentifier;
    std::println(out, R"(

static constexpr auto qt_staticMetaObjectContent{0} = {1}::qt_create_metaobjectdata<qt_meta_tag{0}_t>();
static constexpr auto qt_staticMetaObjectStaticContent{0} = qt_staticMetaObjectContent{0}.staticData;
static constexpr auto qt_staticMetaObjectRelocatingContent{0} = qt_staticMetaObjectContent{0}.relocatingData;
)", n.constData(), cdef->qualified.constData());
}


if (hasStaticMetaCall) generateStaticMetacall();
if (!isQObject) generateMetacall();
if (!cdef->hasQObject) return;

std::println(out, "\nconst QMetaObject *{}::metaObject() const\n{{\n    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;\n}}", 
             cdef->qualified.constData());

auto metaVarSuffix = cdef->hasQNamespace ? "_" + qualifiedClassNameIdentifier : "<qt_meta_tag_" + qualifiedClassNameIdentifier + "_t>";

std::println(out, "\nvoid *{}::qt_metacast(const char *_clname)\n{{\n"
             "    if (!_clname) return nullptr;\n"
             "    if (!strcmp(_clname, qt_staticMetaObjectStaticContent{}.stringdata0))\n"
             "        return static_cast<void*>(this);",
             cdef->qualified.constData(), metaVarSuffix.constData());

for (const auto &cl : cdef->superclassList) {
    if (!cl.classname.isEmpty())
        std::println(out, "    if (!strcmp(_clname, \"{}\"))\n        return static_cast<{}*>(this);", 
                     cl.classname.constData(), cl.classname.constData());
}

for (const auto &iface : cdef->interfaceList) {
    std::println(out, "    if (!strcmp(_clname, {}_iid))\n        return static_cast<{}*>(this);", 
                 iface.constData(), iface.constData());
}

std::println(out, "    return {}::qt_metacast(_clname);\n}}", 
             (!purestSuperClass.isEmpty() && !isQObject) ? purestSuperClass.constData() : "nullptr");

}

void Generator::addStrings(std::span<const StringDef> list)
{
for (const auto &[i, s] : list | std::views::enumerate) {
if (i > 0) std::print(out, ",");
std::print(out, "\n "");
std::size_t len = s.str.size();
for (std::size_t j = 0; j < len; ) {
auto escLen = lengthOfEscapeSequence(s.str, j);
if (escLen == 1) {
char c = s.str.at(j);
if (c == '\' || c == '"') std::print(out, "\{}", c);
else if (c >= 0x20 && c < 0x7F) std::print(out, "{}", c);
else std::print(out, "\x{:02x}", static_cast<unsigned char>(c));
} else {
std::print(out, "{:.{}}", s.str.constData() + j, static_cast<int>(escLen));
}
j += escLen;
}
std::print(out, """);
}
}

void Generator::addClassInfos()
{
for (const auto &c : cdef->classInfoList)
std::println(out, " {{{}, {}}},", stridx(c.name), stridx(c.value));
}

void Generator::addFunctions(std::span<const FunctionDef> list, std::string_view functype)
{
for (const auto &f : list) {
uint flags = 0;
if (f.access == FunctionDef::Private) flags |= 0x00000002;
else if (f.access == FunctionDef::Public) flags |= 0x00000001;
if (f.isConst) flags |= 0x00000004;


std::println(out, "        QtMocHelpers::{}Data({}, {}, {}, {}, 0x{:08x}),",
                 functype, stridx(f.name), f.arguments.size(),
                 f.arguments.isEmpty() ? 0 : stridx(f.arguments.first().name),
                 isBuiltinType(f.normalizedType) ? 0 : stridx(f.normalizedType),
                 flags);
}

}

void Generator::addProperties()
{
for (const auto &p : cdef->propertyList) {
uint flags = 0;
if (!p.read.isEmpty()) flags |= 0x00000001;
if (!p.write.isEmpty()) flags |= 0x00000002;
if (!p.reset.isEmpty()) flags |= 0x00000004;
if (p.constant) flags |= 0x00000400;
if (p.final) flags |= 0x00000800;
if (p.required) flags |= 0x00001000;


std::println(out, "        QtMocHelpers::PropertyData({}, {}, 0x{:08x}),",
                 stridx(p.name), isBuiltinType(p.type) ? 0 : stridx(p.type), flags);
}

}

void Generator::addEnums()
{
for (const auto &e : cdef->enumList) {
const auto &typeName = e.enumName.isNull() ? e.name : e.enumName;
std::print(out, " QtMocHelpers::EnumData<{}>( {}, {},",
disambiguatedTypeName(e.name).constData(), stridx(e.name), stridx(typeName));


if (e.flags) {
        std::string sep = "";
        if (e.flags & EnumIsFlag) { std::print(out, " QMC::EnumIsFlag"); sep = " |"; }
        if (e.flags & EnumIsScoped) std::print(out, "{} QMC::EnumIsScoped", sep);
    } else {
        std::print(out, " QMC::EnumFlags{{}}");
    }

    if (e.values.isEmpty()) {
        std::print(out, "),\n");
        continue;
    }

    std::print(out, ").add({{\n");
    QByteArray prefix = (e.enumName.isNull() ? e.name : e.enumName);
    for (const auto &val : e.values)
        std::println(out, "            {{ {:4}, {}::{} }},", stridx(val), prefix.constData(), val.constData());
    std::print(out, "        }}),\n");
}

}

void Generator::generateMetacall()
{
std::println(out, "\nint {}::qt_metacall(QMetaObject::Call _c, int _id, void **_a)\n{{", cdef->qualified.constData());
if (!purestSuperClass.isEmpty() && (cdef->classname != "QObject"))
std::println(out, " _id = {}::qt_metacall(_c, _id, _a);", purestSuperClass.constData());


std::vector<FunctionDef> methods;
for (const auto &l : {cdef->signalList, cdef->slotList, cdef->methodList})
    for (const auto &f : l) methods.push_back(f);

if (!methods.empty() || !cdef->propertyList.isEmpty())
    std::print(out, "    if (_id < 0) return _id;\n");

if (!methods.empty()) {
    std::println(out, "    if (_c == QMetaObject::InvokeMetaMethod) {{\n"
                 "        if (_id < {0}) qt_static_metacall(this, _c, _id, _a);\n"
                 "        _id -= {0};\n    }}", methods.size());
}


auto methodsWithTypes = methodsWithAutomaticTypesHelper(methods);
if (!methodsWithTypes.empty()) {
     std::print(out, "    else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {{\n");
     std::println(out, "        if (_id < {}) {{", methods.size());
     std::print(out, "            qt_static_metacall(this, _c, _id, _a);\n");
     
     std::println(out, "        }}\n        _id -= {};\n    }}", methods.size());
}

if (!cdef->propertyList.isEmpty()) {
    std::println(out,
        "    else if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty\n"
        "            || _c == QMetaObject::ResetProperty || _c == QMetaObject::BindableProperty\n"
        "            || _c == QMetaObject::RegisterPropertyMetaType) {{\n"
        "        qt_static_metacall(this, _c, _id, _a);\n"
        "        _id -= {};\n    }}", cdef->propertyList.size());
}
std::print(out, "    return _id;\n}}\n");

}

std::unordered_map<QByteArray, std::vector<int>> Generator::automaticPropertyMetaTypesHelper()
{
std::unordered_map<QByteArray, std::vector<int>> result;
for (const auto &[i, p] : cdef->propertyList | std::views::enumerate) {
if (registerableMetaType(p.type) && !isBuiltinType(p.type))
result[cxxTypeTag(p.typeTag) + p.type].push_back(static_cast<int>(i));
}
return result;
}

std::unordered_map<int, std::unordered_map<QByteArray, int>>
Generator::methodsWithAutomaticTypesHelper(std::span<const FunctionDef> methodList)
{
std::unordered_map<int, std::unordered_map<QByteArray, int>> result;
for (const auto &[i, f] : methodList | std::views::enumerate) {
for (const auto &[j, arg] : f.arguments | std::views::enumerate) {
if (registerableMetaType(arg.normalizedType) && !isBuiltinType(arg.normalizedType))
result[static_cast<int>(i)][arg.normalizedType] = static_cast<int>(j);
}
}
return result;
}

void Generator::generateStaticMetacall()
{
std::println(out, "void {}::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)\n{{",
cdef->qualified.constData());


if (cdef->hasQObject) std::println(out, "    auto *_t = static_cast<{} *>(_o);", cdef->classname.constData());
else std::println(out, "    auto *_t = reinterpret_cast<{} *>(_o);", cdef->classname.constData());

auto genCtorArgs = [&](const FunctionDef &f) {
    for (const auto &[i, a] : f.arguments | std::views::enumerate) {
        if (i > 0) std::print(out, ",");
        std::print(out, "(*reinterpret_cast<{0}>(_a[{1}]))", disambiguatedTypeNameForCast(a.normalizedType).constData(), i + 1);
    }
};

if (!cdef->constructorList.isEmpty()) {
    std::print(out, "    if (_c == QMetaObject::CreateInstance) {{\n        switch (_id) {{\n");
    for (const auto &[i, f] : cdef->constructorList | std::views::enumerate) {
        std::print(out, "        case {}: {{ auto *_r = new {}(", i, cdef->classname.constData());
        genCtorArgs(f);
        std::println(out, ");\n            if (_a[0]) *reinterpret_cast<{}**>(_a[0]) = _r; }} break;",
                     (cdef->hasQGadget || cdef->hasQNamespace) ? "void" : "QObject");
    }
    std::print(out, "        default: break;\n        }}\n    }}\n");
    
    std::print(out, "    else if (_c == QMetaObject::ConstructInPlace) {{\n        switch (_id) {{\n");
    for (const auto &[i, f] : cdef->constructorList | std::views::enumerate) {
        std::print(out, "        case {}: {{ new (_a[0]) {}(", i, cdef->classname.constData());
        genCtorArgs(f);
        std::print(out, "); }} break;\n");
    }
    std::print(out, "        default: break;\n        }}\n    }}\n");
}

std::vector<FunctionDef> methods;
for (const auto &l : {cdef->signalList, cdef->slotList, cdef->methodList})
    for (const auto &f : l) methods.push_back(f);

if (!methods.empty()) {
    std::println(out, "    else if (_c == QMetaObject::InvokeMetaMethod) {{\n"
                 "        using FP = void (*)({}*, void**);\n"
                 "        static constexpr FP vtable[] = {{", cdef->classname.constData());
    for (const auto &f : methods) {
        std::print(out, "            []({} *_t, void **_a) {{ ", cdef->classname.constData());
        if (f.normalizedType != "void") std::print(out, "auto _r = ");
        std::print(out, "_t->{}{}(", f.inPrivateClass.isEmpty() ? "" : f.inPrivateClass.constData() + "->", f.name.constData());
        
        if (f.isRawSlot) std::print(out, "QMethodRawArguments{{_a}}");
        else {
            for (const auto &[i, a] : f.arguments | std::views::enumerate) {
                if (i > 0) std::print(out, ",");
                std::print(out, "(*reinterpret_cast<{0}>(_a[{1}]))", disambiguatedTypeNameForCast(a.normalizedType).constData(), i + 1);
            }
            if (f.isPrivateSignal) {
                if (!f.arguments.isEmpty()) std::print(out, ", ");
                std::print(out, "QPrivateSignal()");
            }
        }
        std::print(out, ");");
        if (f.normalizedType != "void")
            std::print(out, " if (_a[0]) *reinterpret_cast<{}*>(_a[0]) = std::move(_r);", disambiguatedTypeName(noRef(f.normalizedType)).constData());
        std::print(out, " }},\n");
    }
    std::print(out, "        }};\n        Q_ASSERT(_id >= 0 && _id < {});\n        vtable[_id](_t, _a);\n    }}\n", methods.size());

    auto methodsWithTypes = methodsWithAutomaticTypesHelper(methods);
    if (!methodsWithTypes.empty()) {
        std::print(out, "    else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {{\n"
                        "        switch (_id) {{\n"
                        "        default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;\n");
        
        for (const auto &[id, argMap] : methodsWithTypes) {
            std::println(out, "        case {}:", id);
            std::print(out, "            switch (*reinterpret_cast<int*>(_a[1])) {{\n"
                            "            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;\n");
            for (const auto &[type, idx] : argMap) {
                std::println(out, "            case {}:", idx);
                std::println(out, "                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< {} >(); break;", type.constData());
            }
            std::print(out, "            }}\n            break;\n");
        }
        std::print(out, "        }}\n    }}\n");
    }
}


if (!cdef->signalList.isEmpty()) {
    std::print(out, "    else if (_c == QMetaObject::IndexOfMethod) {{\n");
    for (const auto &[i, f] : cdef->signalList | std::views::enumerate) {
        if (f.wasCloned || !f.inPrivateClass.isEmpty() || f.isStatic) continue;
        std::println(out, "        if (QtMocHelpers::indexOfMethod<{} ({}::*)(", f.type.rawName.constData(), cdef->classname.constData());
        
        for (const auto &[j, a] : f.arguments | std::views::enumerate) {
            if (j > 0) std::print(out, ", ");
            std::print(out, "{}", (a.type.name + ' ' + a.rightType).constData());
        }
        if (f.isPrivateSignal) {
            if (!f.arguments.isEmpty()) std::print(out, ", ");
            std::print(out, "QPrivateSignal");
        }
        std::println(out, ") {0}>(&{1}::{2}, {3})) return;", 
                     f.isConst ? "const" : "", cdef->classname.constData(), f.name.constData(), i);
    }
    std::print(out, "    }}\n");
}

auto autoProp = automaticPropertyMetaTypesHelper();
if (!autoProp.empty()) {
    std::print(out, "    else if (_c == QMetaObject::RegisterPropertyMetaType) {{\n"
                    "        switch (_id) {{\n"
                    "        default: *reinterpret_cast<int*>(_a[0]) = -1; break;\n");
    for (const auto &[type, indices] : autoProp) {
        for (int idx : indices) {
            std::println(out, "        case {}:", idx);
        }
        std::println(out, "            *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< {} >(); break;", type.constData());
    }
    std::print(out, "        }}\n    }}\n");
}

if (!cdef->propertyList.empty()) {
    bool needGet = false, needSet = false, needReset = false, hasBind = false;
    for (const auto &p : cdef->propertyList) {
        if (!p.read.isEmpty() || !p.member.isEmpty()) needGet = true;
        if (!p.write.isEmpty() || (!p.member.isEmpty() && !p.constant)) needSet = true;
        if (!p.reset.isEmpty()) needReset = true;
        if (!p.bind.isEmpty()) hasBind = true;
    }

    if (needGet) {
        std::print(out, "    else if (_c == QMetaObject::ReadProperty) {{\n"
                        "        void *_v = _a[0];\n"
                        "        switch (_id) {{\n");
        for (const auto &[i, p] : cdef->propertyList | std::views::enumerate) {
            if (p.read.isEmpty() && p.member.isEmpty()) continue;
            auto prefix = (p.inPrivateClass.isEmpty() ? "_t->" : "_t->" + p.inPrivateClass + "->");
            
            std::println(out, "        case {}:", i);
            if (p.gspec == PropertyDef::PointerSpec)
                std::println(out, " _a[0] = const_cast<void*>(reinterpret_cast<const void*>({}{}())); break;", prefix.constData(), p.read.constData());
            else if (p.gspec == PropertyDef::ReferenceSpec)
                std::println(out, " _a[0] = const_cast<void*>(reinterpret_cast<const void*>(&{}{}())); break;", prefix.constData(), p.read.constData());
            else {
                auto type = cxxTypeTag(p.typeTag) + disambiguatedTypeName(p.type, p.typeTag);
                if (p.read == "default")
                    std::println(out, " *reinterpret_cast<{}*>(_v) = {}{}().value(); break;", type.constData(), prefix.constData(), p.bind.constData());
                else if (!p.read.isEmpty())
                    std::println(out, " *reinterpret_cast<{}*>(_v) = {}{}(); break;", type.constData(), prefix.constData(), p.read.constData());
                else
                    std::println(out, " *reinterpret_cast<{}*>(_v) = {}{}; break;", type.constData(), prefix.constData(), p.member.constData());
            }
        }
        std::print(out, "        default: break;\n        }}\n    }}\n");
    }
    
    if (needSet) {
        std::print(out, "    else if (_c == QMetaObject::WriteProperty) {{\n"
                        "        void *_v = _a[0];\n"
                        "        switch (_id) {{\n");
        for (const auto &[i, p] : cdef->propertyList | std::views::enumerate) {
            if (p.constant || (p.write.isEmpty() && p.member.isEmpty())) continue;
            auto prefix = (p.inPrivateClass.isEmpty() ? "_t->" : "_t->" + p.inPrivateClass + "->");
            auto type = cxxTypeTag(p.typeTag) + disambiguatedTypeName(p.type, p.typeTag);
            
            std::println(out, "        case {}:", i);
            if (p.write == "default")
                std::println(out, " {}{}().setValue(*reinterpret_cast<{}*>(_v)); break;", prefix.constData(), p.bind.constData(), type.constData());
            else if (!p.write.isEmpty())
                std::println(out, " {}{}(*reinterpret_cast<{}*>(_v)); break;", prefix.constData(), p.write.constData(), type.constData());
            else
                std::println(out, " {}{} = *reinterpret_cast<{}*>(_v); break;", prefix.constData(), p.member.constData(), type.constData());
        }
        std::print(out, "        default: break;\n        }}\n    }}\n");
    }

    if (needReset) {
        std::print(out, "    else if (_c == QMetaObject::ResetProperty) {{\n"
                        "        switch (_id) {{\n");
        for (const auto &[i, p] : cdef->propertyList | std::views::enumerate) {
            if (p.reset.isEmpty()) continue;
            auto prefix = (p.inPrivateClass.isEmpty() ? "_t->" : "_t->" + p.inPrivateClass + "->");
            std::println(out, "        case {}: {}{}(); break;", i, prefix.constData(), p.reset.constData());
        }
        std::print(out, "        default: break;\n        }}\n    }}\n");
    }

    if (hasBind) {
        std::print(out, "    else if (_c == QMetaObject::BindableProperty) {{\n"
                        "        switch (_id) {{\n");
        for (const auto &[i, p] : cdef->propertyList | std::views::enumerate) {
            if (p.bind.isEmpty()) continue;
            auto prefix = (p.inPrivateClass.isEmpty() ? "_t->" : "_t->" + p.inPrivateClass + "->");
            std::println(out, "        case {}: *static_cast<QUntypedBindable *>(_a[0]) = {}{}(); break;", i, prefix.constData(), p.bind.constData());
        }
        std::print(out, "        default: break;\n        }}\n    }}\n");
    }
}

std::print(out, "    Q_UNUSED(_t); Q_UNUSED(_c); Q_UNUSED(_id); Q_UNUSED(_a);\n}\n");

}

QT_END_NAMESPACE
