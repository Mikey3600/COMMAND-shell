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
static void setup_signal_handlers();
static vector<string> get_path_executables_matching(const string &prefix);
static string longest_common_prefix(const vector<string> &v);
static char **shell_completion(const char *text, int start, int end);

/* ---------- Tokenizer ---------- */
static vector<string> tokenize(const string &line) {
    vector<string> tokens;
    string cur;
    enum Mode { NORMAL, SINGLE_QUOTE, DOUBLE_QUOTE };
    Mode mode = NORMAL;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];

        if (mode == NORMAL) {
            if (c == '\\') {
                if (i + 1 < line.size()) cur += line[++i];
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
            } else if (c == '\\' && i + 1 < line.size()) {
                cur += line[++i];
            } else {
                cur += c;
            }
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

/* ---------- Parser ---------- */
static vector<Command> parse_pipeline(const vector<string> &tokens) {
    vector<Command> pipeline;
    vector<string> current;

    auto flush_command = [&]() {
        if (current.empty()) return;
        Command cmd;
        size_t i = 0;
        while (i < current.size()) {
            const string &tok = current[i];
            if ((tok == "1" || tok == "2") &&
                i + 2 < current.size() &&
                (current[i + 1] == ">" || current[i + 1] == ">>")) {
                Redirection r;
                r.fd = (tok == "1") ? 1 : 2;
                r.append = (current[i + 1] == ">>");
                r.filename = current[i + 2];
                cmd.redirs.push_back(r);
                i += 3;
            } else if ((tok == ">" || tok == ">>") && i + 1 < current.size()) {
                Redirection r;
                r.fd = 1;
                r.append = (tok == ">>");
                r.filename = current[i + 1];
                cmd.redirs.push_back(r);
                i += 2;
            } else {
                cmd.argv.push_back(tok);
                ++i;
            }
        }
        pipeline.push_back(cmd);
        current.clear();
    };

    for (const string &t : tokens) {
        if (t == "|") {
            flush_command();
        } else {
            current.push_back(t);
        }
    }
    flush_command();
    return pipeline;
}

/* ---------- Redirection ---------- */
static void apply_redirections(const vector<Redirection> &redirs) {
    for (const auto &r : redirs) {
        int flags = O_WRONLY | O_CREAT | (r.append ? O_APPEND : O_TRUNC);
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

/* ---------- PATH lookup ---------- */
static string find_executable(const string &cmd) {
    if (cmd.empty()) return "";
    if (cmd.find('/') != string::npos) {
        if (access(cmd.c_str(), X_OK) == 0) return cmd;
        return "";
    }
    const char *path_env = getenv("PATH");
    if (!path_env) return "";
    string path(path_env);
    stringstream ss(path);
    string dir;
    while (getline(ss, dir, ':')) {
        if (dir.empty()) dir = ".";
        string full = dir + "/" + cmd;
        if (access(full.c_str(), X_OK) == 0) return full;
    }
    return "";
}

/* ---------- Builtins ---------- */
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
    if (!getcwd(buf, sizeof(buf))) {
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
        const string &arg = argv[1];
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
                target = arg; // ~user style, leave unchanged
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
    if (argv.size() == 1) {
        for (size_t i = 0; i < shell_history.size(); ++i) {
            cout << setw(5) << (i + 1) << "  " << shell_history[i] << "\n";
        }
        return 0;
    }

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

    if (argv.size() >= 3 && argv[1] == "-a") {
        const string &file = argv[2];
        int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("history -a");
            return 1;
        }
        for (size_t i = last_history_flush_index; i < shell_history.size(); ++i) {
            const string &line = shell_history[i];
            if (write(fd, line.c_str(), line.size()) < 0) perror("write");
            if (write(fd, "\n", 1) < 0) perror("write");
        }
        close(fd);
        last_history_flush_index = shell_history.size();
        return 0;
    }

    if (argv.size() >= 3 && argv[1] == "-w") {
        const string &file = argv[2];
        int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("history -w");
            return 1;
        }
        for (const string &line : shell_history) {
            if (write(fd, line.c_str(), line.size()) < 0) perror("write");
            if (write(fd, "\n", 1) < 0) perror("write");
        }
        close(fd);
        last_history_flush_index = shell_history.size();
        return 0;
    }

    if (argv.size() >= 3 && argv[1] == "-r") {
        const string &file = argv[2];
        int fd = open(file.c_str(), O_RDONLY);
        if (fd < 0) return 0; // missing file is not an error here
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
    const string &name = cmd.argv[0];
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

/* ---------- Execution ---------- */
static void execute_pipeline(vector<Command> &pipeline) {
    size_t n = pipeline.size();
    if (n == 0) return;

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
            signal(SIGINT, SIG_DFL);
            if (n > 1) {
                if (i > 0) {
                    dup2(pipes_fd[2 * (i - 1)], STDIN_FILENO);
                }
                if (i < n - 1) {
                    dup2(pipes_fd[2 * i + 1], STDOUT_FILENO);
                }
                for (int fd : pipes_fd) close(fd);
            }
            apply_redirections(pipeline[i].redirs);
            if (pipeline[i].argv.empty()) _exit(0);
            if (is_builtin(pipeline[i].argv[0])) {
                run_builtin(pipeline[i], true);
                _exit(0);
            }
            vector<char*> argv;
            for (const string &s : pipeline[i].argv) argv.push_back(const_cast<char*>(s.c_str()));
            argv.push_back(nullptr);
            string path = find_executable(pipeline[i].argv[0]);
            if (path.empty()) {
                cerr << pipeline[i].argv[0] << ": command not found\n";
                _exit(127);
            }
            execv(path.c_str(), argv.data());
            perror("execv");
            _exit(127);
        } else {
            pids.push_back(pid);
        }
    }
    if (n > 1) {
        for (int fd : pipes_fd) close(fd);
    }
    for (pid_t pid : pids) {
        waitpid(pid, nullptr, 0);
    }
}

static void execute_line(const string &line) {
    vector<string> tokens = tokenize(line);
    if (tokens.empty()) return;
    vector<Command> pipeline = parse_pipeline(tokens);
    execute_pipeline(pipeline);
}

/* ---------- PATH Autocomplete ---------- */
static vector<string> get_path_executables_matching(const string &prefix) {
    vector<string> matches;
    const char *path_env = getenv("PATH");
    if (!path_env) return matches;
    string path(path_env);
    stringstream ss(path);
    string dir;
    while (getline(ss, dir, ':')) {
        if (dir.empty()) dir = ".";
        DIR *dp = opendir(dir.c_str());
        if (!dp) continue;
        dirent *entry;
        while ((entry = readdir(dp)) != nullptr) {
            string name(entry->d_name);
            if (name.rfind(prefix, 0) == 0) {
                string full = dir + "/" + name;
                if (access(full.c_str(), X_OK) == 0) {
                    matches.push_back(name);
                }
            }
        }
        closedir(dp);
    }
    return matches;
}

static string longest_common_prefix(const vector<string> &v) {
    if (v.empty()) return "";
    string p = v[0];
    for (size_t i = 1; i < v.size(); ++i) {
        size_t j = 0;
        while (j < p.size() && j < v[i].size() && p[j] == v[i][j]) ++j;
        p = p.substr(0, j);
        if (p.empty()) break;
    }
    return p;
}

static char **shell_completion(const char *text, int start, int end) {
    (void)start;
    (void)end;
    vector<string> matches;
    string prefix(text);

    for (const string &b : builtin_names) {
        if (b.rfind(prefix, 0) == 0) matches.push_back(b);
    }
    vector<string> execs = get_path_executables_matching(prefix);
    matches.insert(matches.end(), execs.begin(), execs.end());

    if (matches.empty()) return nullptr;

    if (matches.size() == 1) {
        string add = matches[0].substr(prefix.size());
        add += " ";
        rl_insert_text(add.c_str());
        return nullptr;
    }

    string lcp = longest_common_prefix(matches);
    if (!lcp.empty() && lcp != prefix) {
        string add = lcp.substr(prefix.size());
        add += " ";
        rl_insert_text(add.c_str());
        return nullptr;
    }

    char **res = (char **)malloc((matches.size() + 1) * sizeof(char *));
    for (size_t i = 0; i < matches.size(); ++i) {
        res[i] = strdup(matches[i].c_str());
    }
    res[matches.size()] = nullptr;
    return res;
}

/* ---------- Signals ---------- */
static void setup_signal_handlers() {
    signal(SIGINT, SIG_IGN);
}

/* ---------- Main ---------- */
int main() {
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
        if (line.empty()) continue;
        add_history(line.c_str());
        shell_history.push_back(line);
        execute_line(line);
    }
    return 0;
}










