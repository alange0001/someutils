// Copyright (c) 2020-present, Adriano Lange.  All rights reserved.
// This source code is licensed under both the GPLv2 (found in the
// LICENSE.GPLv2 file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include "alutils/process.h"
#include "alutils/string.h"
#include "alutils/internal.h"
#include "alutils/print.h"

#include <string>
#include <stdexcept>
#include <sstream>
#include <chrono>
#include <thread>

#include <cstdarg>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace alutils {

#define _var2str(var) std::to_string(var).c_str()

////////////////////////////////////////////////////////////////////////////////////
#undef __CLASS__
#define __CLASS__ ""

bool monitor_fgets (char* buffer, int buffer_size, std::FILE* file, bool* stop, uint64_t interval) {
	struct timeval timeout {0,0};

	auto fd = fileno(file);
	PRINT_DEBUG("fd=%s", _var2str(fd));
	fd_set readfds;
	FD_ZERO(&readfds);

	while (!*stop) {
		FD_SET(fd, &readfds);
		auto r = select(fd +1, &readfds, NULL, NULL, &timeout);
		if (r > 0) {
			if (std::fgets(buffer, buffer_size, file) == NULL)
				return false;
			return true;
		}

		if (r < 0)
			throw std::runtime_error("select call error");
		if (std::feof(file))
			return false;
		if (std::ferror(file))
			throw std::runtime_error("file error");

		std::this_thread::sleep_for(std::chrono::milliseconds(interval));
	}

	return false;
}

std::string command_output(const char* cmd) {
	std::string ret;
	const uint buffer_size = 512;
	char buffer[buffer_size]; buffer[0] = '\0'; buffer[buffer_size -1] = '\0';

	PRINT_DEBUG("command: %s", cmd);

	std::FILE* f = popen(cmd, "r");
	if (f == NULL)
		throw std::runtime_error(std::string("error executing command \"")+cmd+"\"");

	while (std::fgets(buffer, buffer_size -1, f) != NULL) {
		ret += buffer;
		if (log_level <= LOG_DEBUG_OUT) {
			for (char* i=buffer; *i != '\0'; i++)
				if (*i == '\n') *i = ' ';
			PRINT_DEBUG_OUT("%s", buffer);
		}
	}

	auto exit_code = pclose(f);
	if (exit_code != 0)
		throw std::runtime_error(sprintf("command \"%s\" returned error %s", cmd, _var2str(exit_code)));

	return ret;
}

std::vector<pid_t> get_children(pid_t parent_pid) {
	std::vector<pid_t> ret;
	PRINT_DEBUG("parent pid: %s", _var2str(parent_pid));
	try {
		std::string cmd = sprintf(
			"getcpid() {                        \n"
			"    cpids=$(pgrep -P $1|xargs)     \n"
			"    for cpid in $cpids;            \n"
			"    do                             \n"
			"        echo \"$cpid\"             \n"
			"        getcpid $cpid              \n"
			"    done                           \n"
			"}                                  \n"
			"getcpid %s |xargs", _var2str(parent_pid));
		auto children = command_output(cmd.c_str());
		PRINT_DEBUG("children: %s", children.c_str());
		auto pids = split_str(children, " ");
		for (auto i: pids) {
			if (i.length() == 0) continue;
			try {
				pid_t pid = parseUint64(i, true, 0, sprintf("error parsing child pid (value=%s)",i).c_str());
				ret.push_back(pid);
			} catch (const std::exception& e) {
				PRINT_ERROR(e.what());
			}
		}
	} catch (const std::exception& e) {
		PRINT_ERROR(e.what());
	}

	return ret;
}

////////////////////////////////////////////////////////////////////////////////////
#undef __CLASS__
#define __CLASS__ "ProcessController::"

ProcessController::ProcessController(const char* name_, const char* cmd,
		std::function<void(const char*)> handler_stdout_, std::function<void(const char*)> handler_stderr_)
: name(name_), handler_stdout(handler_stdout_), handler_stderr(handler_stderr_)
{
	PRINT_DEBUG("constructor. Process %s", name.c_str());

	pid_t child_pid;
	int pipe_stdin[2];
	pipe(pipe_stdin);
	PRINT_DEBUG("pipe_stdin=(%s, %s)", _var2str(pipe_stdin[0]), _var2str(pipe_stdin[1]));
	int pipe_stdout[2];
	pipe(pipe_stdout);
	PRINT_DEBUG("pipe_stdout=(%s, %s)", _var2str(pipe_stdout[0]), _var2str(pipe_stdout[1]));
	int pipe_stderr[2];
	pipe(pipe_stderr);
	PRINT_DEBUG("pipe_stderr=(%s, %s)", _var2str(pipe_stderr[0]), _var2str(pipe_stderr[1]));

	if ((child_pid = fork()) == -1)
		throw std::runtime_error(std::string("fork error on process ")+name);

	if (child_pid == 0) { //// child process ////
		//PRINT_DEBUG("child process initiated");
		close(pipe_stdin[1]);
		dup2(pipe_stdin[0], STDIN_FILENO);
		close(pipe_stdout[0]);
		dup2(pipe_stdout[1], STDOUT_FILENO);
		close(pipe_stderr[0]);
		dup2(pipe_stderr[1], STDERR_FILENO);

		execl("/bin/bash", "/bin/bash", "-c", cmd, (char*)NULL);
		exit(EXIT_FAILURE);
	}

	program_active = true;

	PRINT_DEBUG("child pid=%s", _var2str(child_pid));
	pid   = child_pid;
	close(pipe_stdin[0]);
	if ((f_stdin  = fdopen(pipe_stdin[1], "w")) == NULL)
		throw std::runtime_error(std::string("fdopen (pipe_stdin) error on process ")+name);
	close(pipe_stdout[1]);
	if ((f_stdout = fdopen(pipe_stdout[0], "r")) == NULL)
		throw std::runtime_error(std::string("fdopen (pipe_stdout) error on process ")+name);
	close(pipe_stderr[1]);
	if ((f_stderr = fdopen(pipe_stderr[0], "r")) == NULL)
		throw std::runtime_error(std::string("fdopen (pipe_stderr) error on process ")+name);

	thread_stdout_active = true;
	thread_stdout = std::thread( [this]{this->threadStdout();} );
	thread_stderr_active = true;
	thread_stderr = std::thread( [this]{this->threadStderr();} );
	PRINT_DEBUG("constructor finished", name);
}

ProcessController::~ProcessController() {
	PRINT_DEBUG("destructor");

	must_stop = true;

	if (checkStatus()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (checkStatus()) {
			PRINT_WARN("process %s (pid %s) still active. kill it", name.c_str(), _var2str(pid));
			auto children = get_children(pid);
			for (auto i: children) {
				PRINT_WARN("child (pid %s) of process %s (pid %s) still active. kill it", _var2str(i), name.c_str(), _var2str(pid));
				kill(i, SIGTERM);
			}
			kill(pid, SIGTERM);
		}
	}

	auto status_f_stdin = std::fclose(f_stdin);
	auto status_f_stdout = std::fclose(f_stdout);
	auto status_f_stderr = std::fclose(f_stderr);
	PRINT_DEBUG("status_f_stdin=%s, status_f_stdout=%s, status_f_stderr=%s", _var2str(status_f_stdin), _var2str(status_f_stdout), _var2str(status_f_stderr));

	if (thread_stdout.joinable())
		thread_stdout.join();
	if (thread_stderr.joinable())
		thread_stderr.join();

	PRINT_DEBUG("destructor finished");
}

bool ProcessController::isActive(bool throwexcept) {
	if (thread_exception) {
		if (throwexcept)
			std::rethrow_exception(thread_exception);
		else {
			try { std::rethrow_exception(thread_exception); }
			catch (std::exception& e) {
				PRINT_ERROR("thread exception of program %s: %s", name.c_str(), e.what());
			}
		}
	}
	bool aux_status = checkStatus();
	if (throwexcept && !aux_status) {
		if (exit_code != 0)
			throw std::runtime_error(sprintf("program %s exit code %s", name.c_str(), _var2str(exit_code)));
		if (signal != 0)
			throw std::runtime_error(sprintf("program %s exit with signal %s", name.c_str(), _var2str(signal)));
	}
	return thread_stdout_active && thread_stderr_active && aux_status;
}

bool ProcessController::puts(const std::string value) noexcept {
	PRINT_DEBUG("write: %s", value.c_str());
	if (!checkStatus()) {
		PRINT_ERROR("puts failed. Process %s is not active", name.c_str());
		return false;
	}
	if (std::fputs(value.c_str(), f_stdin) == EOF || std::fflush(f_stdin) == EOF) {
		PRINT_ERROR("fputs/fflush error for process %s", name.c_str());
		return false;
	}
	return true;
}

void ProcessController::threadStdout() noexcept {
	PRINT_DEBUG("initiated for process %s (pid %s)", name.c_str(), _var2str(pid));
	thread_stdout_active = true;

	const uint buffer_size = 1024;
	char buffer[buffer_size]; buffer[0] = '\0'; buffer[buffer_size -1] = '\0';

	try {
		while (!must_stop && std::fgets(buffer, buffer_size -1, f_stdout) != NULL) {
			if (log_level <= LOG_DEBUG_OUT) {
				std::string aux = str_replace(buffer, '\n', ' ');
				PRINT_DEBUG_OUT("stdout line: %s", aux.c_str());
			}
			handler_stdout(buffer);
		}
	} catch(std::exception& e) {
		PRINT_DEBUG("exception received: %s", e.what());
		thread_exception = std::current_exception();
	}
	thread_stdout_active = false;
	PRINT_DEBUG("finish");
}

void ProcessController::threadStderr() noexcept {
	PRINT_DEBUG("initiated for process %s (pid %s)", name.c_str(), _var2str(pid));
	thread_stderr_active = true;

	const uint buffer_size = 1024;
	char buffer[buffer_size]; buffer[0] = '\0'; buffer[buffer_size -1] = '\0';

	try {
		while (!must_stop && std::fgets(buffer, buffer_size -1, f_stderr) != NULL) {
			if (log_level <= LOG_DEBUG_OUT) {
				std::string aux = str_replace(buffer, '\n', ' ');
				PRINT_DEBUG_OUT("stderr line: %s", aux.c_str());
			}
			handler_stderr(buffer);
		}
	} catch(std::exception& e) {
		PRINT_DEBUG("exception received: %s", e.what());
		thread_exception = std::current_exception();
	}
	thread_stderr_active = false;
	PRINT_DEBUG("finish");
}

bool ProcessController::checkStatus() noexcept {
	//PRINT_DEBUG("check status of process %s (pid %s)", name.c_str(), _var2str(pid));
	int status;

	if (!program_active)
		return false;

	auto w = waitpid(pid, &status, WNOHANG);
	if (w == 0)
		return true;
	if (w == -1) {
		program_active = false;
		PRINT_CRITICAL("waitpid error for process %s (pid %s)", name.c_str(), _var2str(pid));
		std::raise(SIGTERM);
	}
	if (WIFEXITED(status)) {
		exit_code = WEXITSTATUS(status);
		program_active = false;
		std::string msg = sprintf("process %s (pid %s) exited, status=%s", name.c_str(), _var2str(pid), _var2str(exit_code));
		if (exit_code != 0)
			PRINT_WARN("%s", msg.c_str());
		else
			PRINT_DEBUG("%s", msg.c_str());
		return false;
	}
	if (WIFSIGNALED(status)) {
		signal = WTERMSIG(status);
		program_active = false;
		PRINT_WARN("process %s (pid %s) killed by signal %s", name.c_str(), _var2str(pid), _var2str(signal));
		return false;
	}
	if (WIFSTOPPED(status)) {
		signal = WSTOPSIG(status);
		program_active = false;
		PRINT_WARN("process %s (pid %s) stopped by signal %s", name.c_str(), _var2str(pid), _var2str(signal));
		return false;
	}
	program_active = (!WIFEXITED(status) && !WIFSIGNALED(status));
	return program_active;
}

////////////////////////////////////////////////////////////////////////////////////
#undef __CLASS__
#define __CLASS__ "ThreadController::"


ThreadController::ThreadController(main_t main) {
	_active = true;
	_stop   = false;
	thread = std::thread( [this, main]{this->run(main);} );
}

ThreadController::~ThreadController() {
	PRINT_DEBUG("destructor begin");
	stop();
	if (thread.joinable())
		thread.join();
	PRINT_DEBUG("destructor end");
}

void ThreadController::stop() {
	PRINT_DEBUG("set _stop=true");
	_stop = true;
}

bool ThreadController::isActive(bool throw_exception) {
	if (thread_exception) {
		if (throw_exception)
			std::rethrow_exception(thread_exception);
		else {
			try { std::rethrow_exception(thread_exception); }
			catch (std::exception& e) {
				PRINT_ERROR("exception received: %s", e.what());
			}
		}
	}
	return _active;
}

void ThreadController::run(main_t main) noexcept {
	try {
		PRINT_DEBUG("initiating thread function");
		main([this]()->bool{return this->_stop;});
	} catch(std::exception& e) {
		PRINT_DEBUG("exception received: %s", e.what());
		thread_exception = std::current_exception();
	}
	PRINT_DEBUG("thread function finished");
	_active = false;
}

} // namespace alutils
