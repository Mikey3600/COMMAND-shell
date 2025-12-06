#include <iostream>
#include <string>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <cstring>
#include <cctype>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

// Tokenizer - supports quoting, escaping, concatenation
std::vector<std::string> tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string current;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    bool escape = false;

    for (size_t i = 0; i < input.size(); i++) {
        char c = input[i];

        if (escape && !inDoubleQuote) {
            current.push_back(c);
            escape = false;
            continue;
        }

        if (inDoubleQuote) {
            if (c == '\\') {
                if (i + 1 < input.size()) {
                    char next = input[i + 1];
                    if (next == '"' || next == '\\') {
                        current.push_back(next);
                        i++;
                        continue;
                    }
                }
                current.push_back('\\');
                continue;
            }
        }

        if (c == '\\' && !inSingleQuote && !inDoubleQuote) {
            escape = true;
            continue;
        }

        if (c == '\'' && !inDoubleQuote) {
            inSingleQuote = !inSingleQuote;
            continue;
        }

        if (c == '"' && !inSingleQuote) {
            inDoubleQuote = !inDoubleQuote;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c)) &&
            !inSingleQuote && !inDoubleQuote) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }

        current.push_back(c);
    }

    if (escape) current.push_back('\\');
    if (!current.empty()) tokens.push_back(current);

    return tokens;
}

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    while (true) {
        std::cout << "$ ";

        std::string input;
        if (!std::getline(std::cin, input)) break;

        std::vector<std::string> parts = tokenize(input);
        if (parts.empty()) continue;

        // ======================================================================
        // REDIRECTION DETECTION
        // ======================================================================

        std::string redirectOutFile;
        bool appendOut = false;

        // Detect > 1> >> 1>>
        for (size_t i = 0; i < parts.size(); i++) {
            if (parts[i] == ">" || parts[i] == "1>") {
                if (i + 1 < parts.size()) {
                    redirectOutFile = parts[i + 1];
                    appendOut = false;
                    parts.erase(parts.begin() + i, parts.begin() + i + 2);
                }
                break;
            }
            if (parts[i] == ">>" || parts[i] == "1>>") {
                if (i + 1 < parts.size()) {
                    redirectOutFile = parts[i + 1];
                    appendOut = true;
                    parts.erase(parts.begin() + i, parts.begin() + i + 2);
                }
                break;
            }
        }

        std::string redirectErrFile;
        bool appendErr = false;

        // Detect 2> and 2>>
        for (size_t i = 0; i < parts.size(); i++) {
            if (parts[i] == "2>") {
                if (i + 1 < parts.size()) {
                    redirectErrFile = parts[i + 1];
                    appendErr = false;
                    parts.erase(parts.begin() + i, parts.begin() + i + 2);
                }
                break;
            }
            if (parts[i] == "2>>") {
                if (i + 1 < parts.size()) {
                    redirectErrFile = parts[i + 1];
                    appendErr = true;
                    parts.erase(parts.begin() + i, parts.begin() + i + 2);
                }
                break;
            }
        }

        // ======================================================================
        // APPLY REDIRECTIONS
        // ======================================================================

        int savedStdout = -1;
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

        int savedStderr = -1;
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

        // ======================================================================
        // BUILTINS
        // ======================================================================

        if (parts.size() == 1 && parts[0] == "exit") {
            goto restore_and_exit;
        }

        if (parts.size() == 1 && parts[0] == "pwd") {
            char buffer[4096];
            if (getcwd(buffer, sizeof(buffer)) != nullptr) {
                std::cout << buffer << std::endl;
            }
            goto restore_std;
        }

        if (parts[0] == "cd") {
            if (parts.size() > 1) {
                std::string path = parts[1];
                if (path == "~") {
                    const char* home = getenv("HOME");
                    if (home != nullptr) {
                        if (chdir(home) != 0)
                            std::cerr << "cd: " << home << ": No such file or directory" << std::endl;
                    }
                } else {
                    if (chdir(path.c_str()) != 0) {
                        std::cerr << "cd: " << path << ": No such file or directory" << std::endl;
                    }
                }
            }
            goto restore_std;
        }

        if (parts[0] == "echo") {
            for (size_t i = 1; i < parts.size(); i++) {
                std::cout << parts[i];
                if (i + 1 < parts.size()) std::cout << " ";
            }
            std::cout << std::endl;
            goto restore_std;
        }

        if (parts[0] == "type") {
            if (parts.size() > 1) {
                std::string target = parts[1];
                if (target == "echo" || target == "exit"
                    || target == "type" || target == "pwd"
                    || target == "cd") {
                    std::cout << target << " is a shell builtin" << std::endl;
                } else {
                    char* pathEnv = getenv("PATH");
                    bool found = false;

                    if (pathEnv != nullptr) {
                        std::string path(pathEnv);
                        size_t start = 0;

                        while (true) {
                            size_t end = path.find(':', start);
                            std::string dir =
                                (end == std::string::npos)
                                ? path.substr(start)
                                : path.substr(start, end - start);

                            if (!dir.empty()) {
                                std::string fullPath = dir + "/" + target;
                                if (access(fullPath.c_str(), X_OK) == 0) {
                                    std::cout << target << " is " << fullPath << std::endl;
                                    found = true;
                                    break;
                                }
                            }
                            if (end == std::string::npos) break;
                            start = end + 1;
                        }
                    }

                    if (!found) {
                        std::cerr << target << ": not found" << std::endl;
                    }
                }
            }
            goto restore_std;
        }

        // ======================================================================
        // EXTERNAL EXECUTION
        // ======================================================================

        {
            std::vector<char*> args;
            for (auto &s: parts) args.push_back(strdup(s.c_str()));
            args.push_back(nullptr);

            char* cmd = args[0];
            bool executed = false;
            char* pathEnv = getenv("PATH");

            if (pathEnv != nullptr) {
                std::string path(pathEnv);
                size_t start = 0;

                while (true) {
                    size_t end = path.find(':', start);
                    std::string dir =
                        (end == std::string::npos)
                        ? path.substr(start)
                        : path.substr(start, end - start);

                    if (!dir.empty()) {
                        std::string fullPath = dir + "/" + cmd;

                        if (access(fullPath.c_str(), X_OK) == 0) {
                            pid_t pid = fork();

                            if (pid == 0) {
                                execv(fullPath.c_str(), args.data());
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

            if (!executed) {
                std::cerr << cmd << ": command not found" << std::endl;
            }

            for (char* ptr: args) if (ptr) free(ptr);
        }

restore_std:

        if (!redirectOutFile.empty()) {
            dup2(savedStdout, STDOUT_FILENO);
            close(savedStdout);
        }

        if (!redirectErrFile.empty()) {
            dup2(savedStderr, STDERR_FILENO);
            close(savedStderr);
        }

        continue;

restore_and_exit:

        if (!redirectOutFile.empty()) {
            dup2(savedStdout, STDOUT_FILENO);
            close(savedStdout);
        }

        if (!redirectErrFile.empty()) {
            dup2(savedStderr, STDERR_FILENO);
            close(savedStderr);
        }

        break;
    }

    return 0;
}











