import unittest
from unittest import mock
import os

from ns3ai_utils import CloseReason
from ns3ai_utils import ErrorReason
from ns3ai_utils import Experiment
from ns3ai_utils import Ns3AiExperimentError
from ns3ai_utils import Ns3AiMsgInterface
from ns3ai_utils import Ns3AiSessionError
from ns3ai_utils import Ns3AiSessionTimeoutError
from ns3ai_utils import Peer
from ns3ai_utils import SessionState
from ns3ai_utils import SchemaValidationMode


class FakeMessageInterface:
    def __init__(self):
        self.calls = []
        self.session_state = 1
        self.error_reason = 0
        self.last_error_peer = 2
        self.cpp2py_struct = object()
        self.py2cpp_struct = object()
        self.cpp2py_vector = []
        self.py2cpp_vector = []

    def GetSessionState(self):
        return self.session_state

    def GetSessionId(self):
        return 101

    def GetGenerationId(self):
        return 202

    def GetCloseReason(self):
        return 1

    def GetErrorReason(self):
        return self.error_reason

    def GetLastErrorPeer(self):
        return self.last_error_peer

    def RequestClose(self, peer, reason):
        self.calls.append(("RequestClose", peer, reason))

    def AcknowledgeClose(self, peer):
        self.calls.append(("AcknowledgeClose", peer))

    def PyRecvBegin(self):
        self.calls.append("PyRecvBegin")
        return "recv-begin"

    def PyRecvEnd(self):
        self.calls.append("PyRecvEnd")
        return "recv-end"

    def PySendBegin(self):
        self.calls.append("PySendBegin")
        return "send-begin"

    def PySendEnd(self):
        self.calls.append("PySendEnd")
        return "send-end"

    def PyGetFinished(self):
        self.calls.append("PyGetFinished")
        return False

    def GetCpp2PyStruct(self):
        return self.cpp2py_struct

    def GetPy2CppStruct(self):
        return self.py2cpp_struct

    def GetCpp2PyVector(self):
        return self.cpp2py_vector

    def GetPy2CppVector(self):
        return self.py2cpp_vector

    def CheckGenerationId(self, generation_id, peer):
        self.calls.append(("CheckGenerationId", generation_id, peer))
        return generation_id == 202


class FailingMessageInterface(FakeMessageInterface):
    def __init__(self, session_state):
        super().__init__()
        self.session_state = session_state
        self.error_reason = 1
        self.last_error_peer = 1

    def PyRecvBegin(self):
        raise RuntimeError("raw recv failure")


class FakeMsgModule:
    default_sync_timeout_us = 0
    cpp2py_schema_hash = 0
    py2cpp_schema_hash = 0
    cpp2py_schema_version = 0
    py2cpp_schema_version = 0
    schema_hash = 0
    schema_version = 0
    Ns3AiError = RuntimeError

    def __init__(self, raw_interface):
        self.raw_interface = raw_interface
        self.constructor_args = None

    def Ns3AiMsgInterfaceImpl(self, *args):
        self.constructor_args = args
        return self.raw_interface

    @property
    def __name__(self):
        return 'FakeMsgModule'


class FakeProcess:
    def __init__(self, returncode=None, stdout="out", stderr="err"):
        self.returncode = returncode
        self.stdout = object()
        self.stderr = object()
        self.pid = 12345
        self._stdout = stdout
        self._stderr = stderr

    def poll(self):
        return self.returncode

    def communicate(self):
        return self._stdout, self._stderr


class Ns3AiMsgInterfaceLifecycleTest(unittest.TestCase):
    def test_exposes_lifecycle_values_as_python_enums_and_preserves_existing_api(self):
        raw_interface = FakeMessageInterface()
        interface = Ns3AiMsgInterface(raw_interface)

        self.assertEqual(interface.session_state, SessionState.Ready)
        self.assertEqual(interface.session_id, 101)
        self.assertEqual(interface.generation_id, 202)
        self.assertEqual(interface.close_reason, CloseReason.Normal)
        self.assertEqual(interface.error_reason, ErrorReason.None_)
        self.assertEqual(interface.last_error_peer, Peer.Py)

        self.assertEqual(interface.PyRecvBegin(), "recv-begin")
        self.assertEqual(interface.PyRecvEnd(), "recv-end")
        self.assertEqual(interface.PySendBegin(), "send-begin")
        self.assertEqual(interface.PySendEnd(), "send-end")
        self.assertFalse(interface.PyGetFinished())
        self.assertIs(interface.GetCpp2PyStruct(), raw_interface.cpp2py_struct)
        self.assertIs(interface.GetPy2CppStruct(), raw_interface.py2cpp_struct)
        self.assertIs(interface.GetCpp2PyVector(), raw_interface.cpp2py_vector)
        self.assertIs(interface.GetPy2CppVector(), raw_interface.py2cpp_vector)
        self.assertTrue(interface.CheckGenerationId(202, Peer.Py))
        with self.assertRaises(TypeError):
            interface.CheckGenerationId(202, "Py")
        self.assertEqual(
            raw_interface.calls,
            [
                "PyRecvBegin",
                "PyRecvEnd",
                "PySendBegin",
                "PySendEnd",
                "PyGetFinished",
                ("CheckGenerationId", 202, Peer.Py),
            ],
        )

    def test_wait_ready_succeeds_for_ready_or_running_session(self):
        raw_interface = FakeMessageInterface()
        raw_interface.last_error_peer = 0
        interface = Ns3AiMsgInterface(raw_interface)

        self.assertTrue(interface.wait_ready(timeout=0))
        self.assertEqual(interface.last_error_peer, Peer.None_)

        raw_interface.session_state = 2

        self.assertTrue(interface.wait_ready(timeout=0))

    def test_wait_ready_raises_structured_error_for_failed_session(self):
        raw_interface = FakeMessageInterface()
        raw_interface.session_state = 5
        raw_interface.error_reason = 3
        raw_interface.last_error_peer = 1
        interface = Ns3AiMsgInterface(raw_interface)

        with self.assertRaises(Ns3AiSessionError) as error:
            interface.wait_ready(timeout=0)

        self.assertEqual(error.exception.error_reason, ErrorReason.ProtocolMismatch)
        self.assertEqual(error.exception.last_error_peer, Peer.Cpp)

    def test_wait_ready_raises_timeout_when_deadline_expires_before_readiness(self):
        raw_interface = FakeMessageInterface()
        raw_interface.session_state = 0
        interface = Ns3AiMsgInterface(raw_interface)

        with self.assertRaises(Ns3AiSessionTimeoutError):
            interface.wait_ready(timeout=0)

    def test_request_close_forwards_python_peer_and_close_reason(self):
        raw_interface = FakeMessageInterface()
        interface = Ns3AiMsgInterface(raw_interface)

        interface.request_close(CloseReason.UserInterrupted)

        self.assertEqual(
            raw_interface.calls,
            [("RequestClose", Peer.Py, CloseReason.UserInterrupted)],
        )

    def test_request_close_rejects_non_enum_reason(self):
        raw_interface = FakeMessageInterface()
        interface = Ns3AiMsgInterface(raw_interface)

        with self.assertRaises(TypeError):
            interface.request_close("UserInterrupted")

    def test_acknowledge_close_forwards_python_peer(self):
        raw_interface = FakeMessageInterface()
        interface = Ns3AiMsgInterface(raw_interface)

        interface.acknowledge_close()

        self.assertEqual(
            raw_interface.calls,
            [("AcknowledgeClose", Peer.Py)],
        )

    def test_delegated_mailbox_error_is_structured_only_when_session_failed(self):
        failed_interface = Ns3AiMsgInterface(FailingMessageInterface(5))

        with self.assertRaises(Ns3AiSessionError) as error:
            failed_interface.PyRecvBegin()

        self.assertEqual(error.exception.error_reason, ErrorReason.Timeout)
        self.assertEqual(error.exception.last_error_peer, Peer.Cpp)

        running_interface = Ns3AiMsgInterface(FailingMessageInterface(2))

        with self.assertRaisesRegex(RuntimeError, "raw recv failure"):
            running_interface.PyRecvBegin()

    def test_experiment_run_returns_wrapper_after_session_is_ready(self):
        raw_interface = FakeMessageInterface()
        msg_module = FakeMsgModule(raw_interface)
        experiment = Experiment("target", ".", msg_module)

        with mock.patch("ns3ai_utils.run_single_ns3", return_value=("ns3 command", FakeProcess())):
            interface = experiment.run()

        self.assertIsInstance(interface, Ns3AiMsgInterface)
        self.assertEqual(interface.session_state, SessionState.Ready)

    def test_experiment_run_reports_subprocess_exit_before_session_readiness(self):
        raw_interface = FakeMessageInterface()
        raw_interface.session_state = 0
        msg_module = FakeMsgModule(raw_interface)
        experiment = Experiment("target", ".", msg_module)

        with mock.patch(
            "ns3ai_utils.run_single_ns3",
            return_value=("ns3 command", FakeProcess(returncode=1, stdout="stdout", stderr="stderr")),
        ):
            with self.assertRaises(Ns3AiExperimentError) as error:
                experiment.run()

        self.assertIn("stdout", str(error.exception))
        self.assertIn("stderr", str(error.exception))

    def test_experiment_run_times_out_when_session_never_ready(self):
        raw_interface = FakeMessageInterface()
        raw_interface.session_state = 0  # Init — never becomes Ready
        msg_module = FakeMsgModule(raw_interface)
        # Use a short but non-zero timeout so the deadline expires quickly
        experiment = Experiment("target", ".", msg_module, syncTimeoutUs=10_000)

        with mock.patch(
            "ns3ai_utils.run_single_ns3",
            return_value=("ns3 command", FakeProcess(returncode=None)),
        ):
            with self.assertRaises(Ns3AiSessionTimeoutError):
                experiment.run()

    def test_experiment_resolves_relative_ns3_path_from_current_working_directory(self):
        raw_interface = FakeMessageInterface()
        msg_module = FakeMsgModule(raw_interface)
        experiment = Experiment("target", "../../../../../", msg_module)
        self.assertEqual(experiment.ns3Path, os.path.abspath("../../../../../"))

    def test_schema_validation_mode_enum_values(self):
        self.assertEqual(SchemaValidationMode.Strict, 0)
        self.assertEqual(SchemaValidationMode.Compatibility, 1)
        self.assertEqual(SchemaValidationMode.Disabled, 2)

    def test_schema_validation_mode_passed_to_constructor(self):
        raw_interface = FakeMessageInterface()
        msg_module = FakeMsgModule(raw_interface)

        interface = Ns3AiMsgInterface.create(
            msg_module,
            schema_validation_mode="compatibility",
        )

        self.assertEqual(msg_module.constructor_args is not None, True)
        args = msg_module.constructor_args
        mode_arg = args[14]  # 15th positional arg
        self.assertEqual(mode_arg, SchemaValidationMode.Compatibility)

    def test_schema_validation_mode_invalid_string_raises(self):
        raw_interface = FakeMessageInterface()
        msg_module = FakeMsgModule(raw_interface)

        with self.assertRaises(ValueError):
            Ns3AiMsgInterface.create(
                msg_module,
                schema_validation_mode="invalid_mode",
            )

    def test_call_raw_passes_through_ns3ai_typed_error(self):
        class FakeNs3AiError(Exception):
            pass
        class FakeNs3AiSchemaError(FakeNs3AiError):
            pass

        class FailingWithNs3AiSchemaError:
            def GetSessionState(self):
                return 5
            def GetErrorReason(self):
                return 3
            def GetLastErrorPeer(self):
                return 1
            def PyRecvBegin(self):
                raise FakeNs3AiSchemaError("ns3-ai schema validation failed: direction=cpp2py")

        raw = FailingWithNs3AiSchemaError()
        interface = Ns3AiMsgInterface(raw, ns3ai_error_type=FakeNs3AiError)

        with self.assertRaises(FakeNs3AiSchemaError) as error:
            interface.PyRecvBegin()

        self.assertIn("direction=cpp2py", str(error.exception))

    def test_call_raw_wraps_plain_runtime_error_when_session_failed(self):
        class FailingWithPlainError:
            def GetSessionState(self):
                return 5
            def GetErrorReason(self):
                return 1
            def GetLastErrorPeer(self):
                return 1
            def PyRecvBegin(self):
                raise RuntimeError("something else broke")

        raw = FailingWithPlainError()
        # 不指定 ns3ai_error_type → 默认 None → 任何 Exception 都视为非 typed error
        interface = Ns3AiMsgInterface(raw)

        with self.assertRaises(Ns3AiSessionError) as error:
            interface.PyRecvBegin()

        self.assertEqual(error.exception.error_reason, ErrorReason.Timeout)
        self.assertEqual(error.exception.last_error_peer, Peer.Cpp)

    def test_call_raw_passes_through_ns3ai_schema_error(self):
        class FailingWithNs3AiSchemaError:
            def GetSessionState(self):
                return 5
            def GetErrorReason(self):
                return 3
            def GetLastErrorPeer(self):
                return 1
            def PyRecvBegin(self):
                raise Ns3AiSessionError(ErrorReason.ProtocolMismatch, Peer.Cpp)

        raw = FailingWithNs3AiSchemaError()
        interface = Ns3AiMsgInterface(raw, ns3ai_error_type=Ns3AiSessionError)

        with self.assertRaises(Ns3AiSessionError) as error:
            interface.PyRecvBegin()

        self.assertEqual(error.exception.error_reason, ErrorReason.ProtocolMismatch)

    def test_compatibility_mode_emits_deprecation_warning(self):
        raw_interface = FakeMessageInterface()
        msg_module = FakeMsgModule(raw_interface)

        with self.assertWarns(DeprecationWarning) as cm:
            Ns3AiMsgInterface.create(
                msg_module,
                schema_validation_mode="compatibility",
            )

        warning_msg = str(cm.warning)
        self.assertIn("compatibility", warning_msg)

    def test_disabled_mode_emits_runtime_warning(self):
        raw_interface = FakeMessageInterface()
        msg_module = FakeMsgModule(raw_interface)

        with self.assertWarns(RuntimeWarning) as cm:
            Ns3AiMsgInterface.create(
                msg_module,
                schema_validation_mode="disabled",
            )

        warning_msg = str(cm.warning)
        self.assertIn("disabled", warning_msg)

    def test_deprecated_schema_hash_fallback_emits_warning(self):
        raw_interface = FakeMessageInterface()

        class OldStyleFakeModule:
            default_sync_timeout_us = 0
            schema_hash = 0xABCD
            schema_version = 42
            Ns3AiError = RuntimeError

            def __init__(self, raw_interface):
                self.raw_interface = raw_interface
                self.constructor_args = None

            def Ns3AiMsgInterfaceImpl(self, *args):
                self.constructor_args = args
                return self.raw_interface

            @property
            def __name__(self):
                return 'OldStyleFakeModule'

        module = OldStyleFakeModule(raw_interface)

        with self.assertWarns(DeprecationWarning) as cm:
            Ns3AiMsgInterface.create(
                module,
                schema_validation_mode="compatibility",
            )

        warning_msg = str(cm.warning)
        self.assertIn('schema_hash', warning_msg)
        self.assertIn('deprecated', warning_msg)
        self.assertIn('cpp2py_schema_hash', warning_msg)

        # 验证值正确 fallback 到了 schema_hash
        args = module.constructor_args
        self.assertEqual(args[10], 0xABCD)  # cpp2py_schema_hash
        self.assertEqual(args[11], 0xABCD)  # py2cpp_schema_hash
        self.assertEqual(args[12], 42)      # cpp2py_schema_version
        self.assertEqual(args[13], 42)      # py2cpp_schema_version

    def test_deprecated_schema_hash_explicit_arg_priority(self):
        raw_interface = FakeMessageInterface()

        class OldStyleFakeModule:
            default_sync_timeout_us = 0
            schema_hash = 0xABCD
            Ns3AiError = RuntimeError

            def __init__(self, raw_interface):
                self.raw_interface = raw_interface
                self.constructor_args = None

            def Ns3AiMsgInterfaceImpl(self, *args):
                self.constructor_args = args
                return self.raw_interface

        module = OldStyleFakeModule(raw_interface)

        # explicit arg 应覆盖 deprecated schema_hash
        interface = Ns3AiMsgInterface.create(
            module,
            cpp2py_schema_hash=0x9999,
            py2cpp_schema_hash=0x8888,
            schema_validation_mode="compatibility",
        )

        args = module.constructor_args
        self.assertEqual(args[10], 0x9999)  # explicit 覆盖 deprecated
        self.assertEqual(args[11], 0x8888)

    def test_experiment_passes_schema_validation_mode(self):
        raw_interface = FakeMessageInterface()
        msg_module = FakeMsgModule(raw_interface)

        experiment = Experiment("target", ".", msg_module,
                                schemaValidationMode="compatibility")

        self.assertEqual(msg_module.constructor_args is not None, True)
        args = msg_module.constructor_args
        mode_arg = args[14]
        self.assertEqual(mode_arg, SchemaValidationMode.Compatibility)

    def test_wrapper_can_create_or_open_shared_memory_session_from_msg_module(self):
        creator_raw = FakeMessageInterface()
        creator_module = FakeMsgModule(creator_raw)

        creator = Ns3AiMsgInterface.create(
            creator_module,
            use_vector=True,
            handle_finish=True,
            shm_size=8192,
            seg_name="seg",
            cpp2py_msg_name="cpp2py",
            py2cpp_msg_name="py2cpp",
            lockable_name="lock",
            sync_timeout_us=123,
            header_name="header",
            cpp2py_schema_hash=11,
            py2cpp_schema_hash=22,
            cpp2py_schema_version=1,
            py2cpp_schema_version=2,
        )

        self.assertIsInstance(creator, Ns3AiMsgInterface)
        self.assertIs(creator.raw_interface, creator_raw)
        self.assertEqual(creator_module.constructor_args[0], True)
        self.assertEqual(creator_module.constructor_args[1:], (
            True,
            True,
            8192,
            "seg",
            "cpp2py",
            "py2cpp",
            "lock",
            123,
            "header",
            11,
            22,
            1,
            2,
            SchemaValidationMode.Strict,
        ))

        opener_raw = FakeMessageInterface()
        opener_module = FakeMsgModule(opener_raw)
        opener = Ns3AiMsgInterface.open(opener_module, seg_name="seg")

        self.assertIs(opener.raw_interface, opener_raw)
        self.assertEqual(opener_module.constructor_args[0], False)


class Ns3AiGymLifecycleMockTest(unittest.TestCase):
    """Gym 收发周期与重置的 L1 mock 测试（wrapper 层，不依赖 ns-3 build artifact 或 protobuf）"""

    def test_gym_step_cycle_receive_state_and_send_action(self):
        """接收 state → 发送 action 的完整 Gym step 循环"""
        state_data = b"state_data_42"
        action_data = b"action_data_99"
        action_buf = bytearray(1024)

        raw = mock.MagicMock()
        raw.GetSessionState.return_value = 1
        raw.GetErrorReason.return_value = 0
        raw.GetLastErrorPeer.return_value = 0
        raw.GetCpp2PyStruct.return_value = state_data
        raw.GetPy2CppStruct.return_value = action_buf

        interface = Ns3AiMsgInterface(raw)

        # Receive state from C++
        interface.PyRecvBegin()
        received = interface.GetCpp2PyStruct()
        interface.PyRecvEnd()
        self.assertEqual(received, state_data)

        # Send action to C++
        interface.PySendBegin()
        buf = interface.GetPy2CppStruct()
        buf[:len(action_data)] = action_data
        interface.PySendEnd()

        # Verify action data written to buffer
        self.assertEqual(bytes(action_buf[:len(action_data)]), action_data)

        # Verify call sequence
        raw.PyRecvBegin.assert_called_once()
        raw.GetCpp2PyStruct.assert_called()
        raw.PyRecvEnd.assert_called_once()
        raw.PySendBegin.assert_called_once()
        raw.GetPy2CppStruct.assert_called()
        raw.PySendEnd.assert_called_once()

    def test_gym_step_cycle_multiple_iterations(self):
        """多步 Gym step 循环：验证序列数据在 wrapper 层正确传递"""
        states = [b"state_0", b"state_1", b"state_2"]
        action_buffers = [bytearray(1024) for _ in range(3)]

        raw = mock.MagicMock()
        raw.GetSessionState.return_value = 1
        raw.GetErrorReason.return_value = 0
        raw.GetLastErrorPeer.return_value = 0
        raw.GetCpp2PyStruct.side_effect = list(states)
        raw.GetPy2CppStruct.side_effect = list(action_buffers)

        interface = Ns3AiMsgInterface(raw)

        for i, expected_state in enumerate(states):
            # Receive state from C++
            interface.PyRecvBegin()
            received = interface.GetCpp2PyStruct()
            interface.PyRecvEnd()
            self.assertEqual(received, expected_state, "step {} state mismatch".format(i))

            # Send action to C++
            action = b"action_%d" % i
            interface.PySendBegin()
            buf = interface.GetPy2CppStruct()
            buf[:len(action)] = action
            interface.PySendEnd()

            # Verify action written to correct buffer
            current_buf = action_buffers[i]
            self.assertEqual(bytes(current_buf[:len(action)]), action,
                             "step {} action mismatch".format(i))

        # Total call counts
        self.assertEqual(raw.PyRecvBegin.call_count, 3)
        self.assertEqual(raw.PyRecvEnd.call_count, 3)
        self.assertEqual(raw.PySendBegin.call_count, 3)
        self.assertEqual(raw.PySendEnd.call_count, 3)

    @mock.patch("ns3ai_utils.kill_proc_tree")
    def test_gym_reset_experiment_restarts_after_kill(self, mock_kill):
        """验证 Experiment kill → re-run 的 reset 周期使 session 恢复 Ready"""
        raw = FakeMessageInterface()
        msg_module = FakeMsgModule(raw)
        exp = Experiment("target", ".", msg_module)

        with mock.patch("ns3ai_utils.run_single_ns3",
                        return_value=("cmd", FakeProcess())):
            interface = exp.run()

        self.assertEqual(interface.session_state, SessionState.Ready)
        self.assertTrue(exp.isalive())

        # Kill the subprocess (Gym reset step 1)
        exp.kill()
        mock_kill.assert_called_once()
        self.assertIsNone(exp.proc)
        mock_kill.reset_mock()

        # Re-run (Gym reset step 2)
        with mock.patch("ns3ai_utils.run_single_ns3",
                        return_value=("cmd", FakeProcess())):
            interface2 = exp.run()

        self.assertEqual(interface2.session_state, SessionState.Ready)
        self.assertTrue(exp.isalive())

    @mock.patch("ns3ai_utils.kill_proc_tree")
    def test_gym_reset_cycle_preserves_communication(self, mock_kill):
        """验证 reset (kill → re-run) 后 wrapper 收发操作仍正常工作"""
        raw = FakeMessageInterface()
        msg_module = FakeMsgModule(raw)
        exp = Experiment("target", ".", msg_module)

        with mock.patch("ns3ai_utils.run_single_ns3",
                        return_value=("cmd", FakeProcess())):
            interface = exp.run()

        # Step cycle before reset — should not raise
        interface.PyRecvBegin()
        interface.PyRecvEnd()
        interface.PySendBegin()
        interface.PySendEnd()

        # Reset (Gym reset lifecycle)
        exp.kill()
        mock_kill.assert_called_once()
        mock_kill.reset_mock()

        with mock.patch("ns3ai_utils.run_single_ns3",
                        return_value=("cmd", FakeProcess())):
            interface2 = exp.run()

        # Step cycle after reset — should also not raise
        interface2.PyRecvBegin()
        interface2.PyRecvEnd()
        interface2.PySendBegin()
        interface2.PySendEnd()


if __name__ == "__main__":
    unittest.main()
