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

/*------------------- TOKENIZER ---------------------*/
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
            } else if (c == '\'') mode = SINGLE_QUOTE;
            else if (c == '"') mode = DOUBLE_QUOTE;
            else if (isspace(static_cast<unsigned char>(c))) {
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
                } else tokens.push_back(">");
            } else if (c == '|') {
                if (!cur.empty()) {
                    tokens.push_back(cur);
                    cur.clear();
                }
                tokens.push_back("|");
            } else cur += c;
        } 
        else if (mode == SINGLE_QUOTE) {
            if (c == '\'') mode = NORMAL;
            else cur += c;
        } 
        else if (mode == DOUBLE_QUOTE) {
            if (c == '"') mode = NORMAL;
            else if (c == '\\' && i + 1 < line.size()) cur += line[++i];
            else cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

/*------------------- PARSER ---------------------*/
static vector<Command> parse_pipeline(const vector<string> &tokens) {
    vector<Command> pipeline;
    vector<string> current;
    for (auto &t : tokens) {
        if (t == "|") {
            if (!current.empty()) {
                Command c;
                size_t i = 0;
                while (i < current.size()) {
                    auto &tok = current[i];
                    if ((tok == "1" || tok == "2") &&
                        i + 2 < current.size() &&
                        (current[i + 1] == ">" || current[i + 1] == ">>"))
                    {
                        Redirection r{ tok=="1"?1:2, current[i+1]==">>", current[i+2] };
                        c.redirs.push_back(r);
                        i += 3;
                    } else if ((tok == ">" || tok == ">>") && i + 1 < current.size()) {
                        Redirection r{1, tok==">>", current[i+1]};
                        c.redirs.push_back(r);
                        i += 2;
                    } else c.argv.push_back(tok), ++i;
                }
                pipeline.push_back(c);
                current.clear();
            }
        } else current.push_back(t);
    }
    if (!current.empty()) {
        Command c;
        size_t i = 0;
        while (i < current.size()) {
            auto &tok = current[i];
            if ((tok == "1" || tok == "2") &&
                i + 2 < current.size() &&
                (current[i + 1] == ">" || current[i + 1] == ">>"))
            {
                Redirection r{ tok=="1"?1:2, current[i+1]==">>", current[i+2]};
                c.redirs.push_back(r);
                i+=3;
            } else if ((tok=="?>" || tok==">>") && i+1<current.size()) {
                Redirection r{1, tok=="?>>", current[i+1]};
                c.redirs.push_back(r);
                i+=2;
            } else c.argv.push_back(tok), ++i;
        }
        pipeline.push_back(c);
    }
    return pipeline;
}

/*----------------- REDIRECTION -------------------*/
static void apply_redirections(const vector<Redirection> &redirs){
    for(auto&r:redirs){
        int flags=O_WRONLY|O_CREAT|(r.append?O_APPEND:O_TRUNC);
        int fd=open(r.filename.c_str(),flags,0644);
        if(fd<0){perror("open");continue;}
        dup2(fd,r.fd),close(fd);
    }
}

/*---------------- PATH LOOKUP (NO CACHE!) --------*/
static string find_executable(const string &cmd){
    if(cmd.empty())return "";
    if(cmd.find('/')!=string::npos){
        if(access(cmd.c_str(),X_OK)==0)return cmd;
        return "";
    }
    const char*path=getenv("PATH");
    if(!path)return "";
    string p(path),dir;
    stringstream ss(p);
    while(getline(ss,dir,':')){
        if(dir.empty())dir=".";
        string full=dir+"/"+cmd;
        if(access(full.c_str(),X_OK)==0)return full;
    }
    return "";
}

/*----------------- BUILTINS -----------------------*/
static bool is_builtin(const string&c){
    return find(builtin_names.begin(),builtin_names.end(),c)!=builtin_names.end();
}

static int builtin_echo(const vector<string>&a){
    for(size_t i=1;i<a.size();++i){if(i>1)cout<<" ";cout<<a[i];}
    cout<<"\n";return 0;
}

static int builtin_pwd(){
    char buf[PATH_MAX];
    if(!getcwd(buf,sizeof(buf))){perror("pwd");return 1;}
    cout<<buf<<"\n";return 0;
}

static int builtin_cd(const vector<string>&a){
    const char*home=getenv("HOME");
    string t;
    if(a.size()<2){
        if(!home){cerr<<"cd: HOME not set\n";return 1;}
        t=home;
    } else{
        auto &arg=a[1];
        if(!arg.empty()&&arg[0]=='~'){
            if(!home){cerr<<"cd: HOME not set\n";return 1;}
            if(arg.size()==1)t=home;
            else if(arg[1]=='/')t=string(home)+arg.substr(1);
            else t=arg;
        } else t=arg;
    }
    if(chdir(t.c_str())!=0){perror("cd");return 1;}
    return 0;
}

static int builtin_type(const vector<string>&a){
    if(a.size()<2){cerr<<"type: usage: type name\n";return 1;}
    int s=0;
    for(size_t i=1;i<a.size();++i){
        auto &n=a[i];
        if(is_builtin(n))cout<<n<<" is a shell builtin\n";
        else{
            string p=find_executable(n);
            if(!p.empty())cout<<n<<" is "<<p<<"\n";
            else cerr<<"type: "<<n<<": not found\n",s=1;
        }
    }
    return s;
}

static int builtin_history(const vector<string>&a){
    if(a.size()==1){
        for(size_t i=0;i<shell_history.size();++i)
            cout<<setw(5)<<(i+1)<<"  "<<shell_history[i]<<"\n";
        return 0;
    }
    if(a.size()==2){
        bool num=true;
        for(char c:a[1]) if(!isdigit(c)) num=false;
        if(num){
            int n=stoi(a[1]);
            size_t total=shell_history.size();
            size_t start=(n>=int(total)?0:total-n);
            for(size_t i=start;i<total;++i)
                cout<<setw(5)<<(i+1)<<"  "<<shell_history[i]<<"\n";
            return 0;
        }
    }
    if(a.size()>=3 && a[1]=="-a"){
        int fd=open(a[2].c_str(),O_WRONLY|O_CREAT|O_APPEND,0644);
        if(fd<0){perror("history -a");return 1;}
        for(size_t i=last_history_flush_index;i<shell_history.size();++i){
            write(fd,shell_history[i].c_str(),shell_history[i].size());
            write(fd,"\n",1);
        }
        close(fd);
        last_history_flush_index=shell_history.size();
        return 0;
    }
    if(a.size()>=3 && a[1]=="-w"){
        int fd=open(a[2].c_str(),O_WRONLY|O_CREAT|O_TRUNC,0644);
        if(fd<0){perror("history -w");return 1;}
        for(auto&s:shell_history){
            write(fd,s.c_str(),s.size());
            write(fd,"\n",1);
        }
        close(fd);
        last_history_flush_index=shell_history.size();
        return 0;
    }
    if(a.size()>=3 && a[1]=="-r"){
        int fd=open(a[2].c_str(),O_RDONLY);
        if(fd<0)return 0;
        FILE*f=fdopen(fd,"r");
        if(!f){close(fd);return 1;}
        char*line=nullptr;size_t len=0;ssize_t r;
        while((r=getline(&line,&len,f))!=-1){
            if(r>0 && line[r-1]=='\n') line[r-1]=0;
            string s=line;
            if(!s.empty()){shell_history.push_back(s);add_history(s.c_str());}
        }
        if(line)free(line);
        fclose(f);
        last_history_flush_index=shell_history.size();
        return 0;
    }
    cerr<<"history: unsupported option\n";
    return 1;
}

static int run_builtin(Command &c,bool child){
    if(c.argv.empty())return 0;
    string n=c.argv[0];
    if(n=="echo")return builtin_echo(c.argv);
    if(n=="pwd")return builtin_pwd();
    if(n=="cd")return builtin_cd(c.argv);
    if(n=="type")return builtin_type(c.argv);
    if(n=="history")return builtin_history(c.argv);
    if(n=="exit"){ if(child)_exit(0); exit(0);}
    return 0;
}

/*-------------------- EXECUTION --------------------*/
static void execute_pipeline(vector<Command>&p){
    size_t n=p.size();
    if(n==0)return;

    if(n==1 && !p[0].argv.empty() && is_builtin(p[0].argv[0])){
        int save1=dup(1),save2=dup(2);
        apply_redirections(p[0].redirs);
        run_builtin(p[0],false);
        dup2(save1,1);dup2(save2,2);
        close(save1);close(save2);
        return;
    }

    vector<pid_t> pids;
    vector<int> pipes;
    if(n>1){
        pipes.resize(2*(n-1));
        for(size_t i=0;i<n-1;++i)pipe(&pipes[2*i]);
    }

    for(size_t i=0;i<n;++i){
        pid_t pid=fork();
        if(pid==0){
            signal(SIGINT,SIG_DFL);
            if(n>1){
                if(i>0)dup2(pipes[2*(i-1)],0);
                if(i<n-1)dup2(pipes[2*i+1],1);
                for(int fd:pipes)close(fd);
            }
            apply_redirections(p[i].redirs);
            if(p[i].argv.empty())_exit(0);
            if(is_builtin(p[i].argv[0])){
                run_builtin(p[i],true);
                _exit(0);
            }
            vector<char*> argv;
            for(auto&s:p[i].argv)argv.push_back((char*)s.c_str());
            argv.push_back(nullptr);
            string path=find_executable(p[i].argv[0]);
            if(path.empty()){cerr<<p[i].argv[0]<<": command not found\n";_exit(127);}
            execv(path.c_str(),argv.data());
            _exit(127);
        } else pids.push_back(pid);
    }
    if(n>1)for(int fd:pipes)close(fd);
    for(pid_t pid:pids)waitpid(pid,nullptr,0);
}

/*-------------------- LINE EXEC ---------------------*/
static void execute_line(const string&line){
    auto tokens=tokenize(line);
    if(tokens.empty())return;
    auto pipe=parse_pipeline(tokens);
    execute_pipeline(pipe);
}

/*------------------ TAB COMPLETION ------------------*/

static char* command_generator(const char*text,int state){
    static size_t i;
    if(state==0)i=0;
    string pre(text);
    while(i<builtin_names.size()){
        auto&n=builtin_names[i++];
        if(n.rfind(pre,0)==0)return strdup(n.c_str());
    }
    return nullptr;
}

static char** shell_completion(const char*text,int start,int end){
    (void)end;
    if(start==0)return rl_completion_matches(text,command_generator);
    return rl_completion_matches(text,rl_filename_completion_function);
}

/*--------------------- SIGNALS ----------------------*/
static void setup_signal_handlers(){
    signal(SIGINT,SIG_IGN);
}

/*------------------------ MAIN ---------------------*/
int main(){
    setup_signal_handlers();
    rl_attempted_completion_function=shell_completion;

    while(true){
        char*input=readline(shell_prompt.c_str());
        if(!input){cout<<"\n";break;}
        string line=input;free(input);
        if(line.empty())continue;
        add_history(line.c_str());
        shell_history.push_back(line);
        execute_line(line);
    }
    return 0;
}






