#include <iostream>
#include <string>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <unistd.h>     // fork(), execv(), access(), X_OK
#include <sys/wait.h>   // waitpid()

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

        // echo builtin
        if (input.rfind("echo ", 0) == 0) {
            std::string text = input.substr(5);
            std::cout << text << std::endl;
            continue;
        }

        // type builtin
        if (input.rfind("type ", 0) == 0) {
            std::string target = input.substr(5);

            // Builtins
            if (target == "echo" || target == "exit" || target == "type") {
                std::cout << target << " is a shell builtin" << std::endl;
                continue;
            }

            char* pathEnv = std::getenv("PATH");
            bool found = false;

            if (pathEnv != nullptr) {
                std::string path(pathEnv);
                size_t start = 0;

                while (true) {
                    size_t end = path.find(':', start);
                    std::string dir = (end == std::string::npos)
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

        // ---------- external command execution ----------
        std::istringstream iss(input);
        std::vector<char*> args;
        std::string token;

        while (iss >> token) {
            args.push_back(strdup(token.c_str()));
        }
        args.push_back(nullptr);

        char* cmd = args[0];
        char* pathEnv = std::getenv("PATH");
        bool executed = false;

        if (pathEnv != nullptr) {
            std::string path(pathEnv);
            size_t start = 0;

            while (true) {
                size_t end = path.find(':', start);
                std::string dir = (end == std::string::npos)
                    ? path.substr(start)
                    : path.substr(start, end - start);

                if (!dir.empty()) {
                    std::string fullPath = dir + "/" + cmd;

                    if (access(fullPath.c_str(), X_OK) == 0) {

                        pid_t pid = fork();

                        if (pid == 0) { // child
                            execv(fullPath.c_str(), args.data());
                            exit(1);
                        } else { // parent
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

