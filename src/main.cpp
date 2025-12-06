#include <iostream>
#include <string>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <cstring>
#include <cctype>
#include <unistd.h>
#include <sys/wait.h>

// Tokenizer supporting:
// - single quotes (all literal)
// - double quotes (\" and \\ escape, others literal)
// - backslash escaping outside quotes
std::vector<std::string> tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string current;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    bool escape = false;

    for (size_t i = 0; i < input.size(); i++) {
        char c = input[i];

        // Handle escape outside quotes
        if (escape && !inDoubleQuote) {
            current.push_back(c);
            escape = false;
            continue;
        }

        // Handle escape inside double quotes
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

        // Start escape (only outside quotes)
        if (c == '\\' && !inSingleQuote && !inDoubleQuote) {
            escape = true;
            continue;
        }

        // Toggle single quote mode
        if (c == '\'' && !inDoubleQuote) {
            inSingleQuote = !inSingleQuote;
            continue;
        }

        // Toggle double quote mode
        if (c == '"' && !inSingleQuote) {
            inDoubleQuote = !inDoubleQuote;
            continue;
        }

        // Token split on whitespace outside quotes
        if (std::isspace(static_cast<unsigned char>(c)) && !inSingleQuote && !inDoubleQuote) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }

        current.push_back(c);
    }

    if (escape) {
        current.push_back('\\');
    }

    if (!current.empty()) {
        tokens.push_back(current);
    }

    return tokens;
}

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    while (true) {
        std::cout << "$ ";

        std::string input;
        if (!std::getline(std::cin, input)) {
            break;
        }

        // exit builtin
        if (input == "exit") {
            break;
        }

        // pwd builtin
        if (input == "pwd") {
            char buffer[4096];
            if (getcwd(buffer, sizeof(buffer)) != nullptr) {
                std::cout << buffer << std::endl;
            }
            continue;
        }

        // cd builtin
        if (input.rfind("cd ", 0) == 0) {
            std::string path = input.substr(3);

            if (path == "~") {
                const char* home = getenv("HOME");
                if (home != nullptr) {
                    if (chdir(home) != 0) {
                        std::cout << "cd: " << home << ": No such file or directory" << std::endl;
                    }
                } else {
                    std::cout << "cd: HOME not set" << std::endl;
                }
                continue;
            }

            if (!path.empty()) {
                if (chdir(path.c_str()) != 0) {
                    std::cout << "cd: " << path << ": No such file or directory" << std::endl;
                }
            }
            continue;
        }

        // echo builtin
        if (input.rfind("echo ", 0) == 0) {
            std::vector<std::string> parts = tokenize(input.substr(5));

            for (size_t i = 0; i < parts.size(); i++) {
                std::cout << parts[i];
                if (i + 1 < parts.size()) std::cout << " ";
            }
            std::cout << std::endl;
            continue;
        }

        // type builtin
        if (input.rfind("type ", 0) == 0) {
            std::string target = input.substr(5);

            if (target == "echo" || target == "exit" ||
                target == "type" || target == "pwd" ||
                target == "cd") {
                std::cout << target << " is a shell builtin" << std::endl;
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

            continue;
        }

        // external execution
        std::vector<std::string> parts = tokenize(input);
        if (parts.empty()) continue;

        // -------- KEY CHANGE --------
        // Because tokenizer already stripped quotes,
        // quoted executable names simply work here.
        std::vector<char*> args;
        for (auto& s : parts) {
            args.push_back(strdup(s.c_str()));
        }
        args.push_back(nullptr);

        char* cmd = args[0];
        char* pathEnv = getenv("PATH");
        bool executed = false;

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
                            exit(1);  // execv failed
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

        for (char* ptr : args) {
            if (ptr) free(ptr);
        }
    }

    return 0;
}







