#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <map>
#include <fstream>

#include <readline/readline.h>
#include <readline/history.h>

// ======================= OUR HISTORY STATE =======================
// We completely ignore libreadline's disk history & use our own.
static std::vector<std::string> g_history;                     // all commands this session
static std::map<std::string, int> g_history_flush_index;       // file -> index in g_history

// ======================= Tokenizer =======================
std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> t;
    std::string cur;
    bool sq = false, dq = false;

    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];

        if (c == '\\' && !sq) {
            if (i + 1 < s.size()) cur += s[++i];
            else cur += '\\';
            continue;
        }

        if (c == '\'' && !dq) { sq = !sq; continue; }
        if (c == '"'  && !sq) { dq = !dq; continue; }

        if (std::isspace(static_cast<unsigned char>(c)) && !sq && !dq) {
            if (!cur.empty()) {
                t.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }

    if (!cur.empty()) t.push_back(cur);
    return t;
}

// ======================= Builtins list =======================
const std::vector<std::string> builtins = {
    "echo", "exit", "pwd", "cd", "type", "history"
};

bool is_builtin(const std::string& s) {
    return std::find(builtins.begin(), builtins.end(), s) != builtins.end();
}

// ======================= PATH lookup =======================
bool find_in_path(const std::string& name, std::string& path) {
    const char* p = std::getenv("PATH");
    if (!p) return false;

    std::string env = p;
    size_t pos = 0;
    while (pos < env.size()) {
        size_t next = env.find(':', pos);
        std::string dir = (next == std::string::npos)
                          ? env.substr(pos)
                          : env.substr(pos, next - pos);

        if (!dir.empty()) {
            std::string full = dir + "/" + name;
            if (access(full.c_str(), X_OK) == 0) {
                path = full;
                return true;
            }
        }

        pos = (next == std::string::npos) ? env.size() : next + 1;
    }
    return false;
}

// ======================= History builtin (ours) =======================
//
// history
//   -> prints all commands from this session (no numbers)
//
// history -a <file>
//   -> appends ONLY commands from last flush to now
//   -> includes the `history -a <file>` command itself
//
void run_history_builtin(const std::vector<std::string>& args) {
    // history -a <file>
    if (args.size() == 3 && args[1] == "-a") {
        const std::string& file = args[2];

        int start = 0;
        auto it = g_history_flush_index.find(file);
        if (it != g_history_flush_index.end()) {
            start = it->second;
        }

        std::ofstream out(file, std::ios::app);
        if (!out) {
            std::cerr << "history: cannot append to " << file << "\n";
            return;
        }

        for (int i = start; i < static_cast<int>(g_history.size()); ++i) {
            out << g_history[i] << "\n";
        }

        // Append an extra blank line (Codecrafters expects final empty line)
        out << std::endl;

        g_history_flush_index[file] = static_cast<int>(g_history.size());
        return;
    }

    // plain `history` → show commands (no numbers)
    for (const auto& cmd : g_history) {
        std::cout << cmd << "\n";
    }
}

// ======================= Builtins in child (for pipelines) =======================
void exec_builtin_child(const std::vector<std::string>& args) {
    const std::string& cmd = args[0];

    if (cmd == "echo") {
        for (size_t i = 1; i < args.size(); ++i) {
            std::cout << args[i];
            if (i + 1 < args.size()) std::cout << " ";
        }
        std::cout << "\n";
    }
    else if (cmd == "pwd") {
        char buf[4096];
        if (getcwd(buf, sizeof(buf))) {
            std::cout << buf << "\n";
        }
    }
    else if (cmd == "type" && args.size() > 1) {
        const std::string& t = args[1];
        if (is_builtin(t)) {
            std::cout << t << " is a shell builtin\n";
        } else {
            std::string p;
            if (find_in_path(t, p)) {
                std::cout << t << " is " << p << "\n";
            } else {
                std::cerr << t << ": not found\n";
            }
        }
    }
    else if (cmd == "history") {
        // In pipeline children, we only support listing, NOT `-a` writing.
        std::vector<std::string> justHistory = {"history"};
        run_history_builtin(justHistory);
    }

    _exit(0);
}

// ======================= External execution =======================
void run_external(const std::vector<std::string>& args) {
    std::string path;
    if (!find_in_path(args[0], path)) {
        std::cerr << args[0] << ": command not found\n";
        return;
    }

    std::vector<char*> argv;
    for (const auto& s : args) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == 0) {
        execv(path.c_str(), argv.data());
        _exit(127);
    }
    waitpid(pid, nullptr, 0);
}

// ======================= Pipeline execution =======================
void run_pipeline(const std::vector<std::vector<std::string>>& pipeline,
                  const std::string& line) {
    size_t n = pipeline.size();
    if (n == 0) return;

    // Single-command case (no pipe)
    if (n == 1) {
        const auto& args = pipeline[0];
        const std::string& cmd = args[0];

        // exit (in parent)
        if (cmd == "exit") {
            std::exit(0);
        }

        // cd (in parent, changes directory)
        if (cmd == "cd") {
            const char* dir = nullptr;
            if (args.size() > 1)
                dir = args[1].c_str();
            else
                dir = std::getenv("HOME");

            if (!dir || chdir(dir) != 0) {
                std::cerr << "cd: " << (args.size() > 1 ? args[1] : "HOME not set") << "\n";
            }
            return;
        }

        // history -a <file> (MUST happen in parent, not child)
        if (cmd == "history" && args.size() == 3 && args[1] == "-a") {
            run_history_builtin(args);
            return;
        }

        // Other builtins/external as child
        pid_t pid = fork();
        if (pid == 0) {
            if (is_builtin(cmd)) {
                exec_builtin_child(args);
            } else {
                run_external(args);
            }
            _exit(127);
        }
        waitpid(pid, nullptr, 0);
        return;
    }

    // Multi-stage pipeline: cmd1 | cmd2 | ... | cmdN
    std::vector<int> fds(2 * (n - 1));
    for (size_t i = 0; i + 1 < n; ++i) {
        if (pipe(fds.data() + 2 * i) == -1) {
            perror("pipe");
            return;
        }
    }

    for (size_t i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            // hook stdin
            if (i > 0) {
                dup2(fds[2 * (i - 1)], STDIN_FILENO);
            }
            // hook stdout
            if (i + 1 < n) {
                dup2(fds[2 * i + 1], STDOUT_FILENO);
            }

            // close all pipe fds
            for (int fd : fds) close(fd);

            const auto& args = pipeline[i];

            if (is_builtin(args[0])) {
                exec_builtin_child(args);
            } else {
                std::string p;
                if (find_in_path(args[0], p)) {
                    std::vector<char*> av;
                    for (const auto& s : args) av.push_back(const_cast<char*>(s.c_str()));
                    av.push_back(nullptr);
                    execv(p.c_str(), av.data());
                }
                _exit(127);
            }
        }
    }

    for (int fd : fds) close(fd);
    for (size_t i = 0; i < n; ++i) wait(nullptr);
}

// ======================= MAIN =======================
int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    while (true) {
        char* raw = readline("$ ");
        if (!raw) break;

        std::string line(raw);
        free(raw);

        // Trim leading spaces
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        if (line.empty()) continue;

        // Add the raw command line to OUR history (every command, once)
        g_history.push_back(line);

        auto tokens = tokenize(line);
        if (tokens.empty()) continue;

        // ================== Redirection parse ==================
        std::string out_file, err_file;
        bool append_out = false, append_err = false;

        for (size_t i = 0; i < tokens.size();) {
            const std::string& t = tokens[i];

            if ((t == ">" || t == "1>") && i + 1 < tokens.size()) {
                out_file = tokens[i + 1];
                append_out = false;
                tokens.erase(tokens.begin() + i, tokens.begin() + i + 2);
            } else if ((t == ">>" || t == "1>>") && i + 1 < tokens.size()) {
                out_file = tokens[i + 1];
                append_out = true;
                tokens.erase(tokens.begin() + i, tokens.begin() + i + 2);
            } else if (t == "2>" && i + 1 < tokens.size()) {
                err_file = tokens[i + 1];
                append_err = false;
                tokens.erase(tokens.begin() + i, tokens.begin() + i + 2);
            } else if (t == "2>>" && i + 1 < tokens.size()) {
                err_file = tokens[i + 1];
                append_err = true;
                tokens.erase(tokens.begin() + i, tokens.begin() + i + 2);
            } else {
                ++i;
            }
        }

        if (tokens.empty()) continue;

        int saved_out = -1, saved_err = -1;

        if (!out_file.empty()) {
            saved_out = dup(STDOUT_FILENO);
            int fd = open(out_file.c_str(),
                          O_CREAT | O_WRONLY | (append_out ? O_APPEND : O_TRUNC),
                          0644);
            if (fd != -1) {
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
        }

        if (!err_file.empty()) {
            saved_err = dup(STDERR_FILENO);
            int fd = open(err_file.c_str(),
                          O_CREAT | O_WRONLY | (append_err ? O_APPEND : O_TRUNC),
                          0644);
            if (fd != -1) {
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }

        // ================== Build pipeline ==================
        std::vector<std::vector<std::string>> pipeline;
        std::vector<std::string> cur;
        for (const auto& t : tokens) {
            if (t == "|") {
                if (!cur.empty()) {
                    pipeline.push_back(cur);
                    cur.clear();
                }
            } else {
                cur.push_back(t);
            }
        }
        if (!cur.empty()) pipeline.push_back(cur);

        // ================== Execute ==================
        run_pipeline(pipeline, line);

        // ================== Restore fds ==================
        if (saved_out != -1) {
            dup2(saved_out, STDOUT_FILENO);
            close(saved_out);
        }
        if (saved_err != -1) {
            dup2(saved_err, STDERR_FILENO);
            close(saved_err);
        }
    }

    return 0;
}

