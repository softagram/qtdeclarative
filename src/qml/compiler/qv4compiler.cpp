/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <qv4compiler_p.h>
#include <qv4compileddata_p.h>
#include <qv4codegen_p.h>
#include <private/qv4string_p.h>
#include <private/qv4value_p.h>
#include <private/qv4alloca_p.h>
#include <private/qqmljslexer_p.h>
#include <private/qqmljsast_p.h>
#include <wtf/MathExtras.h>
#include <QCryptographicHash>

QV4::Compiler::StringTableGenerator::StringTableGenerator()
{
    clear();
}

int QV4::Compiler::StringTableGenerator::registerString(const QString &str)
{
    QHash<QString, int>::ConstIterator it = stringToId.constFind(str);
    if (it != stringToId.cend())
        return *it;
    stringToId.insert(str, strings.size());
    strings.append(str);
    stringDataSize += QV4::CompiledData::String::calculateSize(str);
    return strings.size() - 1;
}

int QV4::Compiler::StringTableGenerator::getStringId(const QString &string) const
{
    Q_ASSERT(stringToId.contains(string));
    return stringToId.value(string);
}

void QV4::Compiler::StringTableGenerator::clear()
{
    strings.clear();
    stringToId.clear();
    stringDataSize = 0;
}

void QV4::Compiler::StringTableGenerator::serialize(CompiledData::Unit *unit)
{
    char *dataStart = reinterpret_cast<char *>(unit);
    quint32_le *stringTable = reinterpret_cast<quint32_le *>(dataStart + unit->offsetToStringTable);
    char *stringData = dataStart + unit->offsetToStringTable + unit->stringTableSize * sizeof(uint);
    for (int i = 0; i < strings.size(); ++i) {
        stringTable[i] = stringData - dataStart;
        const QString &qstr = strings.at(i);

        QV4::CompiledData::String *s = reinterpret_cast<QV4::CompiledData::String *>(stringData);
        s->size = qstr.length();
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
        memcpy(s + 1, qstr.constData(), qstr.length()*sizeof(ushort));
#else
        ushort *uc = reinterpret_cast<ushort *>(s + 1);
        for (int i = 0; i < qstr.length(); ++i)
            uc[i] = qToLittleEndian<ushort>(qstr.at(i).unicode());
#endif

        stringData += QV4::CompiledData::String::calculateSize(qstr);
    }
}

QV4::Compiler::JSUnitGenerator::JSUnitGenerator(QV4::Compiler::Module *module)
    : module(module)
{
    // Make sure the empty string always gets index 0
    registerString(QString());
}

int QV4::Compiler::JSUnitGenerator::registerGetterLookup(const QString &name)
{
    return registerGetterLookup(registerString(name));
}

int QV4::Compiler::JSUnitGenerator::registerGetterLookup(int nameIndex)
{
    CompiledData::Lookup l;
    l.type_and_flags = CompiledData::Lookup::Type_Getter;
    l.nameIndex = nameIndex;
    lookups << l;
    return lookups.size() - 1;
}

int QV4::Compiler::JSUnitGenerator::registerSetterLookup(const QString &name)
{
    return registerSetterLookup(registerString(name));
}

int QV4::Compiler::JSUnitGenerator::registerSetterLookup(int nameIndex)
{
    CompiledData::Lookup l;
    l.type_and_flags = CompiledData::Lookup::Type_Setter;
    l.nameIndex = nameIndex;
    lookups << l;
    return lookups.size() - 1;
}

int QV4::Compiler::JSUnitGenerator::registerGlobalGetterLookup(const QString &name)
{
    return registerGlobalGetterLookup(registerString(name));
}

int QV4::Compiler::JSUnitGenerator::registerGlobalGetterLookup(int nameIndex)
{
    CompiledData::Lookup l;
    l.type_and_flags = CompiledData::Lookup::Type_GlobalGetter;
    l.nameIndex = nameIndex;
    lookups << l;
    return lookups.size() - 1;
}

int QV4::Compiler::JSUnitGenerator::registerRegExp(QQmlJS::AST::RegExpLiteral *regexp)
{
    CompiledData::RegExp re;
    re.stringIndex = registerString(regexp->pattern.toString());

    re.flags = 0;
    if (regexp->flags & QQmlJS::Lexer::RegExp_Global)
        re.flags |= CompiledData::RegExp::RegExp_Global;
    if (regexp->flags &  QQmlJS::Lexer::RegExp_IgnoreCase)
        re.flags |= CompiledData::RegExp::RegExp_IgnoreCase;
    if (regexp->flags &  QQmlJS::Lexer::RegExp_Multiline)
        re.flags |= CompiledData::RegExp::RegExp_Multiline;

    regexps.append(re);
    return regexps.size() - 1;
}

int QV4::Compiler::JSUnitGenerator::registerConstant(QV4::ReturnedValue v)
{
    int idx = constants.indexOf(v);
    if (idx >= 0)
        return idx;
    constants.append(v);
    return constants.size() - 1;
}

QV4::ReturnedValue QV4::Compiler::JSUnitGenerator::constant(int idx)
{
    return constants.at(idx);
}

int QV4::Compiler::JSUnitGenerator::registerJSClass(const QVector<MemberInfo> &members)
{
    // ### re-use existing class definitions.

    const int size = CompiledData::JSClass::calculateSize(members.size());
    jsClassOffsets.append(jsClassData.size());
    const int oldSize = jsClassData.size();
    jsClassData.resize(jsClassData.size() + size);
    memset(jsClassData.data() + oldSize, 0, size);

    CompiledData::JSClass *jsClass = reinterpret_cast<CompiledData::JSClass*>(jsClassData.data() + oldSize);
    jsClass->nMembers = members.size();
    CompiledData::JSClassMember *member = reinterpret_cast<CompiledData::JSClassMember*>(jsClass + 1);

    for (const MemberInfo &memberInfo : members) {
        member->nameOffset = registerString(memberInfo.name);
        member->isAccessor = memberInfo.isAccessor;
        ++member;
    }

    return jsClassOffsets.size() - 1;
}

int QV4::Compiler::JSUnitGenerator::registerJSClass(int count, CompiledData::JSClassMember *members)
{
    const int size = CompiledData::JSClass::calculateSize(count);
    jsClassOffsets.append(jsClassData.size());
    const int oldSize = jsClassData.size();
    jsClassData.resize(jsClassData.size() + size);
    memset(jsClassData.data() + oldSize, 0, size);

    CompiledData::JSClass *jsClass = reinterpret_cast<CompiledData::JSClass*>(jsClassData.data() + oldSize);
    jsClass->nMembers = count;
    CompiledData::JSClassMember *jsClassMembers = reinterpret_cast<CompiledData::JSClassMember*>(jsClass + 1);
    memcpy(jsClassMembers, members, sizeof(CompiledData::JSClassMember)*count);

    return jsClassOffsets.size() - 1;
}

QV4::CompiledData::Unit *QV4::Compiler::JSUnitGenerator::generateUnit(GeneratorOption option)
{
    registerString(module->fileName);
    registerString(module->finalUrl);
    for (Context *f : qAsConst(module->functions)) {
        registerString(f->name);
        for (int i = 0; i < f->arguments.size(); ++i)
            registerString(f->arguments.at(i));
        for (int i = 0; i < f->locals.size(); ++i)
            registerString(f->locals.at(i));
    }

    Q_ALLOCA_VAR(quint32_le, functionOffsets, module->functions.size() * sizeof(quint32_le));
    uint jsClassDataOffset = 0;

    char *dataPtr;
    CompiledData::Unit *unit;
    {
        QV4::CompiledData::Unit tempHeader = generateHeader(option, functionOffsets, &jsClassDataOffset);
        dataPtr = reinterpret_cast<char *>(malloc(tempHeader.unitSize));
        memset(dataPtr, 0, tempHeader.unitSize);
        memcpy(&unit, &dataPtr, sizeof(CompiledData::Unit*));
        memcpy(unit, &tempHeader, sizeof(tempHeader));
    }

    memcpy(dataPtr + unit->offsetToFunctionTable, functionOffsets, unit->functionTableSize * sizeof(quint32_le));

    for (int i = 0; i < module->functions.size(); ++i) {
        Context *function = module->functions.at(i);
        if (function == module->rootContext)
            unit->indexOfRootFunction = i;

        writeFunction(dataPtr + functionOffsets[i], function);
    }

    CompiledData::Lookup *lookupsToWrite = reinterpret_cast<CompiledData::Lookup*>(dataPtr + unit->offsetToLookupTable);
    for (const CompiledData::Lookup &l : qAsConst(lookups))
        *lookupsToWrite++ = l;

    CompiledData::RegExp *regexpTable = reinterpret_cast<CompiledData::RegExp *>(dataPtr + unit->offsetToRegexpTable);
    memcpy(regexpTable, regexps.constData(), regexps.size() * sizeof(*regexpTable));

#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    ReturnedValue *constantTable = reinterpret_cast<ReturnedValue *>(dataPtr + unit->offsetToConstantTable);
    memcpy(constantTable, constants.constData(), constants.size() * sizeof(ReturnedValue));
#else
    quint64_le *constantTable = reinterpret_cast<quint64_le *>(dataPtr + unit->offsetToConstantTable);
    for (int i = 0; i < constants.count(); ++i)
        constantTable[i] = constants.at(i);
#endif

    {
        memcpy(dataPtr + jsClassDataOffset, jsClassData.constData(), jsClassData.size());

        // write js classes and js class lookup table
        quint32_le *jsClassOffsetTable = reinterpret_cast<quint32_le *>(dataPtr + unit->offsetToJSClassTable);
        for (int i = 0; i < jsClassOffsets.count(); ++i)
            jsClassOffsetTable[i] = jsClassDataOffset + jsClassOffsets.at(i);
    }

    // write strings and string table
    if (option == GenerateWithStringTable)
        stringTable.serialize(unit);

    unit->generateChecksum();

    return unit;
}

void QV4::Compiler::JSUnitGenerator::writeFunction(char *f, QV4::Compiler::Context *irFunction) const
{
    QV4::CompiledData::Function *function = (QV4::CompiledData::Function *)f;

    quint32 currentOffset = sizeof(QV4::CompiledData::Function);
    currentOffset = (currentOffset + 7) & ~quint32(0x7);

    function->nameIndex = getStringId(irFunction->name);
    function->flags = 0;
    if (irFunction->hasDirectEval)
        function->flags |= CompiledData::Function::HasDirectEval;
    if (irFunction->usesArgumentsObject)
        function->flags |= CompiledData::Function::UsesArgumentsObject;
    if (irFunction->isStrict)
        function->flags |= CompiledData::Function::IsStrict;
    if (irFunction->hasTry || irFunction->hasWith)
        function->flags |= CompiledData::Function::HasCatchOrWith;
    function->nFormals = irFunction->arguments.size();
    function->formalsOffset = currentOffset;
    currentOffset += function->nFormals * sizeof(quint32);

    function->nLocals = irFunction->locals.size();
    function->localsOffset = currentOffset;
    currentOffset += function->nLocals * sizeof(quint32);

    function->nLineNumbers = irFunction->lineNumberMapping.size();
    function->lineNumberOffset = currentOffset;
    currentOffset += function->nLineNumbers * sizeof(CompiledData::CodeOffsetToLine);

    function->nInnerFunctions = irFunction->nestedContexts.size();

    function->nRegisters = irFunction->registerCount;

    function->nDependingIdObjects = 0;
    function->nDependingContextProperties = 0;
    function->nDependingScopeProperties = 0;

    if (!irFunction->idObjectDependencies.isEmpty()) {
        function->nDependingIdObjects = irFunction->idObjectDependencies.count();
        function->dependingIdObjectsOffset = currentOffset;
        currentOffset += function->nDependingIdObjects * sizeof(quint32);
    }

    if (!irFunction->contextObjectPropertyDependencies.isEmpty()) {
        function->nDependingContextProperties = irFunction->contextObjectPropertyDependencies.count();
        function->dependingContextPropertiesOffset = currentOffset;
        currentOffset += function->nDependingContextProperties * sizeof(quint32) * 2;
    }

    if (!irFunction->scopeObjectPropertyDependencies.isEmpty()) {
        function->nDependingScopeProperties = irFunction->scopeObjectPropertyDependencies.count();
        function->dependingScopePropertiesOffset = currentOffset;
        currentOffset += function->nDependingScopeProperties * sizeof(quint32) * 2;
    }

    function->location.line = irFunction->line;
    function->location.column = irFunction->column;

    function->codeOffset = currentOffset;
    function->codeSize = irFunction->code.size();

    // write formals
    quint32_le *formals = (quint32_le *)(f + function->formalsOffset);
    for (int i = 0; i < irFunction->arguments.size(); ++i)
        formals[i] = getStringId(irFunction->arguments.at(i));

    // write locals
    quint32_le *locals = (quint32_le *)(f + function->localsOffset);
    for (int i = 0; i < irFunction->locals.size(); ++i)
        locals[i] = getStringId(irFunction->locals.at(i));

    // write line numbers
    memcpy(f + function->lineNumberOffset, irFunction->lineNumberMapping.constData(), irFunction->lineNumberMapping.size()*sizeof(CompiledData::CodeOffsetToLine));

    // write QML dependencies
    quint32_le *writtenDeps = (quint32_le *)(f + function->dependingIdObjectsOffset);
    for (int id : irFunction->idObjectDependencies) {
        Q_ASSERT(id >= 0);
        *writtenDeps++ = static_cast<quint32>(id);
    }

    writtenDeps = (quint32_le *)(f + function->dependingContextPropertiesOffset);
    for (auto property : irFunction->contextObjectPropertyDependencies) {
        *writtenDeps++ = property.key(); // property index
        *writtenDeps++ = property.value(); // notify index
    }

    writtenDeps = (quint32_le *)(f + function->dependingScopePropertiesOffset);
    for (auto property : irFunction->scopeObjectPropertyDependencies) {
        *writtenDeps++ = property.key(); // property index
        *writtenDeps++ = property.value(); // notify index
    }

    // write byte code
    memcpy(f + function->codeOffset, irFunction->code.constData(), irFunction->code.size());
}

QV4::CompiledData::Unit QV4::Compiler::JSUnitGenerator::generateHeader(QV4::Compiler::JSUnitGenerator::GeneratorOption option, quint32_le *functionOffsets, uint *jsClassDataOffset)
{
    CompiledData::Unit unit;
    memset(&unit, 0, sizeof(unit));
    memcpy(unit.magic, CompiledData::magic_str, sizeof(unit.magic));
    unit.flags = QV4::CompiledData::Unit::IsJavascript;
    unit.flags |= module->unitFlags;
    unit.version = QV4_DATA_STRUCTURE_VERSION;
    unit.qtVersion = QT_VERSION;
    memset(unit.md5Checksum, 0, sizeof(unit.md5Checksum));
    memset(unit.dependencyMD5Checksum, 0, sizeof(unit.dependencyMD5Checksum));

    quint32 nextOffset = sizeof(CompiledData::Unit);

    unit.functionTableSize = module->functions.size();
    unit.offsetToFunctionTable = nextOffset;
    nextOffset += unit.functionTableSize * sizeof(uint);

    unit.lookupTableSize = lookups.count();
    unit.offsetToLookupTable = nextOffset;
    nextOffset += unit.lookupTableSize * sizeof(CompiledData::Lookup);

    unit.regexpTableSize = regexps.size();
    unit.offsetToRegexpTable = nextOffset;
    nextOffset += unit.regexpTableSize * sizeof(CompiledData::RegExp);

    unit.constantTableSize = constants.size();

    // Ensure we load constants from well-aligned addresses into for example SSE registers.
    nextOffset = static_cast<quint32>(WTF::roundUpToMultipleOf(16, nextOffset));
    unit.offsetToConstantTable = nextOffset;
    nextOffset += unit.constantTableSize * sizeof(ReturnedValue);

    unit.jsClassTableSize = jsClassOffsets.count();
    unit.offsetToJSClassTable = nextOffset;
    nextOffset += unit.jsClassTableSize * sizeof(uint);

    *jsClassDataOffset = nextOffset;
    nextOffset += jsClassData.size();

    nextOffset = (nextOffset + 7) & ~quint32(0x7);

    for (int i = 0; i < module->functions.size(); ++i) {
        Context *f = module->functions.at(i);
        functionOffsets[i] = nextOffset;

        const int qmlIdDepsCount = f->idObjectDependencies.count();
        const int qmlPropertyDepsCount = f->scopeObjectPropertyDependencies.count() + f->contextObjectPropertyDependencies.count();
        nextOffset += QV4::CompiledData::Function::calculateSize(f->arguments.size(), f->locals.size(), f->lineNumberMapping.size(), f->nestedContexts.size(),
                                                                 qmlIdDepsCount, qmlPropertyDepsCount, f->code.size());
    }

    if (option == GenerateWithStringTable) {
        unit.stringTableSize = stringTable.stringCount();
        unit.offsetToStringTable = nextOffset;
        nextOffset += stringTable.sizeOfTableAndData();
    } else {
        unit.stringTableSize = 0;
        unit.offsetToStringTable = 0;
    }
    unit.indexOfRootFunction = -1;
    unit.sourceFileIndex = getStringId(module->fileName);
    unit.finalUrlIndex = getStringId(module->finalUrl);
    unit.sourceTimeStamp = module->sourceTimeStamp.isValid() ? module->sourceTimeStamp.toMSecsSinceEpoch() : 0;
    unit.nImports = 0;
    unit.offsetToImports = 0;
    unit.nObjects = 0;
    unit.offsetToObjects = 0;

    unit.unitSize = nextOffset;

    return unit;
}
