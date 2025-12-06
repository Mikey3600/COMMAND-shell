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

        // Check if the command starts with "echo "
        if (input.rfind("echo ", 0) == 0) {
            // Extract everything after "echo "
            std::string text = input.substr(5);
            std::cout << text << std::endl;
        } else {
            std::cout << input << ": command not found" << std::endl;
        }
    }

    return 0;
}


