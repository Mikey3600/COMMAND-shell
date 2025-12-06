#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <windows.h>   // Windows process API
#include <io.h>        // access() equivalent

#define access _access
#define X_OK 4

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    while (true) {
        std::cout << "$ ";

        std::string input;
        if (!std::getline(std::cin, input)) break;

        // exit builtin
        if (input == "exit") break;

        // echo builtin
        if (input.rfind("echo ", 0) == 0) {
            std::string text = input.substr(5);
            std::cout << text << std::endl;
            continue;
        }

        // type builtin
        if (input.rfind("type ", 0) == 0) {
            std::string target = input.substr(5);

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
                    size_t end = path.find(';', start);

                    std::string dir = (end == std::string::npos)
                        ? path.substr(start)
                        : path.substr(start, end - start);

                    if (!dir.empty()) {
                        std::string fullPath = dir + "\\" + target + ".exe";

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

        // External execution — Windows version
        // (Works for EXE files; not Linux binaries)
        std::istringstream iss(input);
        std::vector<std::string> parts;
        std::string token;
        while (iss >> token) parts.push_back(token);

        if (parts.empty()) continue;

        std::string cmd = parts[0];
        char* pathEnv = std::getenv("PATH");
        bool executed = false;

        if (pathEnv != nullptr) {
            std::string path(pathEnv);
            size_t start = 0;

            while (true) {
                size_t end = path.find(';', start);
                std::string dir = (end == std::string::npos)
                    ? path.substr(start)
                    : path.substr(start, end - start);

                if (!dir.empty()) {
                    std::string fullPath = dir + "\\" + cmd + ".exe";

                    if (access(fullPath.c_str(), X_OK) == 0) {
                        std::string commandLine = fullPath;
                        for (size_t i = 1; i < parts.size(); i++) {
                            commandLine += " " + parts[i];
                        }

                        system(commandLine.c_str());
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
    }

    return 0;
}

