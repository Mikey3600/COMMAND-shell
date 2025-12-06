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

// ======================= Tokenizer (quotes + backslashes) =======================

std::vector<std::string> tokenize(const std::string &input) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inSingle = false, inDouble = false, escape = false;

    for (size_t i = 0; i < input.size(); i++) {
        char c = input[i];

        if (escape && !inDouble) {
            cur.push_back(c);
            escape = false;
            continue;
        }

        if (inDouble && c == '\\') {
            if (i + 1 < input.size()) {
                char next = input[i + 1];
                if (next == '"' || next == '\\') {
                    cur.push_back(next);
                    i++;
                    continue;
                }
            }
            cur.push_back('\\');
            continue;
        }

        if (c == '\\' && !inSingle && !inDouble) {
            escape = true;
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

    if (escape) cur.push_back('\\');
    if (!cur.empty()) tokens.push_back(cur);

    return tokens;
}

// ======================= Builtins list =======================

std::vector<std::string> builtin_list = {
    "echo", "exit", "pwd", "cd", "type"
};

// ======================= PATH scan for executables =======================

std::vector<std::string> find_path_matches(const std::string &prefix) {
    std::vector<std::string> matches;
    char *pathEnv = std::getenv("PATH");
    if (!pathEnv) return matches;

    std::string path(pathEnv);
    size_t start = 0;

    while (true) {
        size_t end = path.find(':', start);
        std::string dir = (end == std::string::npos)
                            ? path.substr(start)
                            : path.substr(start, end - start);

        if (!dir.empty()) {
            DIR *dp = opendir(dir.c_str());
            if (dp) {
                struct dirent *e;
                while ((e = readdir(dp)) != nullptr) {
                    std::string name = e->d_name;
                    if (name.rfind(prefix, 0) == 0) {
                        std::string full = dir + "/" + name;
                        if (access(full.c_str(), X_OK) == 0) {
                            matches.push_back(name);
                        }
                    }
                }
                closedir(dp);
            }
        }

        if (end == std::string::npos) break;
        start = end + 1;
    }
    return matches;
}

// ======================= Longest Common Prefix (LCP) =======================

std::string longest_common_prefix(const std::vector<std::string> &v) {
    if (v.empty()) return "";
    std::string prefix = v[0];
    for (size_t i = 1; i < v.size(); i++) {
        const std::string &s = v[i];
        size_t j = 0;
        while (j < prefix.size() && j < s.size() && prefix[j] == s[j]) {
            ++j;
        }
        prefix.resize(j);
        if (prefix.empty()) break;
    }
    return prefix;
}

// ======================= TAB completion state =======================

static std::string last_prefix;
static int tab_count = 0;

// ======================= type helper: find executable in PATH =======================

bool find_executable_in_path(const std::string &name, std::string &fullPath) {
    char *pathEnv = std::getenv("PATH");
    if (!pathEnv) return false;

    std::string path(pathEnv);
    size_t start = 0;

    while (true) {
        size_t end = path.find(':', start);
        std::string dir = (end == std::string::npos)
                            ? path.substr(start)
                            : path.substr(start, end - start);

        if (!dir.empty()) {
            std::string full = dir + "/" + name;
            if (access(full.c_str(), X_OK) == 0) {
                fullPath = full;
                return true;
            }
        }

        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

// ======================= TAB handler with LCP =======================

int tab_handler(int count, int key) {
    std::string prefix = rl_line_buffer;

    // 1) Builtin completions
    std::vector<std::string> builtin_matches;
    for (auto &b : builtin_list) {
        if (b.rfind(prefix, 0) == 0) {
            builtin_matches.push_back(b);
        }
    }

    // Single builtin match → complete immediately
    if (builtin_matches.size() == 1) {
        const std::string &full = builtin_matches[0];
        rl_replace_line(full.c_str(), 1);
        rl_point = full.size();
        rl_insert_text((char*)" ");
        rl_redisplay();
        last_prefix.clear();
        tab_count = 0;
        return 0;
    }

    // 2) PATH executable matches
    std::vector<std::string> path_matches = find_path_matches(prefix);
    std::sort(path_matches.begin(), path_matches.end());

    // Multiple builtin matches -> apply LCP or multi-list
    if (builtin_matches.size() > 1) {
        std::string lcp = longest_common_prefix(builtin_matches);
        if (!lcp.empty() && lcp.size() > prefix.size()) {
            rl_replace_line(lcp.c_str(), 1);
            rl_point = lcp.size();
            rl_redisplay();
            last_prefix = lcp;
            tab_count = 0;
            return 0;
        }

        if (prefix != last_prefix) tab_count = 0;
        last_prefix = prefix;
        tab_count++;

        if (tab_count == 1) {
            write(STDOUT_FILENO, "\a", 1);
            return 0;
        }

        std::cout << "\n";
        for (size_t i = 0; i < builtin_matches.size(); i++) {
            std::cout << builtin_matches[i];
            if (i + 1 < builtin_matches.size()) std::cout << "  ";
        }
        std::cout << "\n$ " << prefix;
        std::fflush(stdout);

        rl_replace_line(prefix.c_str(), 1);
        rl_point = prefix.size();
        rl_redisplay();
        return 0;
    }

    // No PATH matches either → bell
    if (path_matches.empty()) {
        write(STDOUT_FILENO, "\a", 1);
        return 0;
    }

    // Exactly one PATH match → full completion + space
    if (path_matches.size() == 1) {
        const std::string &full = path_matches[0];
        rl_replace_line(full.c_str(), 1);
        rl_point = full.size();
        rl_insert_text((char*)" ");
        rl_redisplay();
        last_prefix.clear();
        tab_count = 0;
        return 0;
    }

    // Multiple PATH matches → use LCP
    std::string lcp = longest_common_prefix(path_matches);
    if (!lcp.empty() && lcp.size() > prefix.size()) {
        rl_replace_line(lcp.c_str(), 1);
        rl_point = lcp.size();
        rl_redisplay();
        last_prefix = lcp;
        tab_count = 0;
        return 0;
    }

    // LCP == prefix → bell on first TAB, list on second TAB
    if (prefix != last_prefix) tab_count = 0;
    last_prefix = prefix;
    tab_count++;

    if (tab_count == 1) {
        write(STDOUT_FILENO, "\a", 1);
        return 0;
    }

    std::cout << "\n";
    for (size_t i = 0; i < path_matches.size(); i++) {
        std::cout << path_matches[i];
        if (i + 1 < path_matches.size()) std::cout << "  ";
    }
    std::cout << "\n$ " << prefix;
    std::fflush(stdout);

    rl_replace_line(prefix.c_str(), 1);
    rl_point = prefix.size();
    rl_redisplay();
    return 0;
}

// ======================= Builtin detection =======================

bool is_builtin(const std::string &cmd) {
    for (const auto &b : builtin_list) {
        if (b == cmd) return true;
    }
    return false;
}

// ======================= Builtins for child (in pipelines) =======================

void run_builtin_child(const std::vector<std::string> &parts) {
    const std::string &cmd = parts[0];

    if (cmd == "echo") {
        for (size_t i = 1; i < parts.size(); i++) {
            std::cout << parts[i];
            if (i + 1 < parts.size()) std::cout << " ";
        }
        std::cout << std::endl;
        _exit(0);
    }

    if (cmd == "pwd") {
        char buf[4096];
        if (getcwd(buf, sizeof(buf))) {
            std::cout << buf << std::endl;
        }
        _exit(0);
    }

    if (cmd == "cd") {
        // In pipeline, cd shouldn't affect parent shell.
        _exit(0);
    }

    if (cmd == "exit") {
        // In pipeline, exit shouldn't kill parent shell.
        _exit(0);
    }

    if (cmd == "type") {
        if (parts.size() > 1) {
            std::string target = parts[1];

            bool builtin = is_builtin(target);
            if (builtin) {
                std::cout << target << " is a shell builtin" << std::endl;
            } else {
                std::string full;
                if (find_executable_in_path(target, full)) {
                    std::cout << target << " is " << full << std::endl;
                } else {
                    std::cerr << target << ": not found" << std::endl;
                }
            }
        }
        _exit(0);
    }

    _exit(0);
}

// ======================= Run external command (single, no pipe) =======================

void run_single_external(const std::vector<std::string> &parts) {
    std::vector<char*> argv;
    for (auto &s : parts) {
        argv.push_back(strdup(s.c_str()));
    }
    argv.push_back(nullptr);

    char *cmd = argv[0];
    bool executed = false;
    char *pathEnv = std::getenv("PATH");

    if (pathEnv) {
        std::string path(pathEnv);
        size_t start = 0;

        while (true) {
            size_t end = path.find(':', start);
            std::string dir = (end == std::string::npos)
                                ? path.substr(start)
                                : path.substr(start, end - start);

            if (!dir.empty()) {
                std::string full = dir + "/" + cmd;
                if (access(full.c_str(), X_OK) == 0) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        execv(full.c_str(), argv.data());
                        std::exit(1);
                    } else {
                        waitpid(pid, nullptr, 0);
                    }
                    executed = true;
                    break;
                }
            }

            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

    if (!executed) {
        std::cerr << cmd << ": command not found" << std::endl;
    }

    for (char *p : argv) {
        if (p) free(p);
    }
}

// ======================= Run pipeline: left | right (builtins + externals) =======================

void run_pipeline(const std::vector<std::string> &leftParts,
                  const std::vector<std::string> &rightParts) {
    if (leftParts.empty() || rightParts.empty()) {
        return;
    }

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        std::perror("pipe");
        return;
    }

    // Build argv for left (if external)
    std::vector<char*> argvLeft;
    for (auto &s : leftParts) argvLeft.push_back(strdup(s.c_str()));
    argvLeft.push_back(nullptr);

    // Build argv for right (if external)
    std::vector<char*> argvRight;
    for (auto &s : rightParts) argvRight.push_back(strdup(s.c_str()));
    argvRight.push_back(nullptr);

    // LEFT child
    pid_t pid1 = fork();
    if (pid1 == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        if (is_builtin(leftParts[0])) {
            run_builtin_child(leftParts);
        } else {
            std::string fullPath;
            if (!find_executable_in_path(leftParts[0], fullPath)) {
                std::cerr << leftParts[0] << ": command not found" << std::endl;
                _exit(1);
            }
            execv(fullPath.c_str(), argvLeft.data());
            _exit(1);
        }
    }

    // RIGHT child
    pid_t pid2 = fork();
    if (pid2 == 0) {
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        if (is_builtin(rightParts[0])) {
            run_builtin_child(rightParts);
        } else {
            std::string fullPath;
            if (!find_executable_in_path(rightParts[0], fullPath)) {
                std::cerr << rightParts[0] << ": command not found" << std::endl;
                _exit(1);
            }
            execv(fullPath.c_str(), argvRight.data());
            _exit(1);
        }
    }

    // Parent
    close(pipefd[0]);
    close(pipefd[1]);

    waitpid(pid1, nullptr, 0);
    waitpid(pid2, nullptr, 0);

    for (char *p : argvLeft) if (p) free(p);
    for (char *p : argvRight) if (p) free(p);
}

// ======================= MAIN SHELL =======================

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    rl_bind_key('\t', tab_handler);

    while (true) {
        char *line = readline("$ ");
        if (!line) break;

        std::string input(line);
        free(line);

        if (!input.empty()) {
            add_history(input.c_str());
        }

        std::vector<std::string> parts = tokenize(input);
        if (parts.empty()) continue;

        // ======================= REDIRECTION PARSING =======================
        std::string redirectOutFile;
        std::string redirectErrFile;
        bool appendOut = false;
        bool appendErr = false;

        for (size_t i = 0; i < parts.size();) {
            const std::string &tok = parts[i];

            if ((tok == ">" || tok == "1>") && i + 1 < parts.size()) {
                redirectOutFile = parts[i + 1];
                appendOut = false;
                parts.erase(parts.begin() + i, parts.begin() + i + 2);
                continue;
            } else if ((tok == ">>" || tok == "1>>") && i + 1 < parts.size()) {
                redirectOutFile = parts[i + 1];
                appendOut = true;
                parts.erase(parts.begin() + i, parts.begin() + i + 2);
                continue;
            } else if (tok == "2>" && i + 1 < parts.size()) {
                redirectErrFile = parts[i + 1];
                appendErr = false;
                parts.erase(parts.begin() + i, parts.begin() + i + 2);
                continue;
            } else if (tok == "2>>" && i + 1 < parts.size()) {
                redirectErrFile = parts[i + 1];
                appendErr = true;
                parts.erase(parts.begin() + i, parts.begin() + i + 2);
                continue;
            }

            ++i;
        }

        if (parts.empty()) continue;

        int savedStdout = -1;
        int savedStderr = -1;

        if (!redirectOutFile.empty()) {
            savedStdout = dup(STDOUT_FILENO);
            int flags = O_CREAT | O_WRONLY;
            if (appendOut)
                flags |= O_APPEND;
            else
                flags |= O_TRUNC;

            int fd = open(redirectOutFile.c_str(), flags, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
        }

        if (!redirectErrFile.empty()) {
            savedStderr = dup(STDERR_FILENO);
            int flags = O_CREAT | O_WRONLY;
            if (appendErr)
                flags |= O_APPEND;
            else
                flags |= O_TRUNC;

            int fd = open(redirectErrFile.c_str(), flags, 0644);
            if (fd >= 0) {
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }

        // ======================= PIPELINE DETECTION (before builtins) =======================

        size_t pipePos = parts.size();
        for (size_t i = 0; i < parts.size(); ++i) {
            if (parts[i] == "|") {
                pipePos = i;
                break;
            }
        }

        if (pipePos != parts.size()) {
            std::vector<std::string> leftParts(parts.begin(), parts.begin() + pipePos);
            std::vector<std::string> rightParts(parts.begin() + pipePos + 1, parts.end());

            if (!leftParts.empty() && !rightParts.empty()) {
                run_pipeline(leftParts, rightParts);
            }

            if (savedStdout != -1) {
                dup2(savedStdout, STDOUT_FILENO);
                close(savedStdout);
            }
            if (savedStderr != -1) {
                dup2(savedStderr, STDERR_FILENO);
                close(savedStderr);
            }
            continue;
        }

        // ======================= BUILTINS (no pipeline) =======================

        if (parts.size() == 1 && parts[0] == "exit") {
            if (savedStdout != -1) {
                dup2(savedStdout, STDOUT_FILENO);
                close(savedStdout);
            }
            if (savedStderr != -1) {
                dup2(savedStderr, STDERR_FILENO);
                close(savedStderr);
            }
            break;
        }

        if (parts.size() == 1 && parts[0] == "pwd") {
            char buf[4096];
            if (getcwd(buf, sizeof(buf))) {
                std::cout << buf << std::endl;
            }
            if (savedStdout != -1) {
                dup2(savedStdout, STDOUT_FILENO);
                close(savedStdout);
            }
            if (savedStderr != -1) {
                dup2(savedStderr, STDERR_FILENO);
                close(savedStderr);
            }
            continue;
        }

        if (parts[0] == "cd") {
            if (parts.size() > 1) {
                std::string path = parts[1];
                if (path == "~") {
                    const char *home = std::getenv("HOME");
                    if (home) {
                        if (chdir(home) != 0) {
                            std::cerr << "cd: " << home << ": No such file or directory" << std::endl;
                        }
                    } else {
                        std::cerr << "cd: HOME not set" << std::endl;
                    }
                } else {
                    if (chdir(path.c_str()) != 0) {
                        std::cerr << "cd: " << path << ": No such file or directory" << std::endl;
                    }
                }
            }
            if (savedStdout != -1) {
                dup2(savedStdout, STDOUT_FILENO);
                close(savedStdout);
            }
            if (savedStderr != -1) {
                dup2(savedStderr, STDERR_FILENO);
                close(savedStderr);
            }
            continue;
        }

        if (parts[0] == "echo") {
            for (size_t i = 1; i < parts.size(); i++) {
                std::cout << parts[i];
                if (i + 1 < parts.size()) std::cout << " ";
            }
            std::cout << std::endl;
            if (savedStdout != -1) {
                dup2(savedStdout, STDOUT_FILENO);
                close(savedStdout);
            }
            if (savedStderr != -1) {
                dup2(savedStderr, STDERR_FILENO);
                close(savedStderr);
            }
            continue;
        }

        if (parts[0] == "type") {
            if (parts.size() > 1) {
                std::string target = parts[1];

                if (is_builtin(target)) {
                    std::cout << target << " is a shell builtin" << std::endl;
                } else {
                    std::string full;
                    if (find_executable_in_path(target, full)) {
                        std::cout << target << " is " << full << std::endl;
                    } else {
                        std::cerr << target << ": not found" << std::endl;
                    }
                }
            }
            if (savedStdout != -1) {
                dup2(savedStdout, STDOUT_FILENO);
                close(savedStdout);
            }
            if (savedStderr != -1) {
                dup2(savedStderr, STDERR_FILENO);
                close(savedStderr);
            }
            continue;
        }

        // ======================= EXTERNAL EXECUTION (no pipe, no builtin) =======================

        run_single_external(parts);

        if (savedStdout != -1) {
            dup2(savedStdout, STDOUT_FILENO);
            close(savedStdout);
        }
        if (savedStderr != -1) {
            dup2(savedStderr, STDERR_FILENO);
            close(savedStderr);
        }
    }

    return 0;
}





















