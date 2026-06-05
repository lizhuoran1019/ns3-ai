# Multi-agent and parallel environment support

This document describes the first-stage refactor that makes ns3-ai usable by multiple agents and parallel ns-3 environments without shared-memory name collisions.

## Problem in the original design

The original message interface had two global constraints:

1. `Ns3AiMsgInterface` was a singleton.
2. `GetInterface()` returned a function-local `static` implementation object.
3. `Ns3AiMsgInterfaceImpl` also kept the Boost `managed_shared_memory` segment as a function-local `static` object inside the constructor.

That shape is acceptable for a single Python agent connected to a single ns-3 simulation, but it prevents these cases:

- multiple agents in one ns-3 process;
- multiple Gym environments in one ns-3 process;
- multiple Python workers launching the same ns-3 target concurrently;
- vectorized RL training where each worker needs an isolated simulator instance.

The failure mode is not a reinforcement-learning problem. It is an IPC namespace problem: unrelated environments can open the same segment, message object, or semaphore object.

## New shared-memory namespace convention

Every independent environment should use one prefix:

```text
<prefix>.seg
<prefix>.cpp2py
<prefix>.py2cpp
<prefix>.lock
```

The C++ helper is:

```cpp
auto names = ns3::Ns3AiMsgInterface::MakeNames("ns3ai-env-0");
```

The Python helper is:

```python
from ns3ai_utils import make_shm_names
names = make_shm_names("ns3ai-env-0")
```

## Low-level message interface

Existing single-interface code remains valid:

```cpp
auto interface = ns3::Ns3AiMsgInterface::Get();
interface->SetIsMemoryCreator(false);
interface->SetUseVector(false);
interface->SetHandleFinish(true);
auto msg = interface->GetInterface<EnvStruct, ActStruct>();
```

For independent instances, use explicit names or a prefix:

```cpp
auto interface = ns3::Ns3AiMsgInterface::Get();
interface->SetIsMemoryCreator(false);
interface->SetUseVector(false);
interface->SetHandleFinish(true);

auto msg0 = interface->GetInterface<EnvStruct, ActStruct>("ns3ai-env-0");
auto msg1 = interface->GetInterface<EnvStruct, ActStruct>("ns3ai-env-1");
```

Python side:

```python
exp0 = Experiment("target", "../../../../../", py_binding,
                  handleFinish=True, shmPrefix="ns3ai-env-0")
exp1 = Experiment("target", "../../../../../", py_binding,
                  handleFinish=True, shmPrefix="ns3ai-env-1")
```

## Gym interface

Existing Gym examples that use default names remain valid:

```cpp
SetOpenGymInterface(OpenGymInterface::Get());
```

For a named environment:

```cpp
SetOpenGymInterface("ns3ai-gym-env-0");
```

or:

```cpp
SetOpenGymInterface(0);  // uses ns3ai-gym-env-0
```

Python side:

```python
env0 = gym.make(
    "ns3ai_gym_env/Ns3-v0",
    targetName="ns3ai_apb_gym",
    ns3Path="../../../../../",
    envId=0,
)
```

or explicitly:

```python
env0 = gym.make(
    "ns3ai_gym_env/Ns3-v0",
    targetName="ns3ai_apb_gym",
    ns3Path="../../../../../",
    shmPrefix="ns3ai-gym-env-0",
)
```

## Parallel Python launcher

`ParallelExperiment` creates multiple `Experiment` objects with isolated prefixes:

```python
from ns3ai_utils import ParallelExperiment

parallel = ParallelExperiment(
    count=4,
    targetName="ns3ai_apb_msg_stru",
    ns3Path="../../../../../",
    msgModule=py_binding,
    handleFinish=True,
    shmPrefixBase="ns3ai-apb-worker",
)
interfaces = parallel.run_all(show_output=True)
```

This creates prefixes:

```text
ns3ai-apb-worker-0
ns3ai-apb-worker-1
ns3ai-apb-worker-2
ns3ai-apb-worker-3
```

## Multi-agent Gym example

The repository includes `examples/a-plus-b/use-multi-agent-gym` as the smoke test for multiple agents inside one ns-3 process.

Build:

```shell
cd YOUR_NS3_DIRECTORY
./ns3 build ns3ai_apb_multi_agent_gym
```

Run:

```shell
cd contrib/ai/examples/a-plus-b/use-multi-agent-gym
python apb.py
```

The C++ executable creates multiple `OpenGymEnv` objects in one process. Each object binds to a deterministic prefix:

```cpp
SetOpenGymInterface("ns3ai-apb-agent-" + std::to_string(m_agentId));
```

The Python driver creates matching channels before launching ns-3:

```python
self.channels = [AgentChannel(i) for i in range(num_agents)]
self.owner = self.channels[0].exp
self.owner.run(setting={"numAgents": num_agents, "numSteps": num_steps}, show_output=True)
```

Only the first `Experiment` launches the ns-3 process. The other `Experiment` objects only create shared-memory segments. This tests the intended multi-agent topology:

```text
Python process
  agent channel 0  <->  ns3ai-apb-agent-0  <->  C++ OpenGymEnv 0
  agent channel 1  <->  ns3ai-apb-agent-1  <->  C++ OpenGymEnv 1
  agent channel 2  <->  ns3ai-apb-agent-2  <->  C++ OpenGymEnv 2

single ns-3 process contains all C++ OpenGymEnv objects
```

The example observation is `[agent_id, a, b]`. Python verifies that the `agent_id` received on each channel matches the channel id before sending the action `a + b`. A mismatch means IPC namespace isolation is broken.

## Scope of this refactor

This is the infrastructure layer. It does not force all existing examples to become multi-agent examples. The included multi-agent A-Plus-B example is a smoke test for channel isolation, not a full MARL algorithm.
