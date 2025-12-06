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

static int my_history_offset = 0;

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
        if (std::isspace(c) && !sq && !dq) {
            if (!cur.empty()) { t.push_back(cur); cur.clear(); }
        } else cur += c;
    }
    if (!cur.empty()) t.push_back(cur);
    return t;
}

// ======================= Builtins =======================
const std::vector<std::string> builtins = {"echo","exit","pwd","cd","type","history"};

bool is_builtin(const std::string& s) {
    return std::find(builtins.begin(), builtins.end(), s) != builtins.end();
}

bool find_in_path(const std::string& name, std::string& path) {
    const char* p = getenv("PATH");
    if (!p) return false;
    std::string env = p;
    size_t pos = 0;
    while (pos < env.size()) {
        size_t next = env.find(':', pos);
        std::string dir = (next == std::string::npos) ? env.substr(pos) : env.substr(pos, next-pos);
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

// ======================= History builtin =======================
void run_history_builtin(const std::vector<std::string>& args) {
    if (args.size() == 3 && args[1] == "-a") {
        const std::string& file = args[2];

        if (history_length == 0 && access(file.c_str(), F_OK) == 0) {
            read_history(file.c_str());
        }

        int new_entries = history_length - my_history_offset;
        if (new_entries > 0) {
            if (append_history(new_entries, file.c_str()) == 0) {
                my_history_offset = history_length;
            } else {
                std::cerr << "history: cannot append to " << file << '\n';
            }
        }
        return;
    }

    // listing (optional)
    HIST_ENTRY** list = history_list();
    if (!list) return;
    for (int i = 0; list[i]; ++i) {
        printf("%5d  %s\n", history_base + i, list[i]->line);
    }
}

// ======================= Builtin child exec =======================
void exec_builtin_child(const std::vector<std::string>& args) {
    const std::string& cmd = args[0];
    if (cmd == "echo") {
        for (size_t i = 1; i < args.size(); ++i)
            std::cout << args[i] << (i+1 < args.size() ? " " : "\n");
    }
    else if (cmd == "pwd") {
        char buf[4096];
        if (getcwd(buf, sizeof(buf))) std::cout << buf << '\n';
    }
    else if (cmd == "type" && args.size() > 1) {
        const std::string& t = args[1];
        if (is_builtin(t)) std::cout << t << " is a shell builtin\n";
        else {
            std::string p;
            if (find_in_path(t, p)) std::cout << t << " is " << p << '\n';
            else std::cerr << t << ": not found\n";
        }
    }
    else if (cmd == "history") run_history_builtin(args);
    _exit(0);
}

// ======================= External command =======================
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

// ======================= Pipeline =======================
void run_pipeline(const std::vector<std::vector<std::string>>& pipeline, const std::string& original_line) {
    size_t n = pipeline.size();
    if (n == 0) return;

    // Single command
    if (n == 1) {
        const auto& args = pipeline[0];
        const std::string& cmd = args[0];

        if (cmd == "exit") exit(0);
        if (cmd == "cd") {
            const char* dir = args.size() > 1 ? args[1].c_str() : getenv("HOME");
            if (!dir || chdir(dir) != 0)
                std::cerr << "cd: " << (args.size() > 1 ? args[1] : "HOME not set" << '\n';
            return;
        }

        // Special case: history -a → run builtin + add to history NOW
        if (cmd == "history" && args.size() == 3 && args[1] == "-a") {
            add_history(original_line.c_str());  // ← ADD BEFORE running!
            run_history_builtin(args);
            return;
        }

        // Normal builtins & external
        pid_t pid = fork();
        if (pid == 0) {
            if (is_builtin(cmd)) exec_builtin_child(args);
            else run_external(args);
            _exit(127);
        }
        waitpid(pid, nullptr, 0);
        return;
    }

    // Real pipeline → fork all
    std::vector<int> fds(2*(n-1));
    for (size_t i = 0; i+1 < n; ++i) pipe(fds.data() + 2*i);

    for (size_t i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            if (i > 0) dup2(fds[2*(i-1)],   STDIN_FILENO);
            if (i+1 < n) dup2(fds[2*i + 1], STDOUT_FILENO);
            for (int fd : fds) close(fd);

            const auto& args = pipeline[i];
            if (is_builtin(args[0])) exec_builtin_child(args);
            else {
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

    using_history();
    stifle_history(1000);
    my_history_offset = history_length;

    while (true) {
        char* raw = readline("$ ");
        if (!raw) break;

        std::string line(raw);
        free(raw);

        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        if (line.empty()) continue;

        auto tokens = tokenize(line);
        if (tokens.empty()) continue;

        // === Redirection ===
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

        int saved_out = -1, saved_err = -1;
        if (!out_file.empty()) {
            saved_out = dup(STDOUT_FILENO);
            int fd = open(out_file.c_str(), O_CREAT|O_WRONLY|(append_out ? O_APPEND : O_TRUNC), 0644);
            if (fd != -1) { dup2(fd, STDOUT_FILENO); close(fd); }
        }
        if (!err_file.empty()) {
            saved_err = dup(STDERR_FILENO);
            int fd = open(err_file.c_str(), O_CREAT|O_WRONLY|(append_err ? O_APPEND : O_TRUNC), 0644);
            if (fd != -1) { dup2(fd, STDERR_FILENO); close(fd); }
        }

        // === Build pipeline ===
        std::vector<std::vector<std::string>> pipeline;
        std::vector<std::string> cur;
        for (const auto& t : tokens) {
            if (t == "|") {
                if (!cur.empty()) { pipeline.push_back(cur); cur.clear(); }
            } else cur.push_back(t);
        }
        if (!cur.empty()) pipeline.push_back(cur);

        // === Execute ===
        run_pipeline(pipeline, line);

        // === Add to history AFTER everything EXCEPT when we already added it for history -a ===
        bool is_history_a = (pipeline.size() == 1 && pipeline[0].size() == 3 &&
                             pipeline[0][0] == "history" && pipeline[0][1] == "-a");
        if (!is_history_a) {
            add_history(line.c_str());
        }

        // Restore fds
        if (saved_out != -1) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
        if (saved_err != -1) { dup2(saved_err, STDERR_FILENO); close(saved_err); }
    }
    return 0;
}