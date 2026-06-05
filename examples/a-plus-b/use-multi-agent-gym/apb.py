# Copyright (c) 2026
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

import os
import sys
import traceback

from ns3ai_gym_env.envs import Ns3VecEnv


NUM_AGENTS = 3
NUM_STEPS = 5
TARGET_NAME = "ns3ai_apb_multi_agent_gym"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
NS3_PATH = os.path.abspath(os.path.join(SCRIPT_DIR, "../../../../../"))
PREFIX_BASE = "ns3ai-apb-agent"


def run():
    env = Ns3VecEnv(
        TARGET_NAME,
        NS3_PATH,
        num_envs=NUM_AGENTS,
        ns3Settings={"numAgents": NUM_AGENTS, "numSteps": NUM_STEPS},
        shmPrefixBase=PREFIX_BASE,
    )

    try:
        finished = [False] * NUM_AGENTS
        while not all(finished):
            observations = tuple(env.envs[i].get_obs() for i in range(NUM_AGENTS))
            actions = []
            for agent_id, obs in enumerate(observations):
                observed_agent_id, a, b = obs
                if int(observed_agent_id) != agent_id:
                    raise RuntimeError(
                        f"agent channel mismatch: channel={agent_id}, obs_agent={observed_agent_id}"
                    )
                action = int(a) + int(b)
                actions.append([action])
                print(f"agent={agent_id};obs={int(a)},{int(b)};act={action};")

            _, _, terminated, _, _ = env.step(tuple(actions))
            finished = [finished[i] or bool(terminated[i]) for i in range(NUM_AGENTS)]
    finally:
        env.close()


try:
    run()
except Exception as e:
    exc_type, exc_value, exc_traceback = sys.exc_info()
    print("Exception occurred: {}".format(e))
    print("Traceback:")
    traceback.print_tb(exc_traceback)
    exit(1)
finally:
    print("Finally exiting...")
