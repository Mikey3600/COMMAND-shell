#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <map>
#include <fstream>

#include <readline/readline.h>
#include <readline/history.h>

// ======================= OUR OWN HISTORY =======================
static std::vector<std::string> g_history;
static std::map<std::string,int> g_history_flush_index;

// ======================= Tokenizer =======================
std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> t;
    std::string cur;
    bool sq=false, dq=false;

    for(size_t i=0;i<s.size();i++){
        char c=s[i];

        if(c=='\\'&&!sq){
            if(i+1<s.size())cur+=s[++i];
            else cur+='\\';
            continue;
        }
        if(c=='\''&&!dq){sq=!sq;continue;}
        if(c=='"'&&!sq){dq=!dq;continue;}

        if(std::isspace((unsigned char)c)&&!sq&&!dq){
            if(!cur.empty()){t.push_back(cur);cur.clear();}
        } else cur+=c;
    }
    if(!cur.empty())t.push_back(cur);
    return t;
}

// ======================= PATH lookup =======================
bool find_in_path(const std::string& name, std::string& path){
    const char* p=getenv("PATH");
    if(!p)return false;
    std::string env=p;
    size_t pos=0;
    while(pos<env.size()){
        size_t next=env.find(':',pos);
        std::string dir=(next==std::string::npos?env.substr(pos):env.substr(pos,next-pos));
        if(!dir.empty()){
            std::string full=dir+"/"+name;
            if(access(full.c_str(),X_OK)==0){
                path=full;
                return true;
            }
        }
        pos=(next==std::string::npos?env.size():next+1);
    }
    return false;
}

// ======================= History Handling =======================

void history_append_file(const std::string& file){
    int start=0;
    if(g_history_flush_index.count(file))
        start=g_history_flush_index[file];

    std::ofstream out(file, std::ios::app);
    if(!out)return;

    for(int i=start;i<(int)g_history.size();i++)
        out<<g_history[i]<<'\n';

    g_history_flush_index[file]=g_history.size();
}

void history_write_file(const std::string& file){
    std::ofstream out(file, std::ios::trunc);
    if(!out)return;

    for(const auto& cmd: g_history)
        out<<cmd<<'\n';
}

void history_read_file(const std::string& file){
    std::ifstream in(file);
    if(!in)return;
    std::string line;
    while(std::getline(in,line)){
        if(!line.empty())
            g_history.push_back(line);
    }
}

// ======================= Builtin Execution =======================
void exec_builtin_child(const std::vector<std::string>& args){
    const std::string& cmd=args[0];

    if(cmd=="echo"){
        for(size_t i=1;i<args.size();i++){
            std::cout<<args[i];
            if(i+1<args.size())std::cout<<" ";
        }
        std::cout<<"\n";
    }
    else if(cmd=="pwd"){
        char buf[4096];
        if(getcwd(buf,sizeof(buf)))std::cout<<buf<<"\n";
    }
    else if(cmd=="type" && args.size()>1){
        std::string t=args[1];
        if(std::find(
               std::begin({"echo","exit","pwd","cd","type","history"}),
               std::end({"echo","exit","pwd","cd","type","history"}),
               t
        ) != std::end({"echo","exit","pwd","cd","type","history"}))
            std::cout<<t<<" is a shell builtin\n";
        else{
            std::string p;
            if(find_in_path(t,p))std::cout<<t<<" is "<<p<<"\n";
            else std::cerr<<t<<": not found\n";
        }
    }
    else if(cmd=="history"){
        for(size_t i=0;i<g_history.size();i++){
            printf("    %lu  %s\n", i+1, g_history[i].c_str());
        }
    }

    _exit(0);
}

// ======================= External command =======================
void run_external(const std::vector<std::string>& args){
    std::string path;
    if(!find_in_path(args[0],path)){
        std::cerr<<args[0]<<": command not found\n";
        return;
    }
    std::vector<char*> argv;
    for(auto&s:args)argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    pid_t pid=fork();
    if(pid==0){
        execv(path.c_str(),argv.data());
        _exit(127);
    }
    waitpid(pid,nullptr,0);
}

// ======================= Pipeline execution =======================
void run_pipeline(const std::vector<std::vector<std::string>>& pipeline){
    size_t n=pipeline.size();
    if(n==0)return;

    if(n==1){
        const auto &args=pipeline[0];
        const std::string& cmd=args[0];

        if(cmd=="exit")exit(0);

        if(cmd=="cd"){
            const char* dir=(args.size()>1?args[1].c_str():getenv("HOME"));
            if(!dir||chdir(dir)!=0)
                std::cerr<<"cd: "<<(args.size()>1?args[1]:"HOME not set")<<"\n";
            return;
        }

        if(cmd=="history" && args.size()==3){
            if(args[1]=="-a")history_append_file(args[2]);
            else if(args[1]=="-w")history_write_file(args[2]);
            else if(args[1]=="-r")history_read_file(args[2]);
            return;
        }

        pid_t pid=fork();
        if(pid==0){
            if(is_builtin(cmd))exec_builtin_child(args);
            else run_external(args);
            _exit(127);
        }
        waitpid(pid,nullptr,0);
        return;
    }

    std::vector<int> fds(2*(n-1));
    for(size_t i=0;i+1<n;i++)pipe(fds.data()+2*i);

    for(size_t i=0;i<n;i++){
        pid_t pid=fork();
        if(pid==0){
            if(i>0)dup2(fds[2*(i-1)],STDIN_FILENO);
            if(i+1<n)dup2(fds[2*i+1],STDOUT_FILENO);

            for(int fd:fds)close(fd);

            const auto&args=pipeline[i];
            if(is_builtin(args[0]))exec_builtin_child(args);
            else{
                std::string p;
                if(find_in_path(args[0],p)){
                    std::vector<char*> av;
                    for(const auto&s:args)av.push_back(const_cast<char*>(s.c_str()));
                    av.push_back(nullptr);
                    execv(p.c_str(),av.data());
                }
                _exit(127);
            }
        }
    }

    for(int fd:fds)close(fd);
    for(size_t i=0;i<n;i++)wait(nullptr);
}

// ======================= MAIN =======================
int main(){
    std::cout<<std::unitbuf;
    std::cerr<<std::unitbuf;

    while(true){
        char* raw=readline("$ ");
        if(!raw)break;

        std::string line(raw);
        free(raw);

        size_t start=line.find_first_not_of(" \t");
        if(start==std::string::npos)continue;
        line=line.substr(start);
        if(line.empty())continue;

        g_history.push_back(line);

        auto tokens=tokenize(line);
        if(tokens.empty())continue;

        std::vector<std::vector<std::string>> pipeline;
        std::vector<std::string> cur;
        for(const auto&t:tokens){
            if(t=="|"){
                if(!cur.empty()){pipeline.push_back(cur);cur.clear();}
            } else cur.push_back(t);
        }
        if(!cur.empty())pipeline.push_back(cur);

        run_pipeline(pipeline);
    }
    return 0;
}


