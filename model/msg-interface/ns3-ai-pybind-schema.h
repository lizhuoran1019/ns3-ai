/*
 * Copyright (c) 2023 Huazhong University of Science and Technology
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 */

#ifndef NS3_AI_PYBIND_SCHEMA_H
#define NS3_AI_PYBIND_SCHEMA_H

#include "ns3-ai-msg-interface.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>

namespace ns3
{

namespace py = pybind11;

inline void
BindNs3AiMsgSchemaTypes(py::module_& module)
{
    py::enum_<Ns3AiMsgFieldType>(module, "Ns3AiMsgFieldType")
        .value("Bool", Ns3AiMsgFieldType::Bool)
        .value("Int8", Ns3AiMsgFieldType::Int8)
        .value("UInt8", Ns3AiMsgFieldType::UInt8)
        .value("Int16", Ns3AiMsgFieldType::Int16)
        .value("UInt16", Ns3AiMsgFieldType::UInt16)
        .value("Int32", Ns3AiMsgFieldType::Int32)
        .value("UInt32", Ns3AiMsgFieldType::UInt32)
        .value("Int64", Ns3AiMsgFieldType::Int64)
        .value("UInt64", Ns3AiMsgFieldType::UInt64)
        .value("Float", Ns3AiMsgFieldType::Float)
        .value("Double", Ns3AiMsgFieldType::Double)
        .value("RawBytes", Ns3AiMsgFieldType::RawBytes);

    py::class_<Ns3AiMsgField>(module, "Ns3AiMsgField")
        .def(py::init<>())
        .def_readwrite("name", &Ns3AiMsgField::m_name)
        .def_readwrite("type", &Ns3AiMsgField::m_type)
        .def_readwrite("offset", &Ns3AiMsgField::m_offset)
        .def_readwrite("size", &Ns3AiMsgField::m_size)
        .def_readwrite("count", &Ns3AiMsgField::m_count);

    py::class_<Ns3AiMsgSchema>(module, "Ns3AiMsgSchema")
        .def(py::init<>())
        .def_readwrite("name", &Ns3AiMsgSchema::m_name)
        .def_readwrite("version", &Ns3AiMsgSchema::m_version)
        .def_readwrite("schema_hash", &Ns3AiMsgSchema::m_schemaHash)
        .def_readwrite("size", &Ns3AiMsgSchema::m_size)
        .def_readwrite("fields", &Ns3AiMsgSchema::m_fields);

    py::class_<Ns3AiMsgProtocolHeader>(module, "Ns3AiMsgProtocolHeader")
        .def_property_readonly("magic", [](const Ns3AiMsgProtocolHeader& header) {
            return static_cast<uint32_t>(header.m_magic.load(std::memory_order_acquire));
        })
        .def_property_readonly("abi_version", [](const Ns3AiMsgProtocolHeader& header) {
            return static_cast<uint16_t>(header.m_abiVersion.load(std::memory_order_acquire));
        })
        .def_property_readonly("header_size", [](const Ns3AiMsgProtocolHeader& header) {
            return static_cast<uint16_t>(header.m_headerSize.load(std::memory_order_acquire));
        })
        .def_property_readonly("cpp2py_payload_size", [](const Ns3AiMsgProtocolHeader& header) {
            return static_cast<uint32_t>(header.m_cpp2pyPayloadSize.load(std::memory_order_acquire));
        })
        .def_property_readonly("py2cpp_payload_size", [](const Ns3AiMsgProtocolHeader& header) {
            return static_cast<uint32_t>(header.m_py2cppPayloadSize.load(std::memory_order_acquire));
        })
        .def_property_readonly("cpp2py_schema_version", [](const Ns3AiMsgProtocolHeader& header) {
            return static_cast<uint32_t>(
                header.m_cpp2pySchemaVersion.load(std::memory_order_acquire));
        })
        .def_property_readonly("py2cpp_schema_version", [](const Ns3AiMsgProtocolHeader& header) {
            return static_cast<uint32_t>(
                header.m_py2cppSchemaVersion.load(std::memory_order_acquire));
        })
        .def_property_readonly("cpp2py_schema_hash", [](const Ns3AiMsgProtocolHeader& header) {
            return header.m_cpp2pySchemaHash.load(std::memory_order_acquire);
        })
        .def_property_readonly("py2cpp_schema_hash", [](const Ns3AiMsgProtocolHeader& header) {
            return header.m_py2cppSchemaHash.load(std::memory_order_acquire);
        });
}

template <typename FieldType, typename StructType>
FieldType&
GetNs3AiFieldRef(StructType& object, const Ns3AiMsgField& field)
{
    return *reinterpret_cast<FieldType*>(reinterpret_cast<char*>(&object) + field.m_offset);
}

template <typename StructType, typename FieldType>
void
BindNs3AiScalarField(py::class_<StructType>& pyClass, const Ns3AiMsgField& field)
{
    if (field.m_size != sizeof(FieldType) || field.m_count != 1)
    {
        std::ostringstream oss;
        oss << "ns3-ai schema field '" << field.m_name << "' does not match the requested scalar type";
        throw Ns3AiSchemaError(oss.str());
    }

    const std::string name = field.m_name;
    pyClass.def_property(
        name.c_str(),
        [field](StructType& object) -> FieldType {
            return GetNs3AiFieldRef<FieldType>(object, field);
        },
        [field](StructType& object, const FieldType& value) {
            GetNs3AiFieldRef<FieldType>(object, field) = value;
        });
}

template <typename StructType>
void
BindNs3AiField(py::class_<StructType>& pyClass, const Ns3AiMsgField& field)
{
    switch (field.m_type)
    {
    case Ns3AiMsgFieldType::Bool:
        BindNs3AiScalarField<StructType, bool>(pyClass, field);
        return;
    case Ns3AiMsgFieldType::Int8:
        BindNs3AiScalarField<StructType, int8_t>(pyClass, field);
        return;
    case Ns3AiMsgFieldType::UInt8:
        BindNs3AiScalarField<StructType, uint8_t>(pyClass, field);
        return;
    case Ns3AiMsgFieldType::Int16:
        BindNs3AiScalarField<StructType, int16_t>(pyClass, field);
        return;
    case Ns3AiMsgFieldType::UInt16:
        BindNs3AiScalarField<StructType, uint16_t>(pyClass, field);
        return;
    case Ns3AiMsgFieldType::Int32:
        BindNs3AiScalarField<StructType, int32_t>(pyClass, field);
        return;
    case Ns3AiMsgFieldType::UInt32:
        BindNs3AiScalarField<StructType, uint32_t>(pyClass, field);
        return;
    case Ns3AiMsgFieldType::Int64:
        BindNs3AiScalarField<StructType, int64_t>(pyClass, field);
        return;
    case Ns3AiMsgFieldType::UInt64:
        BindNs3AiScalarField<StructType, uint64_t>(pyClass, field);
        return;
    case Ns3AiMsgFieldType::Float:
        BindNs3AiScalarField<StructType, float>(pyClass, field);
        return;
    case Ns3AiMsgFieldType::Double:
        BindNs3AiScalarField<StructType, double>(pyClass, field);
        return;
    case Ns3AiMsgFieldType::RawBytes:
        break;
    }

    std::ostringstream oss;
    oss << "ns3-ai schema field '" << field.m_name << "' cannot be bound as a scalar field";
    throw Ns3AiSchemaError(oss.str());
}

template <typename StructType>
py::class_<StructType>
BindNs3AiSchemaStruct(py::module_& module, const Ns3AiMsgSchema& schema)
{
    if (schema.m_size != sizeof(StructType))
    {
        std::ostringstream oss;
        oss << "ns3-ai schema size for '" << schema.m_name << "' does not match the C++ struct size";
        throw Ns3AiSchemaError(oss.str());
    }

    py::class_<StructType> pyClass(module, schema.m_name.c_str());
    pyClass.def(py::init<>());
    for (const auto& field : schema.m_fields)
    {
        BindNs3AiField(pyClass, field);
    }
    return pyClass;
}

} // namespace ns3

#endif // NS3_AI_PYBIND_SCHEMA_H
