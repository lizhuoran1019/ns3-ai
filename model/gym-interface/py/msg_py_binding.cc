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

#include <ns3/ai-module.h>

#include <pybind11/pybind11.h>

#include <stdexcept>

namespace py = pybind11;

namespace
{

void
ValidateGymMsgSize(const Ns3AiGymMsg& msg)
{
    if (msg.size > MSG_BUFFER_SIZE)
    {
        throw std::runtime_error("ns3-ai Gym message size exceeds the configured buffer size");
    }
}

} // namespace

PYBIND11_MODULE(ns3ai_gym_msg_py, m)
{
    using GymMsgInterface = ns3::Ns3AiMsgInterfaceImpl<Ns3AiGymMsg, Ns3AiGymMsg>;

    m.attr("msg_buffer_size") = MSG_BUFFER_SIZE;
    m.attr("default_sync_timeout_us") = GymMsgInterface::DEFAULT_SYNC_TIMEOUT_US;

    py::class_<Ns3AiGymMsg>(m, "Ns3AiGymMsg")
        .def(py::init<>())
        .def_readwrite("size", &Ns3AiGymMsg::size)
        .def("get_buffer",
             [](Ns3AiGymMsg& msg) {
                 ValidateGymMsgSize(msg);
                 return py::memoryview::from_memory(static_cast<void*>(msg.buffer), msg.size);
             })
        .def("get_buffer_full", [](Ns3AiGymMsg& msg) {
            return py::memoryview::from_memory(static_cast<void*>(msg.buffer), MSG_BUFFER_SIZE);
        });

    py::class_<GymMsgInterface>(m, "Ns3AiMsgInterfaceImpl")
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
                      uint32_t>(),
             py::arg("is_memory_creator"),
             py::arg("use_vector"),
             py::arg("handle_finish"),
             py::arg("size") = 4096,
             py::arg("segment_name") = "My Seg",
             py::arg("cpp2py_msg_name") = "My Cpp to Python Msg",
             py::arg("py2cpp_msg_name") = "My Python to Cpp Msg",
             py::arg("lockable_name") = "My Lockable",
             py::arg("sync_timeout_us") = GymMsgInterface::DEFAULT_SYNC_TIMEOUT_US,
             py::arg("header_name") = "My Header",
             py::arg("cpp2py_schema_hash") = 0,
             py::arg("py2cpp_schema_hash") = 0,
             py::arg("cpp2py_schema_version") = 0,
             py::arg("py2cpp_schema_version") = 0)
        .def("PyRecvBegin", &GymMsgInterface::PyRecvBegin)
        .def("PyRecvEnd", &GymMsgInterface::PyRecvEnd)
        .def("PySendBegin", &GymMsgInterface::PySendBegin)
        .def("PySendEnd", &GymMsgInterface::PySendEnd)
        .def("GetCpp2PyStruct", &GymMsgInterface::GetCpp2PyStruct, py::return_value_policy::reference)
        .def("GetPy2CppStruct", &GymMsgInterface::GetPy2CppStruct, py::return_value_policy::reference);
}
