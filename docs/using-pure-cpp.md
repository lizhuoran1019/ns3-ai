# Using C++-based ML Frameworks in ns-3

## TensorFlow C API

### `libtensorflow` Installation

Targets using TensorFlow C API are enabled only when the TensorFlow C library is configured explicitly with CMake variables. Do not copy third-party binaries into the ns3-ai source tree.

#### For x86-64-based operating systems

1. Download prebuilt library from [TensorFlow official website](https://www.tensorflow.org/install/lang_c).
2. Extract tarball outside the ns3-ai source tree, for example under `/opt` or another dependency directory.

```shell
mkdir -p /opt/libtensorflow
tar -xf PATH_TO_TARBALL -C /opt/libtensorflow --strip-components=1
```

#### For arm64-based macOS

The website does not provide arm64 prebuilt library. You need to build it, or get prebuilt library with `brew`. You can install `libtensorflow` with `brew`:

```shell
brew install tensorflow
```

### Cmake settings

Configure ns-3 with one of the following forms:

```shell
./ns3 configure -- -DNS3AI_LIBTENSORFLOW_ROOT=/opt/libtensorflow
```

or pass include and library directories separately:

```shell
./ns3 configure -- \
  -DNS3AI_LIBTENSORFLOW_INCLUDE_DIR=/opt/libtensorflow/include \
  -DNS3AI_LIBTENSORFLOW_LIBRARY_DIR=/opt/libtensorflow/lib
```

The following variables are available if `libtensorflow` is configured correctly:

- `NS3AI_LIBTENSORFLOW_EXAMPLES`: Whether TensorFlow C examples are enabled. Should be `ON` or `OFF`.
- `TensorFlow_LIBRARIES`: Dynamically-linked libraries.
- `Libtensorflow_INCLUDE_DIR`: The include directory.

These variables are helpful for adding libtensorflow-based examples (targets).

### Example

- Cmake target: `ns3ai_ltecqi_purecpp`

The Python API of TensorFlow provides the full functionality, while the C API
is [in progress and incomplete](https://github.com/tensorflow/docs/blob/master/site/en/r1/guide/extend/bindings.md#current-status).
As a result, the [LTE-CQI](../examples/lte-cqi) example, which uses LSTM and requires Gradients and
Neural Network library, cannot be rewritten into pure C++ version using `libtensorflow`.

However, a [basic example](../examples/lte-cqi/pure-cpp) is provided for checking whether
`libtensorflow` is correctly installed. If it is, `./ns3 run ns3ai_ltecqi_purecpp`
should successfully print TensorFlow's version. The full example using LSTM will be
available when TensorFlow offers sufficient C API to developers.

## PyTorch C++ API

### `libtorch` Installation

Targets using PyTorch C++ API are enabled only when libtorch is configured explicitly with CMake variables. Do not copy third-party binaries into the ns3-ai source tree.

#### For x86-64-based operating systems

1. Download prebuilt library from [PyTorch official website](https://pytorch.org).
2. Unzip it outside the ns3-ai source tree, for example under `/opt` or another dependency directory.

```shell
unzip PATH_TO_ZIP -d /opt/
```

#### For arm64-based macOS

The website does not provide arm64 prebuilt library. You need to build it, or get prebuilt library with `brew`. You can install `libtorch` with `brew`:

```shell
brew install pytorch
```

### Cmake settings

Configure ns-3 with one of the following forms:

```shell
./ns3 configure -- -DNS3AI_LIBTORCH_ROOT=/opt/libtorch
```

or pass include and library directories separately:

```shell
./ns3 configure -- \
  -DNS3AI_LIBTORCH_INCLUDE_DIRS="/opt/libtorch/include;/opt/libtorch/include/torch/csrc/api/include" \
  -DNS3AI_LIBTORCH_LIBRARY_DIR=/opt/libtorch/lib
```

The following variables are available if `libtorch` is configured correctly:

- `NS3AI_LIBTORCH_EXAMPLES`: Whether libtorch examples are enabled. Should be `ON` or `OFF`.
- `Torch_LIBRARIES`: Dynamically-linked libraries.
- `Libtorch_INCLUDE_DIRS`: Include directories.

These variables are helpful for adding libtorch-based examples (targets).
Additionally, you may need to link with Python (`Python_LIBRARIES`) in case
symbols like `_PyBaseObject_Type` are missing.

### Example

- Cmake target: `ns3ai_rltcp_purecpp`

The [message-interface version of the RL-TCP example](../examples/rl-tcp/use-msg)
has been [modified](../examples/rl-tcp/pure-cpp) to run in pure C++, utilizing PyTorch C++ APIs. All RL
functionalities are same with original, except for random seed settings that can
lead to different results. Running `ns3ai_rltcp_purecpp` should by
default apply deep Q-learning algorithm (DQN) to choose TCP parameters, with states
and actions printing in the console.

```shell
pip install -r contrib/ai/examples/rl-tcp/requirements.txt
./ns3 run ns3ai_rltcp_purecpp
```
