import sys
import unittest
import uuid
from pathlib import Path

from ns3ai_utils import CloseReason
from ns3ai_utils import ErrorReason
from ns3ai_utils import Ns3AiMsgInterface
from ns3ai_utils import Peer
from ns3ai_utils import SessionState
from ns3ai_utils import make_shm_names


_REPO_ROOT = Path(__file__).resolve().parents[4]
_APB_STRU_DIR = _REPO_ROOT / "contrib" / "ai" / "examples" / "a-plus-b" / "use-msg-stru"
if str(_APB_STRU_DIR) not in sys.path:
    sys.path.insert(0, str(_APB_STRU_DIR))

try:
    import ns3ai_apb_py_stru as real_msg_module
except ImportError as exc:  # pragma: no cover - exercised only when bindings are not built
    real_msg_module = None
    _IMPORT_ERROR = exc
else:
    _IMPORT_ERROR = None


@unittest.skipIf(real_msg_module is None, "ns3ai_apb_py_stru is not built")
class Ns3AiMsgInterfaceIntegrationTest(unittest.TestCase):
    def _create_interface_pair(self):
        prefix = "ns3ai-test-{}".format(uuid.uuid4().hex)
        names = make_shm_names(prefix)
        creator = Ns3AiMsgInterface.create(
            real_msg_module,
            seg_name=names["segName"],
            cpp2py_msg_name=names["cpp2pyMsgName"],
            py2cpp_msg_name=names["py2cppMsgName"],
            lockable_name=names["lockableName"],
            header_name=names["headerName"],
            sync_timeout_us=1000,
        )
        opener = Ns3AiMsgInterface.open(
            real_msg_module,
            seg_name=names["segName"],
            cpp2py_msg_name=names["cpp2pyMsgName"],
            py2cpp_msg_name=names["py2cppMsgName"],
            lockable_name=names["lockableName"],
            header_name=names["headerName"],
            sync_timeout_us=1000,
        )
        return creator, opener

    def test_python_requests_close_and_cpp_acknowledges(self):
        creator, opener = self._create_interface_pair()

        opener.request_close(CloseReason.Normal)

        self.assertEqual(creator.session_state, SessionState.Closing)
        self.assertEqual(opener.session_state, SessionState.Closing)
        self.assertEqual(creator.close_reason, CloseReason.Normal)

        creator.raw_interface.AcknowledgeClose(Peer.Cpp)

        self.assertEqual(creator.session_state, SessionState.Closed)
        self.assertEqual(opener.session_state, SessionState.Closed)
        self.assertEqual(opener.close_reason, CloseReason.Normal)

    def test_cpp_requests_close_and_python_acknowledges(self):
        creator, opener = self._create_interface_pair()

        creator.raw_interface.RequestClose(Peer.Cpp, CloseReason.UserInterrupted)

        self.assertEqual(creator.session_state, SessionState.Closing)
        self.assertEqual(opener.session_state, SessionState.Closing)
        self.assertEqual(opener.close_reason, CloseReason.UserInterrupted)

        opener.acknowledge_close()

        self.assertEqual(creator.session_state, SessionState.Closed)
        self.assertEqual(opener.session_state, SessionState.Closed)
        self.assertEqual(creator.close_reason, CloseReason.UserInterrupted)

    def test_python_requester_cannot_acknowledge_own_close_request(self):
        creator, opener = self._create_interface_pair()

        opener.request_close(CloseReason.Normal)

        with self.assertRaisesRegex(
            RuntimeError,
            "close acknowledgement must come from the peer that did not request close",
        ):
            opener.acknowledge_close()

        self.assertEqual(creator.session_state, SessionState.Closing)
        self.assertEqual(opener.session_state, SessionState.Closing)

    def test_check_generation_id_updates_session_error_state(self):
        creator, opener = self._create_interface_pair()

        self.assertTrue(opener.CheckGenerationId(opener.generation_id, Peer.Py))
        self.assertFalse(opener.CheckGenerationId(opener.generation_id + 1, Peer.Py))

        self.assertEqual(creator.session_state, SessionState.Error)
        self.assertEqual(opener.session_state, SessionState.Error)
        self.assertEqual(creator.error_reason, ErrorReason.StaleGeneration)
        self.assertEqual(opener.last_error_peer, Peer.Py)


if __name__ == "__main__":
    if _IMPORT_ERROR is not None:
        raise SystemExit(str(_IMPORT_ERROR))
    unittest.main()
