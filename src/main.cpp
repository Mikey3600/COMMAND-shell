#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <readline/readline.h>
#include <readline/history.h>

using namespace std;

vector<string> shell_history;
size_t last_history_flush_index = 0;

string tokenize_input(const string& input, vector<string>& tokens) {
    tokens.clear();
    string current;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escape_next = false;
    
    for (size_t i = 0; i < input.size(); i++) {
        char c = input[i];
        
        if (escape_next) {
            current += c;
            escape_next = false;
            continue;
        }
        
        if (c == '\\' && !in_single_quote) {
            escape_next = true;
            continue;
        }
        
        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }
        
        if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }
        
        if (!in_single_quote && !in_double_quote && (c == ' ' || c == '\t')) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        
        current += c;
    }
    
    if (!current.empty()) {
        tokens.push_back(current);
    }
    
    if (in_single_quote || in_double_quote) {
        return "Unclosed quote";
    }
    
    return "";
}

string find_in_path(const string& cmd) {
    if (cmd.find('/') != string::npos) {
        if (access(cmd.c_str(), X_OK) == 0) {
            return cmd;
        }
        return "";
    }
    
    const char* path_env = getenv("PATH");
    if (!path_env) return "";
    
    string path_str(path_env);
    stringstream ss(path_str);
    string dir;
    
    while (getline(ss, dir, ':')) {
        string full_path = dir + "/" + cmd;
        if (access(full_path.c_str(), X_OK) == 0) {
            return full_path;
        }
    }
    
    return "";
}

bool is_builtin(const string& cmd) {
    return cmd == "echo" || cmd == "exit" || cmd == "pwd" || 
           cmd == "cd" || cmd == "type" || cmd == "history";
}

int execute_builtin(const vector<string>& args) {
    if (args.empty()) return 0;
    
    const string& cmd = args[0];
    
    if (cmd == "echo") {
        for (size_t i = 1; i < args.size(); i++) {
            if (i > 1) cout << " ";
            cout << args[i];
        }
        cout << endl;
        return 0;
    }
    
    if (cmd == "exit") {
        exit(args.size() > 1 ? atoi(args[1].c_str()) : 0);
    }
    
    if (cmd == "pwd") {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) {
            cout << cwd << endl;
            return 0;
        }
        return 1;
    }
    
    if (cmd == "cd") {
        string target;
        if (args.size() == 1) {
            const char* home = getenv("HOME");
            if (!home) {
                cerr << "cd: HOME not set" << endl;
                return 1;
            }
            target = home;
        } else {
            target = args[1];
            if (target[0] == '~') {
                const char* home = getenv("HOME");
                if (!home) {
                    cerr << "cd: HOME not set" << endl;
                    return 1;
                }
                target = string(home) + target.substr(1);
            }
        }
        
        if (chdir(target.c_str()) != 0) {
            cerr << "cd: " << target << ": No such file or directory" << endl;
            return 1;
        }
        return 0;
    }
    
    if (cmd == "type") {
        for (size_t i = 1; i < args.size(); i++) {
            if (is_builtin(args[i])) {
                cout << args[i] << " is a shell builtin" << endl;
            } else {
                string path = find_in_path(args[i]);
                if (!path.empty()) {
                    cout << args[i] << " is " << path << endl;
                } else {
                    cout << args[i] << ": not found" << endl;
                }
            }
        }
        return 0;
    }
    
    if (cmd == "history") {
        if (args.size() == 1) {
            for (size_t i = 0; i < shell_history.size(); i++) {
                cout << "  " << (i + 1) << "  " << shell_history[i] << endl;
            }
            return 0;
        }
        
        if (args[1] == "-r" && args.size() == 3) {
            int fd = open(args[2].c_str(), O_RDONLY);
            if (fd < 0) {
                cerr << "history: cannot read " << args[2] << endl;
                return 1;
            }
            
            string content;
            char buf[4096];
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf))) > 0) {
                content.append(buf, n);
            }
            close(fd);
            
            stringstream ss(content);
            string line;
            while (getline(ss, line)) {
                if (!line.empty()) {
                    add_history(line.c_str());
                    shell_history.push_back(line);
                }
            }
            last_history_flush_index = shell_history.size();
            return 0;
        }
        
        if (args[1] == "-w" && args.size() == 3) {
            int fd = open(args[2].c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                cerr << "history: cannot write " << args[2] << endl;
                return 1;
            }
            
            for (size_t i = 0; i < shell_history.size(); i++) {
                string line = shell_history[i] + "\n";
                write(fd, line.c_str(), line.size());
            }
            close(fd);
            last_history_flush_index = shell_history.size();
            return 0;
        }
        
        if (args[1] == "-a" && args.size() == 3) {
            if (last_history_flush_index >= shell_history.size()) {
                return 0;
            }
            
            int fd = open(args[2].c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd < 0) {
                cerr << "history: cannot write " << args[2] << endl;
                return 1;
            }
            
            for (size_t i = last_history_flush_index; i < shell_history.size(); i++) {
                string line = shell_history[i] + "\n";
                write(fd, line.c_str(), line.size());
            }
            
            close(fd);
            last_history_flush_index = shell_history.size();
            return 0;
        }
        
        if (args.size() == 2) {
            int n = atoi(args[1].c_str());
            size_t start = 0;
            if (n > 0 && (size_t)n < shell_history.size()) {
                start = shell_history.size() - n;
            }
            for (size_t i = start; i < shell_history.size(); i++) {
                cout << "  " << (i + 1) << "  " << shell_history[i] << endl;
            }
            return 0;
        }
        
        return 1;
    }
    
    return 0;
}

struct Command {
    vector<string> args;
    string stdout_file;
    string stderr_file;
    bool stdout_append = false;
    bool stderr_append = false;
};

void parse_redirections(vector<string>& tokens, Command& cmd) {
    cmd.args.clear();
    
    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i] == ">" || tokens[i] == "1>") {
            if (i + 1 < tokens.size()) {
                cmd.stdout_file = tokens[i + 1];
                cmd.stdout_append = false;
                i++;
            }
        } else if (tokens[i] == ">>" || tokens[i] == "1>>") {
            if (i + 1 < tokens.size()) {
                cmd.stdout_file = tokens[i + 1];
                cmd.stdout_append = true;
                i++;
            }
        } else if (tokens[i] == "2>") {
            if (i + 1 < tokens.size()) {
                cmd.stderr_file = tokens[i + 1];
                cmd.stderr_append = false;
                i++;
            }
        } else if (tokens[i] == "2>>") {
            if (i + 1 < tokens.size()) {
                cmd.stderr_file = tokens[i + 1];
                cmd.stderr_append = true;
                i++;
            }
        } else {
            cmd.args.push_back(tokens[i]);
        }
    }
}

int execute_external(const vector<string>& args) {
    if (args.empty()) return 0;
    
    string path = find_in_path(args[0]);
    if (path.empty()) {
        cerr << args[0] << ": command not found" << endl;
        return 127;
    }
    
    vector<char*> argv;
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    
    execv(path.c_str(), argv.data());
    cerr << "execv failed" << endl;
    exit(1);
}

int execute_command(Command& cmd, int input_fd, int output_fd) {
    if (cmd.args.empty()) return 0;
    
    int saved_stdout = -1;
    int saved_stderr = -1;
    
    if (!cmd.stdout_file.empty()) {
        saved_stdout = dup(STDOUT_FILENO);
        int flags = O_WRONLY | O_CREAT;
        flags |= cmd.stdout_append ? O_APPEND : O_TRUNC;
        int fd = open(cmd.stdout_file.c_str(), flags, 0644);
        if (fd < 0) {
            cerr << "Cannot open " << cmd.stdout_file << endl;
            if (saved_stdout >= 0) {
                dup2(saved_stdout, STDOUT_FILENO);
                close(saved_stdout);
            }
            return 1;
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    } else if (output_fd != STDOUT_FILENO) {
        saved_stdout = dup(STDOUT_FILENO);
        dup2(output_fd, STDOUT_FILENO);
    }
    
    if (!cmd.stderr_file.empty()) {
        saved_stderr = dup(STDERR_FILENO);
        int flags = O_WRONLY | O_CREAT;
        flags |= cmd.stderr_append ? O_APPEND : O_TRUNC;
        int fd = open(cmd.stderr_file.c_str(), flags, 0644);
        if (fd < 0) {
            cerr << "Cannot open " << cmd.stderr_file << endl;
            if (saved_stdout >= 0) {
                dup2(saved_stdout, STDOUT_FILENO);
                close(saved_stdout);
            }
            if (saved_stderr >= 0) {
                dup2(saved_stderr, STDERR_FILENO);
                close(saved_stderr);
            }
            return 1;
        }
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    
    if (input_fd != STDIN_FILENO) {
        dup2(input_fd, STDIN_FILENO);
    }
    
    int result;
    if (is_builtin(cmd.args[0])) {
        result = execute_builtin(cmd.args);
    } else {
        result = execute_external(cmd.args);
    }
    
    if (saved_stdout >= 0) {
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
    }
    if (saved_stderr >= 0) {
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }
    
    return result;
}

int execute_pipeline(vector<Command>& commands) {
    if (commands.empty()) return 0;
    
    if (commands.size() == 1) {
        if (is_builtin(commands[0].args[0])) {
            return execute_command(commands[0], STDIN_FILENO, STDOUT_FILENO);
        }
    }
    
    vector<pid_t> pids;
    int prev_pipe_read = STDIN_FILENO;
    
    for (size_t i = 0; i < commands.size(); i++) {
        int pipefd[2];
        if (i < commands.size() - 1) {
            if (pipe(pipefd) < 0) {
                cerr << "pipe failed" << endl;
                return 1;
            }
        }
        
        pid_t pid = fork();
        if (pid < 0) {
            cerr << "fork failed" << endl;
            return 1;
        }
        
        if (pid == 0) {
            if (prev_pipe_read != STDIN_FILENO) {
                dup2(prev_pipe_read, STDIN_FILENO);
                close(prev_pipe_read);
            }
            
            if (i < commands.size() - 1) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }
            
            int result = execute_command(commands[i], STDIN_FILENO, STDOUT_FILENO);
            exit(result);
        }
        
        if (prev_pipe_read != STDIN_FILENO) {
            close(prev_pipe_read);
        }
        
        if (i < commands.size() - 1) {
            close(pipefd[1]);
            prev_pipe_read = pipefd[0];
        }
        
        pids.push_back(pid);
    }
    
    int last_status = 0;
    for (pid_t pid : pids) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            last_status = WEXITSTATUS(status);
        }
    }
    
    return last_status;
}

vector<string> get_completions(const string& text) {
    vector<string> completions;
    
    vector<string> builtins = {"echo", "exit", "pwd", "cd", "type", "history"};
    for (const auto& b : builtins) {
        if (b.find(text) == 0) {
            completions.push_back(b);
        }
    }
    
    const char* path_env = getenv("PATH");
    if (path_env) {
        string path_str(path_env);
        stringstream ss(path_str);
        string dir;
        
        while (getline(ss, dir, ':')) {
            DIR* dirp = opendir(dir.c_str());
            if (!dirp) continue;
            
            struct dirent* entry;
            while ((entry = readdir(dirp)) != nullptr) {
                string name = entry->d_name;
                if (name.find(text) == 0 && name != "." && name != "..") {
                    string full_path = dir + "/" + name;
                    if (access(full_path.c_str(), X_OK) == 0) {
                        bool found = false;
                        for (const auto& c : completions) {
                            if (c == name) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            completions.push_back(name);
                        }
                    }
                }
            }
            closedir(dirp);
        }
    }
    
    return completions;
}

char* completion_generator(const char* text, int state) {
    static vector<string> matches;
    static size_t match_index;
    
    if (state == 0) {
        matches = get_completions(text);
        match_index = 0;
    }
    
    if (match_index < matches.size()) {
        return strdup(matches[match_index++].c_str());
    }
    
    return nullptr;
}

char** command_completion(const char* text, int start, int end) {
    rl_attempted_completion_over = 1;
    if (start == 0) {
        return rl_completion_matches(text, completion_generator);
    }
    return nullptr;
}

int main() {
    rl_attempted_completion_function = command_completion;
    
    while (true) {
        char* input = readline("$ ");
        
        if (!input) {
            cout << endl;
            break;
        }
        
        string line(input);
        free(input);
        
        if (line.empty()) {
            continue;
        }
        
        add_history(line.c_str());
        shell_history.push_back(line);
        
        vector<string> tokens;
        string error = tokenize_input(line, tokens);
        if (!error.empty()) {
            cerr << "Error: " << error << endl;
            continue;
        }
        
        if (tokens.empty()) {
            continue;
        }
        
        vector<vector<string>> pipeline_tokens;
        vector<string> current_cmd;
        
        for (const auto& token : tokens) {
            if (token == "|") {
                if (!current_cmd.empty()) {
                    pipeline_tokens.push_back(current_cmd);
                    current_cmd.clear();
                }
            } else {
                current_cmd.push_back(token);
            }
        }
        if (!current_cmd.empty()) {
            pipeline_tokens.push_back(current_cmd);
        }
        
        vector<Command> commands;
        for (auto& cmd_tokens : pipeline_tokens) {
            Command cmd;
            parse_redirections(cmd_tokens, cmd);
            commands.push_back(cmd);
        }
        
        execute_pipeline(commands);
    }
    
    return 0;
}










