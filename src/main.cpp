#include <iostream>
#include <string>
#include <cstdlib>
#include <filesystem>

// Cross-platform compatibility for access() and X_OK
#ifdef _WIN32
    #include <io.h>
    #define access _access
    #define X_OK 4
#else
    #include <unistd.h>
#endif

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    while (true) {
        std::cout << "$ ";

        std::string input;
        if (!std::getline(std::cin, input)) {
            break;
        }

        // `exit` builtin
        if (input == "exit") {
            break;
        }

        // `echo` builtin
        if (input.rfind("echo ", 0) == 0) {
            std::string text = input.substr(5);
            std::cout << text << std::endl;
            continue;
        }

        // `type` builtin
        if (input.rfind("type ", 0) == 0) {
            std::string target = input.substr(5);

            // Check if target is a builtin
            if (target == "echo" || target == "exit" || target == "type") {
                std::cout << target << " is a shell builtin" << std::endl;
                continue;
            }

            // Search in PATH
            char* pathEnv = std::getenv("PATH");

            if (pathEnv != nullptr) {
                std::string path(pathEnv);
                size_t start = 0;
                bool found = false;

                while (true) {
                    size_t end = path.find(
#ifdef _WIN32
                        ';'
#else
                        ':'
#endif
                        , start);

                    std::string dir = (end == std::string::npos)
                        ? path.substr(start)
                        : path.substr(start, end - start);

                    if (!dir.empty()) {
                        // Build full path
                        std::string fullPath =
#ifdef _WIN32
                            dir + "\\" + target;
#else
                            dir + "/" + target;
#endif

                        // Check executable permission
                        if (access(fullPath.c_str(), X_OK) == 0) {
                            std::cout << target << " is " << fullPath << std::endl;
                            found = true;
                            break;
                        }
                    }

                    if (end == std::string::npos) {
                        break;
                    }

                    start = end + 1;
                }

                if (!found) {
                    std::cout << target << ": not found" << std::endl;
                }

            } else {
                std::cout << target << ": not found" << std::endl;
            }

            continue;
        }

        // Unknown command fallback
        std::cout << input << ": command not found" << std::endl;
    }

    return 0;
}
