#ifndef GENERATOR_H
#define GENERATOR_H

#include "moc.h"
#include <vector>
#include <unordered_map>
#include <span>

QT_BEGIN_NAMESPACE

class Generator
{
    Moc *parser = nullptr;
    FILE *out;
    const ClassDef *cdef;
    QList<uint> meta_data;

public:
    Generator(Moc *moc, const ClassDef *classDef, const QList<QByteArray> &metaTypes,
              const QHash<QByteArray, QByteArray> &knownQObjectClasses,
              const QHash<QByteArray, QByteArray> &knownGadgets,
              const QHash<QByteArray, QByteArray> &hashes,
              FILE *outfile = nullptr, bool requireCompleteTypes = false);
    void generateCode();
    qsizetype registeredStringsCount() { return strings.size(); }

private:
    bool registerableMetaType(const QByteArray &propertyType);
    void registerClassInfoStrings();
    void registerFunctionStrings(std::span<const FunctionDef> list);
    void registerByteArrayVector(std::span<const QByteArray> list);
    void addStrings(const std::vector<StringDef> &strings);
    void addProperties();
    void addEnums();
    void addFunctions(std::span<const FunctionDef> list, const char *functype);
    void addClassInfos();
    void generateTypeInfo(const QByteArray &typeName, bool allowEmptyName = false);
    void registerEnumStrings();
    void registerPropertyStrings();
    void generateMetacall();
    void generateStaticMetacall();
    void generateSignal(const FunctionDef *def, int index);
    void generatePluginMetaData();
    QByteArray disambiguatedTypeName(const QByteArray &name);
    QByteArray disambiguatedTypeName(const QByteArray &name, TypeTags tag);
    QByteArray disambiguatedTypeNameForCast(const QByteArray &name);
    std::unordered_map<QByteArray, int> automaticPropertyMetaTypesHelper();
    std::unordered_map<int, std::unordered_map<QByteArray, int>>
    methodsWithAutomaticTypesHelper(const std::vector<FunctionDef> &methodList);

    void strreg(const QByteArray &);
    int stridx(const QByteArray &);
    std::vector<StringDef> strings;
    std::unordered_map<QByteArray, int> stringCache;
    QByteArray purestSuperClass;
    QList<QByteArray> metaTypes;
    QHash<QByteArray, QByteArray> knownQObjectClasses;
    QHash<QByteArray, QByteArray> knownGadgets;
    QHash<QByteArray, QByteArray> hashes;
    bool requireCompleteTypes;
};

QT_END_NAMESPACE

#endif
