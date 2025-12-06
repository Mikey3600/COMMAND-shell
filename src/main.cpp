std::vector<std::string> tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string current;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;

    for (size_t i = 0; i < input.size(); i++) {
        char c = input[i];

        if (c == '\'' && !inDoubleQuote) {
            // Toggle single-quote mode ONLY if not inside double quotes
            inSingleQuote = !inSingleQuote;
        }
        else if (c == '"' && !inSingleQuote) {
            // Toggle double-quote mode ONLY if not inside single quotes
            inDoubleQuote = !inDoubleQuote;
        }
        else if (std::isspace(c) && !inSingleQuote && !inDoubleQuote) {
            // Split token only when outside ALL quotes
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        }
        else {
            // Literal character
            current.push_back(c);
        }
    }

    if (!current.empty()) {
        tokens.push_back(current);
    }

    return tokens;
}




