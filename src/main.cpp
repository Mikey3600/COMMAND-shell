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

// ======================= Tokenizer (handles quotes + backslashes) =======================

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

// ======================= TAB completion state =======================

static std::string last_prefix;
static int tab_count = 0;

// ======================= TAB handler =======================

int tab_handler(int count, int key) {
    std::string prefix = rl_line_buffer;

    // 1) Builtin completions first
    std::vector<std::string> builtin_matches;
    for (auto &b : builtin_list) {
        if (b.rfind(prefix, 0) == 0) {
            builtin_matches.push_back(b);
        }
    }

    // Single builtin match -> complete immediately
    if (builtin_matches.size() == 1) {
        rl_replace_line(builtin_matches[0].c_str(), 1);
        rl_point = builtin_matches[0].size();
        rl_insert_text((char*)" ");
        rl_redisplay();
        last_prefix.clear();
        tab_count = 0;
        return 0;
    }

    // 2) PATH matches
    std::vector<std::string> path_matches = find_path_matches(prefix);
    std::sort(path_matches.begin(), path_matches.end());

    // If multiple builtins, use multi-match behavior on builtins
    if (builtin_matches.size() > 1) {
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

    // Now handle PATH completions
    if (!path_matches.empty()) {
        if (path_matches.size() == 1) {
            rl_replace_line(path_matches[0].c_str(), 1);
            rl_point = path_matches[0].size();
            rl_insert_text((char*)" ");
            rl_redisplay();
            last_prefix.clear();
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

    // No matches at all -> bell
    write(STDOUT_FILENO, "\a", 1);
    return 0;
}

// ======================= type builtin helper =======================

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

        // Apply stdout redirection
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

        // Apply stderr redirection
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

        // ======================= BUILTINS =======================

        // exit
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

        // pwd
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

        // cd
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

        // echo
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

        // type
        if (parts[0] == "type") {
            if (parts.size() > 1) {
                std::string target = parts[1];

                bool isBuiltin = false;
                for (auto &b : builtin_list) {
                    if (b == target) {
                        isBuiltin = true;
                        break;
                    }
                }

                if (isBuiltin) {
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

        // ======================= EXTERNAL EXECUTION =======================

        {
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

        // ======================= RESTORE FDs =======================

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



















