import unittest
from unittest import mock

import ns3ai_utils
from ns3ai_utils import CloseReason
from ns3ai_utils import ErrorReason
from ns3ai_utils import Experiment
from ns3ai_utils import Ns3AiExperimentError
from ns3ai_utils import Ns3AiMsgInterface
from ns3ai_utils import Ns3AiSessionError
from ns3ai_utils import Ns3AiSessionTimeoutError
from ns3ai_utils import Peer
from ns3ai_utils import SessionState


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
    schema_hash = 0
    schema_version = 0

    def __init__(self, raw_interface):
        self.raw_interface = raw_interface
        self.constructor_args = None

    def Ns3AiMsgInterfaceImpl(self, *args):
        self.constructor_args = args
        return self.raw_interface


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
        self.assertEqual(
            raw_interface.calls,
            ["PyRecvBegin", "PyRecvEnd", "PySendBegin", "PySendEnd", "PyGetFinished"],
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

    def test_close_handshake_uses_python_peer_and_requires_close_reason_enum(self):
        raw_interface = FakeMessageInterface()
        interface = Ns3AiMsgInterface(raw_interface)

        interface.request_close(CloseReason.UserInterrupted)
        interface.acknowledge_close()

        self.assertEqual(
            raw_interface.calls,
            [("RequestClose", Peer.Py, CloseReason.UserInterrupted), ("AcknowledgeClose", Peer.Py)],
        )

        with self.assertRaises(TypeError):
            interface.request_close("UserInterrupted")

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

    def test_experiment_resolves_relative_ns3_path_from_executed_script(self):
        raw_interface = FakeMessageInterface()
        msg_module = FakeMsgModule(raw_interface)

        with mock.patch.object(
            ns3ai_utils.sys,
            "argv",
            ["/repo/contrib/ai/examples/a-plus-b/use-msg-stru/apb.py"],
        ):
            experiment = Experiment("target", "../../../../../", msg_module)

        self.assertEqual(experiment.ns3Path, "/repo")

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
        ))

        opener_raw = FakeMessageInterface()
        opener_module = FakeMsgModule(opener_raw)
        opener = Ns3AiMsgInterface.open(opener_module, seg_name="seg")

        self.assertIs(opener.raw_interface, opener_raw)
        self.assertEqual(opener_module.constructor_args[0], False)


if __name__ == "__main__":
    unittest.main()
