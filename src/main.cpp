#include <algorithm>
#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <stdio.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <fmt/core.h>
#include <readline/history.h>
#include <readline/readline.h>

namespace fs = std::filesystem;

extern char** environ;

#ifdef _WIN32
static const char PATH_SEP = ';'; // Semicolon for Windows
#else
static const char PATH_SEP = ':'; // Colon for Unix-like systems (Linux, macOS)
#endif

static constexpr std::array<std::string_view, 6> builtin_commands = {"echo", "exit", "type", "pwd", "cd", "history"};

std::vector<std::string> parse_user_input(std::string_view input) {
    std::vector<std::string> args;
    // tokenize
    std::vector<char> current;
    current.reserve(32);
    bool in_single_quotes = false;
    bool in_double_quotes = false;
    for (size_t i = 0; i < input.length(); ++i) {
        char c = input[i];
        if (c == ' ' || c == '\t') {
            if (in_single_quotes || in_double_quotes) {
                current.push_back(c);
            } else if (!current.empty()) {
                // store current word
                args.emplace_back(current.begin(), current.end());
                current.clear();
            }
        } else if (c == '\'' && !in_double_quotes) {
            in_single_quotes = !in_single_quotes;
        } else if (c == '"' && !in_single_quotes) {
            in_double_quotes = !in_double_quotes;
        } else if (c == '\\') {
            if (!in_single_quotes && !in_double_quotes) {
                if (i + 1 < input.length()) {
                    ++i;
                    c = input[i];
                    current.push_back(c);
                }
            } else if (in_double_quotes && !in_single_quotes) {
                if (i + 1 < input.length()) {
                    char c2 = input[i + 1];
                    if (c2 == '\\' || c2 == '"' || c2 == '$') {
                        ++i;
                        current.push_back(c2);
                    } else {
                        current.push_back(c);
                    }
                }
            } else {
                current.push_back(c);
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        args.emplace_back(current.begin(), current.end());
    }

    return args;
}

void process_echo(const std::vector<std::string>& args) {
    for (size_t i = 1; i < args.size(); ++i) {
        if (i > 1) {
            std::cout << ' ';
        }
        std::cout << args[i];
    }
    std::cout << std::endl;
}

bool is_executable(fs::directory_entry entry) {
    // check for permissions first
    std::error_code ec;
    (void)fs::status(entry, ec);
    if (ec) {
        return false;
    }
    const bool is_exe =
        !entry.is_directory() && (entry.status().permissions() & fs::perms::owner_exec) == fs::perms::owner_exec;
    return is_exe;
}

std::optional<fs::directory_entry> path_search(std::string_view exe) {
    const char* path_env_c = std::getenv("PATH");
    auto split_ranges = std::string_view(path_env_c) | std::views::split(PATH_SEP);
    for (const auto& subrange : split_ranges) {
        std::string_view path(subrange.begin(), subrange.end());
        if (!fs::exists(path) || !fs::is_directory(path)) {
            continue;
        }
        for (const auto& entry : fs::directory_iterator(path)) {
            if (!is_executable(entry)) {
                continue;
            }
            if (entry.path().filename() == exe) {
                return entry;
            }
        }
    }
    return std::nullopt;
}

bool is_executable_absolute_path(std::string_view path) {
    fs::path check_abs = path;
    if (check_abs.is_absolute()) {
        if (!fs::exists(check_abs) || fs::is_directory(check_abs)) {
        } else if (is_executable(fs::directory_entry(check_abs))) {
            return true;
        }
    }
    return false;
}

bool is_builtin(std::string_view input) {
    const auto it = std::find(builtin_commands.begin(), builtin_commands.end(), input);
    if (it == builtin_commands.end()) {
        return false;
    }
    return true;
}

void process_type(const std::vector<std::string>& args) {
    // 1. check if invalid argument
    if (args.size() != 2) {
        std::cout << "type: Invalid number of arguments" << std::endl;
        return;
    }

    // 2. check if builtin
    if (is_builtin(args.back())) {
        std::cout << std::format("{} is a shell builtin", args.back()) << std::endl;
        return;
    }

    // 3. check if an absolute path and is an executable
    if (is_executable_absolute_path(args.back())) {
        std::cout << std::format("{} is {}", args.back(), args.back()) << std::endl;
        return;
    }

    // 4. check if in PATH
    const auto entry = path_search(args.back());
    if (entry.has_value()) {
        std::cout << std::format("{} is {}", args.back(), entry.value().path().c_str()) << std::endl;
        return;
    }

    // 5. command not found
    std::cout << std::format("{}: not found", args.back()) << std::endl;
}

bool process_exec(const std::vector<std::string> args) {
    // 1. check if there is such executable
    if (args.empty()) {
        return false;
    }
    std::vector<char*> argv;
    argv.reserve(args.size());
    std::string command;
    if (is_executable_absolute_path(args[0])) {
        command = args[0];
    } else if (const auto entry = path_search(args[0]); entry.has_value()) {
        command = entry.value().path();
    } else {
        return false;
    }

    // 2. process the arguments
    for (const auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    // 3. execute!
    pid_t pid = fork();
    if (pid == -1) {
        return false;
    }
    if (pid == 0) {
        // child process
        execve(command.c_str(), argv.data(), environ);
        // if it reaches here, it failed
        perror("execvp");
        exit(-1);
    }
    int status;
    waitpid(pid, &status, 0);
    return true;
}

void process_cd(const std::vector<std::string>& args) {
    const std::string& input = args.back();
    if (args.size() > 2) {
        std::cout << "cd: Invalid number of arguments" << std::endl;
        return;
    }
    if (args.size() == 1 || input == "~") {
        const char* home_env_c = std::getenv("HOME");
        fs::current_path(home_env_c);
        return;
    }
    fs::path p(input);
    std::error_code ec;
    (void)fs::status(p, ec);
    if (ec) {
        if (ec.value() == static_cast<int>(std::errc::no_such_file_or_directory)) {
            std::cout << fmt::format("cd: {}: No such file or directory", input) << std::endl;
        } else if (ec.value() == static_cast<int>(std::errc::permission_denied)) {
            std::cout << fmt::format("cd: {}: Permission denied", input) << std::endl;
        } else {
            std::cout << fmt::format("cd: {}: Unable to cd due to {}", input, ec.message()) << std::endl;
        }
        return;
    }
    fs::current_path(input);
}

void process_history(const std::vector<std::string>& args) {
    if (args.size() == 3) {
        if (args[1] == "-r") {
            std::ifstream infile(args.back());
            if (!infile.is_open()) {
                std::cout << "history: unable to open history file" << std::endl;
                return;
            }
            std::string line;
            while (std::getline(infile, line)) {
                if (line.empty()) {
                    continue;
                }
                add_history(line.c_str());
            }
            infile.close();
            return;
        } else if (args[1] == "-w") {
            std::ofstream outfile(args.back());
            if (!outfile.is_open()) {
                std::cout << "history: unable to open history file" << std::endl;
                return;
            }
            for (int i = 1; i <= history_length; ++i) {
                HIST_ENTRY* hist = history_get(i);
                if (hist == nullptr) {
                    continue;
                }
                outfile << hist->line << std::endl;
            }
            outfile.close();
            return;
        } else if (args[1] == "-a") {
            static int last_written = 0;
            std::ofstream outfile(args.back(), std::ios::app);
            if (!outfile.is_open()) {
                std::cout << "history: unable to open history file" << std::endl;
                return;
            }
            for (int i = last_written + 1; i <= history_length; ++i) {
                HIST_ENTRY* hist = history_get(i);
                if (hist == nullptr) {
                    continue;
                }
                outfile << hist->line << std::endl;
            }
            outfile.close();
            last_written = history_length;
            return;
        } else {
            std::cout << "history: Invalid arguments" << std::endl;
            return;
        }
    }

    int i = 1;
    if (args.size() == 2) {
        i = history_length - std::stoi(args.back()) + 1;
        i = std::min(history_length, i);
        i = std::max(1, i);
    }
    for (; i <= history_length; ++i) {
        HIST_ENTRY* hist = history_get(i);
        if (hist == nullptr) {
            continue;
        }
        std::cout << "    " << i << "  " << hist->line << std::endl;
    }
}

bool process_tokens(std::vector<std::string>& tokens, std::string_view user_input) {
    // check if we need to redirect output
    for (int i = 0; i < 2; ++i) {
        if (tokens.size() >= 2) {

            if (tokens[tokens.size() - 2] == ">" || tokens[tokens.size() - 2] == "1>") {
                int fd = open(tokens.back().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    perror("open");
                    exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
                tokens.pop_back();
                tokens.pop_back();
            }
            if (tokens[tokens.size() - 2] == "2>") {
                int fd = open(tokens.back().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    perror("open");
                    exit(1);
                }
                dup2(fd, STDERR_FILENO);
                close(fd);
                tokens.pop_back();
                tokens.pop_back();
            }
            if (tokens[tokens.size() - 2] == ">>" || tokens[tokens.size() - 2] == "1>>") {
                int fd = open(tokens.back().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd < 0) {
                    perror("open");
                    exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
                tokens.pop_back();
                tokens.pop_back();
            }
            if (tokens[tokens.size() - 2] == "2>>") {
                int fd = open(tokens.back().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd < 0) {
                    perror("open");
                    exit(1);
                }
                dup2(fd, STDERR_FILENO);
                tokens.pop_back();
                tokens.pop_back();
            }
        }
    }

    if (tokens.front() == "exit") {
        if (tokens.size() > 2) {
            std::cout << "exit: Invalid number of arguments" << std::endl;
            return true;
        }
        return false;
    }
    if (tokens.front() == "echo") {
        process_echo(tokens);
        return true;
    }
    if (tokens.front() == "type") {
        process_type(tokens);
        return true;
    }
    if (tokens.front() == "pwd") {
        if (tokens.size() != 1) {
            std::cout << "pwd: Invalid number of arguments" << std::endl;
        } else {
            std::cout << fs::current_path().c_str() << std::endl;
        }
        return true;
    }
    if (tokens.front() == "cd") {
        process_cd(tokens);
        return true;
    }
    if (tokens.front() == "history") {
        if (tokens.size() > 3) {
            std::cout << "history: Invalid number of arguments" << std::endl;
        } else {
            process_history(tokens);
        }
        return true;
    }

    if (process_exec(tokens)) {
        return true;
    }

    std::cout << fmt::format("{}: command not found", user_input) << std::endl;
    return true;
}

bool process_input(std::string_view user_input) {
    std::vector<std::string> tokens = parse_user_input(user_input);
    if (tokens.empty()) {
        return true;
    }

    // check if we need to pipe (assume only 1 pipe for now)
    bool check_pipe = false;
    pid_t pid;
    int pipefd[2]; // 0: read, 1: write

    while (true) {
        auto pipe_delim_it = std::find(tokens.begin(), tokens.end(), "|");
        if (pipe_delim_it != tokens.end()) {
            check_pipe = true;
            if (pipe(pipefd) == -1) {
                perror("pipe");
                exit(1);
            }
            pid = fork();
            if (pid == -1) {
                perror("fork");
                exit(1);
            }
            if (pid != 0) {
                // parent
                close(pipefd[1]);
                dup2(pipefd[0], STDIN_FILENO); // make pipe read as stdin
                tokens = std::vector<std::string>(pipe_delim_it + 1, tokens.end());
            } else {
                // child
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO); // make pipe write as stdout
                tokens = std::vector<std::string>(tokens.begin(), pipe_delim_it);
            }
        } else {
            break;
        }
    }

    bool res = process_tokens(tokens, user_input);

    if (check_pipe) {
        if (pid != 0) {
            // parent
            close(pipefd[0]);
            wait(nullptr);
        } else {
            // child
            close(pipefd[1]);
            exit(0);
        }
    }

    return res;
}

char* command_generator(const char* c_text, int state) {
    // NOTE(josef): can be optimized by using a trie (prefix tree)
    static std::vector<std::string> matches;
    static size_t match_idx = 0;

    // initialize on the first call
    if (state == 0) {
        match_idx = 0;
        matches.clear();

        std::string_view text(c_text);

        // find matches from builtin
        for (auto& cmd : builtin_commands) {
            if (cmd.size() >= text.size() && cmd.compare(0, text.size(), text) == 0) {
                matches.push_back(std::string(cmd));
            }
        }
        const char* path_env_c = std::getenv("PATH");
        auto split_ranges = std::string_view(path_env_c) | std::views::split(PATH_SEP);
        for (const auto& subrange : split_ranges) {
            std::string_view path(subrange.begin(), subrange.end());
            if (!fs::exists(path) || !fs::is_directory(path)) {
                continue;
            }
            for (const auto& entry : fs::directory_iterator(path)) {
                if (is_executable(entry)) {
                    std::string_view cmd = entry.path().filename().c_str();
                    if (cmd.size() >= text.size() && cmd.compare(0, text.size(), text) == 0) {
                        matches.push_back(std::string(cmd));
                    }
                }
            }
        }
    }

    // return matches one at a time on subsequent calls
    if (match_idx < matches.size()) {
        // readline will free this memory
        return strdup(matches[match_idx++].data());
    }
    return nullptr;
}

char** command_completion(const char* text, int start, int end) {
    // auto complete from a list of executables only if it is the first argument
    if (start == 0) {
        return rl_completion_matches(text, command_generator);
    }
    // otherwise, handle other types of completion such as filenames
    return nullptr;
}

int main() {
    // Flush after every std::cout / std:cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    int old_stdin = dup(STDIN_FILENO);
    int old_stdout = dup(STDOUT_FILENO);
    int old_stderr = dup(STDERR_FILENO);

    bool is_running = true;
    char* buf;
    rl_attempted_completion_function = command_completion;
    using_history();

    while (is_running) {
        dup2(old_stdin, STDIN_FILENO);
        dup2(old_stdout, STDOUT_FILENO);
        dup2(old_stderr, STDERR_FILENO);

        buf = readline("$ ");
        if (!buf) {
            break;
        }
        add_history(buf);

        is_running = process_input(buf);

        free(buf);
    }
    close(old_stdin);
    close(old_stdout);
    close(old_stderr);
}










