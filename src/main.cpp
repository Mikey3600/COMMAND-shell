#include <iostream>
#include <string>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>

#include <readline/readline.h>
#include <readline/history.h>

// ======================= Tokenizer =======================

std::vector<std::string> tokenize(const std::string &input) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inS = false, inD = false, esc = false;

    for (size_t i = 0; i < input.size(); i++) {
        char c = input[i];

        if (esc && !inD) {
            cur.push_back(c);
            esc = false;
            continue;
        }

        if (inD && c == '\\') {
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

        if (c == '\\' && !inS && !inD) {
            esc = true;
            continue;
        }

        if (c == '\'' && !inD) {
            inS = !inS;
            continue;
        }
        if (c == '"' && !inS) {
            inD = !inD;
            continue;
        }

        if (isspace((unsigned char)c) && !inS && !inD) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
            continue;
        }

        cur.push_back(c);
    }
    if (esc) cur.push_back('\\');
    if (!cur.empty()) tokens.push_back(cur);

    return tokens;
}

// ======================= PATH executable scanning =======================

std::vector<std::string> find_path_matches(const std::string &prefix) {
    std::vector<std::string> matches;

    char *pathEnv = getenv("PATH");
    if (!pathEnv) return matches;

    std::string path(pathEnv);
    size_t start = 0;

    while (true) {
        size_t end = path.find(':', start);
        std::string dir = (end == std::string::npos) ? path.substr(start) : path.substr(start, end - start);

        if (!dir.empty()) {
            DIR *dp = opendir(dir.c_str());
            if (dp) {
                struct dirent *e;
                while ((e = readdir(dp)) != nullptr) {
                    std::string name = e->d_name;
                    if (name.rfind(prefix, 0) == 0) {
                        std::string full = dir + "/" + name;
                        if (access(full.c_str(), X_OK) == 0) matches.push_back(name);
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

// ======================= Auto-completion state =======================

std::string last_prefix;
int tab_press_count = 0;

// Custom TAB handler
int tab_handler(int count, int key) {
    std::string prefix = rl_line_buffer;

    auto matches = find_path_matches(prefix);
    std::sort(matches.begin(), matches.end());

    // No match → bell only
    if (matches.empty()) {
        write(STDOUT_FILENO, "\a", 1);
        return 0;
    }

    // One match → immediate completion
    if (matches.size() == 1) {
        rl_replace_line(matches[0].c_str(), 1);
        rl_point = matches[0].size();
        rl_insert_text(" ");
        rl_redisplay();
        last_prefix.clear();
        tab_press_count = 0;
        return 0;
    }

    // Multi-match behavior:
    if (prefix != last_prefix) {
        last_prefix = prefix;
        tab_press_count = 0;
    }
    tab_press_count++;

    if (tab_press_count == 1) {
        write(STDOUT_FILENO, "\a", 1);
        return 0;
    }

    std::cout << "\n";
    for (size_t i = 0; i < matches.size(); i++) {
        std::cout << matches[i];
        if (i + 1 < matches.size()) std::cout << "  ";
    }
    std::cout << "\n$ " << prefix;
    fflush(stdout);

    rl_replace_line(prefix.c_str(), 1);
    rl_point = prefix.size();
    rl_redisplay();
    return 0;
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

        if (!input.empty()) add_history(input.c_str());

        auto parts = tokenize(input);
        if (parts.empty()) continue;

        // ======================= exit =======================
        if (parts.size() == 1 && parts[0] == "exit") break;

        // ======================= pwd =======================
        if (parts.size() == 1 && parts[0] == "pwd") {
            char buf[4096];
            if (getcwd(buf, sizeof(buf))) std::cout << buf << std::endl;
            continue;
        }

        // ======================= cd =======================
        if (parts[0] == "cd") {
            if (parts.size() > 1) {
                std::string path = parts[1];
                if (path == "~") {
                    const char *home = getenv("HOME");
                    if (home && chdir(home) != 0)
                        std::cerr << "cd: " << home << ": No such file or directory" << std::endl;
                } else if (chdir(path.c_str()) != 0) {
                    std::cerr << "cd: " << path << ": No such file or directory" << std::endl;
                }
            }
            continue;
        }

        // ======================= echo =======================
        if (parts[0] == "echo") {
            for (size_t i = 1; i < parts.size(); i++) {
                std::cout << parts[i];
                if (i + 1 < parts.size()) std::cout << " ";
            }
            std::cout << std::endl;
            continue;
        }

        // ======================= type builtin =======================
        if (parts[0] == "type") {
            if (parts.size() > 1) {
                std::string target = parts[1];
                if (target == "echo" || target == "exit" || target == "type"
                    || target == "pwd" || target == "cd") {
                    std::cout << target << " is a shell builtin\n";
                } else {
                    bool found = false;
                    auto all = find_path_matches(target);
                    for (auto &x : all) {
                        if (x == target) {
                            std::cout << target << " is /usr/bin/" << target << "\n";
                            found = true;
                        }
                    }
                    if (!found) std::cerr << target << ": not found\n";
                }
            }
            continue;
        }

        // ======================= external run =======================
        {
            std::vector<char *> args;
            for (auto &s : parts) args.push_back(strdup(s.c_str()));
            args.push_back(nullptr);

            char *cmd = args[0];
            bool executed = false;
            char *pathEnv = getenv("PATH");

            if (pathEnv) {
                std::string path(pathEnv);
                size_t start = 0;
                while (true) {
                    size_t end = path.find(':', start);
                    std::string dir = (end == std::string::npos) ? path.substr(start) : path.substr(start, end - start);

                    if (!dir.empty()) {
                        std::string full = dir + "/" + cmd;
                        if (access(full.c_str(), X_OK) == 0) {
                            pid_t pid = fork();
                            if (pid == 0) {
                                execv(full.c_str(), args.data());
                                exit(1);
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

            if (!executed) std::cerr << cmd << ": command not found\n";
            for (char *p : args) if (p) free(p);
        }
    }
    return 0;
}

















