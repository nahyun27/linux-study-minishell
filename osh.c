#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>

#define MAX_LINE 80 /* The maximum length command */
#define READ_END 0 /* The read end of the pipe  */
#define WRITE_END 1 /* The write end of the pipe */

/* function to read and parse command inputs */ 
int read_input(char input_buffer[], char *args[], int *background) {
	int length; /* length of a command */
	int start; /* parameter starting index of input_buffer */
 	int next; /* index of args where the next parameter is placed */

	/* read inputs */
	printf("osh> ");
	fflush(stdout);
	length = read(STDIN_FILENO, input_buffer, MAX_LINE);
	
	if((length < 0)) {
		perror("\ncommand-reading failed");
		exit(-1);
	}
	
	/* init variables */
	start = -1;
  next = 0;

  /* parse inputs (char-by-char check) */
	int i;
	for(i = 0; i < length; i++){
		switch (input_buffer[i]) {
			/* space character */
			case ' ' :
			case '\t' :
				if(start != -1){
					args[next] = &input_buffer[start];
					next++;
				}
				input_buffer[i] = '\0';
				start = -1;
				break;

			/* final charcter (enter) */
			case '\n' :
				if(start != -1){
					args[next] = &input_buffer[start];
					next++;
				}
				input_buffer[i] = '\0';
				args[next] = NULL;
				break;

			/* final character (background) */
			case '&' :
				input_buffer[i] = '\0';
				args[next] = NULL; 
				*background = 1;
				break;
			
			/* other characters */
			default :
				if(start == -1){
					start = i;
				}
		}
	}
	args[next] = NULL;
	return 1;
}

/* function for finding pipe */
/* if '|' is in args, save piped input in piped_args and return 1 */
int is_piped(char** args, char** piped_args) {
	int i;
	for(i = 0; args[i] != NULL; i++){
		if(strcmp(args[i], "|") == 0) {
			int j = 0;
			args[i] = NULL;
			i++;
			
			/* save the piped inputs in piped_args */
			while (args[i] != NULL) {
				piped_args[j] = args[i];
				args[i] = NULL;
				i++;
				j++;
			}
			
			piped_args[j] = NULL;
			return 1;
		}
	}
	return 0;
}


/* function to execute commands */ 
void execute(char** args, const int *background) {
	int input_idx = 0; /* parameter starting index of input file */
	int output_idx = 0; /* parameter starting index of output file */
	int redir_idx = -1; /* location of redirection symbol */
	int fd0, fd1; /* file descriptors */
	pid_t pid = fork(); /* fork */

	/* redirection-check */
	int i; 
	for(i = 0; args[i] != NULL; i++){
		/* if '<' char was in args, save the starting index to input_idx */
		if(strcmp(args[i], "<") == 0) {
			redir_idx = i;
			input_idx = i+1;
		}
		
		/* if '>' char was in args, save the starting index to output_idx */
		if(strcmp(args[i], ">") == 0) {
			redir_idx = i;
			output_idx = i+1;
		} 
	}

	switch (pid) {
		case -1 :
			perror("\nfork failed");
			break;

		/* child process (execute the command) */
		case 0 :
			/* '<' in args */
			if (input_idx != 0) { 
				fd0 = open(args[input_idx], O_RDONLY, 0);
				args[redir_idx] = NULL;
				if (fd0 < 0){
					perror("File failed");
				}
				dup2(fd0, STDIN_FILENO);
				close(fd0);
			}

			/* '>' in args */
			if (output_idx != 0) {
				fd1 = creat(args[output_idx], 0644);
				args[redir_idx] = NULL;
				if (fd1 < 0){
					perror("File failed");
				}
				dup2(fd1, STDOUT_FILENO);
				close(fd1);
			}

			if (execvp(args[0], args) != 0){
				perror("\nexecvp failed");
				exit(-2);
			}
			break;

		/* parent process (wait) */
		default :
			if (*background == 0) {
				wait(NULL);
			}
	}
}

/* function to execute piped commands */
void execute_pipe(char **args, char **pipe_args, const int *background) {
	int fd[2]; /* file descriptors */
	pid_t p1, p2; /* pids */

	if (pipe(fd) < 0) {
		perror("\npipe failed");
		return;
	}

	p1 = fork();
	if (p1 < 0) {
		perror("\nfork failed");
		return;
	}

	/* child process 1 */
	/* write end */
	if (p1 == 0) {
		close(fd[READ_END]);
		dup2(fd[WRITE_END], STDOUT_FILENO);
		close(fd[WRITE_END]);

		if (execvp(args[0], args) < 0) {
			perror("\nexecvp failed");
			exit(0);
		}
	}

	/* parent process */
	else {
		p2 = fork();
		if (p2 < 0) {
			perror("\nfork failed");
			return;
		}

		/* child process 2 */
		/* read end */
		if (p2 == 0) {
			close(fd[WRITE_END]);
			dup2(fd[READ_END], STDIN_FILENO);
			close(fd[READ_END]);

			if (execvp(pipe_args[0], pipe_args) < 0) {
				perror("\nexecvp failed");
				exit(0);
			}
		} 
		/* parent process (wait)  */
		else {
			if (*background == 0) {
				wait(NULL);
			}
		}
	}
}

int main(void) {
	char *args[MAX_LINE/2 + 1]; /* command line arguments */ 
	char *piped_args[MAX_LINE/2 + 1]; /* piped arguments */
	char input_buffer[MAX_LINE]; /* buffer for storing commands */
	
	int should_run; /* flag that determines whether a command has been properly entered */
	int background; /* whether to run in the background (1 when ended with "&") */

	while (1) {
		/* init variables*/
		background = 0;
		should_run = 0;

		/* read commands */
		should_run = read_input(input_buffer, args, &background);

		if (strstr(input_buffer, "exit") != NULL)
			return 0;

		/* fork */
		if (should_run) {
			if (is_piped(args, piped_args)) {
				execute_pipe(args, piped_args, &background);
			}
			else {
				execute(args, &background);
			}
		}
	}
	return 0;
}
