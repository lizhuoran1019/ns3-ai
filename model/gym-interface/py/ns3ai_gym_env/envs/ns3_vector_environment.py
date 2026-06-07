import gymnasium as gym
import time

from ns3ai_utils import Ns3AiSessionTimeoutError
from ns3ai_gym_env.envs.ns3_environment import Ns3Env


class Ns3VecEnv:
    """A synchronous vectorized ns-3 Gymnasium environment.

    This class owns a batch of ``Ns3Env`` workers and exposes vectorized ``reset``
    and ``step`` methods so callers do not have to hand-roll one shared-memory
    channel per agent. Each worker still has an isolated transport namespace, but
    that is an implementation detail hidden behind a single vector-env API.
    """

    def __init__(self,
                 targetName,
                 ns3Path,
                 num_envs,
                 ns3Settings=None,
                 shmSize=1048576,
                 shmPrefixBase="ns3ai-gym-vec-env",
                 env=None,
                 make_env=None,
                 launch_simulation=True,
                 show_output=True):
        if num_envs <= 0:
            raise ValueError("num_envs must be positive")

        self.num_envs = int(num_envs)
        self.single_observation_space = None
        self.single_action_space = None
        self.observation_space = None
        self.action_space = None
        self.envs = []

        for env_id in range(self.num_envs):
            worker_env = make_env(env_id) if make_env is not None else env
            worker_settings = dict(ns3Settings or {})
            worker_settings.setdefault("numEnvs", self.num_envs)
            worker_settings.setdefault("envId", env_id)

            ns3_env = Ns3Env(targetName=targetName,
                             ns3Path=ns3Path,
                             ns3Settings=worker_settings,
                             shmSize=shmSize,
                             envId=env_id,
                             shmPrefix=f"{shmPrefixBase}-{env_id}",
                             env=worker_env,
                             autoStart=False,
                             showOutput=show_output)
            self.envs.append(ns3_env)

        if launch_simulation:
            run_settings = dict(ns3Settings or {})
            run_settings.setdefault("numAgents", self.num_envs)
            self.envs[0].msgInterface = self.envs[0].exp.run(setting=run_settings,
                                                              show_output=show_output)

        for env in self.envs:
            if launch_simulation:
                self._wait_ready(env, self.envs[0].exp)
            env.initialize_env()
        for env in self.envs:
            env.rx_env_state()

        self.single_observation_space = self.envs[0].observation_space
        self.single_action_space = self.envs[0].action_space
        self.observation_space = gym.spaces.Tuple(tuple(env.observation_space for env in self.envs))
        self.action_space = gym.spaces.Tuple(tuple(env.action_space for env in self.envs))

    def _wait_ready(self, env, launcher_exp):
        deadline = time.monotonic() + (env.exp.syncTimeoutUs / 1000000.0)
        while True:
            if not launcher_exp.isalive():
                launcher_exp._raise_subprocess_early_exit()
            try:
                env.msgInterface.wait_ready(timeout=0)
                return
            except Ns3AiSessionTimeoutError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.001)

    def reset(self, seed=None, options=None):
        if any(env.envDirty for env in self.envs):
            raise RuntimeError("Ns3VecEnv cannot reset individual lanes after stepping; create a new vector env")
        observations = []
        infos = []
        for env in self.envs:
            observations.append(env.get_obs())
            infos.append({})
        return tuple(observations), tuple(infos)

    def step(self, actions):
        if len(actions) != self.num_envs:
            raise ValueError(f"expected {self.num_envs} actions, got {len(actions)}")

        for env, action in zip(self.envs, actions):
            env.send_actions(action)

        observations = []
        rewards = []
        terminated = []
        truncated = []
        infos = []
        for env in self.envs:
            env.rx_env_state()
            obs, reward, done, trunc, info = env.get_state()
            env.envDirty = True
            observations.append(obs)
            rewards.append(reward)
            terminated.append(done)
            truncated.append(trunc)
            infos.append(info)

        return tuple(observations), tuple(rewards), tuple(terminated), tuple(truncated), tuple(infos)

    def close(self):
        if not self.envs:
            return
        self.envs[0].close()
        for env in self.envs[1:]:
            try:
                del env.exp
            except AttributeError:
                pass

    def get_random_action(self):
        return tuple(env.get_random_action() for env in self.envs)
