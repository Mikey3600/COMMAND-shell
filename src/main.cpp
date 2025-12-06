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
#include <algorithm>

#include <readline/readline.h>
#include <readline/history.h>

// ====== Tokenizer (handles quotes & escapes) ======

std::vector<std::string> tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string current;
    bool inSingleQuote=false, inDoubleQuote=false, escape=false;

    for(size_t i=0;i<input.size();i++) {
        char c=input[i];

        if (escape && !inDoubleQuote) {
            current.push_back(c);
            escape=false;
            continue;
        }

        if (inDoubleQuote && c=='\\') {
            if (i+1<input.size() && (input[i+1]=='"'||input[i+1]=='\\')) {
                current.push_back(input[i+1]);
                i++;
                continue;
            }
            current.push_back('\\');
            continue;
        }

        if (c=='\\' && !inSingleQuote && !inDoubleQuote) {
            escape=true;
            continue;
        }

        if (c=='\'' && !inDoubleQuote) { inSingleQuote=!inSingleQuote; continue; }
        if (c=='"' && !inSingleQuote)  { inDoubleQuote=!inDoubleQuote; continue; }

        if (std::isspace((unsigned char)c) && !inSingleQuote && !inDoubleQuote) {
            if (!current.empty()) { tokens.push_back(current); current.clear(); }
            continue;
        }

        current.push_back(c);
    }

    if (escape) current.push_back('\\');
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

// ====== Search PATH for matching executables ======

std::vector<std::string> find_path_matches(const std::string& prefix) {
    std::vector<std::string> matches;
    char* pathEnv=getenv("PATH");
    if (!pathEnv) return matches;

    std::string path(pathEnv);
    size_t start=0;

    while(true){
        size_t end=path.find(':',start);
        std::string dir=(end==std::string::npos)?path.substr(start)
                                                :path.substr(start,end-start);
        start=(end==std::string::npos)?std::string::npos:end+1;

        if (!dir.empty()) {
            DIR* dp=opendir(dir.c_str());
            if (dp){
                struct dirent* ent;
                while((ent=readdir(dp))){
                    if (strncmp(ent->d_name, prefix.c_str(), prefix.size())==0) {
                        std::string full=dir+"/"+ent->d_name;
                        if(access(full.c_str(),X_OK)==0)
                            matches.push_back(ent->d_name);
                    }
                }
                closedir(dp);
            }
        }
        if (end==std::string::npos) break;
    }
    return matches;
}

// ====== TAB Logic State ======
static std::string last_prefix;
static int tab_press_count = 0;

// ====== Custom key handler ======

int tab_completion_handler(int count, int key) {
    char* buf = rl_line_buffer;
    std::string input(buf);

    std::string prefix=input;
    if(prefix!=last_prefix) {
        tab_press_count=0;
        last_prefix=prefix;
    }

    tab_press_count++;

    std::vector<std::string> matches=find_path_matches(prefix);
    std::sort(matches.begin(), matches.end());

    if(matches.empty()) {
        rl_insert_text("\x07");
        return 0;
    }

    if(tab_press_count==1) {
        rl_insert_text("\x07");
        return 0;
    }

    std::cout<<std::endl;
    for(size_t i=0;i<matches.size();i++){
        std::cout<<matches[i];
        if(i+1<matches.size()) std::cout<<"  ";
    }
    std::cout<<std::endl;

    rl_replace_line(prefix.c_str(),1);
    rl_point = prefix.size();
    rl_redisplay();

    return 0;
}

// ====== Bind TAB ======

void setup_tab() {
    rl_bind_key('\t', tab_completion_handler);
}

// ====== MAIN SHELL ======

int main(){
    std::cout<<std::unitbuf;
    std::cerr<<std::unitbuf;

    setup_tab();

    while(true){
        char* line=readline("$ ");
        if(!line) break;

        std::string input(line);
        free(line);

        if(!input.empty()) add_history(input.c_str());

        std::vector<std::string> parts=tokenize(input);
        if(parts.empty()) continue;

        std::string redirectOutFile, redirectErrFile;
        bool appendOut=false, appendErr=false;

        for(size_t i=0;i<parts.size();i++){
            if(parts[i]==">"||parts[i]=="1>"){
                redirectOutFile=parts[i+1];
                appendOut=false;
                parts.erase(parts.begin()+i,parts.begin()+i+2);
                break;
            }
            if(parts[i]==">>"||parts[i]=="1>>"){
                redirectOutFile=parts[i+1];
                appendOut=true;
                parts.erase(parts.begin()+i,parts.begin()+i+2);
                break;
            }
        }

        for(size_t i=0;i<parts.size();i++){
            if(parts[i]=="2>"){
                redirectErrFile=parts[i+1];
                appendErr=false;
                parts.erase(parts.begin()+i,parts.begin()+i+2);
                break;
            }
            if(parts[i]=="2>>"){
                redirectErrFile=parts[i+1];
                appendErr=true;
                parts.erase(parts.begin()+i,parts.begin()+i+2);
                break;
            }
        }

        int savedStdout=-1, savedStderr=-1;

        if(!redirectOutFile.empty()){
            savedStdout=dup(STDOUT_FILENO);
            int flags=O_CREAT|O_WRONLY|(appendOut?O_APPEND:O_TRUNC);
            int fd=open(redirectOutFile.c_str(),flags,0644);
            if(fd>=0){dup2(fd,STDOUT_FILENO);close(fd);}
        }

        if(!redirectErrFile.empty()){
            savedStderr=dup(STDERR_FILENO);
            int flags=O_CREAT|O_WRONLY|(appendErr?O_APPEND:O_TRUNC);
            int fd=open(redirectErrFile.c_str(),flags,0644);
            if(fd>=0){dup2(fd,STDERR_FILENO);close(fd);}
        }

        if(parts.size()==1 && parts[0]=="exit") goto restore_exit;

        if(parts.size()==1 && parts[0]=="pwd"){
            char buf[4096];
            if(getcwd(buf,sizeof(buf))) std::cout<<buf<<std::endl;
            goto restore;
        }

        if(parts[0]=="cd"){
            if(parts.size()>1){
                std::string path=parts[1];
                if(path=="~"){
                    const char* home=getenv("HOME");
                    if(home && chdir(home)!=0)
                        std::cerr<<"cd: "<<home<<": No such file or directory"<<std::endl;
                } else {
                    if(chdir(path.c_str())!=0)
                        std::cerr<<"cd: "<<path<<": No such file or directory"<<std::endl;
                }
            }
            goto restore;
        }

        if(parts[0]=="echo"){
            for(size_t i=1;i<parts.size();i++){
                std::cout<<parts[i];
                if(i+1<parts.size()) std::cout<<" ";
            }
            std::cout<<std::endl;
            goto restore;
        }

        if(parts[0]=="type"){
            if(parts.size()>1){
                std::string t=parts[1];
                if(t=="echo"||t=="exit"||t=="cd"||t=="pwd"||t=="type")
                    std::cout<<t<<" is a shell builtin"<<std::endl;
                else{
                    char* pathEnv=getenv("PATH");
                    bool found=false;
                    if(pathEnv){
                        std::string path(pathEnv);
                        size_t start=0;
                        while(true){
                            size_t end=path.find(':',start);
                            std::string dir=(end==std::string::npos)?path.substr(start)
                                                                    :path.substr(start,end-start);
                            if(!dir.empty()){
                                std::string full=dir+"/"+t;
                                if(access(full.c_str(),X_OK)==0){
                                    std::cout<<t<<" is "<<full<<std::endl;
                                    found=true;
                                    break;
                                }
                            }
                            if(end==std::string::npos) break;
                            start=end+1;
                        }
                    }
                    if(!found) std::cerr<<t<<": not found"<<std::endl;
                }
            }
            goto restore;
        }

        {
            std::vector<char*> args;
            for(auto&s:parts) args.push_back(strdup(s.c_str()));
            args.push_back(nullptr);

            char* cmd=args[0];
            bool executed=false;
            char* pathEnv=getenv("PATH");

            if(pathEnv){
                std::string path(pathEnv);
                size_t start=0;
                while(true){
                    size_t end=path.find(':',start);
                    std::string dir=(end==std::string::npos)?path.substr(start)
                                                            :path.substr(start,end-start);
                    if(!dir.empty()){
                        std::string full=dir+"/"+cmd;
                        if(access(full.c_str(),X_OK)==0){
                            pid_t pid=fork();
                            if(pid==0){ execv(full.c_str(),args.data()); exit(1); }
                            else waitpid(pid,nullptr,0);
                            executed=true;
                            break;
                        }
                    }
                    if(end==std::string::npos) break;
                    start=end+1;
                }
            }

            if(!executed)
                std::cerr<<cmd<<": command not found"<<std::endl;

            for(char*p:args) if(p) free(p);
        }

restore:
        if(savedStdout!=-1) {dup2(savedStdout,STDOUT_FILENO);close(savedStdout);}
        if(savedStderr!=-1) {dup2(savedStderr,STDERR_FILENO);close(savedStderr);}
        continue;

restore_exit:
        if(savedStdout!=-1) {dup2(savedStdout,STDOUT_FILENO);close(savedStdout);}
        if(savedStderr!=-1) {dup2(savedStderr,STDERR_FILENO);close(savedStderr);}
        break;
    }
}
















