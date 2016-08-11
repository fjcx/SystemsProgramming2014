#include "sh61.h"
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>


// struct command
//    Data structure describing a command. Add your own stuff.
typedef struct command command;
struct command {
    int argc;      // number of arguments
    char** argv;   // arguments, terminated by NULL
    pid_t pid;     // process ID running this command, -1 if none
    int cond;      // conditional req on prev cmd. -1 no condition.
    int rungrp;    // specifies cmd as a subgrp of the list to run
    int ctrl_blk;  // specifies if cmd is part of ctrl structure
    int pipe_nxt;  // indicates if we should pipe to next cmd
    int redir_app_out;     // flag indicating if stdout is redir as appending
    int redir_app_err;     // flag indicating if stderr is redir as appending
    char *redir_in;        // file to redir stdin to
    char *redir_out;       // file to redir stdout to
    char *redir_err;       // file to redir stderr to
    struct command *next;  // ptr to next command in the list
};

static struct command *head_cmd;  // head of cmd list
static struct command *curr_cmd;  // current cmd
volatile sig_atomic_t si_flag;      // flag indicating sig occurred
static int curr_ctrl_state;          // state of ctrl structure
// state defining where we are in 'if' structure
enum states {NEUTRAL, WANT_THEN, THEN_BLOCK, ELSE_BLOCK};
enum redir_to {RDSTDIN, RDSTDOUT, RDSTDERR, RDAPPSTDOUT, RDAPPSTDERR};

// sigint_handler(sig)
//     handler for sigint.
void sigint_handler(int sig) {
    (void) sig;
    si_flag = 1;
}


// command_alloc()
//    Allocate and return a new command structure.
static command* command_alloc(void) {
    command* c = (command*) malloc(sizeof(command));
    c->argc = 0;
    c->argv = NULL;
    c->pid = -1;
    c->cond = -1;
    c->rungrp = 0;
    c->ctrl_blk = NEUTRAL;
    c->pipe_nxt = 0;
    c->redir_app_out = 0;
    c->redir_app_err = 0;
    c->redir_in = NULL;
    c->redir_out = NULL;
    c->redir_err = NULL;
    c->next = NULL;
    return c;
}


// add_cmd_node(c)
//    Adds a command to the runList
static void add_cmd_node(command* c) {
    // if no args then not a valid cmd
    if (c->argc) {    
        c->ctrl_blk = curr_ctrl_state;
        if (head_cmd == NULL) {
            head_cmd = c;
            curr_cmd = c;
        } else {
            curr_cmd->next = c;
            curr_cmd = c;
        }
    }
}


// command_free(c)
//    Free command structure `c`, including all its words.
static void command_free(command* c) {
    for (int i = 0; i != c->argc; ++i) {
        free(c->argv[i]);
    }
    free(c->argv);
    free(c->redir_in);
    free(c->redir_out);
    free(c->redir_err);
    free(c);
}


// free_command_list(c)
//    Free all commands in list starting with structure 'head'.
static void free_command_list(void) {
    command *free_node;
    command *temp_node;
    free_node = head_cmd;
    // walk through the list and free each cmd
    while(free_node != NULL) {
        temp_node = free_node;
        free_node = free_node->next;
        command_free(temp_node);
    }
    head_cmd = NULL;
    curr_cmd = NULL;
}


// free_command_grp
//    used to free a subgrp of the runlist.
//    subgrp specified by 'rungrp'
static void free_command_grp(int frgrp) {
    command *free_node;
    command *temp_node;
    command *prev_grp = NULL;
    free_node = head_cmd;
    // walk through the list and free each cmd
    while(free_node != NULL) {
        temp_node = free_node;
        free_node = free_node->next;
        // only free subgrp
        if (temp_node->rungrp == frgrp) {
            command_free(temp_node);
            prev_grp->next = NULL;
            curr_cmd = prev_grp;
        } else {
            prev_grp = temp_node;
        }
    }
    if (head_cmd->rungrp == frgrp) {
        head_cmd = NULL;
    }
}


// command_append_arg(c, word)
//    Add `word` as an argument to command `c`. This increments `c->argc`
//    and augments `c->argv`.
static void command_append_arg(command* c, char* word) {
    c->argv = (char**) realloc(c->argv, sizeof(char*) * (c->argc + 2));
    c->argv[c->argc] = word;
    c->argv[c->argc + 1] = NULL;
    ++c->argc;
}


// handle_zombies()
//    Deals with any zombie processes still around.
static void handle_zombies(void) {
    // find any process in zombie state
    while (waitpid((pid_t)(-1), 0, WNOHANG) > 0) { }
}


// change_dir(dir_str)
//    change the current directory to the one specified in dir_str.
//    returns 0 on success, 1 on failure.
static int change_dir(char *dir_str) {    
    int ret = 0;    
    if (dir_str != NULL) {
        if (chdir(dir_str) == -1) {
            fprintf(stderr, "cd: %s: %s\n", dir_str, strerror(errno));
            ret = 1;
        }
    }
    return ret;
}


// do_redir(int redirfd, char *fname, int flags, mode_t mode) 
//    does redirection of an fd to the specified file opened
//    with the specified flags
static void do_redir(int redirfd, char *fname, int flags, mode_t mode) {
    int fd = 0;
    if ((fd = open(fname, flags, mode)) == -1) {
        perror("open");
        exit(1);
    }
    if (dup2(fd, redirfd) == -1) {  
        perror(fname);
        exit(1);
    }
    close(fd);
}


// cmd_redir(command *c) 
//    redirects the output of the cmd as specified
//    by it's 'redir' variables
static void cmd_redir(command *c) {
    if (c->redir_in != NULL) {
        do_redir(STDIN_FILENO, c->redir_in, O_RDONLY, 0666);
    }
    
    if (c->redir_out != NULL) {
        if (c->redir_app_out == 0) {
            do_redir(STDOUT_FILENO, c->redir_out, O_WRONLY|O_CREAT|O_TRUNC, 0666);
        } else {
            // open stdout as appending
            do_redir(STDOUT_FILENO, c->redir_out, O_WRONLY|O_CREAT|O_APPEND, 0666);
        }
    }
    
    if (c->redir_err != NULL) {
        if (c->redir_app_err == 0) {
            do_redir(STDERR_FILENO, c->redir_err, O_WRONLY|O_CREAT|O_TRUNC, 0666);
        } else {
            // open stderr as appending
            do_redir(STDERR_FILENO, c->redir_err, O_WRONLY|O_CREAT|O_APPEND, 0666);
        }
    }
}


// COMMAND EVALUATION

// start_command(c, pgid)
//    Start the single command indicated by `c`. Sets `c->pid` to the child
//    process running the command, and returns `c->pid`.
//
//    PART 1: Fork a child process and run the command using `execvp`.
//    PART 5: Set up a pipeline if appropriate. This may require creating a
//       new pipe (`pipe` system call), and/or replacing the child process's
//       standard input/output with parts of the pipe (`dup2` and `close`).
//       Draw pictures!
//    PART 7: Handle redirections.
//    PART 8: The child process should be in the process group `pgid`, or
//       its own process group (if `pgid == 0`). To avoid race conditions,
//       this will require TWO calls to `setpgid`.
pid_t start_command(command* c, pid_t pgid) {
    (void) pgid;   
    int child_pid;
    if ((child_pid = fork()) == -1) {
        // Error
        perror("fork");
    } else if (child_pid == 0) {
        // Child
        // setting group id
        if (pgid == 0) {
            setpgid(0, 0);
        } else {
            setpgid(0, pgid);
        }
        
        // deal with any redirection of outputs, if specified
        cmd_redir(c);
        execvp(c->argv[0], c->argv);
        perror("cannot execute command");
        exit(1);
    }
    // Parent
    if (pgid == 0) {
        setpgid(child_pid, child_pid);
    } else {
        setpgid(child_pid, pgid);
    }
    return child_pid;
}


// begin_piping(command* c)
//    starts piping all the cmds joined by pipes
command* begin_piping(command* c, pid_t pgid) {
    command *exec_node;
    exec_node = c;
    int end_pipe = 1;
    int first_cmd = 0;
    int pipefd[2];
    int prev_pipefd0;
    
    // piping loop
    while (exec_node != NULL && end_pipe == 1) {
        // create pipe
        if (exec_node->pipe_nxt == 1) {
            int r = pipe(pipefd);
            assert(r >= 0);
        }

        if ((exec_node->pid = fork()) == 0) {
            // Child
            // setting group id
            if (pgid == 0) {
                setpgid(0, 0);
            } else {
                setpgid(0, pgid);
            }
            if (first_cmd != 0) {
                dup2(prev_pipefd0, STDIN_FILENO);
                close(prev_pipefd0);
            }

            if (exec_node->pipe_nxt == 1) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }
            
            // do any redirection, if specified
            cmd_redir(exec_node);
            execvp(exec_node->argv[0], exec_node->argv);
            perror("cannot execute command");
            exit(1);
        } else if (exec_node->pid < 0) {
            // Error
            perror("fork");
        }
        
        // Parent
        if (pgid == 0) {
            pgid = exec_node->pid;
            setpgid(exec_node->pid, exec_node->pid);
        } else {
            setpgid(exec_node->pid, pgid);
        }
        if (first_cmd != 0) {
            // close prev open pipe, (but not on parent)
            close(prev_pipefd0);
        } else {
            first_cmd = 1;
        }
        
        // check for end of pipe
        if (exec_node->pipe_nxt == 0) {
            end_pipe = 0;
            return exec_node;
        } else {
            close(pipefd[1]);
            prev_pipefd0 = pipefd[0]; // previous pipe read fd
            exec_node = exec_node->next;
        }
    }
    return exec_node;
}


// is_control_cmd(char* token)
//    checks if token is if, then, else, or fi
//    if found the curr_ctrl_state is set accordingly.
//    if a ctrl cmd is found in an unexpected order 
//    then an error is thrown and we exit.
//    This allows for a crude if statement. 
//    No nesting is allowed as this will register as
//    a ctrl cmd to occuring in an unexpected order.
int is_control_cmd(char* token) {
    int is_ctrl_cmd = 0;
    if (strcmp(token, "if") == 0) {
        is_ctrl_cmd = 1;
        if (curr_ctrl_state == NEUTRAL) {
            curr_ctrl_state = WANT_THEN;
        } else {
            perror("incorrect if statement format");
            exit(1);
        }
    } else if (strcmp(token,"then") == 0) {
        is_ctrl_cmd = 1;
        if (curr_ctrl_state == WANT_THEN) {
            curr_ctrl_state = THEN_BLOCK;
        } else {
            perror("incorrect if statement format");
            exit(1);
        }
    } else if (strcmp(token,"else") == 0) {
        is_ctrl_cmd = 1;
        if (curr_ctrl_state == THEN_BLOCK) {
            curr_ctrl_state = ELSE_BLOCK;
        } else {
            perror("incorrect if statement format");
            exit(1);
        }
    } else if (strcmp(token,"fi") == 0) {
        is_ctrl_cmd = 1;
        if (curr_ctrl_state == ELSE_BLOCK
            || curr_ctrl_state == THEN_BLOCK) {
            curr_ctrl_state = NEUTRAL;
        } else {
            perror("incorrect if statement format");
            exit(1);
        }
    }
    return is_ctrl_cmd;
}


// run_list(c)
//    Run the command list starting at `c`.
//
//    PART 1: Start the single command `c` with `start_command`,
//        and wait for it to finish using `waitpid`.
//    The remaining parts may require that you change `struct command`
//    (e.g., to track whether a command is in the background)
//    and write code in run_list (or in helper functions!).
//    PART 2: Treat background commands differently.
//    PART 3: Introduce a loop to run all commands in the list.
//    PART 4: Change the loop to handle conditionals.
//    PART 5: Change the loop to handle pipelines. Start all processes in
//       the pipeline in parallel. The status of a pipeline is the status of
//       its LAST command.
//    PART 8: - Choose a process group for each pipeline.
//       - Call `set_foreground(pgid)` before waiting for the pipeline.
//       - Call `set_foreground(0)` once the pipeline is complete.
//       - Cancel the list when you detect interruption.
//    rungrp specifies a sub-group of cmds to run.
void run_list(command* c, int rungrp) {    
    command *exec_node;
    exec_node = c;
    int ctrl_result = -1;
    int prev_exit_stat = 0;
    int pgid = 0;
    // walk through passed list and run each command
    while(exec_node != NULL) {
        pid_t ret_pid = 0;
        int prev_ctrl_state = NEUTRAL;
        int child_status;
        // checks here are for conditional statements like '&&' or '||'
        // or to run a sub-group of the runlist
        // or due to being in an if statement ctrl structure
        if ((rungrp == -1 || exec_node->rungrp == rungrp)
            && (exec_node->cond == -1 || exec_node->cond == prev_exit_stat)
            && (ctrl_result == -1 
                || (exec_node->ctrl_blk == THEN_BLOCK && ctrl_result == 0)
                || (exec_node->ctrl_blk == ELSE_BLOCK && ctrl_result == 1))) {
            
            // flag to indicate if the cmd is cd, which we treat differently
            int is_cd = 0;
            if (strcmp(exec_node->argv[0], "cd") == 0) {
                is_cd = 1;
            }
            // if pipe to next
            if (exec_node->pipe_nxt == 1) {
                // pass cmd to be piped
                exec_node = begin_piping(exec_node, pgid);
                ret_pid = exec_node->pid;
            } else if (is_cd == 1) {
                // change directory built-in cmd
                char* dirname = exec_node->argv[1];
                // record fds before redir, then undo after cd cmd
                int oldfds[3] = {dup(STDIN_FILENO), 
                    dup(STDOUT_FILENO),
                    dup(STDERR_FILENO)};
                // if redirection specified, do it first
                cmd_redir(exec_node);
                if ((prev_exit_stat = change_dir(dirname)) == -1) {
                    perror("cd");
                }
                
                // set back to oldfds after cd
                dup2(oldfds[0], STDIN_FILENO);
                dup2(oldfds[1], STDOUT_FILENO);
                dup2(oldfds[2], STDERR_FILENO);
            } else {
                ret_pid = start_command(exec_node, pgid);
                exec_node->pid = ret_pid;
                if (pgid == 0) {
                    pgid = ret_pid;
                }
            }
            
            if (is_cd == 0) {
                // foreground command
                if (rungrp == -1) {        // not bg process
                    set_foreground(pgid);
                    // do check for signal
                    if (si_flag == 1) {
                        kill(-10, SIGINT);                
                    }
                }
                if (waitpid(ret_pid, &child_status, 0) == -1) {
                    perror("wait");
                }
                
                if (WIFSIGNALED(child_status) != 0 
                    && WTERMSIG(child_status) == SIGINT) {
                    // on ctrl-c, kill the run_list loop
                    exec_node->next = NULL;
                }
                
                // mask child return status
                if (WIFEXITED(child_status)) {
                    prev_exit_stat = WEXITSTATUS(child_status);
                }
                if (rungrp == -1) {        // not bg process
                    set_foreground(0);
                }
            }
            prev_ctrl_state = exec_node->ctrl_blk;
        }
        exec_node = exec_node->next;
        // check for transition from 'if' to 'then', need 'if' result
        if (exec_node != NULL) {
            if (exec_node->ctrl_blk == THEN_BLOCK &&  prev_ctrl_state == WANT_THEN) {
                ctrl_result = prev_exit_stat;
            } else if (exec_node->ctrl_blk == NEUTRAL) {
                ctrl_result = -1;
            }
        }
    }
}


// eval_line(c)
//    Parse the command list in `s` and run it via `run_list`.
void eval_line(const char* s) {
    int type;
    char* token;
    int grp = 0;
    int exgrp = -1;
    int redir_nxt = -1;
    int is_bg = 0;
    // build the command
    command* c = command_alloc();
    while ((s = parse_shell_token(s, &type, &token)) != NULL) {
        if (redir_nxt != -1) {
            // we noted the last token was a redirection, this
            // token is then taken as the filename for that redir
            if (redir_nxt == RDSTDIN) {
                c->redir_in = token;
            } else if (redir_nxt == RDSTDOUT) {
                c->redir_out = token;
            } else if (redir_nxt == RDAPPSTDOUT) {
                c->redir_out = token;
                c->redir_app_out = 1;
            } else if (redir_nxt == RDAPPSTDERR) {
                c->redir_err = token;
                c->redir_app_err = 1;
            } else {
                c->redir_err = token;
            }
            redir_nxt = -1;
        } else if (type ==  TOKEN_REDIRECTION) {    
            // found redirection token, add cmd to list, with fd to redir
            if (*token == '<') {
                redir_nxt = RDSTDIN;         // redir to stdin
            } else if (*token == '>') { 
                if (token[1] == '>') {
                    redir_nxt = RDAPPSTDOUT; // append to stdout
                } else {
                    redir_nxt = RDSTDOUT;    // redir to stdout
                }
            } else if (*token == '2') {
                if (token[2] == '>') {
                    redir_nxt = RDAPPSTDERR; // append to stderr
                } else {
                    redir_nxt = RDSTDERR;    // redir to stderr
                }
            }
        } else if (type ==  TOKEN_PIPE) {
            // found '|', add cmd to list, and pipe indic
            c->pipe_nxt = 1;
            add_cmd_node(c);
            c = command_alloc();
        } else if (type ==  TOKEN_AND) {
            // found '&&', add cmd to list, and add condition
            add_cmd_node(c);
            c = command_alloc();
            c-> cond = 0;     // prev cmd must exit with status 0
        } else if (type ==  TOKEN_OR) {
            // found '||', add cmd to list, and add condition
            add_cmd_node(c);
            c = command_alloc();
            c-> cond = 1;     // prev cmd must exit with status 1
        } else if (type ==  TOKEN_SEQUENCE) {
            // found ';', add cmd to list
            add_cmd_node(c);
            ++grp;              // next runnable grouping
            c = command_alloc();
        } else if (type ==  TOKEN_BACKGROUND) {
            // When encountered '&' we fork and process and run bg cmds 
            // in a subshell. 
            // bg list is taken as everything after the last ';' and before '&'.
            add_cmd_node(c);
            c = command_alloc();
            int shpid;
            if ((shpid = fork()) == -1) {  // Error
                perror("fork");
            } else if (shpid == 0) {       // Child
                // break from loop and start executing the current sub-grp
                is_bg = 1;
                exgrp = grp;
                break;
            } 
            // Parent
            // in main shell clear the previous cmds as that are run in bg process
            if (grp != 0) {
                free_command_grp(grp);  // frees a sub grp of the runlist
            } else {
                free_command_list();    // frees all of the runlist
            }
            c = command_alloc();
        } else if (is_control_cmd(token)) {
            // do nothing, just change ctrl state
        } else {
            // add valid cmd and arg
            command_append_arg(c, token);
            c->rungrp = grp;            
        }
    }
    
    // add the command to the cmdlist
    if (c->argc) {
        add_cmd_node(c);
    } else {
        command_free(c); // cmd has no args and is never used, so free it
    }
    
    if (curr_ctrl_state != NEUTRAL) {
        // must finish if with 'fi'
        perror("if statement not finished");
        exit(1);
    }

    // execute it
    if (head_cmd) {
        run_list(head_cmd, exgrp);    // only runs grp specified
    }
    
    // free all command in list
    free_command_list();
    // if bg sub-shell then exit process
    if (is_bg == 1) {
        exit(0);
    }
}


int main(int argc, char* argv[]) {
    si_flag = 0;
    FILE* command_file = stdin;
    int quiet = 0;
    curr_ctrl_state = NEUTRAL;

    // Check for '-q' option: be quiet (print no prompts)
    if (argc > 1 && strcmp(argv[1], "-q") == 0) {
        quiet = 1;
        --argc, ++argv;
    }

    // Check for filename option: read commands from file
    if (argc > 1) {
        command_file = fopen(argv[1], "rb");
        if (!command_file) {
            perror(argv[1]);
            exit(1);
        }
    }

    // - Put the shell into the foreground
    // - Ignore the SIGTTOU signal, which is sent when the shell is put back
    //   into the foreground
    set_foreground(0);
    handle_signal(SIGTTOU, SIG_IGN);
    // handle ctrl-c
    handle_signal(SIGINT, sigint_handler);

    char buf[BUFSIZ];
    int bufpos = 0;
    int needprompt = 1;

    while (!feof(command_file)) {
        // Print the prompt at the beginning of the line
        if (needprompt && !quiet) {
            printf("sh61[%d]$ ", getpid());
            fflush(stdout);
            needprompt = 0;
        }

        // Read a string, checking for error or EOF
        if (fgets(&buf[bufpos], BUFSIZ - bufpos, command_file) == NULL) {
            if (ferror(command_file) && errno == EINTR) {
                // ignore EINTR errors
                clearerr(command_file);
                buf[bufpos] = 0;
                if (si_flag == 1) {    // for SIGINT
                    si_flag = 0;
                    printf("\n");      // newline and prompt
                    needprompt = 1;
                }
            } else {
                if (ferror(command_file))
                    perror("sh61");
                break;
            }
        }

        // If a complete command line has been provided, run it
        bufpos = strlen(buf);
        if (bufpos == BUFSIZ - 1 || (bufpos > 0 && buf[bufpos - 1] == '\n')) {
            eval_line(buf);
            bufpos = 0;
            needprompt = 1;
        }

        // Handle zombie processes and/or interrupt requests
        handle_zombies();
    }

    return 0;
}