#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <algorithm>
#include <set>
#include <iomanip>
#include <signal.h>
#include <errno.h>
#include <cctype>

#include <readline/readline.h>
#include <readline/history.h>

using namespace std;

struct Redirection {
    int fd;         // 1 for stdout, 2 for stderr
    bool append;    // true for >>, false for >
    string filename;
};

struct Command {
    vector<string> argv;
    vector<Redirection> redirs;
};

static vector<string> shell_history;
static size_t last_history_flush_index = 0;

static vector<string> builtin_names = {
    "echo", "exit", "pwd", "cd", "type", "history"
};

static vector<string> path_executables;
static set<string> path_executables_set;

static string shell_prompt = "$ ";

// Forward declarations
static void execute_line(const string &line);
static bool is_builtin(const string &cmd);
static int run_builtin(Command &cmd, bool in_child);
static void execute_pipeline(vector<Command> &pipeline);
static vector<string> tokenize(const string &line);
static vector<Command> parse_pipeline(const vector<string> &tokens);
static void apply_redirections(const vector<Redirection> &redirs);
static string find_executable(const string &cmd);
static void build_executable_cache();
static void setup_signal_handlers();

// ---------------------- PATH executables cache ----------------------

static void build_executable_cache() {
    const char *path_env = getenv("PATH");
    if (!path_env) return;

    string path_str(path_env);
    stringstream ss(path_str);
    string dir;

    while (getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        DIR *dp = opendir(dir.c_str());
        if (!dp) continue;
        struct dirent *entry;
        while ((entry = readdir(dp)) != nullptr) {
            string name(entry->d_name);
            if (name == "." || name == "..") continue;
            string full = dir + "/" + name;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                if (access(full.c_str(), X_OK) == 0) {
                    if (path_executables_set.insert(name).second) {
                        path_executables.push_back(name);
                    }
                }
            }
        }
        closedir(dp);
    }

    sort(path_executables.begin(), path_executables.end());
}

// ---------------------- Tokenizer & Parser ----------------------

static vector<string> tokenize(const string &line) {
    vector<string> tokens;
    string cur;
    enum Mode { NORMAL, SINGLE_QUOTE, DOUBLE_QUOTE };
    Mode mode = NORMAL;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];

        if (mode == NORMAL) {
            if (c == '\\') {
                if (i + 1 < line.size()) {
                    cur += line[i + 1];
                    ++i;
                }
            } else if (c == '\'') {
                mode = SINGLE_QUOTE;
            } else if (c == '"') {
                mode = DOUBLE_QUOTE;
            } else if (isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) {
                    tokens.push_back(cur);
                    cur.clear();
                }
            } else if (c == '>') {
                if (!cur.empty()) {
                    tokens.push_back(cur);
                    cur.clear();
                }
                if (i + 1 < line.size() && line[i + 1] == '>') {
                    tokens.push_back(">>");
                    ++i;
                } else {
                    tokens.push_back(">");
                }
            } else if (c == '|') {
                if (!cur.empty()) {
                    tokens.push_back(cur);
                    cur.clear();
                }
                tokens.push_back("|");
            } else {
                cur += c;
            }
        } else if (mode == SINGLE_QUOTE) {
            if (c == '\'') {
                mode = NORMAL;
            } else {
                cur += c;
            }
        } else if (mode == DOUBLE_QUOTE) {
            if (c == '"') {
                mode = NORMAL;
            } else if (c == '\\') {
                if (i + 1 < line.size()) {
                    cur += line[i + 1];
                    ++i;
                }
            } else {
                cur += c;
            }
        }
    }

    if (!cur.empty()) {
        tokens.push_back(cur);
    }

    return tokens;
}

static vector<Command> parse_pipeline(const vector<string> &tokens) {
    vector<Command> pipeline;
    vector<string> current_tokens;

    for (const string &t : tokens) {
        if (t == "|") {
            if (!current_tokens.empty()) {
                Command cmd;
                size_t i = 0;
                while (i < current_tokens.size()) {
                    const string &tok = current_tokens[i];
                    if ((tok == "1" || tok == "2") &&
                        i + 2 < current_tokens.size() &&
                        (current_tokens[i + 1] == ">" || current_tokens[i + 1] == ">>"))
                    {
                        Redirection r;
                        r.fd = (tok == "1") ? 1 : 2;
                        r.append = (current_tokens[i + 1] == ">>");
                        r.filename = current_tokens[i + 2];
                        cmd.redirs.push_back(r);
                        i += 3;
                    } else if ((tok == ">" || tok == ">>") && i + 1 < current_tokens.size()) {
                        Redirection r;
                        r.fd = 1;
                        r.append = (tok == ">>");
                        r.filename = current_tokens[i + 1];
                        cmd.redirs.push_back(r);
                        i += 2;
                    } else {
                        cmd.argv.push_back(tok);
                        ++i;
                    }
                }
                pipeline.push_back(cmd);
                current_tokens.clear();
            }
        } else {
            current_tokens.push_back(t);
        }
    }

    if (!current_tokens.empty()) {
        Command cmd;
        size_t i = 0;
        while (i < current_tokens.size()) {
            const string &tok = current_tokens[i];
            if ((tok == "1" || tok == "2") &&
                i + 2 < current_tokens.size() &&
                (current_tokens[i + 1] == ">" || current_tokens[i + 1] == ">>"))
            {
                Redirection r;
                r.fd = (tok == "1") ? 1 : 2;
                r.append = (current_tokens[i + 1] == ">>");
                r.filename = current_tokens[i + 2];
                cmd.redirs.push_back(r);
                i += 3;
            } else if ((tok == ">" || tok == ">>") && i + 1 < current_tokens.size()) {
                Redirection r;
                r.fd = 1;
                r.append = (tok == ">>");
                r.filename = current_tokens[i + 1];
                cmd.redirs.push_back(r);
                i += 2;
            } else {
                cmd.argv.push_back(tok);
                ++i;
            }
        }
        pipeline.push_back(cmd);
    }

    return pipeline;
}

// ---------------------- Redirections ----------------------

static void apply_redirections(const vector<Redirection> &redirs) {
    for (const auto &r : redirs) {
        int flags = O_WRONLY | O_CREAT;
        if (r.append) {
            flags |= O_APPEND;
        } else {
            flags |= O_TRUNC;
        }
        int fd = open(r.filename.c_str(), flags, 0644);
        if (fd < 0) {
            perror("open");
            continue;
        }
        if (dup2(fd, r.fd) < 0) {
            perror("dup2");
        }
        close(fd);
    }
}

// ---------------------- PATH search ----------------------

static string find_executable(const string &cmd) {
    if (cmd.empty()) return "";

    // If cmd contains a slash, do not search PATH
    if (cmd.find('/') != string::npos) {
        if (access(cmd.c_str(), X_OK) == 0) {
            return cmd;
        }
        return "";
    }

    const char *path_env = getenv("PATH");
    if (!path_env) return "";

    string path_str(path_env);
    stringstream ss(path_str);
    string dir;

    while (getline(ss, dir, ':')) {
        if (dir.empty()) dir = ".";
        string full = dir + "/" + cmd;
        if (access(full.c_str(), X_OK) == 0) {
            return full;
        }
    }

    return "";
}

// ---------------------- Builtins ----------------------

static bool is_builtin(const string &cmd) {
    return find(builtin_names.begin(), builtin_names.end(), cmd) != builtin_names.end();
}

static int builtin_echo(const vector<string> &argv) {
    for (size_t i = 1; i < argv.size(); ++i) {
        if (i > 1) cout << " ";
        cout << argv[i];
    }
    cout << "\n";
    return 0;
}

static int builtin_pwd() {
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf)) == nullptr) {
        perror("pwd");
        return 1;
    }
    cout << buf << "\n";
    return 0;
}

static int builtin_cd(const vector<string> &argv) {
    const char *home = getenv("HOME");
    string target;
    if (argv.size() < 2) {
        if (!home) {
            cerr << "cd: HOME not set\n";
            return 1;
        }
        target = home;
    } else {
        string arg = argv[1];
        if (!arg.empty() && arg[0] == '~') {
            if (!home) {
                cerr << "cd: HOME not set\n";
                return 1;
            }
            if (arg.size() == 1) {
                target = home;
            } else if (arg[1] == '/') {
                target = string(home) + arg.substr(1);
            } else {
                target = arg; // ~something, leave as is
            }
        } else {
            target = arg;
        }
    }

    if (chdir(target.c_str()) != 0) {
        perror("cd");
        return 1;
    }
    return 0;
}

static int builtin_type(const vector<string> &argv) {
    if (argv.size() < 2) {
        cerr << "type: usage: type name\n";
        return 1;
    }
    int status = 0;
    for (size_t i = 1; i < argv.size(); ++i) {
        const string &name = argv[i];
        if (is_builtin(name)) {
            cout << name << " is a shell builtin\n";
        } else {
            string path = find_executable(name);
            if (!path.empty()) {
                cout << name << " is " << path << "\n";
            } else {
                cerr << "type: " << name << ": not found\n";
                status = 1;
            }
        }
    }
    return status;
}

static int builtin_history(const vector<string> &argv) {
    // history with no args → full history
    if (argv.size() == 1) {
        for (size_t i = 0; i < shell_history.size(); ++i) {
            cout << setw(5) << (i + 1) << "  " << shell_history[i] << "\n";
        }
        return 0;
    }

    // history <number> → last N entries, but keep global numbering
    if (argv.size() == 2) {
        bool isNum = true;
        for (char c : argv[1]) {
            if (!isdigit(static_cast<unsigned char>(c))) {
                isNum = false;
                break;
            }
        }
        if (isNum) {
            int n = stoi(argv[1]);
            if (n <= 0) return 0;

            size_t total = shell_history.size();
            size_t start = (n >= static_cast<int>(total)) ? 0 : total - n;

            for (size_t i = start; i < total; ++i) {
                cout << setw(5) << (i + 1) << "  " << shell_history[i] << "\n";
            }
            return 0;
        }
    }

    // history -a <file> → append new commands since last flush
    if (argv.size() >= 3 && argv[1] == "-a") {
        const string &file = argv[2];
        int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("history -a");
            return 1;
        }
        for (size_t i = last_history_flush_index; i < shell_history.size(); ++i) {
            const string &line = shell_history[i];
            if (write(fd, line.c_str(), line.size()) < 0) {
                perror("write");
            }
            if (write(fd, "\n", 1) < 0) {
                perror("write");
            }
        }
        close(fd);
        last_history_flush_index = shell_history.size();
        return 0;
    }

    // history -w <file> → write entire history, overwrite file
    if (argv.size() >= 3 && argv[1] == "-w") {
        const string &file = argv[2];
        int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("history -w");
            return 1;
        }
        for (size_t i = 0; i < shell_history.size(); ++i) {
            const string &line = shell_history[i];
            if (write(fd, line.c_str(), line.size()) < 0) {
                perror("write");
            }
            if (write(fd, "\n", 1) < 0) {
                perror("write");
            }
        }
        close(fd);
        last_history_flush_index = shell_history.size();
        return 0;
    }

    // history -r <file> → read entries from file into memory
    if (argv.size() >= 3 && argv[1] == "-r") {
        const string &file = argv[2];
        int fd = open(file.c_str(), O_RDONLY);
        if (fd < 0) {
            // If file doesn't exist, that's fine
            return 0;
        }
        FILE *f = fdopen(fd, "r");
        if (!f) {
            close(fd);
            perror("history -r");
            return 1;
        }
        char *line = nullptr;
        size_t len = 0;
        ssize_t nread;
        while ((nread = getline(&line, &len, f)) != -1) {
            if (nread > 0 && line[nread - 1] == '\n') {
                line[nread - 1] = '\0';
            }
            string s(line);
            if (!s.empty()) {
                shell_history.push_back(s);
                add_history(s.c_str());
            }
        }
        if (line) free(line);
        fclose(f);
        last_history_flush_index = shell_history.size();
        return 0;
    }

    cerr << "history: unsupported option\n";
    return 1;
}

static int run_builtin(Command &cmd, bool in_child) {
    if (cmd.argv.empty()) return 0;
    const string &name = cmd.argv[0];

    if (name == "echo") {
        return builtin_echo(cmd.argv);
    } else if (name == "pwd") {
        return builtin_pwd();
    } else if (name == "cd") {
        return builtin_cd(cmd.argv);
    } else if (name == "type") {
        return builtin_type(cmd.argv);
    } else if (name == "history") {
        return builtin_history(cmd.argv);
    } else if (name == "exit") {
        if (in_child) {
            _exit(0);
        } else {
            exit(0);
        }
    }

    return 0;
}

// ---------------------- Pipeline execution ----------------------

static void execute_pipeline(vector<Command> &pipeline) {
    size_t n = pipeline.size();
    if (n == 0) return;

    // Single builtin: run in parent so it can affect state (cd, exit, etc.)
    if (n == 1 && !pipeline[0].argv.empty() && is_builtin(pipeline[0].argv[0])) {
        int saved_stdout = dup(STDOUT_FILENO);
        int saved_stderr = dup(STDERR_FILENO);
        if (saved_stdout < 0 || saved_stderr < 0) {
            perror("dup");
            return;
        }

        apply_redirections(pipeline[0].redirs);
        run_builtin(pipeline[0], false);

        if (dup2(saved_stdout, STDOUT_FILENO) < 0) perror("dup2");
        if (dup2(saved_stderr, STDERR_FILENO) < 0) perror("dup2");
        close(saved_stdout);
        close(saved_stderr);
        return;
    }

    vector<pid_t> pids;
    pids.reserve(n);

    vector<int> pipes_fd;
    if (n > 1) {
        pipes_fd.resize(2 * (n - 1));
        for (size_t i = 0; i < n - 1; ++i) {
            if (pipe(&pipes_fd[2 * i]) < 0) {
                perror("pipe");
                return;
            }
        }
    }

    for (size_t i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return;
        } else if (pid == 0) {
            // Child
            signal(SIGINT, SIG_DFL);

            if (n > 1) {
                if (i > 0) {
                    int in_fd = pipes_fd[2 * (i - 1)];
                    if (dup2(in_fd, STDIN_FILENO) < 0) {
                        perror("dup2");
                        _exit(1);
                    }
                }
                if (i < n - 1) {
                    int out_fd = pipes_fd[2 * i + 1];
                    if (dup2(out_fd, STDOUT_FILENO) < 0) {
                        perror("dup2");
                        _exit(1);
                    }
                }
                for (size_t j = 0; j < pipes_fd.size(); ++j) {
                    close(pipes_fd[j]);
                }
            }

            apply_redirections(pipeline[i].redirs);

            if (pipeline[i].argv.empty()) {
                _exit(0);
            }

            const string &cmd_name = pipeline[i].argv[0];
            if (is_builtin(cmd_name)) {
                run_builtin(pipeline[i], true);
                _exit(0);
            } else {
                vector<char*> argv;
                for (size_t k = 0; k < pipeline[i].argv.size(); ++k) {
                    argv.push_back(const_cast<char*>(pipeline[i].argv[k].c_str()));
                }
                argv.push_back(nullptr);

                string path = find_executable(cmd_name);
                if (path.empty()) {
                    cerr << cmd_name << ": command not found\n";
                    _exit(127);
                }
                execv(path.c_str(), argv.data());
                perror("execv");
                _exit(127);
            }
        } else {
            // Parent
            pids.push_back(pid);
        }
    }

    if (n > 1) {
        for (size_t j = 0; j < pipes_fd.size(); ++j) {
            close(pipes_fd[j]);
        }
    }

    for (pid_t pid : pids) {
        int status;
        waitpid(pid, &status, 0);
    }
}

// ---------------------- Line execution ----------------------

static void execute_line(const string &line) {
    vector<string> tokens = tokenize(line);
    if (tokens.empty()) return;

    vector<Command> pipeline = parse_pipeline(tokens);
    execute_pipeline(pipeline);
}

// ---------------------- Readline completion ----------------------

static char *command_generator(const char *text, int state) {
    static size_t index_builtin;
    static size_t index_exec;

    if (state == 0) {
        index_builtin = 0;
        index_exec = 0;
    }

    string prefix(text);

    while (index_builtin < builtin_names.size()) {
        const string &name = builtin_names[index_builtin++];
        if (name.compare(0, prefix.size(), prefix) == 0) {
            return strdup(name.c_str());
        }
    }

    while (index_exec < path_executables.size()) {
        const string &name = path_executables[index_exec++];
        if (name.compare(0, prefix.size(), prefix) == 0) {
            return strdup(name.c_str());
        }
    }

    return nullptr;
}

static char **shell_completion(const char *text, int start, int end) {
    (void)end;
    if (start == 0) {
        return rl_completion_matches(text, command_generator);
    } else {
        return rl_completion_matches(text, rl_filename_completion_function);
    }
}

// ---------------------- Signals ----------------------

static void setup_signal_handlers() {
    signal(SIGINT, SIG_IGN);
}

// ---------------------- Main loop ----------------------

int main() {
    build_executable_cache();
    setup_signal_handlers();

    rl_attempted_completion_function = shell_completion;

    while (true) {
        char *input = readline(shell_prompt.c_str());
        if (!input) {
            cout << "\n";
            break;
        }

        string line(input);
        free(input);

        if (line.empty()) {
            continue;
        }

        add_history(line.c_str());
        shell_history.push_back(line);

        execute_line(line);
    }

    return 0;
}





