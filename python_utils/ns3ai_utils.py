# Copyright (c) 2019-2023 Huazhong University of Science and Technology
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation;
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#
# Author: Pengyu Liu <eic_lpy@hust.edu.cn>
#         Hao Yin <haoyin@uw.edu>
#         Muyuan Shen <muyuan_shen@hust.edu.cn>

import os
import shlex
import subprocess
import psutil
import sys
import time
from enum import IntEnum


SIMULATION_EARLY_ENDING = 0.5
DEFAULT_SYNC_TIMEOUT_US = 300000000


class Peer(IntEnum):
    None_ = 0
    Cpp = 1
    Py = 2


class SessionState(IntEnum):
    Init = 0
    Ready = 1
    Running = 2
    Closing = 3
    Closed = 4
    Error = 5


class CloseReason(IntEnum):
    None_ = 0
    Normal = 1
    UserInterrupted = 2


class ErrorReason(IntEnum):
    None_ = 0
    Timeout = 1
    PeerDeath = 2
    ProtocolMismatch = 3
    StaleGeneration = 4
    UserInterrupted = 5
    InvalidState = 6


class Ns3AiSessionError(RuntimeError):
    def __init__(self, error_reason, last_error_peer):
        self.error_reason = ErrorReason(error_reason)
        self.last_error_peer = Peer(last_error_peer)
        super().__init__(
            'ns3ai_utils: Shared-memory session failed with error_reason={} last_error_peer={}'.format(
                self.error_reason.name,
                self.last_error_peer.name,
            )
        )


class Ns3AiSessionTimeoutError(TimeoutError):
    pass


class SchemaValidationMode(IntEnum):
    Strict = 0
    Compatibility = 1
    Disabled = 2


_SCHEMA_VALIDATION_MODE_NAMES = {
    "strict": SchemaValidationMode.Strict,
    "compatibility": SchemaValidationMode.Compatibility,
    "disabled": SchemaValidationMode.Disabled,
}


class Ns3AiMsgInterface:
    def __init__(self, raw_interface, ns3ai_error_type=None):
        self._raw_interface = raw_interface
        self._ns3ai_error_type = ns3ai_error_type

    @classmethod
    def create(cls, msg_module, **kwargs):
        return cls._from_msg_module(msg_module, True, **kwargs)

    @classmethod
    def open(cls, msg_module, **kwargs):
        return cls._from_msg_module(msg_module, False, **kwargs)

    @classmethod
    def _from_msg_module(cls,
                         msg_module,
                         is_memory_creator,
                         use_vector=False,
                         handle_finish=False,
                         shm_size=4096,
                         seg_name="My Seg",
                         cpp2py_msg_name="My Cpp to Python Msg",
                         py2cpp_msg_name="My Python to Cpp Msg",
                         lockable_name="My Lockable",
                         sync_timeout_us=None,
                         header_name="My Header",
                         cpp2py_schema_hash=None,
                         py2cpp_schema_hash=None,
                         cpp2py_schema_version=None,
                         py2cpp_schema_version=None,
                         schema_validation_mode="strict"):
        if sync_timeout_us is None:
            sync_timeout_us = getattr(msg_module, 'default_sync_timeout_us', DEFAULT_SYNC_TIMEOUT_US)
        cpp2py_schema_hash = _module_default(cpp2py_schema_hash, msg_module, 'schema_hash')
        py2cpp_schema_hash = _module_default(py2cpp_schema_hash, msg_module, 'schema_hash')
        cpp2py_schema_version = _module_default(cpp2py_schema_version, msg_module, 'schema_version')
        py2cpp_schema_version = _module_default(py2cpp_schema_version, msg_module, 'schema_version')
        mode = _parse_schema_validation_mode(schema_validation_mode)
        mode_enum = (getattr(msg_module, 'Ns3AiSchemaValidationMode')(int(mode))
                     if hasattr(msg_module, 'Ns3AiSchemaValidationMode')
                     else int(mode))
        return cls(msg_module.Ns3AiMsgInterfaceImpl(
            is_memory_creator,
            use_vector,
            handle_finish,
            shm_size,
            seg_name,
            cpp2py_msg_name,
            py2cpp_msg_name,
            lockable_name,
            sync_timeout_us,
            header_name,
            cpp2py_schema_hash,
            py2cpp_schema_hash,
            cpp2py_schema_version,
            py2cpp_schema_version,
            mode_enum,
        ),
                   ns3ai_error_type=getattr(msg_module, 'Ns3AiError', None))

    @property
    def raw_interface(self):
        return self._raw_interface

    @property
    def session_state(self):
        return SessionState(self._raw_interface.GetSessionState())

    @property
    def session_id(self):
        return self._raw_interface.GetSessionId()

    @property
    def generation_id(self):
        return self._raw_interface.GetGenerationId()

    @property
    def close_reason(self):
        return CloseReason(self._raw_interface.GetCloseReason())

    @property
    def error_reason(self):
        return ErrorReason(self._raw_interface.GetErrorReason())

    @property
    def last_error_peer(self):
        return Peer(self._raw_interface.GetLastErrorPeer())

    def wait_ready(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            state = self.session_state
            if state in (SessionState.Ready, SessionState.Running):
                return True
            if state == SessionState.Error:
                raise Ns3AiSessionError(self.error_reason, self.last_error_peer)
            if time.monotonic() >= deadline:
                raise Ns3AiSessionTimeoutError(
                    'ns3ai_utils: Timed out waiting for shared-memory session readiness'
                )
            time.sleep(0.001)

    def request_close(self, reason):
        if not isinstance(reason, CloseReason):
            raise TypeError('ns3ai_utils: close reason must be a CloseReason enum value')
        return self._raw_interface.RequestClose(Peer.Py, reason)

    def acknowledge_close(self):
        return self._raw_interface.AcknowledgeClose(Peer.Py)

    def CheckGenerationId(self, generation_id, peer):
        if not isinstance(peer, Peer):
            raise TypeError('ns3ai_utils: peer must be a Peer enum value')
        return self._raw_interface.CheckGenerationId(generation_id, peer)

    def _call_raw(self, method_name):
        try:
            return getattr(self._raw_interface, method_name)()
        except Exception as exc:
            if self._ns3ai_error_type is not None and isinstance(exc, self._ns3ai_error_type):
                raise
            if self.session_state == SessionState.Error:
                raise Ns3AiSessionError(self.error_reason, self.last_error_peer)
            raise

    def PyRecvBegin(self):
        return self._call_raw('PyRecvBegin')

    def PyRecvEnd(self):
        return self._call_raw('PyRecvEnd')

    def PySendBegin(self):
        return self._call_raw('PySendBegin')

    def PySendEnd(self):
        return self._call_raw('PySendEnd')

    def PyGetFinished(self):
        return self._call_raw('PyGetFinished')

    def GetCpp2PyStruct(self):
        return self._raw_interface.GetCpp2PyStruct()

    def GetPy2CppStruct(self):
        return self._raw_interface.GetPy2CppStruct()

    def GetCpp2PyVector(self):
        return self._raw_interface.GetCpp2PyVector()

    def GetPy2CppVector(self):
        return self._raw_interface.GetPy2CppVector()


def get_setting(setting_map):
    args = []
    for key, value in setting_map.items():
        args.append('--{}={}'.format(key, value))
    return args


def make_shm_names(prefix):
    return {
        'segName': '{}.seg'.format(prefix),
        'cpp2pyMsgName': '{}.cpp2py'.format(prefix),
        'py2cppMsgName': '{}.py2cpp'.format(prefix),
        'lockableName': '{}.lock'.format(prefix),
        'headerName': '{}.header'.format(prefix),
    }


def _format_cmd(args):
    return ' '.join(shlex.quote(str(arg)) for arg in args)


def _make_run_env(path, env=None):
    run_env = os.environ.copy()
    if env:
        run_env.update(env)

    lib_path = os.path.abspath(os.path.join(path, 'build', 'lib'))
    existing = run_env.get('LD_LIBRARY_PATH')
    if existing:
        run_env['LD_LIBRARY_PATH'] = '{}{}{}'.format(lib_path, os.pathsep, existing)
    else:
        run_env['LD_LIBRARY_PATH'] = lib_path
    return run_env


def _module_default(value, module, attr, fallback=0):
    if value is None:
        return getattr(module, attr, fallback)
    return value


def _parse_schema_validation_mode(mode):
    if isinstance(mode, SchemaValidationMode):
        return mode
    name = mode.lower()
    if name not in _SCHEMA_VALIDATION_MODE_NAMES:
        raise ValueError(
            "ns3ai_utils: schema_validation_mode must be 'strict', "
            "'compatibility', or 'disabled', got '{}'".format(mode)
        )
    return _SCHEMA_VALIDATION_MODE_NAMES[name]


def _resolve_ns3_path(ns3_path):
    return os.path.abspath(ns3_path)


def run_single_ns3(path, pname, setting=None, env=None, show_output=False):
    path = os.path.abspath(path)
    run_env = _make_run_env(path, env)
    exec_path = os.path.join(path, 'ns3')

    cmd = [exec_path, 'run', pname]
    if setting:
        cmd.append('--')
        cmd.extend(get_setting(setting))

    popen_kwargs = {
        'text': True,
        'env': run_env,
        'cwd': path,
        'stdin': subprocess.PIPE,
        'start_new_session': True,
    }
    if not show_output:
        popen_kwargs.update({
            'stdout': subprocess.PIPE,
            'stderr': subprocess.PIPE,
        })

    proc = subprocess.Popen(cmd, **popen_kwargs)
    return _format_cmd(cmd), proc


def kill_proc_tree(p, timeout=None, on_terminate=None):
    print('ns3ai_utils: Killing subprocesses...')
    if isinstance(p, int):
        p = psutil.Process(p)
    elif not isinstance(p, psutil.Process):
        p = psutil.Process(p.pid)
    ch = [p] + p.children(recursive=True)
    for c in ch:
        try:
            c.kill()
        except Exception:
            continue
    succ, err = psutil.wait_procs(ch, timeout=timeout, callback=on_terminate)
    return succ, err


class Ns3AiExperimentError(RuntimeError):
    pass


class Experiment:
    def __init__(self, targetName, ns3Path, msgModule,
                 handleFinish=False,
                 useVector=False, vectorSize=None,
                 shmSize=4096,
                 segName="My Seg",
                 cpp2pyMsgName="My Cpp to Python Msg",
                 py2cppMsgName="My Python to Cpp Msg",
                 lockableName="My Lockable",
                 syncTimeoutUs=None,
                 headerName="My Header",
                 cpp2pySchemaHash=None,
                 py2cppSchemaHash=None,
                 cpp2pySchemaVersion=None,
                 py2cppSchemaVersion=None,
                 schemaValidationMode="strict",
                 shmPrefix=None,
                 env=None):
        self.targetName = targetName
        self.ns3Path = _resolve_ns3_path(ns3Path)
        self.msgModule = msgModule
        self.handleFinish = handleFinish
        self.useVector = useVector
        self.vectorSize = vectorSize
        self.shmSize = shmSize
        if shmPrefix is not None:
            names = make_shm_names(shmPrefix)
            segName = names['segName']
            cpp2pyMsgName = names['cpp2pyMsgName']
            py2cppMsgName = names['py2cppMsgName']
            lockableName = names['lockableName']
            headerName = names['headerName']
        if syncTimeoutUs is None:
            syncTimeoutUs = getattr(msgModule, 'default_sync_timeout_us', DEFAULT_SYNC_TIMEOUT_US)
        self.shmPrefix = shmPrefix
        self.segName = segName
        self.cpp2pyMsgName = cpp2pyMsgName
        self.py2cppMsgName = py2cppMsgName
        self.lockableName = lockableName
        self.syncTimeoutUs = syncTimeoutUs
        self.headerName = headerName
        self.cpp2pySchemaHash = _module_default(cpp2pySchemaHash, msgModule, 'schema_hash')
        self.py2cppSchemaHash = _module_default(py2cppSchemaHash, msgModule, 'schema_hash')
        self.cpp2pySchemaVersion = _module_default(cpp2pySchemaVersion, msgModule, 'schema_version')
        self.py2cppSchemaVersion = _module_default(py2cppSchemaVersion, msgModule, 'schema_version')
        self.env = env or {}

        self.msgInterface = Ns3AiMsgInterface.create(
            msgModule,
            use_vector=self.useVector,
            handle_finish=self.handleFinish,
            shm_size=self.shmSize,
            seg_name=self.segName,
            cpp2py_msg_name=self.cpp2pyMsgName,
            py2cpp_msg_name=self.py2cppMsgName,
            lockable_name=self.lockableName,
            sync_timeout_us=self.syncTimeoutUs,
            header_name=self.headerName,
            cpp2py_schema_hash=self.cpp2pySchemaHash,
            py2cpp_schema_hash=self.py2cppSchemaHash,
            cpp2py_schema_version=self.cpp2pySchemaVersion,
            py2cpp_schema_version=self.py2cppSchemaVersion,
            schema_validation_mode=schemaValidationMode,
        )
        if self.useVector:
            if self.vectorSize is None:
                raise ValueError('ns3ai_utils: Using vector but size is unknown')
            self.msgInterface.GetCpp2PyVector().resize(self.vectorSize)
            self.msgInterface.GetPy2CppVector().resize(self.vectorSize)

        self.proc = None
        self.simCmd = None
        print('ns3ai_utils: Experiment initialized')

    def __del__(self):
        try:
            self.kill()
        except Exception:
            pass
        try:
            del self.msgInterface
        except AttributeError:
            pass
        print('ns3ai_utils: Experiment destroyed')

    def run(self, setting=None, show_output=False, env=None):
        self.kill()
        run_env = self.env.copy()
        if env:
            run_env.update(env)
        self.simCmd, self.proc = run_single_ns3(
            self.ns3Path, self.targetName, setting=setting, env=run_env, show_output=show_output)
        print("ns3ai_utils: Running ns-3 with: ", self.simCmd)
        self._wait_ready_or_subprocess_exit()
        return self.msgInterface

    def kill(self):
        if self.proc and self.isalive():
            kill_proc_tree(self.proc)
            self.proc = None
            self.simCmd = None

    def isalive(self):
        return self.proc is not None and self.proc.poll() is None

    def _raise_subprocess_early_exit(self):
        stdout = None
        stderr = None
        if self.proc.stdout is not None or self.proc.stderr is not None:
            stdout, stderr = self.proc.communicate()
        raise Ns3AiExperimentError(
            'ns3ai_utils: Subprocess died very early while running `{}`. stdout={!r}, stderr={!r}'.format(
                self.simCmd, stdout, stderr
            )
        )

    def _wait_ready_or_subprocess_exit(self):
        deadline = time.monotonic() + (self.syncTimeoutUs / 1000000.0)
        while True:
            if not self.isalive():
                self._raise_subprocess_early_exit()
            try:
                self.msgInterface.wait_ready(timeout=0)
                return
            except Ns3AiSessionTimeoutError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.001)


class ParallelExperiment:
    def __init__(self, count, targetName, ns3Path, msgModule,
                 handleFinish=False,
                 useVector=False, vectorSize=None,
                 shmSize=4096,
                 syncTimeoutUs=None,
                 cpp2pySchemaHash=None,
                 py2cppSchemaHash=None,
                 cpp2pySchemaVersion=None,
                 py2cppSchemaVersion=None,
                 schemaValidationMode="strict",
                 shmPrefixBase='ns3ai-env',
                 env=None):
        if count <= 0:
            raise ValueError('ns3ai_utils: count must be positive')
        self.experiments = []
        for i in range(count):
            prefix = '{}-{}'.format(shmPrefixBase, i)
            self.experiments.append(
                Experiment(targetName, ns3Path, msgModule,
                           handleFinish=handleFinish,
                           useVector=useVector,
                           vectorSize=vectorSize,
                           shmSize=shmSize,
                           syncTimeoutUs=syncTimeoutUs,
                           cpp2pySchemaHash=cpp2pySchemaHash,
                           py2cppSchemaHash=py2cppSchemaHash,
                           cpp2pySchemaVersion=cpp2pySchemaVersion,
                           py2cppSchemaVersion=py2cppSchemaVersion,
                           schemaValidationMode=schemaValidationMode,
                           shmPrefix=prefix,
                           env=env)
            )

    def run_all(self, settings=None, show_output=False, envs=None):
        interfaces = []
        for i, exp in enumerate(self.experiments):
            setting = None
            if isinstance(settings, list):
                setting = settings[i]
            elif isinstance(settings, dict):
                setting = settings.copy()
            run_env = None
            if isinstance(envs, list):
                run_env = envs[i]
            elif isinstance(envs, dict):
                run_env = envs.copy()
            interfaces.append(exp.run(setting=setting, show_output=show_output, env=run_env))
        return interfaces

    def kill(self):
        for exp in self.experiments:
            exp.kill()

    def __del__(self):
        self.kill()


__all__ = [
    'CloseReason',
    'ErrorReason',
    'Experiment',
    'Ns3AiExperimentError',
    'Ns3AiMsgInterface',
    'Ns3AiSessionError',
    'Ns3AiSessionTimeoutError',
    'ParallelExperiment',
    'Peer',
    'SchemaValidationMode',
    'SessionState',
    'make_shm_names',
    'DEFAULT_SYNC_TIMEOUT_US',
]
