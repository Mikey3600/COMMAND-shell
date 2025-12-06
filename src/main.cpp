#include <filesystem>
#include <unistd.h> // for access()

else if (input.rfind("type ", 0) == 0) {
    std::string target = input.substr(5);

    // Builtins
    if (target == "echo" || target == "exit" || target == "type") {
        std::cout << target << " is a shell builtin" << std::endl;
        continue;
    }

    // Get PATH variable
    char* pathEnv = std::getenv("PATH");
    if (pathEnv != nullptr) {
        std::string path(pathEnv);

        // Split PATH by ':' (Linux environment here)
        size_t start = 0;
        while (true) {
            size_t end = path.find(':', start);
            std::string dir = (end == std::string::npos)
                                  ? path.substr(start)
                                  : path.substr(start, end - start);

            if (!dir.empty()) {
                std::string fullPath = dir + "/" + target;

                // Check executable existence & permission
                if (access(fullPath.c_str(), X_OK) == 0) {
                    std::cout << target << " is " << fullPath << std::endl;
                    break; // Found — stop searching
                }
            }

            if (end == std::string::npos) {
                std::cout << target << ": not found" << std::endl;
                break;
            }

            start = end + 1;
        }
    } else {
        std::cout << target << ": not found" << std::endl;
    }
}




