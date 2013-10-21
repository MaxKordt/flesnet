#pragma once
/**
 * \file ChildProcessManager.hpp
 *
 * 2012, 2013, Jan de Cuveland <cmail@cuveland.de>
 */

#include "global.hpp"
#include <sys/wait.h>
#include <vector>
#include <csignal>

enum ProcessStatus {
    None,
    Running,
    Terminating
};

struct ChildProcess
{
    pid_t pid;
    std::string path;
    std::vector<std::string> arg;
    void* owner;
    ProcessStatus status;
};

class ChildProcessManager
{
public:
    static ChildProcessManager& get()
    {
        static ChildProcessManager instance;
        return instance;
    }

    bool start_process(ChildProcess child_process)
    {
        pid_t pid = fork();
        if (pid == 0) {
            // child
            std::vector<const char*> c_arg;
            std::transform(child_process.arg.begin(), child_process.arg.end(),
                           std::back_inserter(c_arg),
                           [](std::string & s) { return s.c_str(); });
            c_arg.push_back(nullptr);
            execvp(child_process.path.c_str(),
                   const_cast<char* const*>(&c_arg[0]));
            out.error() << "execvp() failed: " << strerror(errno);
            exit(0);
        } else {
            // parent
            if (pid > 0) {
                child_process.pid = pid;
                child_process.status = Running;
                _child_processes.push_back(child_process);
                out.debug() << "child process started";
                return true;
            } else {
                out.error() << "fork() failed: " << strerror(errno);
                return false;
            }
        }
    }

    bool stop_process(std::vector<ChildProcess>::iterator child_process)
    {
        if (child_process >= _child_processes.begin()
            && child_process < _child_processes.end()) {
            child_process->status = Terminating;
            kill(child_process->pid, SIGTERM);
            return true;
        } else {
            return false;
        }
    }

    void stop_processes(void* owner)
    {
        for (auto it = _child_processes.begin(); it < _child_processes.end();
             ++it) {
            if (it->owner == owner)
                stop_process(it);
        }
    }

    void stop_all_processes()
    {
        for (auto it = _child_processes.begin(); it < _child_processes.end();
             ++it) {
            stop_process(it);
        }
    }

private:
    ChildProcessManager()
    {
        install_sigchld_handler();
    }

    ~ChildProcessManager()
    {
        stop_all_processes();
    }

    void install_sigchld_handler()
    {
        struct sigaction sa;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sa.sa_handler = sigchld_handler;
        sigaction(SIGCHLD, &sa, nullptr);
    }

    static void sigchld_handler(int sig)
    {
        assert(sig == SIGCHLD);

        pid_t pid;
        int status;

        std::vector<ChildProcess>& child_processes
            = ChildProcessManager::get().child_processes();

        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            // process with given pid has exited
            int idx = -1;
            for (std::size_t i = 0; i != child_processes.size(); ++i)
                if (child_processes[i].pid == pid)
                    idx = i;
            if (idx < 0) {
                out.error() << "unknown child process died";
            } else {
                switch (child_processes.at(idx).status) {
                case Terminating:
                    out.debug() << "child process successfully terminated";
                    break;
                case Running:
                case None:
                    out.error() << "child process died unexpectedly";
                    out.error() << "TODO: restart not yet implemented"; // TODO!
                    break;
                }
            }
        }
    }

    std::vector<ChildProcess>& child_processes()
    {
        return _child_processes;
    }

    std::vector<ChildProcess> _child_processes;
};
