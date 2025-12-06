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

#include <readline/readline.h>
#include <readline/history.h>

using namespace std;

struct Redirection {
    int fd;
    bool append;
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
            if (c == '\'') mode = NORMAL; else cur += c;
        } else if (mode == DOUBLE_QUOTE) {
            if (c == '"') mode = NORMAL;
            else if (c == '\\') {
                if (i + 1 < line.size()) {
                    cur += line[i + 1];
                    ++i;
                }
            } else cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
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
        } else current_tokens.push_back(t);
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

static void apply_redirections(const vector<Redirection> &redirs) {
    for (const auto &r : redirs) {
        int flags = O_WRONLY | O_CREAT;
        if (r.append) flags |= O_APPEND; else flags |= O_TRUNC;
        int fd = open(r.filename.c_str(), flags, 0644);
        if (fd < 0) { perror("open"); continue; }
        if (dup2(fd, r.fd) < 0) perror("dup2");
        close(fd);
    }
}

static string find_executable(const string &cmd) {
    if (cmd.empty()) return "";
    if (cmd.find('/') != string::npos) {
        if (access(cmd.c_str(), X_OK) == 0) return cmd;
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
        if (access(full.c_str(), X_OK) == 0) return full;
    }
    return "";
}

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
    if (!getcwd(buf, sizeof(buf))) { perror("pwd"); return 1; }
    cout << buf << "\n";
    return 0;
}

static int builtin_cd(const vector<string> &argv) {
    const char *home = getenv("HOME");
    string target;
    if (argv.size() < 2) {
        if (!home) { cerr << "cd: HOME not set\n"; return 1; }
        target = home;
    } else {
        string arg = argv[1];
        if (!arg.empty() && arg[0] == '~') {
            if (!home) { cerr << "cd: HOME not set\n"; return 1; }
            if (arg.size() == 1) target = home;
            else if (arg[1] == '/') target = string(home) + arg.substr(1);
            else target = arg;
        } else target = arg;
    }
    if (chdir(target.c_str()) != 0) { perror("cd"); return 1; }
    return 0;
}

static int builtin_type(const vector<string> &argv) {
    if (argv.size() < 2) { cerr << "type: usage: type name\n"; return 1; }
    int status = 0;
    for (size_t i = 1; i < argv.size(); ++i) {
        const string &name = argv[i];
        if (is_builtin(name)) cout << name << " is a shell builtin\n";
        else {
            string path = find_executable(name);
            if (!path.empty()) cout << name << " is " << path << "\n";
            else { cerr << "type: " << name << ": not found\n"; status = 1; }
        }
    }
    return status;
}

static int builtin_history(const vector<string> &argv) {
    if (argv.size() == 1) {
        for (size_t i = 0; i < shell_history.size(); ++i) {
            cout << setw(5) << (i + 1) << "  " << shell_history[i] << "\n";
        }
        return 0;
    }
    if (argv.size() >= 3 && argv[1] == "-a") {
        const string &file = argv[2];
        int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) { perror("history -a"); return 1; }
        for (size_t i = last_history_flush_index; i < shell_history.size(); ++i) {
            const string &line = shell_history[i];
            write(fd, line.c_str(), line.size());
            write(fd, "\n", 1);
        }
        close(fd);
        last_history_flush_index = shell_history.size();
        return 0;
    }
    if (argv.size() >= 3 && argv[1] == "-w") {
        const string &file = argv[2];
        int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("history -w"); return 1; }
        for (size_t i = 0; i < shell_history.size(); ++i) {
            const string &line = shell_history[i];
            write(fd, line.c_str(), line.size());
            write(fd, "\n", 1);
        }
        close(fd);
        last_history_flush_index = shell_history.size();
        return 0;
    }
    if (argv.size() >= 3 && argv[1] == "-r") {
        const string &file = argv[2];
        int fd = open(file.c_str(), O_RDONLY);
        if (fd < 0) return 0;
        FILE *f = fdopen(fd, "r");
        if (!f) { close(fd); perror("history -r"); return 1; }
        char *line = nullptr;
        size_t len = 0;
        ssize_t nread;
        while ((nread = getline(&line, &len, f)) != -1) {
            if (nread > 0 && line[nread - 1] == '\n') line[nread - 1] = '\0';
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
    string name = cmd.argv[0];
    if (name == "echo") return builtin_echo(cmd.argv);
    if (name == "pwd") return builtin_pwd();
    if (name == "cd") return builtin_cd(cmd.argv);
    if (name == "type") return builtin_type(cmd.argv);
    if (name == "history") return builtin_history(cmd.argv);
    if (name == "exit") {
        if (in_child) _exit(0);
        exit(0);
    }
    return 0;
}

static void execute_pipeline(vector<Command> &pipeline) {
    size_t n = pipeline.size();
    if (n == 0) return;
    if (n == 1 && !pipeline[0].argv.empty() && is_builtin(pipeline[0].argv[0])) {
        int saved_stdout = dup(STDOUT_FILENO);
        int saved_stderr = dup(STDERR_FILENO);
        apply_redirections(pipeline[0].redirs);
        run_builtin(pipeline[0], false);
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdout);
        close(saved_stderr);
        return;
    }
    vector<pid_t> pids;
    vector<int> pipes_fd;
    if (n > 1) {
        pipes_fd.resize(2 * (n - 1));
        for (size_t i = 0; i < n - 1; ++i) {
            pipe(&pipes_fd[2 * i]);
        }
    }
    for (size_t i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            if (n > 1) {
                if (i > 0) dup2(pipes_fd[2 * (i - 1)], STDIN_FILENO);
                if (i < n - 1) dup2(pipes_fd[2 * i + 1], STDOUT_FILENO);
                for (int fd : pipes_fd) close(fd);
            }
            apply_redirections(pipeline[i].redirs);
            if (pipeline[i].argv.empty()) _exit(0);
            string cmd = pipeline[i].argv[0];
            if (is_builtin(cmd)) {
                run_builtin(pipeline[i], true);
                _exit(0);
            } else {
                vector<char*> argv;
                for (auto &s : pipeline[i].argv) argv.push_back((char*)s.c_str());
                argv.push_back(nullptr);
                string path = find_executable(cmd);
                if (path.empty()) {
                    cerr << cmd << ": command not found\n";
                    _exit(127);
                }
                execv(path.c_str(), argv.data());
                perror("execv");
                _exit(127);
            }
        } else pids.push_back(pid);
    }
    if (n > 1) {
        for (int fd : pipes_fd) close(fd);
    }
    for (pid_t pid : pids) waitpid(pid, nullptr, 0);
}

static void execute_line(const string &line) {
    vector<string> tokens = tokenize(line);
    if (tokens.empty()) return;
    vector<Command> pipeline = parse_pipeline(tokens);
    execute_pipeline(pipeline);
}

static char *command_generator(const char *text, int state) {
    static size_t i1, i2;
    if (state == 0) { i1 = 0; i2 = 0; }
    string prefix(text);
    while (i1 < builtin_names.size()) {
        const string &n = builtin_names[i1++];
        if (n.compare(0, prefix.size(), prefix) == 0) return strdup(n.c_str());
    }
    while (i2 < path_executables.size()) {
        const string &n = path_executables[i2++];
        if (n.compare(0, prefix.size(), prefix) == 0) return strdup(n.c_str());
    }
    return nullptr;
}

static char **shell_completion(const char *text, int start, int end) {
    (void)end;
    if (start == 0) return rl_completion_matches(text, command_generator);
    return rl_completion_matches(text, rl_filename_completion_function);
}

static void setup_signal_handlers() {
    signal(SIGINT, SIG_IGN);
}

int main() {
    build_executable_cache();
    setup_signal_handlers();
    rl_attempted_completion_function = shell_completion;
    while (true) {
        char *input = readline(shell_prompt.c_str());
        if (!input) { cout << "\n"; break; }
        string line(input);
        free(input);
        if (line.empty()) continue;
        add_history(line.c_str());
        shell_history.push_back(line);
        execute_line(line);
    }
    return 0;
}




