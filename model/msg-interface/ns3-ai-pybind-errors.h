/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 */

#ifndef NS3_AI_PYBIND_ERRORS_H
#define NS3_AI_PYBIND_ERRORS_H

#include "ns3-ai-errors.h"

#include <pybind11/pybind11.h>

namespace ns3
{

namespace py = pybind11;

/**
 * \brief Register ns3-ai typed exception classes in a pybind module.
 *
 * Each pybind module that exposes Ns3AiMsgInterfaceImpl or schema binding
 * helpers must call this function during module initialization so that
 * Python callers can catch individual error categories.
 *
 * Python-visible hierarchy:
 *   Ns3AiError (Exception)
 *   ├── Ns3AiRuntimeError
 *   ├── Ns3AiTimeoutError
 *   ├── Ns3AiProtocolError
 *   └── Ns3AiSchemaError
 */
inline void
BindNs3AiErrorTypes(py::module_& module)
{
    auto ns3AiError = py::register_exception<Ns3AiError>(module, "Ns3AiError", PyExc_Exception);
    py::register_exception<Ns3AiRuntimeError>(module, "Ns3AiRuntimeError", ns3AiError.ptr());
    py::register_exception<Ns3AiTimeoutError>(module, "Ns3AiTimeoutError", ns3AiError.ptr());
    py::register_exception<Ns3AiProtocolError>(module, "Ns3AiProtocolError", ns3AiError.ptr());
    py::register_exception<Ns3AiSchemaError>(module, "Ns3AiSchemaError", ns3AiError.ptr());
}

} // namespace ns3

#endif // NS3_AI_PYBIND_ERRORS_H
