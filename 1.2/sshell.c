#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h> // for open stuff
#include <ctype.h> // isspace
#include <signal.h> // for sigchld handling

#define MAX_BG_JOBS 16
#define MAX_COMMANDS 4
#define MAX_ARGS_PER_CMD 16
#define CMDLINE_MAX 512

// flag set by sigchld handler to indicate background job completion
static sig_atomic_t sigchld_flag = 0;

// signal handler to catch child termination and report background job completion
static void sigchld_handler(int signum) {
    (void)signum;
    sigchld_flag = 1;
}

typedef struct {
    char *args[MAX_ARGS_PER_CMD];
    char *input_f;
    char *output_f;
    int background;  // flag to indicate if command should run in background
} Command;

// background job structure
typedef struct {
    pid_t pids[MAX_COMMANDS];
    int pid_count;           
    char* command;
    int active;              
} BackgroundJob;

// queue for background jobs
typedef struct {
    BackgroundJob jobs[MAX_BG_JOBS];
    int num_jobs;
} BgJobQueue;

// global background job queue for signal handler access
static BgJobQueue bg_queue;

/**
 * @brief initialize the background job queue
 * 
 * @param queue the queue to initialize
 */
void init_bg_queue(BgJobQueue *queue) {
    queue->num_jobs = 0;
    for (int i = 0; i < MAX_BG_JOBS; i++) {
        queue->jobs[i].active = 0;
        queue->jobs[i].command = NULL;
        queue->jobs[i].pid_count = 0;
    }
}

/**
 * @brief add a new background job to the queue
 * 
 * @param queue the queue to add to
 * @param pids array of process IDs
 * @param pid_count number of processes
 * @param command original command string
 * @return int 0 on success, -1 if queue is full
 */
int add_bg_job(BgJobQueue *queue, pid_t pids[], int pid_count, char* command) {
    if (queue->num_jobs >= MAX_BG_JOBS) {
        return -1;  // queue full
    }
    
    int index = queue->num_jobs;
    queue->jobs[index].pid_count = pid_count;
    queue->jobs[index].command = strdup(command);
    queue->jobs[index].active = 1;
    
    // copy pids
    for (int i = 0; i < pid_count; i++) {
        queue->jobs[index].pids[i] = pids[i];
    }
    
    queue->num_jobs++;
    return 0;
}

/**
 * @brief check for completed background jobs
 * 
 * @param queue the queue to check
 * @return int number of jobs that were completed and reported
 */
int check_completed_bg_jobs(BgJobQueue *queue) {
    int completed_count = 0;
    
    for (int job_idx = 0; job_idx < queue->num_jobs; job_idx++) {
        BackgroundJob *job = &queue->jobs[job_idx];
        
        if (!job->active) continue;  // skip jobs that are already marked as inactive
        
        // check if all processes in this job have completed
        int all_completed = 1;
        int exit_status[MAX_COMMANDS] = {0};
        
        for (int i = 0; i < job->pid_count; i++) {
            int status;
            pid_t result = waitpid(job->pids[i], &status, WNOHANG);
            
            if (result == 0) {
                // process still running
                all_completed = 0;
                break;
            } else if (result > 0) {
                // process has completed
                if (WIFEXITED(status)) {
                    exit_status[i] = WEXITSTATUS(status);
                }
            }
        }
        
        if (all_completed) {
            // report job completion in fifo order
            fprintf(stderr, "+ completed '%s' ", job->command);
            for (int i = 0; i < job->pid_count; i++) {
                fprintf(stderr, "[%d]", exit_status[i]);
            }
            fprintf(stderr, "\n");
            
            // clean up and mark as inactive
            free(job->command);
            job->command = NULL;
            job->active = 0;
            completed_count++;
        }
    }
    
    // clean up the queue by removing inactive jobs (shifting active ones to the front)
    if (completed_count > 0) {
        int new_index = 0;
        for (int i = 0; i < queue->num_jobs; i++) {
            if (queue->jobs[i].active) {
                if (i != new_index) {
                    queue->jobs[new_index] = queue->jobs[i];
                }
                new_index++;
            }
        }
        queue->num_jobs = new_index;
    }
    
    return completed_count;
}

/**
 * @brief pads | < > chars with spaces so that it makes splitting using strtok easier
 * 
 * @param line whole line
 */
void pad_spaces_if_missing(char *line) {
    char temp[CMDLINE_MAX];
    int temp_index = 0;

    for (int i = 0; line[i] != '\0'; i++) {
        // for: | < >
        if (line[i] == '<' || line[i] == '>' || line[i] == '|') {
            // add space before if not there
            if (temp_index > 0 && temp[temp_index - 1] != ' ') {
                temp[temp_index] = ' ';
                temp_index++;
            }

            temp[temp_index] = line[i];
            temp_index++;

            if (line[i + 1] != '\0' && line[i + 1] != ' ') {
                temp[temp_index] = ' ';
                temp_index++;
            }
        } else {
            temp[temp_index] = line[i];
            temp_index++;
        }
    }

    temp[temp_index] = '\0';
    strcpy(line, temp);
}

/**
 * @brief trims leading and trailing spaces
 * 
 * @param str 
 */
void trim_spaces(char *str) {
    // leading spaces
    while (isspace(*str)) {
        str++;
    }
    
    // trailing spaces
    char *end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) {
        end--;
    }

    *(end + 1) = '\0';
}

/**
 * @brief parses command line and returns number of commands. commands are stored in array of Command structs
 * 
 * @param line line to process
 * @param commands commands struct list
 * @return int num commands or -1 if invalid
 */
int parse_command(char *line, Command commands[]) {
    char *sub_commands_by_pipe[MAX_COMMANDS];
    int num_commands = 0;
    int background = 0;

    trim_spaces(line);
    
    // check if the command ends with &, indicating a background job
    int len = strlen(line);
    if (len > 0 && line[len-1] == '&') {
        background = 1;
        line[len-1] = '\0';  // remove the & character
        trim_spaces(line);   // trim any spaces before the &
        
        if (strlen(line) == 0) {
            fprintf(stderr, "Error: missing command\n");
            return -1;
        }
    }

    // check if command has a & that's not in the end
    char *ampersand = strchr(line, '&');
    if (ampersand != NULL && ampersand != line + strlen(line) - 1) {
        fprintf(stderr, "Error: mislocated background sign\n");
        return -1;
    }

    if (line[0] == '|' || line[strlen(line) - 1] == '|') {
        fprintf(stderr, "Error: missing command\n");
        return -1;
    }

    // split when | symbol
    char *sub_command = strtok(line, "|");
    while (sub_command != NULL && num_commands < MAX_COMMANDS) {
        sub_commands_by_pipe[num_commands] = sub_command;
        num_commands++;
        sub_command = strtok(NULL, "|");
    }

    // parse each command looking for < or > and managing args
    for (int i = 0; i < num_commands; i++) {
        trim_spaces(sub_commands_by_pipe[i]);

        if (sub_commands_by_pipe[i][0] == '\0' || 
            (sub_commands_by_pipe[i][0] == '>' || sub_commands_by_pipe[i][0] == '<')) {
            fprintf(stderr, "Error: missing command\n");
            return -1;
        }

        Command *cmd = &commands[i];
        cmd->input_f = NULL;
        cmd->output_f = NULL;
        cmd->background = 0;
        
        // only the last command can be a background job
        if (i == num_commands - 1 && background) {
            cmd->background = 1;
        }

        int num_args = 0;
        char *arg_split_space = strtok(sub_commands_by_pipe[i], " ");
        while (arg_split_space != NULL) {
            if (strcmp(arg_split_space, "<") == 0) {
                if (i > 0) {
                    fprintf(stderr, "Error: mislocated input redirection\n");
                    return -1;
                }
                arg_split_space = strtok(NULL, " ");
                if (arg_split_space) {
                    cmd->input_f = strdup(arg_split_space); // modified later so must use copy
                    int input_fd = open(arg_split_space, O_RDONLY);
                    if (input_fd == -1) {
                        fprintf(stderr, "Error: cannot open input file\n");
                        close(input_fd);
                        return -1;
                    }
                    close(input_fd);
                } else {
                    fprintf(stderr, "Error: no input file\n");
                    return -1;
                }
            } else if (strcmp(arg_split_space, ">") == 0) {
                if (i < num_commands - 1) {
                    fprintf(stderr, "Error: mislocated output redirection\n");
                    return -1;
                }
                arg_split_space = strtok(NULL, " ");
                if (arg_split_space) {
                    cmd->output_f = strdup(arg_split_space);
                    int output_fd = open(arg_split_space, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (output_fd == -1) {
                        fprintf(stderr, "Error: cannot open output file\n");
                        close(output_fd);
                        return -1;
                    }
                    close(output_fd);
                } else {
                    fprintf(stderr, "Error: no output file\n");
                    return -1;
                }
            } else { // normal non < or >
                cmd->args[num_args] = strdup(arg_split_space);
                num_args++;
            }
            arg_split_space = strtok(NULL, " "); // next one
        }
        if (num_args > MAX_ARGS_PER_CMD) {
            fprintf(stderr, "Error: too many process arguments\n");
            return -1;
        }
        cmd->args[num_args] = NULL; // null terminate
    }

    return num_commands;
}

/**
 * @brief debug function to print out command struct list
 * 
 * @param commands commands list
 * @param num_jobs number of commands
 */
void preview_command_list(Command commands[], int num_jobs) {
    for (int i = 0; i < num_jobs; i++) {
        printf("----------------------------\n");
        printf("Command %d:\n", i + 1);
        printf("\tArgs: ");
        for (int j = 0; commands[i].args[j] != NULL; j++) {
            printf("{%s} ", commands[i].args[j]);
        }
        printf("\n");
        if (commands[i].input_f)
            printf("\tInput: %s\n", commands[i].input_f);
        if (commands[i].output_f)
            printf("\tOutput: %s\n", commands[i].output_f);
    }
    printf("----------------------------\n");
}

int main() {
    char cmd[CMDLINE_MAX];
    char *eof;
    Command commands[MAX_COMMANDS];
    int args_index = -1;

    init_bg_queue(&bg_queue);

    // register the SIGCHLD signal handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; // restart interrupted syscalls and don't catch child stop signals
    sigaction(SIGCHLD, &sa, NULL);

    while (1)
    {
        // check for and report any completed background jobs before printing prompt
        if (sigchld_flag) {
            check_completed_bg_jobs(&bg_queue);
            sigchld_flag = 0;
        }
        char *nl;

        /* print prompt */
        printf("sshell@ucd$ ");
        fflush(stdout);

        /* get command line */
        eof = fgets(cmd, CMDLINE_MAX, stdin);
        if (!eof)
            /* make EOF equate to exit */
            strncpy(cmd, "exit\n", CMDLINE_MAX);

        /* print command line if stdin is not provided by terminal */
        if (!isatty(STDIN_FILENO))
        {
            printf("%s", cmd);
            fflush(stdout);
        }

        /* remove trailing newline from command line */
        nl = strchr(cmd, '\n');
        if (nl)
            *nl = '\0';

        if (cmd[0] == '\0') { // for empty commands
            continue;
        }

        char* original_command = strdup(cmd);

        pad_spaces_if_missing(cmd); 

        args_index = parse_command(cmd, commands);
        
        // if there are too many args, then we skip execution
        if (args_index < 0) {
            free(original_command);
            continue;
        }
        
        // check for exit when background jobs are active
        if (!strcmp(commands->args[0], "exit") && bg_queue.num_jobs > 0) {
            fprintf(stderr, "Error: active job still running\n");

            // check if any background jobs have completed
            if (sigchld_flag) {
                check_completed_bg_jobs(&bg_queue);
                sigchld_flag = 0;
            }

            // print exit
            fprintf(stderr, "+ completed 'exit' [1]\n");
            free(original_command);
            continue;
        }
        
        if (!strcmp(commands->args[0], "exit"))
        {
            fprintf(stderr, "Bye...\n");
            fprintf(stderr, "+ completed 'exit' [0]\n");
            free(original_command);
            exit(0);
        }

        if (!strcmp(commands->args[0], "pwd"))
        {
            char cwd[CMDLINE_MAX];
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                fprintf(stdout, "%s\n", cwd);
                fprintf(stderr, "+ completed '%s' [0]\n", original_command);
            }
            free(original_command);
            continue;
        }
        
        if (!strcmp(commands->args[0], "cd"))
        {
            // can assume only one arg
            int chdir_status = chdir(commands->args[1]);
            if (chdir_status != 0) {
                fprintf(stderr, "Error: cannot cd into directory\n");
                fprintf(stderr, "+ completed '%s' [1]\n", original_command);
            } else {
                fprintf(stderr, "+ completed '%s' [0]\n", original_command);
            }
            free(original_command);
            continue;
        }

        // check if this is a background job
        int is_background = commands[args_index-1].background;
        int pipe_fds[3][2];

        for (int i = 0; i < args_index; i++) {
            if (pipe(pipe_fds[i]) == -1) {
                perror("pipe");
                exit(1);
            }
        }

        pid_t pids[MAX_COMMANDS] = {0};
        int exit_status[MAX_COMMANDS] = {0};

        // loop per command
        for (int i = 0; i < args_index; i++) {
            pid_t pid;
            pid = fork();

            if (pid == 0) {  // child
                // input redirection
                if (commands[i].input_f) {
                    int input_fd = open(commands[i].input_f, O_RDONLY);
                    dup2(input_fd, STDIN_FILENO);
                    close(input_fd);
                } else if (i > 0) {
                    // not the first command, read from prev pipe
                    dup2(pipe_fds[i - 1][0], STDIN_FILENO);
                }

                // output redirection
                if (commands[i].output_f) {
                    int output_fd = open(commands[i].output_f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    dup2(output_fd, STDOUT_FILENO);
                    close(output_fd);
                } else if (i < args_index - 1) {
                    // not last command, write next pipe
                    dup2(pipe_fds[i][1], STDOUT_FILENO);
                }

                // close all fds from piping
                for (int j = 0; j < args_index; j++) {
                    close(pipe_fds[j][0]);
                    close(pipe_fds[j][1]);
                }

                execvp(commands[i].args[0], commands[i].args);
                fprintf(stderr, "Error: command not found\n");
                exit(1);
            }
            else if (pid < 0) {
                perror("fork");
                exit(1);
            } else { // parent
                pids[i] = pid;
            }
        }

        // parent process: close all pipes
        for (int i = 0; i < args_index; i++) {
            close(pipe_fds[i][0]);
            close(pipe_fds[i][1]);
        }

        // for background jobs, don't wait and save info
        if (is_background) {
            // Store PIDs and command info for background job
            add_bg_job(&bg_queue, pids, args_index, original_command);
        } else {
            // for foreground jobs, wait as before
            for (int i = 0; i < args_index; i++) {
                int status;
                waitpid(pids[i], &status, 0);

                // exit status of the last command
                if (WIFEXITED(status)) {
                    exit_status[i] = WEXITSTATUS(status);
                }
            }

            // before reporting foreground completion, check and report any background jobs that have finished
            if (sigchld_flag) {
                check_completed_bg_jobs(&bg_queue);
                sigchld_flag = 0;
            }

            // completion status for foreground job
            fprintf(stderr, "+ completed '%s' ", original_command);
            for (int i = 0; i < args_index; i++) {
                fprintf(stderr, "[%d]", exit_status[i]);
            }
            fprintf(stderr, "\n");
            free(original_command);
        }
    }

    // free any remaining resources
    if (bg_queue.num_jobs > 0) {
        for (int i = 0; i < bg_queue.num_jobs; i++) {
            if (bg_queue.jobs[i].active) {
                free(bg_queue.jobs[i].command);
            }
        }
    }

    return EXIT_SUCCESS;
}
