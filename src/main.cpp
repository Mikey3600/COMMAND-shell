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
    "echo", "exit", "pwd", "cd", "type", "history"
};

bool is_builtin(const std::string &cmd) {
    for (const auto &b : builtin_list) {
        if (b == cmd) return true;
    }
    return false;
}

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

// ======================= LCP (Longest Common Prefix) =======================

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

// ======================= PATH helper for type & pipeline =======================

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

// ======================= TAB completion state + handler =======================

static std::string last_prefix;
static int tab_count = 0;

int tab_handler(int count, int key) {
    std::string prefix = rl_line_buffer;

    // Builtin matches
    std::vector<std::string> builtin_matches;
    for (auto &b : builtin_list) {
        if (b.rfind(prefix, 0) == 0) {
            builtin_matches.push_back(b);
        }
    }

    // Single builtin match → immediate complete + space
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

    // PATH matches
    std::vector<std::string> path_matches = find_path_matches(prefix);
    std::sort(path_matches.begin(), path_matches.end());

    // Multiple builtin matches: LCP or list
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

    // Multiple PATH matches → LCP
    std::string lcp = longest_common_prefix(path_matches);
    if (!lcp.empty() && lcp.size() > prefix.size()) {
        rl_replace_line(lcp.c_str(), 1);
        rl_point = lcp.size();
        rl_redisplay();
        last_prefix = lcp;
        tab_count = 0;
        return 0;
    }

    // LCP == prefix: first TAB bell, second TAB list
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

/**
 * @brief Handles the logic for the history builtin, including listing, reading (-r), writing (-w), and appending (-a).
 */
void run_history_builtin(const std::vector<std::string> &parts) {
    // Check for arguments and perform history file operations first
    if (parts.size() == 3) {
        const std::string& option = parts[1];
        const std::string& path = parts[2];

        if (option == "-r") {
            // read_history_range appends history from the file to memory.
            if (read_history_range(path.c_str(), -1, -1) != 0) {
                std::cerr << "history: cannot read " << path << std::endl;
            }
            return;
        }
        
        if (option == "-w") {
            // write_history writes all in-memory history to the file, truncating/creating it.
            if (write_history(path.c_str()) != 0) {
                std::cerr << "history: cannot write " << path << std::endl;
            }
            return;
        }

        if (option == "-a") {
            // append_history appends new history entries (since last I/O operation) to the file.
            // history_length tells readline to append only entries since the last I/O.
            if (append_history(history_length, path.c_str()) != 0) {
                std::cerr << "history: cannot append to " << path << std::endl;
            }
            return;
        }
    }
    
    // Fall through to listing history (history or history <n>)

    int limit = 0;
    if (parts.size() == 2) {
        try {
            // Attempt to parse a numeric limit argument
            limit = std::stoi(parts[1]);
        } catch (const std::exception& e) {
            // Invalid number format
            std::cerr << "history: invalid option or numeric argument required" << std::endl;
            return;
        }
    } else if (parts.size() > 3 || (parts.size() == 2 && (parts[1] == "-r" || parts[1] == "-w" || parts[1] == "-a"))) {
        // Catch invalid argument counts for list command
         std::cerr << "history: invalid usage" << std::endl;
         return;
    }


    HISTORY_STATE *state = history_get_history_state();
    if (!state) return;

    HIST_ENTRY **history = history_list();
    if (!history) return;

    int history_count = 0;
    for (HIST_ENTRY **h = history; *h; h++) {
        history_count++;
    }

    int display_count = history_count;
    if (limit > 0 && limit < history_count) {
        display_count = limit;
    }

    int start_index = history_count - display_count;
    int line_number = history_base + start_index;

    for (int i = start_index; i < history_count; i++) {
        std::cout << "    " << line_number << "  " << history[i]->line << "\n";
        line_number++;
    }
}


// ======================= Builtins in child (for pipelines) =======================

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
        _exit(0);
    }
    
    // History handling for pipeline/child processes
    if (cmd == "history") {
        // Only run the listing part if piped. File operations are parent-shell logic.
        if (parts.size() == 1 || (parts.size() == 2 && parts[1] != "-r" && parts[1] != "-w" && parts[1] != "-a")) {
            run_history_builtin(parts); 
        }
        _exit(0); 
    }

    _exit(0);
}

// ======================= Single external (no pipeline) =======================

void run_single_external(const std::vector<std::string> &parts) {
    std::vector<char*> argv;
    for (auto &s : parts) argv.push_back(strdup(s.c_str()));
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

    // Clean up allocated strings
    for (char *p : argv) if (p) free(p);
}

// ======================= Multi-stage pipeline: cmd1 | cmd2 | ... | cmdN =======================

void run_pipeline_multi(const std::vector<std::vector<std::string>> &commands) {
    size_t n = commands.size();
    if (n == 0) return;
    
    // Special handling for a single command in the pipeline logic (just run as child)
    if (n == 1) {
        std::vector<char*> argv;
        for (auto &s : commands[0]) argv.push_back(strdup(s.c_str()));
        argv.push_back(nullptr);

        pid_t pid = fork();
        if (pid == 0) {
            if (is_builtin(commands[0][0])) {
                run_builtin_child(commands[0]);
            } else {
                std::string fullPath;
                if (!find_executable_in_path(commands[0][0], fullPath)) {
                    std::cerr << commands[0][0] << ": command not found" << std::endl;
                    _exit(1);
                }
                execv(fullPath.c_str(), argv.data());
                _exit(1);
            }
        } else {
            waitpid(pid, nullptr, 0);
        }

        for (char *p : argv) if (p) free(p);
        return;
    }

    // Create pipes: one between each pair of commands (2*(N-1) file descriptors)
    std::vector<int> pipes(2 * (n - 1));
    for (size_t i = 0; i < n - 1; ++i) {
        if (pipe(&pipes[2 * i]) == -1) {
            perror("pipe");
            return;
        }
    }

    std::vector<pid_t> pids(n);

    for (size_t i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            // Child i
            
            // Connect stdin if not the first command
            if (i > 0) {
                int in_fd = pipes[2 * (i - 1)];
                dup2(in_fd, STDIN_FILENO);
            }
            // Connect stdout if not the last command
            if (i < n - 1) {
                int out_fd = pipes[2 * i + 1];
                dup2(out_fd, STDOUT_FILENO);
            }

            // Close all pipe fds in the child
            for (size_t k = 0; k < 2 * (n - 1); ++k) {
                close(pipes[k]);
            }

            // Run command
            const auto &parts = commands[i];

            if (is_builtin(parts[0])) {
                run_builtin_child(parts);
            } else {
                std::vector<char*> argv;
                for (auto &s : parts) argv.push_back(strdup(s.c_str()));
                argv.push_back(nullptr);

                std::string fullPath;
                if (!find_executable_in_path(parts[0], fullPath)) {
                    std::cerr << parts[0] << ": command not found" << std::endl;
                    for (char *p : argv) if (p) free(p);
                    _exit(1);
                }
                execv(fullPath.c_str(), argv.data());
                for (char *p : argv) if (p) free(p);
                _exit(1);
            }
        } else if (pid > 0) {
            pids[i] = pid;
        } else {
            perror("fork");
            return;
        }
    }

    // Parent: Close all pipe ends
    for (size_t k = 0; k < 2 * (n - 1); ++k) {
        close(pipes[k]);
    }

    // Wait for all children
    for (size_t i = 0; i < n; ++i) {
        if (pids[i] > 0) {
            waitpid(pids[i], nullptr, 0);
        }
    }
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

        // Add to history BEFORE parsing/execution, ensuring history is complete
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

        // Extract redirection arguments and remove them from the 'parts' vector
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

        // Apply redirection
        if (!redirectOutFile.empty()) {
            savedStdout = dup(STDOUT_FILENO);
            int flags = O_CREAT | O_WRONLY;
            flags |= appendOut ? O_APPEND : O_TRUNC;

            int fd = open(redirectOutFile.c_str(), flags, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
        }

        if (!redirectErrFile.empty()) {
            savedStderr = dup(STDERR_FILENO);
            int flags = O_CREAT | O_WRONLY;
            flags |= appendErr ? O_APPEND : O_TRUNC;

            int fd = open(redirectErrFile.c_str(), flags, 0644);
            if (fd >= 0) {
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }

        // ======================= PIPELINE DETECTION =======================

        bool hasPipe = false;
        for (auto &t : parts) {
            if (t == "|") {
                hasPipe = true;
                break;
            }
        }

        if (hasPipe) {
            std::vector<std::vector<std::string>> commands;
            std::vector<std::string> current;

            for (auto &t : parts) {
                if (t == "|") {
                    if (!current.empty()) {
                        commands.push_back(current);
                        current.clear();
                    }
                } else {
                    current.push_back(t);
                }
            }
            if (!current.empty()) {
                commands.push_back(current);
            }

            if (!commands.empty()) {
                run_pipeline_multi(commands);
            }
        }

        // ======================= BUILTINS (no pipeline) =======================
        else if (is_builtin(parts[0])) {
            const std::string& cmd = parts[0];

            if (cmd == "exit") {
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

            if (cmd == "pwd") {
                char buf[4096];
                if (getcwd(buf, sizeof(buf))) {
                    std::cout << buf << std::endl;
                }
            }

            if (cmd == "cd") {
                if (parts.size() > 1) {
                    std::string path = parts[1];
                    if (path == "~") {
                        const char *home = std::getenv("HOME");
                        path = home ? home : "";
                    }

                    if (!path.empty()) {
                        if (chdir(path.c_str()) != 0) {
                            std::cerr << "cd: " << parts[1] << ": No such file or directory" << std::endl;
                        }
                    } else if (path == "~" && !std::getenv("HOME")) {
                        std::cerr << "cd: HOME not set" << std::endl;
                    }
                }
            }

            if (cmd == "echo") {
                for (size_t i = 1; i < parts.size(); i++) {
                    std::cout << parts[i];
                    if (i + 1 < parts.size()) std::cout << " ";
                }
                std::cout << std::endl;
            }
            
            if (cmd == "history") {
                run_history_builtin(parts);
            }

            if (cmd == "type") {
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
            }
        }

        // ======================= EXTERNAL (no pipe) =======================
        else {
            run_single_external(parts);
        }

        // ======================= RESTORE STDOUT/STDERR =======================

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




















