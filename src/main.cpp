#include <iostream>
#include <string>

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    while (true) {
        std::cout << "$ ";

        std::string input;
        if (!std::getline(std::cin, input)) {
            break;
        }

        if (input == "exit") {
            break;
        }

        // Handle echo command
        if (input.rfind("echo ", 0) == 0) {
            std::string text = input.substr(5);
            std::cout << text << std::endl;
        }
        // Handle type command
        else if (input.rfind("type ", 0) == 0) {
            std::string target = input.substr(5);

            // Check known builtins
            if (target == "echo" || target == "exit" || target == "type") {
                std::cout << target << " is a shell builtin" << std::endl;
            } else {
                std::cout << target << ": not found" << std::endl;
            }
        }
        // Default fallback
        else {
            std::cout << input << ": command not found" << std::endl;
        }
    }

    return 0;
}



