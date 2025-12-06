#include <iostream>
#include <string>

int main() {
    // Flush after every std::cout / std:cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    while (true) {
        std::cout << "$ ";                 // Print prompt

        std::string input;
        if (!std::getline(std::cin, input)) {
            break; // If input stream closes, exit loop (tester controls this)
        }

        std::cout << input << ": command not found" << std::endl;
    }

    return 0;
}
