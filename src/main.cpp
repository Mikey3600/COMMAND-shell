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

// Tokenizer supporting quotes and escapes
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

        // Tokenize once for builtin + redirection
        std::vector<std::string> parts = tokenize(input);
        if (parts.empty()) continue;

        // Detect redirection before executing
        std::string redirectFile;
        for (size_t i = 0; i < parts.size(); i++) {
            if (parts[i] == ">" || parts[i] == "1>") {
                if (i + 1 < parts.size()) {
                    redirectFile = parts[i + 1];
                    parts.erase(parts.begin() + i, parts.begin() + i + 2);
                }
                break;
            }
        }

        // Redirection: temporarily replace stdout FD
        int savedStdout = -1;
        if (!redirectFile.empty()) {
            savedStdout = dup(STDOUT_FILENO);

            int fd = open(redirectFile.c_str(),
                          O_CREAT | O_WRONLY | O_TRUNC,
                          0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
        }

        // ========== Builtins (now redirection-aware) ==========

        // exit
        if (parts.size() == 1 && parts[0] == "exit") {
            if (!redirectFile.empty()) {
                dup2(savedStdout, STDOUT_FILENO);
                close(savedStdout);
            }
            break;
        }

        // pwd
        if (parts.size() == 1 && parts[0] == "pwd") {
            char buffer[4096];
            if (getcwd(buffer, sizeof(buffer)) != nullptr) {
                std::cout << buffer << std::endl;
            }

            if (!redirectFile.empty()) {
                dup2(savedStdout, STDOUT_FILENO);
                close(savedStdout);
            }
            continue;
        }

        // cd
        if (parts[0] == "cd") {
            if (parts.size() > 1) {
                std::string path = parts[1];
                if (path == "~") {
                    const char* home = getenv("HOME");
                    if (home != nullptr) {
                        if (chdir(home) != 0)
                            std::cout << "cd: " << home << ": No such file or directory" << std::endl;
                    }
                } else {
                    if (chdir(path.c_str()) != 0) {
                        std::cout << "cd: " << path << ": No such file or directory" << std::endl;
                    }
                }
            }

            if (!redirectFile.empty()) {
                dup2(savedStdout, STDOUT_FILENO);
                close(savedStdout);
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

            if (!redirectFile.empty()) {
                dup2(savedStdout, STDOUT_FILENO);
                close(savedStdout);
            }
            continue;
        }

        // type
        if (parts[0] == "type") {
            if (parts.size() > 1) {
                std::string target = parts[1];
                if (target == "echo" || target == "exit" ||
                    target == "type" || target == "pwd" ||
                    target == "cd") {

                    std::cout << target << " is a shell builtin" << std::endl;

                    if (!redirectFile.empty()) {
                        dup2(savedStdout, STDOUT_FILENO);
                        close(savedStdout);
                    }
                    continue;
                }

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
                    std::cout << target << ": not found" << std::endl;
                }

                if (!redirectFile.empty()) {
                    dup2(savedStdout, STDOUT_FILENO);
                    close(savedStdout);
                }
                continue;
            }
        }

        // ========== External command execution ==========

        std::vector<char*> args;
        for (auto& s : parts) args.push_back(strdup(s.c_str()));
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
            std::cout << cmd << ": command not found" << std::endl;
        }

        if (!redirectFile.empty()) {
            dup2(savedStdout, STDOUT_FILENO);
            close(savedStdout);
        }

        for (char* ptr : args) if (ptr) free(ptr);
    }

    return 0;
}









