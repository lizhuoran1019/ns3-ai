# Copyright (c) 2026
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

import os
import sys
import traceback

from ns3ai_gym_env import messages_pb2 as pb
from ns3ai_gym_env import ns3ai_gym_msg_py as py_binding
from ns3ai_utils import Experiment


NUM_AGENTS = 3
NUM_STEPS = 5
TARGET_NAME = "ns3ai_apb_multi_agent_gym"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
NS3_PATH = os.path.abspath(os.path.join(SCRIPT_DIR, "../../../../../"))
PREFIX_BASE = "ns3ai-apb-agent"


def make_prefix(agent_id):
    return f"{PREFIX_BASE}-{agent_id}"


def create_data(data_container_pb):
    if data_container_pb.type != pb.Box:
        raise RuntimeError("Expected Box observation")

    box_container_pb = pb.BoxDataContainer()
    data_container_pb.data.Unpack(box_container_pb)

    if box_container_pb.dtype == pb.UINT:
        return list(box_container_pb.uintData)
    if box_container_pb.dtype == pb.INT:
        return list(box_container_pb.intData)
    if box_container_pb.dtype == pb.DOUBLE:
        return list(box_container_pb.doubleData)
    return list(box_container_pb.floatData)


def pack_box_action(action):
    data_container = pb.DataContainer()
    data_container.type = pb.Box

    box_container = pb.BoxDataContainer()
    box_container.shape.extend([1])
    box_container.dtype = pb.UINT
    box_container.uintData.extend([int(action)])

    data_container.data.Pack(box_container)
    return data_container


class AgentChannel:
    def __init__(self, agent_id):
        self.agent_id = agent_id
        self.exp = Experiment(
            TARGET_NAME,
            NS3_PATH,
            py_binding,
            shmPrefix=make_prefix(agent_id),
        )
        self.msg_interface = self.exp.msgInterface
        self.action_space = None
        self.observation_space = None
        self.initialized = False

    def initialize_if_needed(self):
        if self.initialized:
            return

        sim_init_msg = pb.SimInitMsg()
        self.msg_interface.PyRecvBegin()
        request = self.msg_interface.GetCpp2PyStruct().get_buffer()
        sim_init_msg.ParseFromString(request)
        self.msg_interface.PyRecvEnd()

        self.action_space = sim_init_msg.actSpace
        self.observation_space = sim_init_msg.obsSpace

        reply = pb.SimInitAck()
        reply.done = True
        reply.stopSimReq = False
        reply_bytes = reply.SerializeToString()
        assert len(reply_bytes) <= py_binding.msg_buffer_size

        self.msg_interface.PySendBegin()
        self.msg_interface.GetPy2CppStruct().size = len(reply_bytes)
        self.msg_interface.GetPy2CppStruct().get_buffer_full()[:len(reply_bytes)] = reply_bytes
        self.msg_interface.PySendEnd()
        self.initialized = True

    def step_once(self):
        self.initialize_if_needed()

        env_state_msg = pb.EnvStateMsg()
        self.msg_interface.PyRecvBegin()
        request = self.msg_interface.GetCpp2PyStruct().get_buffer()
        env_state_msg.ParseFromString(request)
        self.msg_interface.PyRecvEnd()

        obs = create_data(env_state_msg.obsData)
        done = env_state_msg.isGameOver

        if done:
            self.send_stop_ack()
            return True

        observed_agent_id, a, b = obs
        if int(observed_agent_id) != self.agent_id:
            raise RuntimeError(
                f"agent channel mismatch: channel={self.agent_id}, obs_agent={observed_agent_id}"
            )

        action = int(a) + int(b)
        reply = pb.EnvActMsg()
        reply.actData.CopyFrom(pack_box_action(action))
        reply_bytes = reply.SerializeToString()
        assert len(reply_bytes) <= py_binding.msg_buffer_size

        self.msg_interface.PySendBegin()
        self.msg_interface.GetPy2CppStruct().size = len(reply_bytes)
        self.msg_interface.GetPy2CppStruct().get_buffer_full()[:len(reply_bytes)] = reply_bytes
        self.msg_interface.PySendEnd()

        print(f"agent={self.agent_id};obs={int(a)},{int(b)};act={action};")
        return False

    def send_stop_ack(self):
        reply = pb.EnvActMsg()
        reply.stopSimReq = True
        reply_bytes = reply.SerializeToString()
        assert len(reply_bytes) <= py_binding.msg_buffer_size

        self.msg_interface.PySendBegin()
        self.msg_interface.GetPy2CppStruct().size = len(reply_bytes)
        self.msg_interface.GetPy2CppStruct().get_buffer_full()[:len(reply_bytes)] = reply_bytes
        self.msg_interface.PySendEnd()

    def close(self):
        del self.exp


class MultiAgentRunner:
    def __init__(self, num_agents, num_steps):
        self.num_agents = num_agents
        self.num_steps = num_steps
        self.channels = [AgentChannel(i) for i in range(num_agents)]
        self.owner = self.channels[0].exp

    def run(self):
        setting = {"numAgents": self.num_agents, "numSteps": self.num_steps}
        self.owner.run(setting=setting, show_output=True)

        finished = [False] * self.num_agents
        while not all(finished):
            for channel in self.channels:
                if not finished[channel.agent_id]:
                    finished[channel.agent_id] = channel.step_once()

    def close(self):
        for channel in self.channels:
            channel.close()


runner = MultiAgentRunner(NUM_AGENTS, NUM_STEPS)

try:
    runner.run()
except Exception as e:
    exc_type, exc_value, exc_traceback = sys.exc_info()
    print("Exception occurred: {}".format(e))
    print("Traceback:")
    traceback.print_tb(exc_traceback)
    exit(1)
finally:
    print("Finally exiting...")
    runner.close()
