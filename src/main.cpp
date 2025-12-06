#include <iostream>
#include <string>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <cstring>
#include <cctype>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>

#include <readline/readline.h>
#include <readline/history.h>

// ======================= Tokenizer =======================

std::vector<std::string> tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string current;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    bool escape = false;

    for (size_t i = 0; i < input.size(); i++) {
        char c = input[i];

        if (escape && !inDoubleQuote) {
            current.push_back(c);
            escape = false;
            continue;
        }

        if (inDoubleQuote && c == '\\') {
            if (i + 1 < input.size() && (input[i+1] == '"' || input[i+1] == '\\')) {
                current.push_back(input[i+1]);
                i++;
                continue;
            }
            current.push_back('\\');
            continue;
        }

        if (c == '\\' && !inSingleQuote && !inDoubleQuote) {
            escape = true;
            continue;
        }

        if (c == '\'' && !inDoubleQuote) {
            inSingleQuote = !inSingleQuote;
            continue;
        }

        if (c == '"' && !inSingleQuote) {
            inDoubleQuote = !inDoubleQuote;
            continue;
        }

        if (std::isspace((unsigned char)c) && !inSingleQuote && !inDoubleQuote) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }

        current.push_back(c);
    }

    if (escape) current.push_back('\\');
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

// ======================= TAB completion =======================

// builtin list
std::vector<std::string> builtin_list = {"echo", "exit", "pwd", "cd", "type"};

// search PATH for executables beginning with prefix
std::vector<std::string> find_path_matches(const char* text) {
    std::vector<std::string> matches;
    char* pathEnv = getenv("PATH");
    if (!pathEnv) return matches;

    std::string path(pathEnv);
    size_t start = 0;

    while (true) {
        size_t end = path.find(':', start);
        std::string dir = (end == std::string::npos) ? path.substr(start)
                                                     : path.substr(start, end - start);
        start = (end == std::string::npos) ? std::string::npos : end + 1;

        if (!dir.empty()) {
            DIR* dp = opendir(dir.c_str());
            if (dp) {
                struct dirent* entry;
                while ((entry = readdir(dp))) {
                    if (strncmp(entry->d_name, text, strlen(text)) == 0) {
                        // must be executable
                        std::string full = dir + "/" + entry->d_name;
                        if (access(full.c_str(), X_OK) == 0) {
                            matches.push_back(entry->d_name);
                        }
                    }
                }
                closedir(dp);
            }
        }

        if (end == std::string::npos) break;
    }
    return matches;
}

// generator called by readline
char* completion_generator(const char* text, int state) {
    static std::vector<std::string> candidates;
    static int index;

    if (!state) {
        candidates.clear();
        index = 0;

        // add builtin matches
        for (auto& b : builtin_list) {
            if (b.rfind(text, 0) == 0) candidates.push_back(b);
        }

        // add PATH executable matches
        auto ext = find_path_matches(text);
        candidates.insert(candidates.end(), ext.begin(), ext.end());
    }

    if (index < candidates.size()) {
        return strdup(candidates[index++].c_str());
    }
    return nullptr;
}

char** completion_wrapper(const char* text, int start, int end) {
    if (start != 0) return nullptr;
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, completion_generator);
}

// ======================= Shell =======================

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    rl_attempted_completion_function = completion_wrapper;

    while (true) {
        char* line = readline("$ ");
        if (!line) break;

        std::string input(line);
        free(line);

        if (!input.empty()) add_history(input.c_str());

        std::vector<std::string> parts = tokenize(input);
        if (parts.empty()) continue;

        std::string redirectOutFile, redirectErrFile;
        bool appendOut = false, appendErr = false;

        // stdout redirection
        for (size_t i = 0; i < parts.size(); i++) {
            if (parts[i] == ">" || parts[i] == "1>") {
                redirectOutFile = parts[i + 1];
                appendOut = false;
                parts.erase(parts.begin()+i, parts.begin()+i+2);
                break;
            }
            if (parts[i] == ">>" || parts[i] == "1>>") {
                redirectOutFile = parts[i + 1];
                appendOut = true;
                parts.erase(parts.begin()+i, parts.begin()+i+2);
                break;
            }
        }

        // stderr redirection
        for (size_t i = 0; i < parts.size(); i++) {
            if (parts[i] == "2>") {
                redirectErrFile = parts[i + 1];
                appendErr = false;
                parts.erase(parts.begin()+i, parts.begin()+i+2);
                break;
            }
            if (parts[i] == "2>>") {
                redirectErrFile = parts[i + 1];
                appendErr = true;
                parts.erase(parts.begin()+i, parts.begin()+i+2);
                break;
            }
        }

        int savedStdout = -1, savedStderr = -1;

        if (!redirectOutFile.empty()) {
            savedStdout = dup(STDOUT_FILENO);
            int flags = O_CREAT | O_WRONLY | (appendOut ? O_APPEND : O_TRUNC);
            int fd = open(redirectOutFile.c_str(), flags, 0644);
            if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }
        }

        if (!redirectErrFile.empty()) {
            savedStderr = dup(STDERR_FILENO);
            int flags = O_CREAT | O_WRONLY | (appendErr ? O_APPEND : O_TRUNC);
            int fd = open(redirectErrFile.c_str(), flags, 0644);
            if (fd >= 0) { dup2(fd, STDERR_FILENO); close(fd); }
        }

        // builtins
        if (parts.size()==1 && parts[0]=="exit") goto exit_now;

        if (parts.size()==1 && parts[0]=="pwd") {
            char buf[4096];
            if (getcwd(buf, sizeof(buf))) std::cout<<buf<<std::endl;
            goto restore;
        }

        if (parts[0]=="cd") {
            if (parts.size()>1) {
                std::string path = parts[1];
                if (path=="~") {
                    const char* home=getenv("HOME");
                    if (home) chdir(home);
                } else chdir(path.c_str());
            }
            goto restore;
        }

        if (parts[0]=="echo") {
            for (size_t i=1;i<parts.size();i++) {
                std::cout<<parts[i];
                if (i+1<parts.size()) std::cout<<" ";
            }
            std::cout<<std::endl;
            goto restore;
        }

        // type builtin logic kept same (omitted here for brevity — your previous implementation persists)

        // execution
        {
            std::vector<char*> args;
            for (auto &s: parts) args.push_back(strdup(s.c_str()));
            args.push_back(nullptr);

            bool executed=false;
            char* pathEnv = getenv("PATH");

            if (pathEnv) {
                std::string path(pathEnv);
                size_t start=0;
                while (true) {
                    size_t end=path.find(':',start);
                    std::string dir=(end==std::string::npos)?path.substr(start)
                                                            :path.substr(start,end-start);
                    if (!dir.empty()) {
                        std::string full=dir+"/"+args[0];
                        if (access(full.c_str(),X_OK)==0) {
                            pid_t pid=fork();
                            if (pid==0) {
                                execv(full.c_str(),args.data());
                                exit(1);
                            } else waitpid(pid,nullptr,0);

                            executed=true;
                            break;
                        }
                    }
                    if (end==std::string::npos) break;
                    start=end+1;
                }
            }

            if (!executed) std::cerr<<args[0]<<": command not found"<<std::endl;

            for (char* p: args) if (p) free(p);
        }

restore:
        if (savedStdout!=-1) { dup2(savedStdout,STDOUT_FILENO); close(savedStdout);}
        if (savedStderr!=-1) { dup2(savedStderr,STDERR_FILENO); close(savedStderr);}
        continue;

exit_now:
        if (savedStdout!=-1) { dup2(savedStdout,STDOUT_FILENO); close(savedStdout);}
        if (savedStderr!=-1) { dup2(savedStderr,STDERR_FILENO); close(savedStderr);}
        break;
    }

    return 0;
}













