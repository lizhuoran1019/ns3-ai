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
import time


SIMULATION_EARLY_ENDING = 0.5   # wait and see if the subprocess is running after creation
DEFAULT_SYNC_TIMEOUT_US = 300000000


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


def run_single_ns3(path, pname, setting=None, env=None, show_output=False):
    path = os.path.abspath(path)
    run_env = _make_run_env(path, env)
    exec_path = os.path.join(path, 'ns3')

    cmd = [exec_path, 'run', pname]
    if setting:
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


# used to kill the ns-3 script process and its child processes
def kill_proc_tree(p, timeout=None, on_terminate=None):
    print('ns3ai_utils: Killing subprocesses...')
    if isinstance(p, int):
        p = psutil.Process(p)
    elif not isinstance(p, psutil.Process):
        p = psutil.Process(p.pid)
    ch = [p]+p.children(recursive=True)
    for c in ch:
        try:
            # print("\t-- {}, pid={}, ppid={}".format(psutil.Process(c.pid).name(), c.pid, c.ppid()))
            # print("\t   \"{}\"".format(" ".join(c.cmdline())))
            c.kill()
        except Exception:
            continue
    succ, err = psutil.wait_procs(ch, timeout=timeout,
                                  callback=on_terminate)
    return succ, err


class Ns3AiExperimentError(RuntimeError):
    pass


# This class sets up the shared memory and runs the simulation process.
class Experiment:
    # init ns-3 environment
    # \param[in] memSize : share memory size
    # \param[in] targetName : program name of ns3
    # \param[in] path : current working directory
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
                 cpp2pySchemaHash=0,
                 py2cppSchemaHash=0,
                 cpp2pySchemaVersion=0,
                 py2cppSchemaVersion=0,
                 shmPrefix=None,
                 env=None):
        self.targetName = targetName  # ns-3 target name, not file name
        self.ns3Path = os.path.abspath(ns3Path)
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
        self.cpp2pySchemaHash = cpp2pySchemaHash
        self.py2cppSchemaHash = py2cppSchemaHash
        self.cpp2pySchemaVersion = cpp2pySchemaVersion
        self.py2cppSchemaVersion = py2cppSchemaVersion
        self.env = env or {}

        self.msgInterface = msgModule.Ns3AiMsgInterfaceImpl(
            True,
            self.useVector,
            self.handleFinish,
            self.shmSize,
            self.segName,
            self.cpp2pyMsgName,
            self.py2cppMsgName,
            self.lockableName,
            self.syncTimeoutUs,
            self.headerName,
            self.cpp2pySchemaHash,
            self.py2cppSchemaHash,
            self.cpp2pySchemaVersion,
            self.py2cppSchemaVersion,
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

    # run ns3 script in cmd with the setting being input
    # \param[in] setting : ns3 script input parameters(default : None)
    # \param[in] show_output : whether to show output or not(default : False)
    def run(self, setting=None, show_output=False, env=None):
        self.kill()
        run_env = self.env.copy()
        if env:
            run_env.update(env)
        self.simCmd, self.proc = run_single_ns3(
            self.ns3Path, self.targetName, setting=setting, env=run_env, show_output=show_output)
        print("ns3ai_utils: Running ns-3 with: ", self.simCmd)
        # raise if an early error occurred, such as wrong target name
        time.sleep(SIMULATION_EARLY_ENDING)
        if not self.isalive():
            stdout = None
            stderr = None
            if self.proc.stdout is not None or self.proc.stderr is not None:
                stdout, stderr = self.proc.communicate()
            raise Ns3AiExperimentError(
                'ns3ai_utils: Subprocess died very early while running `{}`. stdout={!r}, stderr={!r}'.format(
                    self.simCmd, stdout, stderr
                )
            )
        return self.msgInterface

    def kill(self):
        if self.proc and self.isalive():
            kill_proc_tree(self.proc)
            self.proc = None
            self.simCmd = None

    def isalive(self):
        return self.proc is not None and self.proc.poll() is None


class ParallelExperiment:
    def __init__(self, count, targetName, ns3Path, msgModule,
                 handleFinish=False,
                 useVector=False, vectorSize=None,
                 shmSize=4096,
                 syncTimeoutUs=None,
                 cpp2pySchemaHash=0,
                 py2cppSchemaHash=0,
                 cpp2pySchemaVersion=0,
                 py2cppSchemaVersion=0,
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
    'Experiment',
    'ParallelExperiment',
    'make_shm_names',
    'Ns3AiExperimentError',
    'DEFAULT_SYNC_TIMEOUT_US',
]
