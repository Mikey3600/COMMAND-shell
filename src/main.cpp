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

#include <readline/readline.h>
#include <readline/history.h>

// We cannot use the name "history_offset" → readline already declares it
static int my_history_offset = 0;

// ======================= Tokenizer =======================
std::vector<std::string> tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string cur;
    bool in_sq = false, in_dq = false;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        if (c == '\\' && !in_sq) {
            if (i + 1 < input.size()) cur += input[++i];
            else cur += '\\';
            continue;
        }
        if (c == '\'' && !in_dq) { in_sq = !in_sq; continue; }
        if (c == '"'  && !in_sq) { in_dq = !in_dq; continue; }

        if (std::isspace(c) && !in_sq && !in_dq) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

// ======================= Built-ins =======================
const std::vector<std::string> builtins = {"echo","exit","pwd","cd","type","history"};

bool is_builtin(const std::string& s) {
    return std::find(builtins.begin(), builtins.end(), s) != builtins.end();
}

// ======================= PATH helpers =======================
bool find_in_path(const std::string& name, std::string& path) {
    const char* env = getenv("PATH");
    if (!env) return false;
    std::string p = env;
    size_t pos = 0;
    while (pos < p.size()) {
        size_t next = p.find(':', pos);
        std::string dir = (next == std::string::npos) ? p.substr(pos) : p.substr(pos, next-pos);
        if (!dir.empty()) {
            std::string full = dir + "/" + name;
            if (access(full.c_str(), X_OK) == 0) {
                path = full;
                return true;
            }
        }
        pos = (next == std::string::npos) ? p.size() : next + 1;
    }
    return false;
}

// ======================= History builtin =======================
void run_history_builtin(const std::vector<std::string>& args) {
    if (args.size() == 3) {
        const std::string& opt = args[1];
        const std::string& file = args[2];

        if (opt == "-a") {
            // If history is empty but file exists → read it first so append_history knows the base
            if (history_length == 0 && access(file.c_str(), F_OK) == 0) {
                read_history(file.c_str()); // ignore error – best effort
            }

            int new_entries = history_length - my_history_offset;
            if (new_entries > 0) {
                if (append_history(new_entries, file.c_str()) == 0) {
                    my_history_offset = history_length;   // success → move marker
                } else {
                    std::cerr << "history: cannot append to " << file << '\n';
                }
            }
            return;
        }
        // -r and -w are not required for this stage, but we keep them simple
        if (opt == "-r") { read_history(file.c_str()); return; }
        if (opt == "-w") { write_history(file.c_str()); return; }
    }

    // default: list history
    int limit = 0;
    if (args.size() == 2) {
        try { limit = std::stoi(args[1]); } catch(...) { limit = 0; }
    }

    HIST_ENTRY** list = history_list();
    if (!list) return;
    int total = 0; while (list[total]) ++total;

    int start = (limit > 0 && limit < total) ? total - limit : 0;
    for (int i = start; i < total; ++i) {
        printf("%5d  %s\n", history_base + i, list[i]->line);
    }
}

// ======================= Execute builtin in child =======================
void exec_builtin_child(const std::vector<std::string>& args) {
    const std::string& cmd = args[0];
    if (cmd == "echo") {
        for (size_t i = 1; i < args.size(); ++i) std::cout << args[i] << (i+1 < args.size() ? " " : "\n");
    } else if (cmd == "pwd") {
        char buf[4096];
        if (getcwd(buf, sizeof(buf))) std::cout << buf << '\n';
    } else if (cmd == "type" && args.size() > 1) {
        const std::string& t = args[1];
        if (is_builtin(t)) std::cout << t << " is a shell builtin\n";
        else {
            std::string p;
            if (find_in_path(t, p)) std::cout << t << " is " << p << '\n';
            else std::cerr << t << ": not found\n";
        }
    } else if (cmd == "history") {
        run_history_builtin(args);
    }
    _exit(0);
}

// ======================= Run external command =======================
void run_external(const std::vector<std::string>& args) {
    std::string path;
    if (!find_in_path(args[0], path)) {
        std::cerr << args[0] << ": command not found\n";
        return;
    }
    std::vector<char*> argv;
    for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == 0) { execv(path.c_str(), argv.data()); _exit(127); }
    waitpid(pid, nullptr, 0);
}

// ======================= Pipeline (includes single command case) =======================
void run_pipeline(const std::vector<std::vector<std::string>>& cmds) {
    size_t n = cmds.size();
    if (n == 0) return;

    // single command → run builtin in parent if possible
    if (n == 1) {
        const auto& args = cmds[0];
        const std::string& cmd = args[0];

        if (cmd == "exit") exit(0);
        if (cmd == "cd") {
            const char* dir = args.size() > 1 ? args[1].c_str() : getenv("HOME");
            if (!dir || chdir(dir) != 0)
                std::cerr << "cd: " << (args.size() > 1 ? args[1] : "HOME not set") << '\n';
            return;
        }
        if (is_builtin(cmd)) {
            pid_t p = fork();
            if (p == 0) exec_builtin_child(args);
            waitpid(p, nullptr, 0);
            return;
        }
        run_external(args);
        return;
    }

    // real pipeline
    std::vector<int> pipes(2*(n-1));
    for (size_t i = 0; i+1 < n; ++i) pipe(pipes.data() + 2*i);

    for (size_t i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            if (i > 0)  dup2(pipes[2*(i-1)],   STDIN_FILENO);
            if (i+1 < n) dup2(pipes[2*i + 1], STDOUT_FILENO);
            for (int fd : pipes) close(fd);

            const auto& args = cmds[i];
            if (is_builtin(args[0])) exec_builtin_child(args);
            else {
                std::string path;
                if (find_in_path(args[0], path)) {
                    std::vector<char*> argv;
                    for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
                    argv.push_back(nullptr);
                    execv(path.c_str(), argv.data());
                }
                _exit(127);
            }
        }
    }
    for (int fd : pipes) close(fd);
    for (size_t i = 0; i < n; ++i) wait(nullptr);
}

// ======================= Main =======================
int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    using_history();
    stifle_history(1000);
    my_history_offset = history_length;   // ← critical

    while (true) {
        char* raw = readline("$ ");
        if (!raw) break;  // EOF

        std::string line(raw);
        free(raw);

        // trim leading whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // *** IMPORTANT: always add command to history first ***
        if (!line.empty()) add_history(line.c_str());

        auto tokens = tokenize(line);
        if (tokens.empty()) continue;

        // redirection handling
        std::string out_file, err_file;
        bool append_out = false, append_err = false;

        for (size_t i = 0; i < tokens.size(); ) {
            const std::string& t = tokens[i];
            if ((t == ">" || t == "1>") && i+1 < tokens.size()) {
                out_file = tokens[i+1]; append_out = false;
                tokens.erase(tokens.begin()+i, tokens.begin()+i+2);
            } else if ((t == ">>" || t == "1>>") && i+1 < tokens.size()) {
                out_file = tokens[i+1]; append_out = true;
                tokens.erase(tokens.begin()+i, tokens.begin()+i+2);
            } else if (t == "2>" && i+1 < tokens.size()) {
                err_file = tokens[i+1]; append_err = false;
                tokens.erase(tokens.begin()+i, tokens.begin()+i+2);
            } else if (t == "2>>" && i+1 < tokens.size()) {
                err_file = tokens[i+1]; append_err = true;
                tokens.erase(tokens.begin()+i, tokens.begin()+i+2);
            } else ++i;
        }

        if (tokens.empty()) continue;

        int saved_out = -1, saved_err = -1;
        if (!out_file.empty()) {
            saved_out = dup(STDOUT_FILENO);
            int fd = open(out_file.c_str(),
                          O_CREAT | O_WRONLY | (append_out ? O_APPEND : O_TRUNC), 0644);
            if (fd != -1) { dup2(fd, STDOUT_FILENO); close(fd); }
        }
        if (!err_file.empty()) {
            saved_err = dup(STDERR_FILENO);
            int fd = open(err_file.c_str(),
                          O_CREAT | O_WRONLY | (append_err ? O_APPEND : O_TRUNC), 0644);
            if (fd != -1) { dup2(fd, STDERR_FILENO); close(fd); }
        }

        // split into pipeline
        std::vector<std::vector<std::string>> pipeline;
        std::vector<std::string> cur;
        for (const auto& t : tokens) {
            if (t == "|") {
                if (!cur.empty()) { pipeline.push_back(cur); cur.clear(); }
            } else cur.push_back(t);
        }
        if (!cur.empty()) pipeline.push_back(cur);

        // execute
        run_pipeline(pipeline);

        // restore fd
        if (saved_out != -1) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
        if (saved_err != -1) { dup2(saved_err, STDERR_FILENO); close(saved_err); }
    }
    return 0;
}