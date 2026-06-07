/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 */

#ifndef NS3_AI_ERRORS_H
#define NS3_AI_ERRORS_H

#include <stdexcept>
#include <string>

namespace ns3
{

/**
 * \brief Base exception type for all ns3-ai runtime failures.
 *
 * All ns3-ai typed exceptions inherit from this class, which itself inherits
 * from std::runtime_error so that existing catch-all handlers continue to work.
 */
class Ns3AiError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/**
 * \brief General runtime / misconfiguration failure.
 *
 * Use for invalid user input, missing shared-memory objects, configuration
 * errors, serialization failures, and other recoverable runtime conditions
 * that do not have a more specific category.
 */
class Ns3AiRuntimeError : public Ns3AiError
{
  public:
    using Ns3AiError::Ns3AiError;
};

/**
 * \brief Synchronization timeout failure.
 *
 * Use when a blocking semaphore wait or peer state transition exceeds the
 * configured timeout and no other error reason (peer death, protocol mismatch)
 * applies.
 */
class Ns3AiTimeoutError : public Ns3AiError
{
  public:
    using Ns3AiError::Ns3AiError;
};

/**
 * \brief Protocol header, synchronization state, sequence, or state-machine
 *        violation.
 *
 * Use for:
 * - Protocol header validation failures (magic, ABI version, header size)
 * - Peer state transition violations (e.g. sending while not in Ready state)
 * - Session state violations
 * - Sequence number mismatches
 * - Access mode violations (struct vs vector getter mismatch)
 */
class Ns3AiProtocolError : public Ns3AiError
{
  public:
    using Ns3AiError::Ns3AiError;
};

/**
 * \brief Schema size, hash, version, or binding mismatch.
 *
 * Use when the C++ compile-time struct layout does not match the
 * shared-memory schema negotiated with the peer, including struct size,
 * schema hash, schema version, or field binding mismatches.
 */
class Ns3AiSchemaError : public Ns3AiError
{
  public:
    using Ns3AiError::Ns3AiError;
};

} // namespace ns3

#endif // NS3_AI_ERRORS_H
