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
_APB_VEC_DIR = _REPO_ROOT / "contrib" / "ai" / "examples" / "a-plus-b" / "use-msg-vec"
_LTE_CQI_DIR = _REPO_ROOT / "contrib" / "ai" / "examples" / "lte-cqi" / "use-msg"
_RL_TCP_DIR = _REPO_ROOT / "contrib" / "ai" / "examples" / "rl-tcp" / "use-msg"

for _d in [_APB_STRU_DIR, _APB_VEC_DIR, _LTE_CQI_DIR, _RL_TCP_DIR]:
    if str(_d) not in sys.path:
        sys.path.insert(0, str(_d))

try:
    import ns3ai_apb_py_stru as real_msg_module
except ImportError as exc:
    real_msg_module = None
    _IMPORT_ERROR = exc
else:
    _IMPORT_ERROR = None

# 尝试导入其余 binding 模块（用于 4 模块 import smoke）
try:
    import ns3ai_apb_py_vec as _vec_module
    import ns3ai_ltecqi_py as _lte_module
    import ns3ai_rltcp_msg_py as _rl_module
    _ALL_BINDINGS_AVAILABLE = True
except ImportError:
    _ALL_BINDINGS_AVAILABLE = False


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
            schema_validation_mode="compatibility",
        )
        opener = Ns3AiMsgInterface.open(
            real_msg_module,
            seg_name=names["segName"],
            cpp2py_msg_name=names["cpp2pyMsgName"],
            py2cpp_msg_name=names["py2cppMsgName"],
            lockable_name=names["lockableName"],
            header_name=names["headerName"],
            sync_timeout_us=1000,
            schema_validation_mode="compatibility",
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

    def test_module_exposes_non_zero_bidirectional_schema_attrs(self):
        self.assertIsNotNone(real_msg_module)
        self.assertNotEqual(real_msg_module.cpp2py_schema_hash, 0,
                            "module exposes non-zero cpp2py_schema_hash")
        self.assertNotEqual(real_msg_module.py2cpp_schema_hash, 0,
                            "module exposes non-zero py2cpp_schema_hash")
        self.assertEqual(real_msg_module.cpp2py_schema_version, 1,
                         "module exposes cpp2py_schema_version=1")
        self.assertEqual(real_msg_module.py2cpp_schema_version, 1,
                         "module exposes py2cpp_schema_version=1")

    def test_strict_mode_works_with_non_zero_default_schema(self):
        """验证 strict 模式下不传 hash/version 也能正常构造（从 TypeSchemaDefaults 读取非零值）"""
        prefix = "ns3ai-slice2-strict-{}".format(uuid.uuid4().hex)
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
        self.assertEqual(creator.session_state, SessionState.Ready)
        self.assertEqual(opener.session_state, SessionState.Ready)


@unittest.skipIf(not _ALL_BINDINGS_AVAILABLE,
                 "one or more example bindings are not built")
class AllMessageBindingsSchemaMetadataTest(unittest.TestCase):
    """验证所有 4 个 binding 组可在同一进程导入且暴露非零 schema metadata。"""

    def test_all_bindings_expose_non_zero_schema_attrs(self):
        import ns3ai_apb_py_stru as s
        import ns3ai_apb_py_vec as v
        import ns3ai_ltecqi_py as l
        import ns3ai_rltcp_msg_py as r

        for name, mod in [('stru', s), ('vec', v), ('lte', l), ('rl', r)]:
            with self.subTest(binding=name):
                self.assertNotEqual(mod.cpp2py_schema_hash, 0,
                                    f"{name}: cpp2py_schema_hash")
                self.assertNotEqual(mod.py2cpp_schema_hash, 0,
                                    f"{name}: py2cpp_schema_hash")
                self.assertEqual(mod.cpp2py_schema_version, 1,
                                 f"{name}: cpp2py_schema_version")
                self.assertEqual(mod.py2cpp_schema_version, 1,
                                 f"{name}: py2cpp_schema_version")


if __name__ == "__main__":
    if _IMPORT_ERROR is not None:
        raise SystemExit(str(_IMPORT_ERROR))
    unittest.main()
