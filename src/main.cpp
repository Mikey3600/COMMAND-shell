#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <readline/readline.h>
#include <readline/history.h>
using namespace std;
// --------------------------- Helpers ---------------------------
static void perror_prefix(const string &ctx) {
    cerr << ctx << ": " << strerror(errno) << "\n";
}
struct FdGuard {
    int fd{-1};
    FdGuard() = default;
    FdGuard(int fd_): fd(fd_) {}
    ~FdGuard() { if (fd >= 0) close(fd); }
    int release() { int t = fd; fd = -1; return t; }
};
// --------------------------- Tokenizer ---------------------------
class Tokenizer {
public:
    // Splits input into tokens respecting quotes and escapes.
    // Returns empty error on success, or an error message.
    static string tokenize(const string &input, vector<string> &out) {
        out.clear();
        string cur;
        bool in_squote = false, in_dquote = false;
        for (size_t i = 0; i < input.size(); ++i) {
            char c = input[i];
            if (in_squote) {
                if (c == '\'') { in_squote = false; }
                else cur.push_back(c);
                continue;
            }
            if (in_dquote) {
                if (c == '\\' && i + 1 < input.size()) {
                    char next = input[i+1];
                    if (next == '"' || next == '\\' || next == '$' || next == '`') {
                        cur.push_back(next); ++i; continue;
                    }
                    // keep backslash literal for others
                } else if (c == '"') { in_dquote = false; continue; }
                cur.push_back(c);
                continue;
            }
            if (c == '\\' && i + 1 < input.size()) {
                cur.push_back(input[i+1]); ++i; continue;
            }
            if (c == '\'') { in_squote = true; continue; }
            if (c == '"') { in_dquote = true; continue; }
            if (isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
                continue;
            }
            // keep meta tokens separate: | > >> 2> 2>> &
            if (c == '|' || c == '&') {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
                string s(1, c); out.push_back(s); continue;
            }
            if (c == '>') {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
                if (i + 1 < input.size() && input[i+1] == '>') { out.push_back(">>"); ++i; }
                else out.push_back(">");
                continue;
            }
            if (c == '2') {
                if (i + 1 < input.size() && input[i+1] == '>') {
                    if (!cur.empty()) { out.push_back(cur); cur.clear(); }
                    i++;
                    if (i + 1 < input.size() && input[i+1] == '>') { out.push_back("2>>"); ++i; }
                    else out.push_back("2>");
                    continue;
                }
            }
            cur.push_back(c);
        }
        if (in_squote || in_dquote) return "Unclosed quote";
        if (!cur.empty()) out.push_back(cur);
        return "";
    }
};
// --------------------------- Command AST ---------------------------
struct Command {
    vector<string> argv;
    string stdout_file;
    bool stdout_append = false;
    string stderr_file;
    bool stderr_append = false;
    bool background = false;
};
// parse a sequence of tokens into a pipeline (vector<Command>).
// supports: |, >, >>, 2>, 2>>, & (background only on last command)
class Parser {
public:
    static bool is_redir_token(const string &t) {
        return t == ">" || t == ">>" || t == "2>" || t == "2>>";
    }
    static string parse_pipeline(const vector<string> &tokens, vector<Command> &out) {
        out.clear();
        Command cur;
        size_t i = 0, n = tokens.size();
        while (i < n) {
            const string &tok = tokens[i];
            if (tok == "|") {
                if (cur.argv.empty()) return "syntax error near '|'";
                out.push_back(cur);
                cur = Command();
                ++i; continue;
            }
            if (tok == "&") {
                // background: only allowed as final token (or before trailing spaces)
                if (i != n - 1) return "syntax error: '&' must be at pipeline end";
                cur.background = true;
                ++i; continue;
            }
            if (is_redir_token(tok)) {
                if (i + 1 >= n) return "syntax error: missing filename after redirection";
                const string &file = tokens[i+1];
                if (tok == ">" ) { cur.stdout_file = file; cur.stdout_append = false; }
                else if (tok == ">>") { cur.stdout_file = file; cur.stdout_append = true; }
                else if (tok == "2>") { cur.stderr_file = file; cur.stderr_append = false; }
                else if (tok == "2>>") { cur.stderr_file = file; cur.stderr_append = true; }
                i += 2; continue;
            }
            // normal argument
            cur.argv.push_back(tok);
            ++i;
        }
        if (!cur.argv.empty() || !cur.stdout_file.empty() || !cur.stderr_file.empty()) {
            out.push_back(cur);
        }
        if (out.empty()) return "empty command";
        return "";
    }
};
// --------------------------- Builtins ---------------------------
class Builtins {
public:
    static bool is_builtin(const string &cmd) {
        return cmd == "cd" || cmd == "exit" || cmd == "pwd" || cmd == "echo" || cmd == "history" || cmd == "type";
    }
    // execute builtin in the current process when needed (cd/exit/pwd/history)
    static int execute_in_shell(const Command &c, vector<string> &history, const string &histfile) {
        if (c.argv.empty()) return 0;
        const string &cmd = c.argv[0];
        if (cmd == "cd") {
            string target;
            if (c.argv.size() == 1) {
                const char *home = getenv("HOME");
                if (!home) { cerr << "cd: HOME not set\n"; return 1; }
                target = home;
            } else {
                target = c.argv[1];
                if (target.size() > 0 && target[0] == '~') {
                    const char *home = getenv("HOME");
                    if (!home) { cerr << "cd: HOME not set\n"; return 1; }
                    target = string(home) + target.substr(1);
                }
            }
            if (chdir(target.c_str()) != 0) {
                perror_prefix("cd");
                return 1;
            }
            return 0;
        }
        if (cmd == "exit") {
            int code = 0;
            if (c.argv.size() > 1) code = stoi(c.argv[1]);
            // persist history if set
            if (!histfile.empty()) {
                // use readline write_history if possible
                write_history(histfile.c_str());
            }
            exit(code);
        }
        if (cmd == "pwd") {
            char buf[4096];
            if (getcwd(buf, sizeof(buf))) {
                cout << buf << "\n"; return 0;
            } else { perror_prefix("pwd"); return 1; }
        }
        if (cmd == "echo") {
            for (size_t i = 1; i < c.argv.size(); ++i) {
                if (i > 1) cout << " ";
                cout << c.argv[i];
            }
            cout << "\n";
            return 0;
        }
        if (cmd == "history") {
            // simple history listing
            for (size_t i = 0; i < history.size(); ++i) {
                cout << " " << (i+1) << " " << history[i] << "\n";
            }
            return 0;
        }
        if (cmd == "type") {
            for (size_t i = 1; i < c.argv.size(); ++i) {
                if (is_builtin(c.argv[i])) {
                    cout << c.argv[i] << " is a shell builtin\n";
                } else {
                    string p = find_in_path(c.argv[i]);
                    if (!p.empty()) cout << c.argv[i] << " is " << p << "\n";
                    else cout << c.argv[i] << ": not found\n";
                }
            }
            return 0;
        }
        return -1; // not a handled builtin
    }
    // helper for PATH lookup
    static string find_in_path(const string &cmd) {
        if (cmd.find('/') != string::npos) {
            if (access(cmd.c_str(), X_OK) == 0) return cmd;
            return "";
        }
        const char *p = getenv("PATH");
        if (!p) return "";
        string path(p);
        stringstream ss(path);
        string dir;
        while (getline(ss, dir, ':')) {
            string f = dir + "/" + cmd;
            if (access(f.c_str(), X_OK) == 0) return f;
        }
        return "";
    }
};
// --------------------------- Executor ---------------------------
class Executor {
public:
    // execute pipeline of commands
    static int execute_pipeline(vector<Command> &pipeline) {
        size_t n = pipeline.size();
        // If single command and it's a shell builtin that must run in-shell (cd/exit/pwd/history), run it here.
        if (n == 1 && Builtins::is_builtin(pipeline[0].argv[0])) {
            // run builtin in shell for those that require it
            // (cd/exit/pwd/history/type/echo we handle in shell)
            // But in pipeline context (not here), builtins should be forked.
            // We'll let caller decide; here assume caller already determined context.
            return Builtins::execute_in_shell(pipeline[0], s_history_ref(), s_histfile_ref());
        }
        // Create pipes between processes
        vector<int> pipes; // store pipe fds pairs [r0,w0, r1,w1,...]
        for (size_t i = 0; i + 1 < n; ++i) {
            int fds[2];
            if (pipe(fds) != 0) { perror_prefix("pipe"); return 1; }
            pipes.push_back(fds[0]); pipes.push_back(fds[1]);
        }
        vector<pid_t> pids;
        for (size_t i = 0; i < n; ++i) {
            pid_t pid = fork();
            if (pid < 0) { perror_prefix("fork"); cleanup_pipes(pipes); return 1; }
            if (pid == 0) {
                // child
                // setup stdin
                if (i > 0) {
                    int read_end = pipes[(i-1)*2];
                    if (dup2(read_end, STDIN_FILENO) < 0) { perror_prefix("dup2 stdin"); _exit(1); }
                }
                // setup stdout
                if (i + 1 < n) {
                    int write_end = pipes[i*2 + 1];
                    if (dup2(write_end, STDOUT_FILENO) < 0) { perror_prefix("dup2 stdout"); _exit(1); }
                }
                // close all pipe fds in child
                for (int fd : pipes) close(fd);
                // setup redirections for this command
                if (!pipeline[i].stdout_file.empty()) {
                    int flags = O_WRONLY | O_CREAT | (pipeline[i].stdout_append ? O_APPEND : O_TRUNC);
                    int fd = open(pipeline[i].stdout_file.c_str(), flags, 0644);
                    if (fd < 0) { perror_prefix("open stdout file"); _exit(1); }
                    if (dup2(fd, STDOUT_FILENO) < 0) { perror_prefix("dup2 stdout file"); _exit(1); }
                    close(fd);
                }
                if (!pipeline[i].stderr_file.empty()) {
                    int flags = O_WRONLY | O_CREAT | (pipeline[i].stderr_append ? O_APPEND : O_TRUNC);
                    int fd = open(pipeline[i].stderr_file.c_str(), flags, 0644);
                    if (fd < 0) { perror_prefix("open stderr file"); _exit(1); }
                    if (dup2(fd, STDERR_FILENO) < 0) { perror_prefix("dup2 stderr file"); _exit(1); }
                    close(fd);
                }
                // If this is a builtin that doesn't need shell state, execute in child.
                if (!pipeline[i].argv.empty() && Builtins::is_builtin(pipeline[i].argv[0])) {
                    // Execute builtin that is safe in child (echo, type)
                    if (pipeline[i].argv[0] == "echo" || pipeline[i].argv[0] == "type") {
                        Builtins::execute_in_shell(pipeline[i], s_history_ref(), s_histfile_ref());
                        _exit(0);
                    }
                    // other builtins that require shell state should've been handled in parent when single.
                }
                // Prepare argv for execvp
                vector<char*> argv;
                for (auto &a : pipeline[i].argv) argv.push_back(const_cast<char*>(a.c_str()));
                argv.push_back(nullptr);
                // Find program: let execvp search PATH
                execvp(argv[0], argv.data());
                // if execvp returns, it failed
                cerr << argv[0] << ": " << strerror(errno) << "\n";
                _exit(127);
            } else {
                // parent
                pids.push_back(pid);
            }
        }
        // parent: close pipes
        for (int fd : pipes) close(fd);
        // wait for children (unless background flag on last command)
        bool background = pipeline.back().background;
        int last_status = 0;
        if (!background) {
            for (pid_t pid : pids) {
                int status;
                if (waitpid(pid, &status, 0) < 0) { perror_prefix("waitpid"); }
                if (WIFEXITED(status)) last_status = WEXITSTATUS(status);
            }
        } else {
            // Background: don't wait; just print job info
            cout << "[" << pids.size() << "] " << pids.back() << "\n";
        }
        return last_status;
    }
    // expose a static reference to shell history and histfile for builtins used in children
    static vector<string>& s_history_ref() {
        static vector<string> hist;
        return hist;
    }
    static string& s_histfile_ref() {
        static string path;
        return path;
    }
    static void cleanup_pipes(vector<int>& pipes) {
        for (int fd : pipes) if (fd >= 0) close(fd);
    }
};
// --------------------------- Signal handling ---------------------------
static void sigchld_handler(int) {
    // non-blocking reaper - loop to reap all children
    while (true) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;
        // optionally print notification about background job completion
    }
}
// --------------------------- Simple completion (readline hooks) ---------------------------
static char* completion_generator(const char* text, int state) {
    static vector<string> matches;
    static size_t idx;
    if (state == 0) {
        matches.clear(); idx = 0;
        string t(text);
        // builtins
        vector<string> builtins = {"cd","exit","pwd","echo","history","type"};
        for (auto &b : builtins) if (b.find(t) == 0) matches.push_back(b);
        // PATH executables - simple scan
        const char *p = getenv("PATH");
        if (p) {
            string path(p);
            stringstream ss(path);
            string dir;
            while (getline(ss, dir, ':')) {
                DIR *d = opendir(dir.c_str());
                if (!d) continue;
                struct dirent *ent;
                while ((ent = readdir(d)) != nullptr) {
                    string name = ent->d_name;
                    if (name.find(t) == 0 && name != "." && name != "..") {
                        string full = dir + "/" + name;
                        if (access(full.c_str(), X_OK) == 0) {
                            if (find(matches.begin(), matches.end(), name) == matches.end())
                                matches.push_back(name);
                        }
                    }
                }
                closedir(d);
            }
        }
    }
    if (idx < matches.size()) return strdup(matches[idx++].c_str());
    return nullptr;
}
static char** completion_wrapper(const char* text, int start, int end) {
    rl_attempted_completion_over = 1;
    if (start == 0) return rl_completion_matches(text, completion_generator);
    return nullptr;
}
// --------------------------- Top-level Shell ---------------------------
class Shell {
    vector<string> history;
    string histfile;
public:
    Shell() {
        // load readline hook
        rl_attempted_completion_function = completion_wrapper;
        const char *hf = getenv("HISTFILE");
        if (hf) histfile = hf;
        else {
            // optional fallback: ~/.my_shell_history
            const char *home = getenv("HOME");
            if (home) histfile = string(home) + "/.my_shell_history";
        }
        // try to load history via readline
        if (!histfile.empty()) read_history(histfile.c_str());
        // also populate internal history vector
        HIST_ENTRY **h = history_list();
        if (h) {
            for (int i = 0; h[i]; ++i) history.push_back(h[i]->line);
        }
        // give child code access
        Executor::s_history_ref() = history;
        Executor::s_histfile_ref() = histfile;
        // install SIGCHLD handler
        struct sigaction sa{};
        sa.sa_handler = sigchld_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa, nullptr);
    }
    void repl() {
        while (true) {
            char *line_raw = readline("$ ");
            if (!line_raw) { cout << "\n"; flush_history(); break; }
            string line(line_raw);
            free(line_raw);
            if (line.empty()) continue;
            add_history(line.c_str());
            history.push_back(line);
            vector<string> tokens;
            string err = Tokenizer::tokenize(line, tokens);
            if (!err.empty()) { cerr << "Error: " << err << "\n"; continue; }
            vector<Command> pipeline;
            string perr = Parser::parse_pipeline(tokens, pipeline);
            if (!perr.empty()) { cerr << "Parse error: " << perr << "\n"; continue; }
            // If single command and builtin that must run inside shell (cd/exit/pwd/history/type), execute here
            if (pipeline.size() == 1 && Builtins::is_builtin(pipeline[0].argv[0])) {
                // builtin that needs shell state (cd/exit/pwd/history/type/echo) - execute inside process
                int r = Builtins::execute_in_shell(pipeline[0], history, histfile);
                (void) r;
                continue;
            }
            // Otherwise execute pipeline (forks children as needed)
            int status = Executor::execute_pipeline(pipeline);
            (void) status;
        }
    }
    void flush_history() {
        if (!histfile.empty()) {
            if (write_history(histfile.c_str()) != 0) {
                // if write_history fails, fallback to manual
                FILE *f = fopen(histfile.c_str(), "a");
                if (f) {
                    for (auto &h : history) {
                        fprintf(f, "%s\n", h.c_str());
                    }
                    fclose(f);
                }
            }
        }
    }
};
// --------------------------- Main ---------------------------
int main(int argc, char **argv) {
    Shell shell;
    shell.repl();
    return 0;
}
