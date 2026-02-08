// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#ifndef GENERATOR_H
#define GENERATOR_H

#include "moc.h"
#include <unordered_map>

QT_BEGIN_NAMESPACE

class Generator final
{
    Moc *parser = nullptr;
    FILE *out = nullptr;
    const ClassDef *cdef = nullptr;
    
    QList<QByteArray> strings;
    std::unordered_map<QByteArray, int> stringCache;
    
    const QList<QByteArray> metaTypes;
    const QHash<QByteArray, QByteArray> knownQObjectClasses;
    const QHash<QByteArray, QByteArray> knownGadgets;
    const QHash<QByteArray, QByteArray> hashes;
    
    QByteArray purestSuperClass;
    const bool requireCompleteTypes = false;

public:
    Generator(Moc *moc, const ClassDef *classDef, const QList<QByteArray> &metaTypes,
              const QHash<QByteArray, QByteArray> &knownQObjectClasses,
              const QHash<QByteArray, QByteArray> &knownGadgets,
              const QHash<QByteArray, QByteArray> &hashes,
              FILE *outfile = nullptr, bool requireCompleteTypes = false);
    
    void generateCode();
    
    [[nodiscard]] qsizetype registeredStringsCount() const { return strings.size(); }

private:
    bool registerableMetaType(const QByteArray &propertyType);
    void registerClassInfoStrings();
    void registerFunctionStrings(const QList<FunctionDef> &list);
    void registerByteArrayVector(const QList<QByteArray> &list);
    void addStrings(const QByteArrayList &strings);
    void addProperties();
    void addEnums();
    void addFunctions(const QList<FunctionDef> &list, const char *functype);
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
    
    QMultiMap<QByteArray, int> automaticPropertyMetaTypesHelper();
    QMap<int, QMultiMap<QByteArray, int>>
    methodsWithAutomaticTypesHelper(const QList<FunctionDef> &methodList);
    
    void strreg(const QByteArray &s);
    int stridx(const QByteArray &s);
};

QT_END_NAMESPACE

#endif
