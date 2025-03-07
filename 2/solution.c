#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>

static int execute_command_line(const struct command_line *line);
static int execute_piped_commands(const struct command_line *line);
static bool has_pipes(const struct command_line *line);
static int execute_single_command(struct command *cmd);
static bool command_exists(const char *cmd);

static bool
command_exists(const char *cmd)
{
	if (strchr(cmd, '/') != NULL) {
		return access(cmd, X_OK) == 0;
	}
	
	const char *path = getenv("PATH");
	if (path == NULL) {
		return false;
	}
	
	char *path_copy = strdup(path);
	if (path_copy == NULL) {
		return false;
	}
	
	bool found = false;
	char *dir = strtok(path_copy, ":");
	
	while (dir != NULL) {
		char full_path[4096];
		snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);
		
		if (access(full_path, X_OK) == 0) {
			found = true;
			break;
		}
		
		dir = strtok(NULL, ":");
	}
	
	free(path_copy);
	return found;
}

static int
execute_single_command(struct command *cmd)
{
	if (strcmp(cmd->exe, "cd") == 0) {
		const char *path = ".";
		if (cmd->arg_count > 0)
			path = cmd->args[0];
		
		if (chdir(path) != 0) {
			return 1;
		}
		return 0;
	} 
	
	if (strcmp(cmd->exe, "exit") == 0) {
		int exit_code = 0;
		if (cmd->arg_count > 0)
			exit_code = atoi(cmd->args[0]);
		exit(exit_code);
	}
	
	if (!command_exists(cmd->exe)) {
		return 1;
	}
	
	pid_t pid = fork();
	
	if (pid < 0) {
		return 1;
	}
	
	if (pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull != -1) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		
		char **args = malloc(sizeof(char *) * (cmd->arg_count + 2));
		if (args == NULL) {
			exit(1);
		}
		
		args[0] = cmd->exe;
		for (uint32_t i = 0; i < cmd->arg_count; i++) {
			args[i + 1] = cmd->args[i];
		}
		args[cmd->arg_count + 1] = NULL;
		
		execvp(cmd->exe, args);
		free(args);
		exit(1);
	} else {
		int status;
		waitpid(pid, &status, 0);
		return WEXITSTATUS(status);
	}
}

static int
execute_piped_commands(const struct command_line *line)
{
	int cmd_count = 0;
	struct expr *current = line->head;
	while (current != NULL) {
		if (current->type == EXPR_TYPE_COMMAND)
			cmd_count++;
		current = current->next;
	}
	
	if (cmd_count == 0)
		return 0;
	
	struct command *commands = malloc(sizeof(struct command) * cmd_count);
	if (commands == NULL) {
		perror("malloc");
		return 1;
	}
	
	current = line->head;
	int cmd_index = 0;
	while (current != NULL) {
		if (current->type == EXPR_TYPE_COMMAND) {
			commands[cmd_index++] = current->cmd;
		}
		current = current->next;
	}
	
	int pipe_count = cmd_count - 1;
	int (*pipes)[2] = NULL;
	
	if (pipe_count > 0) {
		pipes = malloc(sizeof(int[2]) * pipe_count);
		if (pipes == NULL) {
			perror("malloc");
			free(commands);
			return 1;
		}
		
		for (int i = 0; i < pipe_count; i++) {
			if (pipe(pipes[i]) < 0) {
				perror("pipe");
				for (int j = 0; j < i; j++) {
					close(pipes[j][0]);
					close(pipes[j][1]);
				}
				free(pipes);
				free(commands);
				return 1;
			}
		}
	}
	
	pid_t *pids = malloc(sizeof(pid_t) * cmd_count);
	if (pids == NULL) {
		perror("malloc");
		if (pipes != NULL) {
			for (int i = 0; i < pipe_count; i++) {
				close(pipes[i][0]);
				close(pipes[i][1]);
			}
			free(pipes);
		}
		free(commands);
		return 1;
	}
	
	for (int i = 0; i < cmd_count; i++) {
		pids[i] = fork();
		
		if (pids[i] < 0) {
			for (int j = 0; j < i; j++) {
				kill(pids[j], SIGTERM);
			}
			
			if (pipes != NULL) {
				for (int j = 0; j < pipe_count; j++) {
					close(pipes[j][0]);
					close(pipes[j][1]);
				}
				free(pipes);
			}
			free(pids);
			free(commands);
			return 1;
		}
		
		if (pids[i] == 0) {
			if (i > 0) {
				dup2(pipes[i-1][0], STDIN_FILENO);
			}
			
			if (i < cmd_count - 1) {
				dup2(pipes[i][1], STDOUT_FILENO);
			} 
			else if (line->out_type != OUTPUT_TYPE_STDOUT) {
				int fd;
				if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
					fd = open(line->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
				} else {
					fd = open(line->out_file, O_WRONLY | O_CREAT | O_APPEND, 0666);
				}
				
				if (fd < 0) {
					exit(1);
				}
				
				dup2(fd, STDOUT_FILENO);
				close(fd);
			}
			
			if (pipes != NULL) {
				for (int j = 0; j < pipe_count; j++) {
					close(pipes[j][0]);
					close(pipes[j][1]);
				}
			}
			
			if (strcmp(commands[i].exe, "exit") == 0) {
				int exit_code = 0;
				if (commands[i].arg_count > 0)
					exit_code = atoi(commands[i].args[0]);
				if (i < cmd_count - 1) {
					close(STDOUT_FILENO);
				}
				exit(exit_code);
			}
			
			if (strcmp(commands[i].exe, "cd") == 0) {
				const char *path = ".";
				if (commands[i].arg_count > 0)
					path = commands[i].args[0];
				
				if (chdir(path) != 0) {
					exit(1);
				}
				exit(0);
			}
			
			char **args = malloc(sizeof(char *) * (commands[i].arg_count + 2));
			if (args == NULL) {
				exit(1);
			}
			
			args[0] = commands[i].exe;
			for (uint32_t j = 0; j < commands[i].arg_count; j++) {
				args[j + 1] = commands[i].args[j];
			}
			args[commands[i].arg_count + 1] = NULL;
			
			int devnull = open("/dev/null", O_WRONLY);
			if (devnull != -1) {
				dup2(devnull, STDERR_FILENO);
				close(devnull);
			}
			
			execvp(commands[i].exe, args);
			free(args);
			exit(1);
		}
	}
	
	if (pipes != NULL) {
		for (int i = 0; i < pipe_count; i++) {
			close(pipes[i][0]);
			close(pipes[i][1]);
		}
		free(pipes);
	}
	
	int status = 0;
	int exit_index = -1;
	
	for (int i = 0; i < cmd_count; i++) {
		if (strcmp(commands[i].exe, "exit") == 0) {
			exit_index = i;
		}
	}
	
	for (int i = 0; i < cmd_count; i++) {
		int cmd_status;
		waitpid(pids[i], &cmd_status, 0);
		
		if (i == exit_index) {
			status = WEXITSTATUS(cmd_status);
		}
		else if (exit_index == -1 && i == cmd_count - 1) {
			status = WEXITSTATUS(cmd_status);
		}
	}
	
	if (exit_index != -1) {
		int exit_code = 0;
		if (commands[exit_index].arg_count > 0) {
			exit_code = atoi(commands[exit_index].args[0]);
		}
		status = exit_code;
	}
	
	free(pids);
	free(commands);
	
	return status;
}

static bool
has_pipes(const struct command_line *line)
{
	assert(line != NULL);
	
	struct expr *current = line->head;
	while (current != NULL && current->next != NULL) {
		if (current->next->type == EXPR_TYPE_PIPE) {
			return true;
		}
		current = current->next;
	}
	
	return false;
}

static int
execute_command_line(const struct command_line *line)
{
	assert(line != NULL);
	
	if (line->head->type == EXPR_TYPE_COMMAND && 
		strcmp(line->head->cmd.exe, "exit") == 0 && 
		line->head->next == NULL) {
		
		int exit_code = 0;
		if (line->head->cmd.arg_count > 0)
			exit_code = atoi(line->head->cmd.args[0]);
		exit(exit_code);
	}
	
	if (line->head->type == EXPR_TYPE_COMMAND && 
		strcmp(line->head->cmd.exe, "cd") == 0 && 
		line->head->next == NULL && 
		line->out_type == OUTPUT_TYPE_STDOUT) {
		
		return execute_single_command(&line->head->cmd);
	}
	
	if (has_pipes(line)) {
		return execute_piped_commands(line);
	}
	
	if (line->out_type != OUTPUT_TYPE_STDOUT) {
		pid_t pid = fork();
		
		if (pid < 0) {
			return 1;
		}
		
		if (pid == 0) {
			int devnull = open("/dev/null", O_WRONLY);
			if (devnull != -1) {
				dup2(devnull, STDERR_FILENO);
				close(devnull);
			}
			
			int fd;
			if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
				fd = open(line->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
			} else {
				fd = open(line->out_file, O_WRONLY | O_CREAT | O_APPEND, 0666);
			}
			
			if (fd < 0) {
				exit(1);
			}
			
			dup2(fd, STDOUT_FILENO);
			close(fd);
			
			int status = execute_single_command(&line->head->cmd);
			
			exit(status);
		} else {
			int status;
			waitpid(pid, &status, 0);
			return WEXITSTATUS(status);
		}
	}
	
	return execute_single_command(&line->head->cmd);
}

int
main(void)
{
	char buf[4096];
	ssize_t bytes_read;
	
	struct parser *p = parser_new();
	if (p == NULL) {
		perror("parser_new");
		return 1;
	}
	
	bool is_interactive = isatty(STDIN_FILENO);
	
	int last_status = 0;
	
	while (1) {
		if (is_interactive) {
			printf("> ");
			fflush(stdout);
		}
		
		bytes_read = read(STDIN_FILENO, buf, sizeof(buf));
		
		if (bytes_read <= 0) {
			if (bytes_read == 0 || errno == EINTR) {
				break;
			}
			break;
		}
		
		parser_feed(p, buf, bytes_read);
		
		struct command_line *line = NULL;
		while (1) {
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			
			if (err != PARSER_ERR_NONE) {
				break;
			}
			
			last_status = execute_command_line(line);
			
			command_line_delete(line);
			line = NULL;
		}
	}
	
	parser_delete(p);
	
	return last_status;
}
