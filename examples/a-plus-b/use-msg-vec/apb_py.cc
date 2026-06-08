/*
 * Copyright (c) 2023 Huazhong University of Science and Technology
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors:  Muyuan Shen <muyuan_shen@hust.edu.cn>
 */

#include "apb.h"

#include <ns3/ai-module.h>

#include "ns3-ai-pybind-errors.h"

#include <iostream>
#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace ns3
{

template <>
struct Ns3AiMsgTypeSchemaDefaults<EnvStruct>
{
    static constexpr uint64_t SchemaHash = ENV_STRUCT_SCHEMA_HASH;
    static constexpr uint32_t SchemaVersion = ENV_STRUCT_SCHEMA_VERSION;
};

template <>
struct Ns3AiMsgTypeSchemaDefaults<ActStruct>
{
    static constexpr uint64_t SchemaHash = ACT_STRUCT_SCHEMA_HASH;
    static constexpr uint32_t SchemaVersion = ACT_STRUCT_SCHEMA_VERSION;
};

} // namespace ns3

PYBIND11_MAKE_OPAQUE(ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>::Cpp2PyMsgVector);
PYBIND11_MAKE_OPAQUE(ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>::Py2CppMsgVector);

PYBIND11_MODULE(ns3ai_apb_py_vec, m)
{
    ns3::BindNs3AiErrorTypes(m);
    py::enum_<ns3::Ns3AiSchemaValidationMode>(m, "Ns3AiSchemaValidationMode")
        .value("Strict", ns3::Ns3AiSchemaValidationMode::Strict)
        .value("Compatibility", ns3::Ns3AiSchemaValidationMode::Compatibility)
        .value("Disabled", ns3::Ns3AiSchemaValidationMode::Disabled)
        .export_values();
    py::class_<EnvStruct>(m, "PyEnvStruct")
        .def(py::init<>())
        .def_readwrite("a", &EnvStruct::env_a)
        .def_readwrite("b", &EnvStruct::env_b);

    py::class_<ActStruct>(m, "PyActStruct").def(py::init<>()).def_readwrite("c", &ActStruct::act_c);

    py::class_<ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>::Cpp2PyMsgVector>(m, "PyEnvVector")
        .def(
            "resize",
            static_cast<void (ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>::Cpp2PyMsgVector::*)(
                ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>::Cpp2PyMsgVector::size_type)>(
                &ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>::Cpp2PyMsgVector::resize))
        .def("__len__", &ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>::Cpp2PyMsgVector::size)
        .def(
            "__getitem__",
            [](ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>::Cpp2PyMsgVector& vec,
               uint32_t i) -> EnvStruct& {
                if (i >= vec.size())
                {
                    std::cerr << "Invalid index " << i << " for vector, whose size is "
                              << vec.size() << std::endl;
                    exit(1);
                }
                return vec.at(i);
            },
            py::return_value_policy::reference);

    py::class_<ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>::Py2CppMsgVector>(m, "PyActVector")
        .def(
            "resize",
            static_cast<void (ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>::Py2CppMsgVector::*)(
                ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>::Py2CppMsgVector::size_type)>(
                &ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>::Py2CppMsgVector::resize))
        .def("__len__", &ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>::Py2CppMsgVector::size)
        .def(
            "__getitem__",
            [](ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>::Py2CppMsgVector& vec,
               uint32_t i) -> ActStruct& {
                if (i >= vec.size())
                {
                    std::cerr << "Invalid index " << i << " for vector, whose size is "
                              << vec.size() << std::endl;
                    exit(1);
                }
                return vec.at(i);
            },
            py::return_value_policy::reference);

    using MsgInterface = ns3::Ns3AiMsgInterfaceImpl<EnvStruct, ActStruct>;

    py::class_<MsgInterface>(m, "Ns3AiMsgInterfaceImpl")
        .def(py::init<bool,
                      bool,
                      bool,
                      uint32_t,
                      const char*,
                      const char*,
                      const char*,
                      const char*,
                      uint64_t,
                      const char*,
                      uint64_t,
                      uint64_t,
                      uint32_t,
                      uint32_t, ns3::Ns3AiSchemaValidationMode>(),
             py::arg("is_memory_creator"),
             py::arg("use_vector"),
             py::arg("handle_finish"),
             py::arg("size") = 4096,
             py::arg("segment_name") = "My Seg",
             py::arg("cpp2py_msg_name") = "My Cpp to Python Msg",
             py::arg("py2cpp_msg_name") = "My Python to Cpp Msg",
             py::arg("lockable_name") = "My Lockable",
             py::arg("sync_timeout_us") = MsgInterface::DEFAULT_SYNC_TIMEOUT_US,
             py::arg("header_name") = "My Header",
             py::arg("cpp2py_schema_hash") = py::int_(ns3::Ns3AiMsgTypeSchemaDefaults<EnvStruct>::SchemaHash),
             py::arg("py2cpp_schema_hash") = py::int_(ns3::Ns3AiMsgTypeSchemaDefaults<ActStruct>::SchemaHash),
             py::arg("cpp2py_schema_version") = ns3::Ns3AiMsgTypeSchemaDefaults<EnvStruct>::SchemaVersion,
             py::arg("py2cpp_schema_version") = ns3::Ns3AiMsgTypeSchemaDefaults<ActStruct>::SchemaVersion,
             py::arg("schema_validation_mode") = ns3::Ns3AiSchemaValidationMode::Strict)
        .def("GetSessionState",
             [](const MsgInterface& interface) {
                 return static_cast<uint8_t>(interface.GetSessionState());
             })
        .def("GetSessionId", &MsgInterface::GetSessionId)
        .def("GetGenerationId", &MsgInterface::GetGenerationId)
        .def("GetCloseReason",
             [](const MsgInterface& interface) {
                 return static_cast<uint8_t>(interface.GetCloseReason());
             })
        .def("GetErrorReason",
             [](const MsgInterface& interface) {
                 return static_cast<uint8_t>(interface.GetErrorReason());
             })
        .def("GetLastErrorPeer",
             [](const MsgInterface& interface) {
                 return static_cast<uint8_t>(interface.GetLastErrorPeer());
             })
        .def("RequestClose",
             [](const MsgInterface& interface, uint8_t peer, uint8_t reason) {
                 interface.RequestClose(static_cast<ns3::Ns3AiMsgPeer>(peer),
                                        static_cast<ns3::Ns3AiMsgCloseReason>(reason));
             })
        .def("AcknowledgeClose",
             [](const MsgInterface& interface, uint8_t peer) {
                 interface.AcknowledgeClose(static_cast<ns3::Ns3AiMsgPeer>(peer));
             })
        .def("CheckGenerationId",
             [](const MsgInterface& interface, uint64_t generationId, uint8_t peer) {
                 return interface.CheckGenerationId(generationId,
                                                    static_cast<ns3::Ns3AiMsgPeer>(peer));
             })
        .def("PyRecvBegin", &MsgInterface::PyRecvBegin)
        .def("PyRecvEnd", &MsgInterface::PyRecvEnd)
        .def("PySendBegin", &MsgInterface::PySendBegin)
        .def("PySendEnd", &MsgInterface::PySendEnd)
        .def("PyGetFinished", &MsgInterface::PyGetFinished)
        .def("GetCpp2PyVector",
             &MsgInterface::GetCpp2PyVector,
             py::return_value_policy::reference)
        .def("GetPy2CppVector",
             &MsgInterface::GetPy2CppVector,
             py::return_value_policy::reference);

    m.attr("cpp2py_schema_hash") = py::int_(ENV_STRUCT_SCHEMA_HASH);
    m.attr("py2cpp_schema_hash") = py::int_(ACT_STRUCT_SCHEMA_HASH);
    m.attr("cpp2py_schema_version") = ENV_STRUCT_SCHEMA_VERSION;
    m.attr("py2cpp_schema_version") = ACT_STRUCT_SCHEMA_VERSION;
    m.attr("schema_hash") = py::int_(ENV_STRUCT_SCHEMA_HASH);
    m.attr("schema_version") = ENV_STRUCT_SCHEMA_VERSION;
}
