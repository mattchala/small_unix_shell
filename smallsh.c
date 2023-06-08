#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#define WORD_LIMIT 512


/*
 * Matt Chalabian
 * CS 344 - Operating Systems - Winter 2023
 * Assignment 3 - Smallsh
 * Due February 26, 2023 
 */



// primary functions
void print_prompt();  // print PS1 prompt
void get_input(char **input_pointer, size_t *input_size, char **words);  // get input, put in tokens, expansions, make sure not too many words
void process_input(char **words); // call other funcs in this func 

// helper functions
void str_gsub(char **haystack); 
void str_gsub_helper(char *str, char **haystack, char const *needle, char const *sub, size_t hay_len, size_t needle_len, size_t sub_len);
void cd_built_in(char **words);
void exit_built_in(char **words);
void non_built_in_commands(char **words, int is_background_process, int in_flag, int out_flag, char *in_file, char *out_file);
void parse_for_comment(char **words);
int parse_for_background(char **words);
void parse_for_redirect(char **words, int*in_flag, int *out_flag, char **in_file, char **out_file);
void manage_background_processes();

// global variables
int last_foreground_error_code = 0;     // $? variable
pid_t last_background_process = -1;     // $! variable
struct sigaction ignore_action = {0}, prev_sigint_action = {0}, prev_sigtstp_action = {0};



/*
 *
 * ###################
 * #                 #
 * #  main function  #
 * #                 #
 * ###################
 *
 */
int
main(void)
{
  // ignore sigtstp & sigint
  ignore_action.sa_handler = SIG_IGN;
  sigaction(SIGTSTP, &ignore_action, &prev_sigtstp_action);
  sigaction(SIGINT, &ignore_action, &prev_sigint_action);

  // set up input string pointer outside of loop
  char *input_pointer = NULL;
  size_t input_size = 0;

  // program loop:
  for (;;)
  {
    manage_background_processes();

    // for copying tokens over to expandable words
    char **words = NULL;  
    words = malloc(sizeof(*words) * WORD_LIMIT); 

    print_prompt();
    
    sigaction(SIGINT, &prev_sigint_action, NULL);

    get_input(&input_pointer, &input_size, words);

    sigaction(SIGINT, &ignore_action, NULL);

    process_input(words);

    for (int i = 0; i < WORD_LIMIT; i++) words[i] = NULL;  // null all words args
  }
  free(input_pointer);  
  return 0;
}





/*
 *
 * this prints the shell command prompt, depending on what PS1 is set to
 *
 */
void print_prompt()
{
  if (getenv("PS1") == NULL)
  {
    fprintf(stderr, "");
  }
  else
  {
    fprintf(stderr, "%s", getenv("PS1"));
  }
}



/*
 *
 * this gets the input from the user from stdin and separates the input into
 *  tokens and also carries out expansion
 *
 */
void get_input(char **input_pointer, size_t *input_size, char **words)
{
  // get user input, delimited by newline
  getline(input_pointer, input_size, stdin);

  // close program if EOF
  if (feof(stdin))
  {
    fprintf(stderr, "\nexit\n");
    exit(last_foreground_error_code);
  }

  // set token delimiter variable based on IFS enviornment variable
  char *IFS_val = NULL;
  IFS_val = getenv("IFS");
  if (IFS_val == NULL) IFS_val = " \t\n";

  // split into tokens, duplicate into words array
  char* token = strtok(*input_pointer, IFS_val);
  int word_amount = 0;
  for (;;)
  {
    if (token == NULL) break;
    if (word_amount >= WORD_LIMIT)
    {
      fprintf(stderr, "\nERROR: too many args: word count is %i\n", word_amount);
      last_foreground_error_code = 1;
      return;
    }
    // duplicate into words array
    words[word_amount] = strdup(token);
    
    // carry out expansion 
    str_gsub(&words[word_amount]);

    token = strtok(NULL, IFS_val);
    word_amount += 1;
  }
}



/*
 *
 * this is used by get_input to expand certain substrings
 *  this function handles different conditions and calls a helper function to carry out the actual expansion
 *
 * SOURCE CITED : uses prof gambord's provided example as basis
 *
 */
void str_gsub(char **haystack)
{
  char *str = *haystack;
  size_t hay_len = strlen(str);
  size_t needle_len = 2;
  size_t sub_len;

  size_t position = 1;
  while (position < hay_len)
  {
    if (str[position-1] == '$')
    {
      if (str[position] == '$')
      {
        // smallsh process id expansion for $$
        char pid_str[20] = { '\0' };
        sprintf(pid_str, "%d", getpid());
        sub_len = strlen(pid_str);
        str_gsub_helper(str, haystack, "$$", pid_str, hay_len, needle_len, sub_len);
        hay_len = hay_len - needle_len + sub_len;  
        position = position - needle_len + sub_len + 1;
      }
      else if (str[position] == '?')
      {
        // exit status of last foreground command expansion for $?
        char err_code_string[20] = { '\0' };
        sprintf(err_code_string, "%d", last_foreground_error_code);
        sub_len = strlen(err_code_string);
        str_gsub_helper(str, haystack, "$?", err_code_string, hay_len, needle_len, sub_len);
        hay_len = hay_len - needle_len + sub_len; 
        position = position - needle_len + sub_len + 1;
      }
      else if (str[position] == '!')
      {
        // most recent background process id expansion for $!
        char pid_str[20] = { '\0' };
        if (last_background_process != -1) sprintf(pid_str, "%d", last_background_process);
        sub_len = strlen(pid_str);
        str_gsub_helper(str, haystack, "$!", pid_str, hay_len, needle_len, sub_len);
        hay_len = hay_len - needle_len + sub_len;
        position = position - needle_len + sub_len + 1;
      }
    }
    else if (str[position] == '/' && str[position-1] == '~')
    {
      // home directory expansion for ~/
      char* path = getenv("HOME");
      sub_len = strlen(path);
      str_gsub_helper(str, haystack, "~/", path, hay_len, 1, sub_len);
      hay_len = hay_len - 1 + sub_len;
      position = position - needle_len + sub_len + 1;
    }
    position += 1;
  }
}



/*
 *
 * this is the str_gsub helper function that carries out the actual variable expansion, depending on 
 *  the arguments passed in by the str_gsub function
 *
 * SOURCE CITED : uses prof gambord's provided example as basis
 *
 */
void str_gsub_helper(char *str, char **haystack, char const *needle, char const *sub, size_t hay_len, size_t needle_len, size_t sub_len)
{
  str = strstr(str, needle);
  ptrdiff_t off = str - *haystack;
  if (sub_len > needle_len)
  {
    str = realloc(*haystack, sizeof **haystack * (hay_len + sub_len - needle_len + 1));
    if (!str) goto exit;
    *haystack = str;
    str = *haystack + off;
  }
  memmove(str + sub_len, str + needle_len, hay_len + 1 - off - needle_len);
  memcpy(str, sub, sub_len);
  hay_len = hay_len + sub_len - needle_len;
  str += sub_len;

  str = *haystack;
  if (sub_len < needle_len)
  {
    str = realloc(*haystack, sizeof **haystack * (hay_len + 1));
    if (!str) goto exit;
    *haystack = str;
  }
exit:
  return;
}



/*
 *
 * this is the primary function for processing the user input and uses a series
 *  of helper functions for specific tasks
 *
 */
void process_input(char **words)
{
  if (words[0] == NULL) return; 
                
  parse_for_comment(words); 

  if (strcmp(words[0], "cd") == 0)
  {
    cd_built_in(words);
    return;
  }

  else if (strcmp(words[0], "exit") == 0)
  {
    exit_built_in(words); 
    return;
  }

  int background_process = parse_for_background(words);

  // redirection variable setup
  int in_flag = 0;
  int out_flag = 0;
  char *in_file = NULL;
  char *out_file = NULL;

  parse_for_redirect(words, &in_flag, &out_flag, &in_file, &out_file);
  parse_for_redirect(words, &in_flag, &out_flag, &in_file, &out_file);
  
  // redirect error check
  if (in_flag > 1 || out_flag > 1)
  {
    fprintf(stderr, "\nError Redirecting\n");
    last_foreground_error_code = 1;
    fflush(stderr);
    return;
  }

  non_built_in_commands(words, background_process, in_flag, out_flag, in_file, out_file); // TODO : signals, redirect, save child exit code
}



/*
 *
 * cd built-in logc
 *
 */
void cd_built_in(char **words)
{
  if (words[2] != NULL) 
  {
    fprintf(stderr, "\nERROR: Too many arguments!\n");
    fflush(stderr);
    last_foreground_error_code = 1;
    return;
  } 

  if (words[1] == NULL)
  {
    if (chdir(getenv("HOME")) != 0) 
    {
      fprintf(stderr, "\nError changing directory.\n");
      last_foreground_error_code = 1;
      fflush(stderr);
      return;
    }
  }
  else
  {
    if (chdir(words[1]) != 0)
    {
      fprintf(stderr, "\nError changing directory.\n"); 
      last_foreground_error_code = 1;
      fflush(stderr);
      return;
    }
  }
  last_foreground_error_code = 0;
}



/*
 *
 * exit built-in logic
 *
 */ 
void exit_built_in(char **words)
{
  // check for too many args (more than 1)
  if (words[2] != NULL)
  {
    fprintf(stderr, "\nERROR: Too many arguments!\n");
    last_foreground_error_code = 1;
    fflush(stderr);
    return;
  }

  // check if the argument is an int
  if (words[1] != NULL)
  {
    int i = 0;
    while (words[1][i] != '\0')
    {
      if (isdigit(words[1][i]) == 0)
      {
        fprintf(stderr, "\nERROR: Argument not an integer!\n");
        last_foreground_error_code = 1;
        fflush(stderr);
        return;
      }
      i++;
    }
  }

  // carry out exit
  fprintf(stderr, "\nexit\n");

  // send SIGINT to children 
  kill(0, SIGINT);  

  if (words[1] == NULL) exit(last_foreground_error_code);

  exit(atoi(words[1])); 
}



/*
 *
 * function for handling all non-built-in commands
 *
 * SOURCE CITED : used a small bit of my old code in this, but it mostly is similar to the exploration code
 *
 */ 
void non_built_in_commands(char **words, int is_background_process, int in_flag, int out_flag, char *in_file, char *out_file)
{
  int child_status;
  pid_t spawn_pid = fork();

  switch(spawn_pid)
  {
    case -1: // error case

      perror("fork()\n");
      exit(1);        
      break;

    case 0:  // child process case
  
      // reset signals to default for children
      sigaction(SIGTSTP, &prev_sigtstp_action, NULL);
      sigaction(SIGINT, &prev_sigint_action, NULL);

      // redirect stdout to in_file
      if (in_flag == 1) dup2(open(in_file, O_RDONLY | O_CLOEXEC), STDIN_FILENO); 

      // redirect stdin to out_file
      if (out_flag == 1) dup2(open(out_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0777), STDOUT_FILENO);

      execvp(words[0], words);
      perror("execvp");
      exit(2);
      break;

    default:  // parent process case
      
      // foreground process
      if (is_background_process == 0)
      { 
        spawn_pid = waitpid(spawn_pid, &child_status, 0);
        if (WIFEXITED(child_status)) last_foreground_error_code = WEXITSTATUS(child_status);

        if (WIFSIGNALED(child_status)) last_foreground_error_code = 128 + WTERMSIG(child_status);

        if (WIFSTOPPED(child_status))
        {
          fprintf(stderr, "Child process %d stopped. Continuing.\n", spawn_pid);
          fflush(stderr);
          kill(spawn_pid, SIGCONT);
          last_background_process = spawn_pid;
        }
      }

      // background process
      else  // background
      {
        last_background_process = spawn_pid;
      }
      break;
  }
}



/*
 *
 * comment parsing function
 *
 */
void parse_for_comment(char **words)
{
  int comment_found = 0;
  int i = 0;
  while (i < WORD_LIMIT)
  {
    if (words[i] != NULL)
    {
      if (strcmp(words[i], "#") == 0) comment_found = 1;
    }
    if (comment_found == 1) words[i] = NULL;
    i++;
  }
}



/*
 *
 * background parsing function
 *
 */
int parse_for_background(char **words)
{
  int i = 0;
  while (i < WORD_LIMIT && words[i] != NULL) i++;

  if (strcmp(words[i-1], "&") == 0)
  {
    words[i-1] = NULL;
    return 1;
  }
  return 0;
}



/*
 *
 * redirect parsing function - runs twice  - possible issues  < in < in : double redir in same command, maybe do additional check
 *
 */
void parse_for_redirect(char **words, int*in_flag, int *out_flag, char **in_file, char **out_file)
{
  int i = 0;
  while (i < WORD_LIMIT && words[i] != NULL) i++;

  if (i < 2) return;

  if (strcmp(words[i - 2], "<") == 0)
  {
    *in_flag = *in_flag + 1;
    *in_file = strdup(words[i - 1]);
    words[i - 1] = NULL;
    words[i - 2] = NULL;
  }
  else if (strcmp(words[i - 2], ">") == 0)
  {
    *out_flag = *out_flag + 1;
    *out_file = strdup(words[i - 1]);
    words[i - 1] = NULL;
    words[i - 2] = NULL;
  }
}



/*
 *
 * function that checks status of any current background processes
 *
 */
void manage_background_processes()
{
  int background_status;
  pid_t background_pid = waitpid(0, &background_status, WNOHANG | WUNTRACED);
  while (background_pid > 0)
  {
    // if completed
    if (WIFEXITED(background_status))
    {
      fprintf(stderr, "Child process %d done. Exit status %d.\n", background_pid, WEXITSTATUS(background_status));
      fflush(stderr);
    }
    // if terminated
    else if (WIFSIGNALED(background_status))
    {
      fprintf(stderr, "Child process %d done. Signaled %d.\n", background_pid, WTERMSIG(background_status));
      fflush(stderr);
    }
    else if (WIFSTOPPED(background_status))
    {
      fprintf(stderr, "Child process %d stopped. Continuing.\n", background_pid);
      fflush(stderr);
      kill(background_pid, SIGCONT);
    }
    // check if other children finished / terminated during loop
    background_pid = waitpid(0, &background_status, WNOHANG | WUNTRACED);
  }
}

