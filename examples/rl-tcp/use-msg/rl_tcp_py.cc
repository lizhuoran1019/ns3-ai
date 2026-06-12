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
 * Author:  Muyuan Shen <muyuan_shen@hust.edu.cn>
 */

#include "tcp-rl-env.h"

#include <ns3/ai-module.h>

#include "ns3-ai-pybind-errors.h"

#include <pybind11/pybind11.h>

namespace py = pybind11;

PYBIND11_MODULE(ns3ai_rltcp_msg_py, m)
{
    ns3::BindNs3AiErrorTypes(m);
    py::enum_<ns3::Ns3AiSchemaValidationMode>(m, "Ns3AiSchemaValidationMode", py::module_local())
        .value("Strict", ns3::Ns3AiSchemaValidationMode::Strict)
        .value("Compatibility", ns3::Ns3AiSchemaValidationMode::Compatibility)
        .value("Disabled", ns3::Ns3AiSchemaValidationMode::Disabled)
        .export_values();
    py::class_<ns3::TcpRlEnv>(m, "PyEnvStruct", py::module_local())
        .def(py::init<>())
        .def_readwrite("nodeId", &ns3::TcpRlEnv::nodeId)
        .def_readwrite("socketUid", &ns3::TcpRlEnv::socketUid)
        .def_readwrite("envType", &ns3::TcpRlEnv::envType)
        .def_readwrite("simTime_us", &ns3::TcpRlEnv::simTime_us)
        .def_readwrite("ssThresh", &ns3::TcpRlEnv::ssThresh)
        .def_readwrite("cWnd", &ns3::TcpRlEnv::cWnd)
        .def_readwrite("segmentSize", &ns3::TcpRlEnv::segmentSize)
        .def_readwrite("segmentsAcked", &ns3::TcpRlEnv::segmentsAcked)
        .def_readwrite("bytesInFlight", &ns3::TcpRlEnv::bytesInFlight);

    py::class_<ns3::TcpRlAct>(m, "PyActStruct", py::module_local())
        .def(py::init<>())
        .def_readwrite("new_ssThresh", &ns3::TcpRlAct::new_ssThresh)
        .def_readwrite("new_cWnd", &ns3::TcpRlAct::new_cWnd);

    using MsgInterface = ns3::MailboxTransportImpl<ns3::TcpRlEnv, ns3::TcpRlAct>;

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
             py::arg("cpp2py_schema_hash") = py::int_(ns3::Ns3AiMsgTypeSchemaDefaults<ns3::TcpRlEnv>::SchemaHash),
             py::arg("py2cpp_schema_hash") = py::int_(ns3::Ns3AiMsgTypeSchemaDefaults<ns3::TcpRlAct>::SchemaHash),
             py::arg("cpp2py_schema_version") = ns3::Ns3AiMsgTypeSchemaDefaults<ns3::TcpRlEnv>::SchemaVersion,
             py::arg("py2cpp_schema_version") = ns3::Ns3AiMsgTypeSchemaDefaults<ns3::TcpRlAct>::SchemaVersion,
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
                 interface.RequestClose(static_cast<ns3::TransportPeer>(peer),
                                        static_cast<ns3::TransportCloseReason>(reason));
             })
        .def("AcknowledgeClose",
             [](const MsgInterface& interface, uint8_t peer) {
                 interface.AcknowledgeClose(static_cast<ns3::TransportPeer>(peer));
             })
        .def("CheckGenerationId",
             [](const MsgInterface& interface, uint64_t generationId, uint8_t peer) {
                 return interface.CheckGenerationId(generationId,
                                                    static_cast<ns3::TransportPeer>(peer));
             })
        .def("PyRecvBegin", &MsgInterface::PyRecvBegin)
        .def("PyRecvEnd", &MsgInterface::PyRecvEnd)
        .def("PySendBegin", &MsgInterface::PySendBegin)
        .def("PySendEnd", &MsgInterface::PySendEnd)
        .def("PyGetFinished", &MsgInterface::PyGetFinished)
        .def("GetCpp2PyStruct",
             &MsgInterface::GetCpp2PyStruct,
             py::return_value_policy::reference)
        .def("GetPy2CppStruct",
             &MsgInterface::GetPy2CppStruct,
             py::return_value_policy::reference);

    m.attr("cpp2py_schema_hash") = py::int_(ns3::TCP_RL_ENV_SCHEMA_HASH);
    m.attr("py2cpp_schema_hash") = py::int_(ns3::TCP_RL_ACT_SCHEMA_HASH);
    m.attr("cpp2py_schema_version") = ns3::TCP_RL_ENV_SCHEMA_VERSION;
    m.attr("py2cpp_schema_version") = ns3::TCP_RL_ACT_SCHEMA_VERSION;
    m.attr("schema_hash") = py::int_(ns3::TCP_RL_ENV_SCHEMA_HASH);
    m.attr("schema_version") = ns3::TCP_RL_ENV_SCHEMA_VERSION;
}
