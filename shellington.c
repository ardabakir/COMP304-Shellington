#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>            //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <dirent.h>
#include <linux/sched.h>
#include <ctype.h>				//to use the tolower function in plist awesome command
const char * sysname = "shellington";

enum return_codes {
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};
struct command_t {
	char *name;
	bool background;
	bool auto_complete;
	int arg_count;
	char **args;
	char *redirects[3]; // in/out redirection
	struct command_t *next; // for piping
};
/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t * command)
{
	int i=0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background?"yes":"no");
	printf("\tNeeds Auto-complete: %s\n", command->auto_complete?"yes":"no");
	printf("\tRedirects:\n");
	for (i=0;i<3;i++)
		printf("\t\t%d: %s\n", i, command->redirects[i]?command->redirects[i]:"N/A");
	printf("\tArguments (%d):\n", command->arg_count);
	for (i=0;i<command->arg_count;++i)
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	if (command->next)
	{
		printf("\tPiped to:\n");
		print_command(command->next);
	}


}
/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command)
{
	if (command->arg_count)
	{
		for (int i=0; i<command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}
	for (int i=0;i<3;++i)
		if (command->redirects[i])
			free(command->redirects[i]);
	if (command->next)
	{
		free_command(command->next);
		command->next=NULL;
	}
	free(command->name);
	free(command);
	return 0;
}
/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt()
{
	char cwd[1024], hostname[1024];
    gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}
/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command)
{
	const char *splitters=" \t"; // split at whitespace
	int index, len;
	len=strlen(buf);
	while (len>0 && strchr(splitters, buf[0])!=NULL) // trim left whitespace
	{
		buf++;
		len--;
	}
	while (len>0 && strchr(splitters, buf[len-1])!=NULL)
		buf[--len]=0; // trim right whitespace

	if (len>0 && buf[len-1]=='?') // auto-complete
		command->auto_complete=true;
	if (len>0 && buf[len-1]=='&') // background
		command->background=true;

	char *pch = strtok(buf, splitters);
	command->name=(char *)malloc(strlen(pch)+1);
	if (pch==NULL)
		command->name[0]=0;
	else
		strcpy(command->name, pch);

	command->args=(char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index=0;
	char temp_buf[1024], *arg;
	while (1)
	{
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch) break;
		arg=temp_buf;
		strcpy(arg, pch);
		len=strlen(arg);

		if (len==0) continue; // empty arg, go for next
		while (len>0 && strchr(splitters, arg[0])!=NULL) // trim left whitespace
		{
			arg++;
			len--;
		}
		while (len>0 && strchr(splitters, arg[len-1])!=NULL) arg[--len]=0; // trim right whitespace
		if (len==0) continue; // empty arg, go for next

		// piping to another command
		if (strcmp(arg, "|")==0)
		{
			struct command_t *c=malloc(sizeof(struct command_t));
			int l=strlen(pch);
			pch[l]=splitters[0]; // restore strtok termination
			index=1;
			while (pch[index]==' ' || pch[index]=='\t') index++; // skip whitespaces

			parse_command(pch+index, c);
			pch[l]=0; // put back strtok termination
			command->next=c;
			continue;
		}

		// background process
		if (strcmp(arg, "&")==0)
			continue; // handled before

		// handle input redirection
		redirect_index=-1;
		if (arg[0]=='<')
			redirect_index=0;
		if (arg[0]=='>')
		{
			if (len>1 && arg[1]=='>')
			{
				redirect_index=2;
				arg++;
				len--;
			}
			else redirect_index=1;
		}
		if (redirect_index != -1)
		{
			command->redirects[redirect_index]=malloc(len);
			strcpy(command->redirects[redirect_index], arg+1);
			continue;
		}

		// normal arguments
		if (len>2 && ((arg[0]=='"' && arg[len-1]=='"')
			|| (arg[0]=='\'' && arg[len-1]=='\''))) // quote wrapped arg
		{
			arg[--len]=0;
			arg++;
		}
		command->args=(char **)realloc(command->args, sizeof(char *)*(arg_index+1));
		command->args[arg_index]=(char *)malloc(len+1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count=arg_index;
	return 0;
}
void prompt_backspace()
{
	putchar(8); // go back 1
	putchar(' '); // write empty over
	putchar(8); // go back 1 again
}
/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command)
{
	int index=0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

    // tcgetattr gets the parameters of the current terminal
    // STDIN_FILENO will tell tcgetattr that it should write the settings
    // of stdin to oldt
    static struct termios backup_termios, new_termios;
    tcgetattr(STDIN_FILENO, &backup_termios);
    new_termios = backup_termios;
    // ICANON normally takes care that one line at a time will be processed
    // that means it will return if it sees a "\n" or an EOF or an EOL
    new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
    // Those new settings will be set to STDIN
    // TCSANOW tells tcsetattr to change attributes immediately.
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);


    //FIXME: backspace is applied before printing chars
	show_prompt();
	int multicode_state=0;
	buf[0]=0;
  	while (1)
  	{
		c=getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		if (c==9) // handle tab
		{
			buf[index++]='?'; // autocomplete
			break;
		}

		if (c==127) // handle backspace
		{
			if (index>0)
			{
				prompt_backspace();
				index--;
			}
			continue;
		}
		if (c==27 && multicode_state==0) // handle multi-code keys
		{
			multicode_state=1;
			continue;
		}
		if (c==91 && multicode_state==1)
		{
			multicode_state=2;
			continue;
		}
		if (c==65 && multicode_state==2) // up arrow
		{
			int i;
			while (index>0)
			{
				prompt_backspace();
				index--;
			}
			for (i=0;oldbuf[i];++i)
			{
				putchar(oldbuf[i]);
				buf[i]=oldbuf[i];
			}
			index=i;
			continue;
		}
		else
			multicode_state=0;

		putchar(c); // echo the character
		buf[index++]=c;
		if (index>=sizeof(buf)-1) break;
		if (c=='\n') // enter key
			break;
		if (c==4) // Ctrl+D
			return EXIT;
  	}
  	if (index>0 && buf[index-1]=='\n') // trim newline from the end
  		index--;
  	buf[index++]=0; // null terminate string

  	strcpy(oldbuf, buf);

  	parse_command(buf, command);

  	// print_command(command); // DEBUG: uncomment for debugging

    // restore the old settings
    tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
  	return SUCCESS;
}

char shortcut[50][1024];	//to keep the shortcuts in memory
char mark[50][1024];		//to keep the bookmarks in memory
//number of entries in bookmarks and shortcuts
int shortcutIndex = 0;		
int markIndex = 0;
int process_command(struct command_t *command);
int main()
{
	while (1)
	{
		struct command_t *command=malloc(sizeof(struct command_t));
		memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

		int code;
		code = prompt(command);
		if (code==EXIT) break;

		code = process_command(command);
		if (code==EXIT) break;

		free_command(command);
	}

	printf("\n");
	return 0;
}

int process_command(struct command_t *command)
{
	int r;
	if (strcmp(command->name, "")==0) return SUCCESS;

	if (strcmp(command->name, "exit")==0)
		return EXIT;

	if (strcmp(command->name, "cd")==0)
	{
		if (command->arg_count > 0)
		{
			r=chdir(command->args[0]);
			if (r==-1)
				printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
			return SUCCESS;
		}
	}
	if(strcmp(command->name, "short")==0){
			
		if(strcmp(command->args[0], "set") == 0){
			
			char dir[1024];
			getcwd(dir,sizeof(dir));
			//tries to find the shortcut
			//if it finds the entry changes it to the new shortcut key
			for(int i=0;i<shortcutIndex; i+=2){
				if(strcmp(shortcut[i],command->args[1])==0){
					strcpy(shortcut[i+1],dir);
					return SUCCESS;
				}
			}
			//adds two items in shortcut array
			//one of them is the index of the shortcut and the other is the real shortcut
			strcpy(shortcut[shortcutIndex], command->args[1]);
			strcpy(shortcut[shortcutIndex+1],dir);
			shortcutIndex += 2;
			
			return SUCCESS;
		}
		else if(strcmp(command->args[0],"jump")==0)
		{
			//looks for the shortcut key by iterating over the shortcut array 
			//if it finds it jumps to the related directory
			//else returns exit
			for(int i=0;i<shortcutIndex; i+=2){
				if(strcmp(shortcut[i],command->args[1])==0){
					chdir(shortcut[i+1]);
					return SUCCESS;
				}
			}
		}
		else{
			return EXIT;
		}
	}
	
	
	pid_t pid=fork();
	if (pid==0) // child
	{
		/// This shows how to do exec with environ (but is not available on MacOs)
	    // extern char** environ; // environment variables
		// execvpe(command->name, command->args, environ); // exec+args+path+environ

		/// This shows how to do exec with auto-path resolve
		// add a NULL argument to the end of args, and the name to the beginning
		// as required by exec

		// increase args size by 2
		command->args=(char **)realloc(
			command->args, sizeof(char *)*(command->arg_count+=2));

		// shift everything forward by 1
		for (int i=command->arg_count-2;i>0;--i)
			command->args[i]=command->args[i-1];

		// set args[0] as a copy of name
		command->args[0]=strdup(command->name);
		// set args[arg_count-1] (last) to NULL
		command->args[command->arg_count-1]=NULL;

		//execvp(command->name, command->args); // exec+args+path
		
		/// TODO: do your own exec with path resolving using execv()

		
		if(strcmp(command->args[0],"bookmark")==0){

			if(strcmp(command->args[1],"-l")==0){
				
				char num[30];
				//iterates over the bookmarked array 
				//prints the bookmarks with their indexes
				for(int i=0; i<markIndex; i++){
					sprintf(num, "%d" ,i);	//converts integer to string
					printf("%s %s\n",num,mark[i]);			
				}
				return SUCCESS;
			}
			else if(strcmp(command->args[1],"-i")==0){
				
				char temp[1024];								//holds the bookmarked command
				char* arguments[256];			
				char* token;
				strcpy(temp,mark[atoi(command->args[2])]);		//copies the bookmark in the corresponding index to temp 
				
				token = strtok(temp," ");						//seperates the string by the white spaces
				token++;										//deletes the first index of token which is a quotation mark
				
				char* commandName = token;
				int i = 1;
				token = strtok(NULL," ");
				//if token is null the bookmarked command consists of only one letter
				//deletes the last char of the codename since it is a quotation mark
				//then copies the commandName to the arguments array
				if(token == NULL){
					commandName[strlen(commandName)-1] = '\0';
					arguments[0] = commandName;
				}else{
					arguments[0] = commandName;
					//adds all tokens to the arguments array to be used in execv
					while(token != NULL){
						arguments[i] = token;
						i++;
						token = strtok(NULL," ");
					}
					arguments[i-1][strlen(arguments[i-1])-1] = '\0';	//deletes the quotation mark at the end
				}
				
				arguments[i] = NULL;		//adds null as the last element of the arguments array
				
				char path[1024];
				//if commandName is cd changes directory
				if(strcmp(commandName,"cd")==0){
					chdir(arguments[1]);
				}
				//these commands are not stored in /bin/ directory but in /usr/bin/ directory
				if(strcmp(command->args[0], "gcc")==0 || strcmp(command->args[0],"crontab") == 0 || strcmp(command->args[0],"vim")==0){
					strcpy(path,"/usr/bin/");
				}else{
					strcpy(path,"/bin/");		//most commands are in /bin/ directory
				}
				
				strcat(path,commandName);
				
				execv(path,arguments);
				return SUCCESS;
			}
			else if(strcmp(command->args[1],"-d")==0){
				int delIndex = atoi(command->args[2]);
				//if delIndex is larger than the array's size returns without doing anything
				if(markIndex>delIndex){
					//starting from the delIndex copies all the elements to their left
					for(int i=delIndex; i<markIndex-1; i++){
						strcpy(mark[i],"");
						strcpy(mark[i],mark[i+1]);

					}
					strcpy(mark[markIndex-1],""); // deletes the cntents of the last element since it is a duplicate
					markIndex--;				  // decrements the array size
				}else
				return SUCCESS;
			}
			else{
				char tmp[1024];
				int i=2;
				//if the arguments consists of only one word adds quotations to the beginning and to the end
				//otherwise it keeps the quotation marks so we don't need to add manually
				if(command->args[i]==NULL){
					strcpy(tmp,"\"");
					strcat(tmp,command->args[1]);
					strcat(tmp,"\"");
				}else{
					strcpy(tmp,command->args[1]);
				}
				//iterates over the arguments and creates the tmp string
				while(command->args[i]!=NULL){
					strcat(tmp," ");
					strcat(tmp,command->args[i]);
					i++;
				}
				//adds tmp to the mark array and increment the size
				strcpy(mark[markIndex],tmp);
				markIndex++;

				return SUCCESS;
			}
		}
		else if(strcmp(command->args[0],"remindme")==0){
			char message[1024];
			strcpy(message,command->args[2]);
			int i=3;
			while(command->args[i]!=NULL){
				strcat(message," ");
				strcat(message,command->args[i]);
				i++;
			}
			printf("%s\n",message);

			char hour[10];
			char minute[10];
			char* token;
			token = strtok(command->args[1],".");
			strcpy(hour,token);
			token = strtok(NULL,".");
			strcpy(minute,token);

			printf("hour:%s minute:%s\n",hour,minute);
			FILE *fpNotify;
			fpNotify = fopen("notification.sh", "w");
			fprintf(fpNotify, "#!/bin/bash\n");
			fprintf(fpNotify,"echo %s\n",message);
			fclose(fpNotify);

			char cwd[1024];
			getcwd(cwd,sizeof(cwd));
			strcat(cwd,"/notification");


			FILE *fpCrontab;
			fpCrontab = fopen("cronFile","w");
			fprintf(fpCrontab,"%s %s * * * %s\n",minute,hour,cwd);
			fclose(fpCrontab);
			char* arguments[] = {"crontab","cronFile",NULL};
            execv("/usr/bin/crontab",arguments);
			return SUCCESS;

		}else if(strcmp(command->args[0],"plist")==0){
			DIR *d;
			struct dirent *dir;
			//opens the current directory
			d = opendir(".");
			if (d) {
				//checks every directory in current path
				while ((dir = readdir(d)) != NULL) {
					if(strcmp(command->args[1],"-s")==0){
						//this one is default 
						//if the directory's name contains the given input prints the directory name
						if(strstr(dir->d_name,command->args[2])!=NULL){
							printf("%s\n", dir->d_name);
						}
					}else if(strcmp(command->args[1],"-i")==0){
						//case insensitive
						char temp[1024];
						//copes the directory name to temp, then converts all the letters to lowercase
						strcpy(temp,dir->d_name);
						for(int i = 0; temp[i]; i++){
							temp[i] = tolower(temp[i]);
						}
						//if lowercase directory name contains the given input prints the directory name
						//otherwise converts the given input to lowercase to try again
						if(strstr(temp,command->args[2])!=NULL){
							printf("%s\n", dir->d_name);
						}else{
							char tempCommand[1024];
							strcpy(tempCommand,command->args[2]);
							for(int i = 0; tempCommand[i];i++){
								tempCommand[i] = tolower(tempCommand[i]);
							}
							if(strstr(temp,tempCommand)!=NULL){
								printf("%s\n", dir->d_name);
							}
						}
					}else if(strcmp(command->args[1],"-sw")==0){
						//creates a string of length of the given input
						char temp[strlen(command->args[2])];
						//copies the first "input length" amount of chars to temp
						strncpy(temp,dir->d_name,strlen(command->args[2]));
						//if the input matches the temp string prints the directory name
						if(strcmp(command->args[2],temp)==0){
							printf("%s\n", dir->d_name);
						}
					}else{
						//default case same with -s 
						if(strstr(dir->d_name,command->args[1])!=NULL){
					 		printf("%s\n", dir->d_name);
					 	}
					}
					
				}
				closedir(d);
				return SUCCESS;
			}
		}else if(strcmp(command->args[0],"calculate")==0){
			char* eptr;
			double op1 = strtod(command->args[2],&eptr);
			double result;
			if(strcmp(command->args[1],"+")==0){
				result = op1;
				for(int i=3; i<command->arg_count-1;i++){
					result += strtod(command->args[i],&eptr);
				}
			}else if(strcmp(command->args[1],"-")==0){
				result = op1;
				for(int i=3; i<command->arg_count-1;i++){
					result -= strtod(command->args[i],&eptr);
				}	
			}else if(strcmp(command->args[1],"/")==0){
				result = op1;
				for(int i=3; i<command->arg_count-1;i++){
					result /= strtod(command->args[i],&eptr);
				}	
			}else if(strcmp(command->args[1],"x")==0){
				result = op1;
				for(int i=3; i<command->arg_count-1;i++){
					result *= strtod(command->args[i],&eptr);
				}
			}else if(strcmp(command->args[1],"%")==0){
				if(command->arg_count !=5 ){
					printf("You should enter two integers with modulus operator");
					return EXIT;
				}else{
					double op2 = strtod(command->args[3],&eptr);
					result = (double)((int)op1 % (int)op2);
				}
			}else if (strcmp(command->args[1],"!")==0){
				result = 1.0;
				for(int i=1;i<=op1;i++){
					result *= i;
				}
			}
			printf("result is: %.2lf\n",result);
			return SUCCESS;
		}
		char *path;
   		path = (char *) malloc(150);
		//commands that run on linux are stored in /bin/ path
		if(strcmp(command->args[0], "gcc")==0 || strcmp(command->args[0],"crontab") == 0 || strcmp(command->args[0],"vim")==0 || strcmp(command->args[0],"notify-send")==0){
			strcpy(path,"/usr/bin/");
		}else{
			strcpy(path, "/bin/");
		}
		//add the command to the end of the path to run in execv
		strcat(path,command->args[0]);

		execv(path,command->args);
		
		exit(0);
	}
	else
	{
		if (!command->background)
			wait(0); // wait for child process to finish
		return SUCCESS;
	}

	// TODO: your implementation here

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}
