--- START OF FILE generator.h ---

#ifndef GENERATOR_H
#define GENERATOR_H

#include "moc.h"
#include <vector>
#include <unordered_map>
#include <string_view>
// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
#include <span>
#include <optional>

QT_BEGIN_NAMESPACE

class Generator
{
	Moc *parser = nullptr;
	FILE *out;
	const ClassDef *cdef;
	std::vector<uint32_t> meta_data;

	public:
	Generator(Moc *moc, const ClassDef *classDef, const QList<QByteArray> &metaTypes,
	const QHash<QByteArray, QByteArray> &knownQObjectClasses,
	const QHash<QByteArray, QByteArray> &knownGadgets,
	const QHash<QByteArray, QByteArray> &hashes,
	FILE *outfile = nullptr, bool requireCompleteTypes = false);


	void generateCode();
	std::size_t registeredStringsCount() const { return strings.size(); }

	private:
	bool registerableMetaType(const QByteArray &propertyType);
	void registerClassInfoStrings();
	void registerFunctionStrings(std::span<const FunctionDef> list);
	void registerByteArrayVector(std::span<const QByteArray> list);
	void addStrings(std::span<const StringDef> list);
	void addProperties();
	void addEnums();
	void addFunctions(std::span<const FunctionDef> list, std::string_view functype);
	void addClassInfos();
	void generateMetacall();
	void generateStaticMetacall();
	void registerEnumStrings();
	void registerPropertyStrings();

	QByteArray disambiguatedTypeName(const QByteArray &name);
	QByteArray disambiguatedTypeName(const QByteArray &name, TypeTags tag);
	QByteArray disambiguatedTypeNameForCast(const QByteArray &name);

	std::unordered_map<QByteArray, std::vector<int>> automaticPropertyMetaTypesHelper();
	std::unordered_map<int, std::unordered_map<QByteArray, int>>
	methodsWithAutomaticTypesHelper(std::span<const FunctionDef> methodList);

	void strreg(const QByteArray &s);
	int stridx(const QByteArray &s);

	struct StringData {
		QByteArray str;
		int index;
	};

	std::vector<StringDef> strings;
	std::unordered_map<QByteArray, int> stringCache;
	QByteArray purestSuperClass;
	std::vector<QByteArray> metaTypes;
	std::unordered_map<QByteArray, QByteArray> knownQObjectClasses;
	std::unordered_map<QByteArray, QByteArray> knownGadgets;
	std::unordered_map<QByteArray, QByteArray> hashes;
	bool requireCompleteTypes;

};

QT_END_NAMESPACE

#endif
