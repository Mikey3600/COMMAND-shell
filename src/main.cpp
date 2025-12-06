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
#include <stdexcept>
#include <filesystem>

#include <readline/readline.h>
#include <readline/history.h>

// Global for tracking appended history
static int history_offset = 0;

// ======================= Tokenizer (handles quotes + backslashes) =======================

std::vector<std::string> tokenize(const std::string &input) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inSingle = false, inDouble = false;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        if (c == '\\' && !inSingle) {
            if (i + 1 < input.size()) {
                char next = input[++i];
                if (inDouble && (next != '"' && next != '\\' && next != '$' && next != '`')) {
                    cur.push_back('\\');
                }
                cur.push_back(next);
            } else {
                cur.push_back('\\');
            }
            continue;
        }

        if (c == '\'' && !inDouble) {
            inSingle = !inSingle;
            continue;
        }
        if (c == '"' && !inSingle) {
            inDouble = !inDouble;
            continue;
        }

        if (std::isspace((unsigned char)c) && !inSingle && !inDouble) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
            continue;
        }

        cur.push_back(c);
    }

    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

// ======================= Built-ins =======================

const std::vector<std::string> builtins = {"echo", "exit", "pwd", "cd", "type", "history"};

bool is_builtin(const std::string &cmd) {
    return std::find(builtins.begin(), builtins.end(), cmd) != builtins.end();
}

// ======================= PATH helpers =======================

bool find_in_path(const std::string &name, std::string &out_path) {
    const char *path_env = std::getenv("PATH");
    if (!path_env) return false;

    std::string path(path_env);
    size_t pos = 0;
    while (pos < path.size()) {
        size_t next = path.find(':', pos);
        std::string dir = (next == std::string::npos) ? path.substr(pos) : path.substr(pos, next - pos);

        if (!dir.empty()) {
            std::string full = dir + "/" + name;
            if (access(full.c_str(), X_OK) == 0) {
                out_path = full;
                return true;
            }
        }
        pos = (next == std::string::npos) ? path.size() : next + 1;
    }
    return false;
}

std::vector<std::string> find_executable_matches(const std::string &prefix) {
    std::vector<std::string> matches;
    const char *path_env = std::getenv("PATH");
    if (!path_env) return matches;

    std::string path(path_env);
    size_t pos = 0;
    while (pos < path.size()) {
        size_t next = path.find(':', pos);
        std::string dir = (next == std::string::npos) ? path.substr(pos) : path.substr(pos, next - pos);

        if (!dir.empty()) {
            DIR *d = opendir(dir.c_str());
            if (d) {
                struct dirent *ent;
                while ((ent = readdir(d))) {
                    std::string name = ent->d_name;
                    if (name.rfind(prefix, 0) == 0) {
                        std::string full = dir + "/" + name;
                        if (access(full.c_str(), X_OK) == 0) {
                            matches.push_back(name);
                        }
                    }
                }
                closedir(d);
            }
        }
        pos = (next == std::string::npos) ? path.size() : next + 1;
    }
    std::sort(matches.begin(), matches.end());
    return matches;
}

std::string longest_common_prefix(const std::vector<std::string> &v) {
    if (v.empty()) return "";
    std::string pref = v[0];
    for (const auto &s : v) {
        size_t i = 0;
        while (i < pref.size() && i < s.size() && pref[i] == s[i]) ++i;
        pref.resize(i);
        if (pref.empty()) break;
    }
    return pref;
}

// ======================= TAB completion =======================

static std::string last_tab_prefix;
static int tab_press_count = 0;

int tab_completion_handler(int, int) {
    std::string line(rl_line_buffer);
    std::string prefix = line.substr(0, rl_point);

    // Remove trailing spaces for matching
    size_t last = prefix.find_last_not_of(" \t");
    if (last != std::string::npos) prefix.resize(last + 1);

    // Built-in matches
    std::vector<std::string> matches;
    for (const auto &b : builtins) {
        if (b.rfind(prefix, 0) == 0) matches.push_back(b);
    }

    // PATH matches
    auto path_matches = find_executable_matches(prefix);
    matches.insert(matches.end(), path_matches.begin(), path_matches.end());

    if (matches.empty()) {
        rl_bell();
        return 0;
    }

    if (matches.size() == 1) {
        rl_replace_line(matches[0].c_str(), 1);
        rl_point = matches[0].size();
        rl_insert_text(" ");
        rl_redisplay();
        last_tab_prefix.clear();
        tab_press_count = 0;
        return 0;
    }

    std::string lcp = longest_common_prefix(matches);
    if (lcp.size() > prefix.size()) {
        rl_replace_line(lcp.c_str(), 1);
        rl_point = lcp.size();
        rl_redisplay();
        last_tab_prefix = lcp;
        tab_press_count = 0;
        return 0;
    }

    // Multiple matches: first TAB → bell, second → list
    if (prefix != last_tab_prefix) tab_press_count = 0;
    last_tab_prefix = prefix;
    tab_press_count++;

    if (tab_press_count == 1) {
        rl_bell();
        return 0;
    }

    std::cout << '\n';
    for (size_t i = 0; i < matches.size(); ++i) {
        std::cout << matches[i] << (i + 1 < matches.size() ? "  " : "\n");
    }
    std::cout << "$ " << rl_line_buffer;
    std::fflush(stdout);

    rl_redisplay();
    return 0;
}

// ======================= History builtin =======================

void run_history_builtin(const std::vector<std::string> &args) {
    if (args.size() >= 3) {
        const std::string &opt = args[1];
        const std::string &file = args[2];

        if (opt == "-r") {
            if (read_history(file.c_str()) == 0) {
                history_offset = history_length;
            } else {
                std::cerr << "history: cannot read " << file << '\n';
            }
            return;
        }

        if (opt == "-w") {
            if (write_history(file.c_str()) != 0) {
                std::cerr << "history: cannot write " << file << '\n';
            } else {
                history_offset = history_length;
            }
            return;
        }

        if (opt == "-a") {
            // If history is empty but file exists → read it first so append_history knows base
            if (history_length == 0 && access(file.c_str(), F_OK) == 0) {
                read_history(file.c_str()); // ignore error, best effort
            }

            int new_entries = history_length - history_offset;
            if (new_entries > 0) {
                if (append_history(new_entries, file.c_str()) == 0) {
                    history_offset = history_length;
                } else {
                    std::cerr << "history: cannot append to " << file << '\n';
                }
            }
            return;
        }
    }

    // Default: list history
    int limit = 0;
    if (args.size() == 2) {
        try {
            limit = std::stoi(args[1]);
            if (limit <= 0) limit = 0;
        } catch (...) {
            std::cerr << "history: numeric argument required\n";
            return;
        }
    }

    HIST_ENTRY **list = history_list();
    if (!list) return;

    int total = 0;
    while (list[total]) ++total;

    int start = 0;
    if (limit > 0 && limit < total) start = total - limit;

    for (int i = start; i < total; ++i) {
        printf("%5d  %s\n", history_base + i, list[i]->line);
    }
}

// ======================= Execute builtin in child (for pipelines) =======================

void exec_builtin_child(const std::vector<std::string> &args) {
    const std::string &cmd = args[0];

    if (cmd == "echo") {
        for (size_t i = 1; i < args.size(); ++i) {
            std::cout << args[i] << (i + 1 < args.size() ? " " : "\n");
        }
    } else if (cmd == "pwd") {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) std::cout << cwd << '\n';
    } else if (cmd == "type" && args.size() > 1) {
        const std::string &t = args[1];
        if (is_builtin(t)) {
            std::cout << t << " is a shell builtin\n";
        } else {
            std::string path;
            if (find_in_path(t, path)) {
                std::cout << t << " is " << path << '\n';
            } else {
                std::cerr << t << ": not found\n";
            }
        }
    } else if (cmd == "history") {
        run_history_builtin(args);
    }
    // cd and exit do nothing in child
    _exit(0);
}

// ======================= Run single external command =======================

void run_external(const std::vector<std::string> &args) {
    std::vector<char*> argv;
    for (const auto &s : args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    std::string path;
    if (!find_in_path(args[0], path)) {
        std::cerr << args[0] << ": command not found\n";
        for (char *p : argv) if (p) free(p);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        execv(path.c_str(), argv.data());
        _exit(127);
    } else if (pid > 0) {
        waitpid(pid, nullptr, 0);
    }
    for (char *p : argv) if (p) free(p);
}

// ======================= Pipeline execution =======================

void run_pipeline(const std::vector<std::vector<std::string>>& pipeline) {
    size_t n = pipeline.size();
    if (n == 0) return;

    if (n == 1) {
        const auto &cmd = pipeline[0];
        if (is_builtin(cmd[0])) {
            // Run builtin directly in parent if no pipe
            if (cmd[0] == "cd") {
                const char *dir = (cmd.size() > 1) ? cmd[1].c_str() : getenv("HOME");
                if (!dir || chdir(dir) != 0) {
                    std::cerr << "cd: " << (cmd.size() > 1 ? cmd[1] : "HOME not set") << '\n';
                }
            } else if (cmd[0] == "exit") {
                exit(0);
            } else {
                // Run other builtins in child to be safe with output
                pid_t p = fork();
                if (p == 0) exec_builtin_child(cmd);
                waitpid(p, nullptr, 0);
            }
        } else {
            run_external(cmd);
        }
        return;
    }

    // Multiple commands → real pipeline
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
            // Redirect input
            if (i > 0) dup2(fds[2 * (i - 1)], STDIN_FILENO);
            // Redirect output
            if (i + 1 < n) dup2(fds[2 * i + 1], STDOUT_FILENO);

            // Close all fds
            for (int fd : fds) close(fd);

            const auto &cmd = pipeline[i];
            if (is_builtin(cmd[0])) {
                exec_builtin_child(cmd);
            } else {
                std::string path;
                if (!find_in_path(cmd[0], path)) {
                    std::cerr << cmd[0] << ": command not found\n";
                    _exit(127);
                }
                std::vector<char*> argv;
                for (const auto &a : cmd) argv.push_back(const_cast<char*>(a.c_str()));
                argv.push_back(nullptr);
                execv(path.c_str(), argv.data());
                _exit(127);
            }
        }
    }

    for (int fd : fds) close(fd);
    for (size_t i = 0; i < n; ++i) wait(nullptr);
}

// ======================= Main =======================

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    using_history();
    stifle_history(1000);
    history_offset = history_length;  // Critical fix

    rl_bind_key('\t', tab_completion_handler);

    while (true) {
        char *raw = readline("$ ");
        if (!raw) break;  // EOF

        std::string line(raw);
        free(raw);

        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Add to history unless it's a history command
        if (!line.empty() && line.rfind("history", 0) != 0) {
            add_history(line.c_str());
        }

        auto tokens = tokenize(line);
        if (tokens.empty()) continue;

        // === Redirection handling ===
        std::string out_file, err_file;
        bool append_out = false, append_err = false;

        for (size_t i = 0; i < tokens.size(); ) {
            const std::string &t = tokens[i];
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

        // Apply redirections
        int saved_out = -1, saved_err = -1;
        if (!out_file.empty()) {
            saved_out = dup(STDOUT_FILENO);
            int flags = O_CREAT | O_WRONLY | (append_out ? O_APPEND : O_TRUNC);
            int fd = open(out_file.c_str(), flags, 0644);
            if (fd != -1) { dup2(fd, STDOUT_FILENO); close(fd); }
        }
        if (!err_file.empty()) {
            saved_err = dup(STDERR_FILENO);
            int flags = O_CREAT | O_WRONLY | (append_err ? O_APPEND : O_TRUNC);
            int fd = open(err_file.c_str(), flags, 0644);
            if (fd != -1) { dup2(fd, STDERR_FILENO); close(fd); }
        }

        // === Split into pipeline ===
        std::vector<std::vector<std::string>> pipeline;
        std::vector<std::string> cur;
        for (const auto &t : tokens) {
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

        // Execute
        if (!pipeline.empty()) {
            if (is_builtin(pipeline[0][0]) && pipeline.size() == 1) {
                const auto &cmd = pipeline[0][0];
                const auto &args = pipeline[0];

                if (cmd == "exit") {
                    if (saved_out != -1) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
                    if (saved_err != -1) { dup2(saved_err, STDERR_FILENO); close(saved_err); }
                    exit(0);
                } else if (cmd == "cd") {
                    const char *dir = args.size() > 1 ? args[1].c_str() : getenv("HOME");
                    if (!dir || chdir(dir) != 0) {
                        std::cerr << "cd: " << (args.size() > 1 ? args[1] : "HOME not set") << '\n';
                    }
                } else if (cmd == "history") {
                    run_history_builtin(args);
                } else {
                    // echo, pwd, type → run in child for correct output
                    pid_t p = fork();
                    if (p == 0) exec_builtin_child(args);
                    waitpid(p, nullptr, 0);
                }
            } else {
                run_pipeline(pipeline);
            }
        }

        // Restore stdout/stderr
        if (saved_out != -1) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
        if (saved_err != -1) { dup2(saved_err, STDERR_FILENO); close(saved_err); }
    }

    return 0;
}