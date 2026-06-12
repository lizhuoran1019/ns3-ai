import sys
import types
from pathlib import Path
from unittest import mock

# ── sys.path 注入：使 model/gym-interface/py 可导入 ──
_REPO_ROOT = Path(__file__).resolve().parents[2]
_GYM_PY_DIR = _REPO_ROOT / "model" / "gym-interface" / "py"
if str(_GYM_PY_DIR) not in sys.path:
    sys.path.insert(0, str(_GYM_PY_DIR))


# ── sys.modules 注入：mock 掉 pybind11 编译产物 ──
class _FakeMsgInterfaceImpl:
    """用于在 import Ns3Env 之前占位的 fake binding impl。
    在测试中 env.msgInterface 会被替换为 FakeGymInterface，
    此占位类只需保证 Experiment.__init__ 正常运行即可。"""

    def __init__(self, *args, **kwargs):
        pass

    def GetSessionState(self):
        return 1  # Ready

    def SetSessionState(self, *_args):
        pass

    def GetCpp2PyStruct(self):
        return None

    def GetPy2CppStruct(self):
        return None

    def PyRecvBegin(self):
        pass

    def PyRecvEnd(self):
        pass

    def PySendBegin(self):
        pass

    def PySendEnd(self):
        pass


_fake_py_binding = types.ModuleType('ns3ai_gym_env.ns3ai_gym_msg_py')
_fake_py_binding.msg_buffer_size = 1024 * 1024
_fake_py_binding.Ns3AiMsgInterfaceImpl = _FakeMsgInterfaceImpl
sys.modules["ns3ai_gym_env.ns3ai_gym_msg_py"] = _fake_py_binding

# ── 实际导入 ──
from ns3ai_gym_env.envs.ns3_environment import Ns3Env
from ns3ai_gym_env import messages_pb2 as pb

from gymnasium import spaces
import unittest


# ── Fake 基础设施 ──
class _FakeStruct:
    """模拟 pybind 返回的 struct 对象，
    get_buffer() 返回读取数据，
    get_buffer_full() 返回写入 buffer 用于捕获写回。"""

    def __init__(self, data=b""):
        self._buffer = bytearray(data)
        self.size = 0

    def get_buffer(self):
        return bytes(self._buffer)

    def get_buffer_full(self):
        return self._buffer


class FakeGymInterface:
    """有状态的 Gym mock 接口。
    内部维护 _read_q 读取队列，每次 PyRecvBegin/GetCpp2PyStruct
    从队首 pop 下一个 payload。写回数据记录在 self.writes 列表中。"""

    def __init__(self, read_payloads):
        self._read_q = list(read_payloads)
        self.writes = []
        self._current_write = _FakeStruct()

    def PyRecvBegin(self):
        pass

    def GetCpp2PyStruct(self):
        return _FakeStruct(self._read_q.pop(0))

    def PyRecvEnd(self):
        pass

    def PySendBegin(self):
        self._current_write = _FakeStruct()

    def GetPy2CppStruct(self):
        return self._current_write

    def PySendEnd(self):
        self.writes.append(bytes(self._current_write.get_buffer_full()))

    @property
    def last_written(self):
        return self.writes[-1]


# ── 辅助函数：构造 protobuf 载荷 ──
def _make_discrete_init_msg(n_obs=1, n_act=1, sequence=0):
    """构造 SimInitMsg，含 Discrete obs/act space。"""
    msg = pb.SimInitMsg()
    msg.sequence = sequence

    obs_desc = pb.SpaceDescription()
    obs_desc.type = pb.Discrete
    obs_space = pb.DiscreteSpace()
    obs_space.n = n_obs
    obs_desc.space.Pack(obs_space)
    msg.obsSpace.CopyFrom(obs_desc)

    act_desc = pb.SpaceDescription()
    act_desc.type = pb.Discrete
    act_space = pb.DiscreteSpace()
    act_space.n = n_act
    act_desc.space.Pack(act_space)
    msg.actSpace.CopyFrom(act_desc)

    return msg.SerializeToString()


def _make_env_state_msg(obs_value=0,
                        reward=0.0,
                        is_game_over=False,
                        sequence=0,
                        terminated=None,
                        truncated=None,
                        reason=None,
                        error_code=None,
                        error_message=None):
    """构造 EnvStateMsg，含 Discrete 观测数据。"""
    msg = pb.EnvStateMsg()
    msg.sequence = sequence
    msg.reward = reward
    msg.isGameOver = is_game_over
    if terminated is not None:
        msg.terminated = terminated
    if truncated is not None:
        msg.truncated = truncated
    if reason is not None:
        msg.reason = reason
    elif is_game_over:
        msg.reason = pb.EnvStateMsg.GameOver
    if error_code is not None:
        msg.errorCode = error_code
    if error_message is not None:
        msg.errorMessage = error_message

    data_container = pb.DataContainer()
    data_container.type = pb.Discrete
    discrete_data = pb.DiscreteDataContainer()
    discrete_data.data = obs_value
    data_container.data.Pack(discrete_data)
    msg.obsData.CopyFrom(data_container)

    return msg.SerializeToString()


class Ns3EnvMockTest(unittest.TestCase):

    def test_initialize_env_sets_up_spaces_and_receives_state(self):
        sim_init_payload = _make_discrete_init_msg(n_obs=1, n_act=1, sequence=0)
        init_state_payload = _make_env_state_msg(obs_value=0, reward=0.0,
                                                  is_game_over=False, sequence=0)
        fake = FakeGymInterface([sim_init_payload, init_state_payload])

        env = Ns3Env("target", ".", autoStart=False)
        env.msgInterface = fake
        env.initialize_env()
        env.rx_env_state()

        # 验证 space 初始化
        self.assertIsInstance(env.action_space, spaces.Discrete)
        self.assertEqual(env.action_space.n, 1)
        self.assertIsInstance(env.observation_space, spaces.Discrete)
        self.assertEqual(env.observation_space.n, 1)

        # 验证写回：第一次写回是 SimInitAck
        ack = pb.SimInitAck()
        ack.ParseFromString(fake.writes[0])
        self.assertTrue(ack.done)
        self.assertFalse(ack.stopSimReq)
        self.assertEqual(ack.sequence, 0)

        # 验证初始状态
        obs, reward, terminated, truncated, info = env.get_state()
        self.assertEqual(obs, 0)
        self.assertEqual(reward, 0.0)
        self.assertFalse(terminated)
        self.assertFalse(env.is_game_over())

    def test_step_cycle_happy_path(self):
        sim_init = _make_discrete_init_msg(n_obs=2, n_act=1, sequence=0)
        state_seq0 = _make_env_state_msg(obs_value=0, reward=0.0,
                                          is_game_over=False, sequence=0)
        state_seq1 = _make_env_state_msg(obs_value=1, reward=0.5,
                                          is_game_over=False, sequence=1)
        fake = FakeGymInterface([sim_init, state_seq0, state_seq1])

        env = Ns3Env("target", ".", autoStart=False)
        env.msgInterface = fake
        env.initialize_env()
        env.rx_env_state()

        # 执行一步
        obs, reward, terminated, truncated, info = env.step(0)

        # 验证返回
        self.assertEqual(obs, 1)
        self.assertEqual(reward, 0.5)
        self.assertFalse(terminated)
        self.assertFalse(env.is_game_over())
        self.assertTrue(env.envDirty)

        # 验证写回：第1次 SimInitAck, 第2次 EnvActMsg
        act = pb.EnvActMsg()
        act.ParseFromString(fake.writes[1])
        self.assertEqual(act.sequence, 0)
        self.assertFalse(act.stopSimReq)
        # 验证 action payload 序列化正确（step(0) 传入 Discrete action=0）
        self.assertEqual(act.actData.type, pb.Discrete)
        discrete_act = pb.DiscreteDataContainer()
        act.actData.data.Unpack(discrete_act)
        self.assertEqual(discrete_act.data, 0)

    def test_step_with_game_over_returns_terminated(self):
        sim_init = _make_discrete_init_msg(n_obs=2, n_act=1, sequence=0)
        state_seq0 = _make_env_state_msg(obs_value=0, reward=0.0,
                                          is_game_over=False, sequence=0)
        state_seq1 = _make_env_state_msg(obs_value=1, reward=0.5,
                                          is_game_over=True, sequence=1)
        fake = FakeGymInterface([sim_init, state_seq0, state_seq1])

        env = Ns3Env("target", ".", autoStart=False)
        env.msgInterface = fake
        env.initialize_env()
        env.rx_env_state()

        obs, reward, terminated, truncated, info = env.step(0)

        # 验证终止状态
        self.assertEqual(obs, 1)
        self.assertEqual(reward, 0.5)
        self.assertTrue(terminated)
        self.assertTrue(env.is_game_over())
        self.assertTrue(env.envDirty)

        # 写回序列：SimInitAck → EnvActMsg(action) → EnvActMsg(stopSimReq)
        act = pb.EnvActMsg()
        act.ParseFromString(fake.writes[1])  # step 发送的 action
        self.assertEqual(act.sequence, 0)
        self.assertFalse(act.stopSimReq)

        close = pb.EnvActMsg()
        close.ParseFromString(fake.writes[2])  # gameOver 触发的关闭命令
        self.assertTrue(close.stopSimReq)

        with self.assertRaisesRegex(RuntimeError, "reset\\(\\) must be called before step\\(\\)"):
            env.step(0)

    def test_step_with_truncation_returns_truncated(self):
        sim_init = _make_discrete_init_msg(n_obs=2, n_act=1, sequence=0)
        state_seq0 = _make_env_state_msg(obs_value=0, reward=0.0, sequence=0)
        state_seq1 = _make_env_state_msg(obs_value=1,
                                         reward=0.5,
                                         is_game_over=True,
                                         sequence=1,
                                         terminated=False,
                                         truncated=True,
                                         reason=pb.EnvStateMsg.EpisodeTruncated)
        fake = FakeGymInterface([sim_init, state_seq0, state_seq1])

        env = Ns3Env("target", ".", autoStart=False)
        env.msgInterface = fake
        env.initialize_env()
        env.rx_env_state()

        obs, reward, terminated, truncated, info = env.step(0)

        self.assertEqual(obs, 1)
        self.assertEqual(reward, 0.5)
        self.assertFalse(terminated)
        self.assertTrue(truncated)
        self.assertEqual(info["ns3ai_reason"], "episode_truncated")

        with self.assertRaisesRegex(RuntimeError, "reset\\(\\) must be called before step\\(\\)"):
            env.step(0)

    def test_step_with_environment_error_raises_runtime_error(self):
        sim_init = _make_discrete_init_msg(n_obs=2, n_act=1, sequence=0)
        state_seq0 = _make_env_state_msg(obs_value=0, reward=0.0, sequence=0)
        state_seq1 = _make_env_state_msg(obs_value=1,
                                         reward=0.0,
                                         is_game_over=True,
                                         sequence=1,
                                         reason=pb.EnvStateMsg.EnvironmentError,
                                         error_code=17,
                                         error_message="illegal action")
        fake = FakeGymInterface([sim_init, state_seq0, state_seq1])

        env = Ns3Env("target", ".", autoStart=False)
        env.msgInterface = fake
        env.initialize_env()
        env.rx_env_state()

        with self.assertRaisesRegex(RuntimeError, "illegal action"):
            env.step(0)

        with self.assertRaisesRegex(RuntimeError, "reset\\(\\) must be called before step\\(\\)"):
            env.step(0)

    def test_step_with_simulation_end_preserves_reason(self):
        sim_init = _make_discrete_init_msg(n_obs=2, n_act=1, sequence=0)
        state_seq0 = _make_env_state_msg(obs_value=0, reward=0.0, sequence=0)
        state_seq1 = _make_env_state_msg(obs_value=1,
                                         reward=0.5,
                                         is_game_over=True,
                                         sequence=1,
                                         terminated=False,
                                         truncated=True,
                                         reason=pb.EnvStateMsg.SimulationEnd)
        fake = FakeGymInterface([sim_init, state_seq0, state_seq1])

        env = Ns3Env("target", ".", autoStart=False)
        env.msgInterface = fake
        env.initialize_env()
        env.rx_env_state()

        obs, reward, terminated, truncated, info = env.step(0)

        self.assertEqual(obs, 1)
        self.assertEqual(reward, 0.5)
        self.assertFalse(terminated)
        self.assertTrue(truncated)
        self.assertEqual(info["ns3ai_reason"], "simulation_end")

    def test_reset_after_game_over_reinitializes_environment(self):
        """验证 reset() 在 game over 后重新初始化并返回新 observation"""
        # ── 第一轮 session payload ──
        sim_init = _make_discrete_init_msg(n_obs=2, n_act=1, sequence=0)
        state_seq0 = _make_env_state_msg(obs_value=0, reward=0.0,
                                          is_game_over=False, sequence=0)
        state_seq1 = _make_env_state_msg(obs_value=1, reward=0.5,
                                          is_game_over=True, sequence=1)
        fake1 = FakeGymInterface([sim_init, state_seq0, state_seq1])

        # ── reset 后第二轮 session payload ──
        reset_init = _make_discrete_init_msg(n_obs=11, n_act=1, sequence=0)
        reset_state = _make_env_state_msg(obs_value=10, reward=0.0,
                                           is_game_over=False, sequence=0)
        fake2 = FakeGymInterface([reset_init, reset_state])

        env = Ns3Env("target", ".", autoStart=False)
        env.msgInterface = fake1
        env.initialize_env()
        env.rx_env_state()

        # step 到 game over
        obs, reward, terminated, truncated, info = env.step(0)
        self.assertTrue(terminated)
        self.assertTrue(env.is_game_over())

        # reset — 接管 exp.run() 返回 fake2 从而跳过真实子进程
        with mock.patch.object(env.exp, 'run', return_value=fake2):
            new_obs, info = env.reset()

        self.assertEqual(new_obs, 10)
        self.assertFalse(env.is_game_over())
        self.assertFalse(env.envDirty)

        # 验证写回：reset 期间应重新发送 SimInitAck
        ack = pb.SimInitAck()
        ack.ParseFromString(fake2.writes[0])
        self.assertTrue(ack.done)


if __name__ == "__main__":
    unittest.main()
