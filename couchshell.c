#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>

#define MAX_INPUT_SIZE 1024
#define MAX_HISTORY_SIZE 64
#define BUF_SIZE 4096
#define OPT_NUM 64
#define ANSI_COLOR_RESET "\x1b[0m" 
#define ANSI_COLOR_BLUE "\x1b[34m" 
#define ANSI_COLOR_MAGENTA "\x1b[35m" 
#define ANSI_COLOR_CYAN "\x1b[36m" 
#define ANSI_COLOR_WHITE "\x1b[37m"
#define NONE_REDIR   0
#define INPUT_REDIR  1
#define OUTPUT_REDIR 2
#define APPEND_REDIR  3
#define SHARED_MEMORY_SIZE (2 * sizeof(int) + 64 * sizeof(int))
#define trimSpace(start) do{\
			while(isspace(*start)) ++start;\
		}while(0)

char cwd[MAX_INPUT_SIZE]; //当前目录
char log_path[MAX_INPUT_SIZE];//历史log
char *commands[OPT_NUM];//被管道隔开的命令
char *myargv[OPT_NUM];//指针数组,存放参数
int lastcode = 0;
int lastsig = 0;//echo
int redirType = NONE_REDIR;
char* redirFile = NULL;//重定向

int history_log(const char *, const char *);
int command_back(char*);
void command_redi(char*);
void combine_path(char *);
void copy_file(const char *, const char *);
void copy_directory(const char *, const char *);
void print_process_info(const char *,const char *);
void recursive_remove(const char *);
const char *get_extension(const char *);
const char *get_color_code(const char *);
int myhelp();
int myhistory(const char *);
int myls(char *);
int myps();
int mytree(const char *, int, int *);
int mymv();
int mycp();
int myrm();
int myrun();
int sheller(int*, int*);

// 将输入写入日志文件的函数
int history_log(const char *input, const char *path) {
	FILE *log_file = fopen(path, "a");
	if (log_file == NULL) {
		perror("Error writing");
		return -1;
	}
	fprintf(log_file, "%s\n", input);
	fclose(log_file);
	return 0;
}

int command_back(char* commands) {
	assert(commands);
	char* start = commands;
	char* end = commands + strlen(commands);
	while (start < end) {
		if (*start == 'h' && start[1] == 'e') {
			break;
		} else if (*end == '&') {
			*end = '\0';
			return 1;
		} else if (*end == ' ' || *end == '\0') {
			end--;
		} else {
			break;
		}
	}
	return 0;
}

void command_redi(char* commands) {
	assert(commands);
	char* start = commands;
	char* end = commands + strlen(commands);
	while (start < end) {
		if (*start == 'h' && start[1] == 'e') {
			break;
		} else if (*start == '>') {
			*start = '\0';
			start++;
			if (*start == '>') {
				// "ls -a >> file.log"
				redirType = APPEND_REDIR;
				start++;
			} else {
				// "ls -a >	file.log"
				redirType = OUTPUT_REDIR;
			}
			trimSpace(start);
			redirFile = start;
			break;
		} else if (*start == '<') {
			//"cat < file.txt"
			*start = '\0';
			start++;
			trimSpace(start);
			// 填写重定向信息
			redirType = INPUT_REDIR;
			redirFile = start;
			break;
		} else {
			start++;
		}
	}
}

void combine_path(char *path) {
	strcpy(path,myargv[2]);
	strcat(path,"/");
	char *file_name = strrchr(myargv[1], '/');
	if(file_name == NULL) {
		file_name = myargv[1];
	}
	strcat(path, file_name);
}

void copy_file(const char *source, const char *dest) {
	int source_fd, dest_fd;
	char buffer[BUF_SIZE];
	ssize_t bytesRead, bytesWritten;
	//打开源文件
	source_fd = open(source, O_RDONLY);
	if (source_fd == -1) {
		perror("open source");
		return;
	}
	//创建或打开目标文件（覆盖）
	dest_fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (dest_fd == -1) {
		perror("open destination");
		close(source_fd);
		return;
	}
	//读取数据写入目标文件
	while ((bytesRead = read(source_fd, buffer, BUF_SIZE)) > 0) {
		bytesWritten = write(dest_fd, buffer, bytesRead);
		if (bytesWritten != bytesRead) {
			perror("write");
			close(source_fd);
			close(dest_fd);
			return;
		}
	}
	close(source_fd);
	close(dest_fd);
	return;
}

void copy_directory(const char *source, const char *dest) {
	DIR *dir;
	struct dirent *entry;
	struct stat statbuf;
	char source_path[PATH_MAX];
	char dest_path[PATH_MAX];
	//打开源目录
	dir = opendir(source);
	if (dir == NULL) {
		perror("opendir source");
		return;
	}
	//创建目标目录
	if (mkdir(dest, S_IRWXU | S_IRWXG | S_IRWXO) == -1) {
		perror("mkdir destination");
		closedir(dir);
		return;
	}
	//读取目录
	while ((entry = readdir(dir)) != NULL) {
		//限制性格式化拼接路径字符串
		snprintf(source_path, PATH_MAX, "%s/%s", source, entry->d_name);
		snprintf(dest_path, PATH_MAX, "%s/%s", dest, entry->d_name);
		//使用 stat 获取文件信息
		if (stat(source_path, &statbuf) == -1) {
			perror("stat");
			closedir(dir);
			return;
		}
		//递归copy子目录
		if (S_ISDIR(statbuf.st_mode)) {
			if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
				copy_directory(source_path, dest_path);
			}
		} else {
			//递归copy文件
			copy_file(source_path, dest_path);
		}
	}
	closedir(dir);
	return;
}

void print_process_info(const char *pid,const char *tty) {
	char path[1024];
	FILE *statusFile, *statFile;
	// 构造status文件路径
	snprintf(path, sizeof(path), "/proc/%s/status", pid);
	statusFile = fopen(path, "r");
	if (statusFile == NULL) {
		perror("Error opening status file");
		return;
	}
	// 读取并打印status文件内容
	char line[1024];
	while (fgets(line, sizeof(line), statusFile) != NULL) {
		// 查找TTY字段
		if (strncmp(line, "State:", 6) == 0) {
			// 如果TTY字段与当前终端相同，打印进程信息
			if (strstr(line, "R") != NULL) {
				printf("running Process ID: %s\n", pid);
				fseek(statusFile, 0, SEEK_SET);
				// 重新设置文件指针到文件开头
				while (fgets(line, sizeof(line), statusFile) != NULL) {
					if((strncmp(line, "Name:", 5) == 0)||(strncmp(line, "State:", 6) == 0)||(strncmp(line, "PPid:", 5) == 0)||(strncmp(line, "Threads:", 8) == 0)||(strncmp(line, "VmPin:", 6) == 0)) {
						printf("%s", line);
					}
				}
				break;
			}
		}
	}
	fclose(statusFile);
}

// 递归删除目录及其内容的函数
void recursive_remove(const char *path) {
	// 打开目录
	DIR *dir = opendir(path);
	struct dirent *entry;

	// 检查目录是否打开成功
	if (dir == NULL) {
		perror("Error opening directory");
		return;
	}

	// 遍历目录条目
	while ((entry = readdir(dir)) != NULL) {
		// 忽略当前目录和父目录条目
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
			// 构建条目的完整路径
			char full_path[PATH_MAX];
			snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

			// 获取文件/目录信息
			struct stat file_info;
			if (stat(full_path, &file_info) == -1) {
				perror("Error getting file/directory information");
			} else {
				// 检查是否是一个目录
				if (S_ISDIR(file_info.st_mode)) {
					// 递归删除子目录
					recursive_remove(full_path);
				} else {
					// 删除单个文件
					if (remove(full_path) != 0) {
						perror("Error deleting file");
					} else {
						printf("Deleted file: %s\n", full_path);
					}
				}
			}
		}
	}

	// 关闭目录
	closedir(dir);

	// 删除空目录本身
	if (rmdir(path) != 0) {
		perror("Error deleting directory");
	} else {
		printf("Deleted directory: %s\n", path);
	}
}

// 获取文件扩展名
const char *get_extension(const char *filename) {
	const char *dot = strrchr(filename, '.');
	if (!dot || dot == filename) {
		return ""; // 没有扩展名
	}
	return dot + 1;
}

// 根据文件类型返回对应颜色代码
const char *get_color_code(const char *extension) {
	if (strcmp(extension, "zip") == 0 || strcmp(extension, "tar") == 0 || strcmp(extension, "gz") == 0) {
		return "\033[1;31m"; // 红色
	} else if (strcmp(extension, "exe") == 0 || strcmp(extension, "sh") == 0) {
		return "\033[1;32m"; // 绿色
	} else if (strcmp(extension, "jpg") == 0 || strcmp(extension, "png") == 0) {
		return "\033[1;35m"; // 紫色
	} else {
		return "\033[1;37m"; // 白色
	} 
}

int myhelp() {
	if(myargv[1] == NULL) {
		printf("These shell commands are defined internally. Type `help -all' to see this list.\nType `help name' to find out more about the function `name'.\n\n");
	}
	else if(strcmp(myargv[1], "-all") == 0) {
		printf("These shell commands are defined internally. Type `help -all' to see this list.\nType `help name' to find out more about the function `name'.\n\n");
		printf("q				Quit the shell.\n");
		printf("cd [DIR]			Change the shell working directory.\n");
		printf("cp SOURCE DEST			Copy SOURCE to DEST.\n");
		printf("ls [FILE]...			List information about the FILEs.\n");
		printf("mv SOURCE DEST			Rename SOURCE to DEST, or move SOURCE(s) to DIRECTORY.\n");
		printf("ps [process]...			Display information about processes.\n");
		printf("rm FILE ...			remove files or directories\n");
		printf("pwd				print name of current/working directory\n");
		printf("history				History Library\n");
		printf("echo [string]			Display all the input strings.\n");
		printf("tree DIRECTORY			Display the structure of a directory as a tree.\n");
		printf("background[i] Process &		Run a command in the background[i].\n");
		printf("Pipe |				Output is input of next command.\n");
		printf("Redirectory <			Input of command comes from file instead.\n");
		printf("Redirectory >			Write output in file instead.\n");
		printf("Redirectory >>			Append output in file instead.\n");
	} else if(strcmp(myargv[1], "q") == 0) {
		printf("q\n\nNAME\n\tq - Quit the shell.\n\nSYNOPSIS\n\tq ...\n\nDESCRIPTION\n\tTo close the shell.\n\n");
	} else if(strcmp(myargv[1], "cd") == 0) {
		printf("cd\n\nNAME\n\tcd - Change the shell working directory.\n\nSYNOPSIS\n\tcd [DIR]\n\nDESCRIPTION\n\tChange the current directory to DIR. The default DIR is root.\n\n");
	} else if(strcmp(myargv[1], "cp") == 0) {
		printf("cp\n\nNAME\n\tcp - Copy files and directories\n\nSYNOPSIS\n\tcp SOURCE DEST\n\nDESCRIPTION\n\tCopy SOURCE to DEST.\n\tSOURCE and DEST can be a path of a file or a directory. SOURCE must be existent.\n\tIf DEST is existent, it can copy a file to a directory, or overwrite a file with a file, or error.\n\tIf DEST is NOT existent, it can copy SOURCE with a new name DEST.\n\n");
	} else if(strcmp(myargv[1], "ls") == 0) {
		printf("ls\n\nNAME\n\tls - List directory contents\n\nSYNOPSIS\n\tls [FILE]...\n\nDESCRIPTION\n\tList information about the FILEs (the current directory by default).\n\tThe color of directories is blue and others' is white.\n\n");
	} else if(strcmp(myargv[1], "mv") == 0) {
		printf("mv\n\nNAME\n\tmv - Move (rename) files\n\nSYNOPSIS\n\tmv SOURCE DEST\n\nDESCRIPTION\n\tRename SOURCE to DEST, or move SOURCE(s) to DIRECTORY.\n\tSOURCE and DEST can be a path of a file or a directory. SOURCE must be existent.\n\tIf DEST is existent, it can move SOURCE to a directory, or overwrite a file with a file, or error.\n\tIf DEST is NOT existent, it can rename SOURCE with DEST, or error.\n\n");
	} else if(strcmp(myargv[1], "rm") == 0) {
		printf("rm\n\nNAME\n\trm - remove files or directories\n\nSYNOPSIS\n\trm FILE ...\n\nDESCRIPTION\n\tIt can remove files or directories if you input.\n\n");
	} else if(strcmp(myargv[1], "pwd") == 0) {
		printf("pwd\n\nNAME\n\tpwd - print name of current/working directory\n\nSYNOPSIS\n\tpwd\n\nDESCRIPTION\n\tPrint the full filename of the current working directory.\n\n");
	} else if(strcmp(myargv[1], "history") == 0) {
		printf("history\n\nNAME\n\thistory - History Library\n\nSYNOPSIS\n\thistory\n\nDESCRIPTION\n\tMany  programs  read input from the user a line at a time.  The History library is able to keep track of those lines, associate arbitrary data with each line, and utilize information from previous lines in composing new ones.\n\n");
	} else if(strcmp(myargv[1], "ps") == 0) {
		printf("ps\n\nNAME\n\tps - Display information about processes.\n\nSYNOPSIS\n\tps [process]...\n\nDESCRIPTION\n\tDisplay information about the specified processes.\n\n");
	} else if(strcmp(myargv[1], "echo") == 0) {
		printf("echo\n\nNAME\n\techo - Display all the input strings.\n\nSYNOPSIS\n\techo [string]...\n\techo $?\n\nDESCRIPTION\n\tDisplay the input strings which has no spaces.\nIf $? is the only option, display the last code and signal.\n\n");
	} else if(strcmp(myargv[1], "tree") == 0) {
		printf("tree\n\nNAME\n\ttree - Display the structure of a directory as a tree.\n\nSYNOPSIS\n\ttree DIRECTORY\n\nDESCRIPTION\n\tDisplays the structure of the specified directory. If not specified, the current directory is used.\n\n");
	} else if(strcmp(myargv[1], "&") == 0) {
		printf("&\n\nNAME\n\t& - Run a command in the background[i].\n\nSYNOPSIS\n\tbackground[i] Process &.\n\nDESCRIPTION\n\tNo specific options. The command will run in the background[i].\n\n");
	} else if(strcmp(myargv[1], "|") == 0) {
		printf("|\n\nNAME\n\t| - Output is input of next command.\n\nSYNOPSIS\n\tcommand[1]|command[2]|...|command[n]\n\nDESCRIPTION\n\tThe output of command[t] will not show on shell, instead, they will be the arguement of the command[t+1], until there are no more |.\n\n");
	} else if(strcmp(myargv[1], "<") == 0) {
		printf("<\n\nNAME\n\t< - Input of command comes from file instead.\n\nSYNOPSIS\n\tcommand < [FILE]\n\nDESCRIPTION\n\tRead the file as the arguement of the command.\n\n");
	} else if(strcmp(myargv[1], ">") == 0) {
		printf(">\n\nNAME\n\t> - Write output in file instead.\n\nSYNOPSIS\n\tcommand > [FILE]\n\nDESCRIPTION\n\tWrite the output of the command in the file, and they will not be shown on shell.\n\n");
	} else if(strcmp(myargv[1], ">>") == 0) {
		printf(">>\n\nNAME\n\t>> - Append output in file instead.\n\nSYNOPSIS\n\tcommand >> [FILE]\n\nDESCRIPTION\n\tAppend the output of the command in the file, and they will not be shown on shell.\n\n");
	} else {
		printf("No such command: %s\n", myargv[1]);
	}
	return 0;
}

int myhistory(const char *path) {
	FILE *log_file = fopen(path, "r");
	if (log_file == NULL) {
		perror("Error reading");
		return -1;
	}
	char line[MAX_INPUT_SIZE];
	int line_number = 1;
	while (fgets(line, sizeof(line), log_file) != NULL) {
		line[strcspn(line, "\n")] = '\0';
		// 移除换行符
		printf("%d. %s\n", line_number++, line);
	}
	fclose(log_file);
	return 0;
}

int myls(char *direct) {
	DIR *dir;
	struct dirent *entry;
	if((dir = opendir(direct)) == NULL) {
		perror("opendir");
		return -1;
	}
	// 读取目录中的文件
	while((entry = readdir(dir)) != NULL) {
		if((strcmp(entry->d_name,".") == 0) || (strcmp(entry->d_name,"..") == 0)) {
			continue;
		}
		if (entry->d_type == DT_DIR) {
			printf(ANSI_COLOR_BLUE "%s " ANSI_COLOR_RESET, entry->d_name);
		} else {
			printf(ANSI_COLOR_WHITE "%s " ANSI_COLOR_RESET, entry->d_name);
		}
	}
	printf("\n");
	closedir(dir);
	// 关闭目录
	return 0;
}

int myps() {
	DIR *dir;
	struct dirent *entry;
	// 获取当前终端
	char *tty = ttyname(STDIN_FILENO);
	if (tty == NULL) {
		perror("Error getting terminal");
		return -1;
	}
	// 打开/proc目录
	dir = opendir("/proc");
	if (dir == NULL) {
		perror("Unable to open /proc directory");
		return -1;
	}
	// 遍历/proc目录中的子目录
	while ((entry = readdir(dir)) != NULL) {
		// 确保entry是数字（进程ID）
		if (entry->d_type == DT_DIR && isdigit(entry->d_name[0])) {
			print_process_info(entry->d_name,tty);
			// 获取进程信息，例如读取进程的stat文件
		}
	}
	closedir(dir);
	return 0;
}

int mytree(const char *path, int depth, int lastdir[100]) {
	DIR *dir;
	struct dirent *entry;
	const char *reset = "\033[0m";
	const char *color;
	if (!(dir = opendir(path))) {
		perror("opendir");
		return -1;
	}
	if(depth==1) {
		if(strcmp(path, cwd) == 0) {
			printf(".\n");
		}
		else {
			color = "\033[1;34m";
			printf("%s%s%s\n",color,path,reset);
		}
	}
	while ((entry = readdir(dir)) != NULL) {
		long currentPos = telldir(dir);
		lastdir[depth]=0;
		if(readdir(dir)==NULL) {
			lastdir[depth]=1;
		}
		seekdir(dir, currentPos);
		if (entry->d_type == DT_DIR) {
			// Ignore "." and ".." directories
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
				continue;
			}
			color = "\033[1;34m";
			// 蓝色
			// Print directory name with indentation
			for (int i = 1; i < depth ; ++i) {
				if(lastdir[i] == 1) {
					printf("	");
				} else printf("│   ");
			}
			if(lastdir[depth]==1) {
				printf("\u2502\u2500\u2500\u2500%s%s%s\n", color, entry->d_name, reset);
			} else {
				printf("│\u2500\u2500\u2500%s%s%s\n", color, entry->d_name, reset);
			}
			// Recursive call to mytree for the subdirectory
			char pathBuf[MAX_INPUT_SIZE];
			snprintf(pathBuf, sizeof(pathBuf), "%s/%s", path, entry->d_name);
			mytree(pathBuf, depth + 1, lastdir);
		} else {
			const char *extension = get_extension(entry->d_name);
			const char *color;
			color = get_color_code(extension);
			// Print file name with indentation
			for (int i = 1; i < depth ; ++i) {
				if(lastdir[i] == 1) {
					printf("	");
				} else printf("│   ");
			}
			if(lastdir[depth]==1) {
				printf("\u2502\u2500\u2500\u2500%s%s%s\n", color, entry->d_name, reset);
			} else {
				printf("│\u2500\u2500\u2500%s%s%s\n", color, entry->d_name, reset);
			}
		}
	}
	closedir(dir);
	return 0;
}

int mymv() {
	if(myargv[1] != NULL && myargv[2] != NULL && myargv[3] == NULL) {
		struct stat file1;
		// 使用 stat 判断第一个路径是否存在
		if (stat(myargv[1], &file1) == -1) {
			printf("%s not exist.\n", myargv[1]);
			return -1;
		} else {
			struct stat file2;
			// 使用 stat 判断第二个路径是否存在
			if (stat(myargv[2], &file2) == -1) {
				//不存在,rename
				if(rename(myargv[1],myargv[2]) == -1) {
					printf("Maybe a file rename to a directory!\n");
					perror("rename");
				}
			} else if(S_ISDIR(file2.st_mode)) {
				//2是存在的目录,组装路径
				char path[MAX_INPUT_SIZE];
				combine_path(path);
				if(rename(myargv[1],path) == -1) {
					perror("rename");
				}
			} else {
				//2是存在的文件,1是文件则覆盖
				if(rename(myargv[1],myargv[2]) == -1) {
					printf("Maybe directory to file!\n");
					perror("rename");
				}
			}
		}
	} else {
		printf("argv num wrong, please input command with 2 parameters.\n");
	}
	return 0;
}

int mycp() {
	if(myargv[1] != NULL && myargv[2] != NULL && myargv[3] == NULL) {
		struct stat file1;
		// 使用 stat 判断第一个路径是否存在
		if (stat(myargv[1], &file1) == -1) {
			printf("%s not exist.\n", myargv[1]);
			return -1;
		} else {
			struct stat file2;
			// 使用 stat 判断第二个路径是否存在
			if (stat(myargv[2], &file2) != -1 && S_ISDIR(file2.st_mode)) {
				//2是存在的目录,组装路径
				char path[MAX_INPUT_SIZE];
				combine_path(path);
				if(S_ISDIR(file1.st_mode)) {
					printf("omitting directory '%s'.You can appoint a name for new dir to solve.\n",myargv[1]);
				} else {
					copy_file(myargv[1], path);
				}
			} else {
				if(S_ISDIR(file1.st_mode)) {
					copy_directory(myargv[1], myargv[2]);
				} else {
					copy_file(myargv[1], myargv[2]);
				}
			}
		}
	} else {
		printf("argv num wrong, please input command with 2 parameters.\n");
	}
	return 0;
}

int myrm() {
	// 检查是否至少有一个参数
	if (myargv[1] != NULL) {
		// 迭代命令行参数
		for (int i = 1; myargv[i]; ++i) {
			// 检查文件或目录是否存在
			if (access(myargv[i], F_OK) == -1) {
				perror("Error accessing file or directory");
			} else {
				// 获取文件/目录信息
				struct stat file_info;
				if (stat(myargv[i], &file_info) == -1) {
					perror("Error getting file/directory information");
				} else {
					// 检查是否是一个目录
					if (S_ISDIR(file_info.st_mode)) {
						// 调用递归函数删除目录及其内容
						recursive_remove(myargv[i]);
					} else {
						// 删除单个文件
						if (remove(myargv[i]) != 0) {
							perror("Error deleting file");
						} else {
							printf("Deleted file: %s\n", myargv[i]);
						}
					}
				}
			}
		}
	} else {
		// 如果没有提供参数则打印一条消息
		printf("Please input command with at least 1 parameter.\n");
	}
	return 0;
}

int myrun() {
	//for (int i = 0;myargv[i]!=NULL;++i)printf("%s ", myargv[i]);
	//test命令
	if(myargv[0] && strcmp(myargv[0],"test") == 0) {
		printf("this is a test.\n");
		return 0;
	}
	//help命令
	else if(myargv[0] != NULL && strcmp(myargv[0],"help") == 0) {
		return myhelp();
	}
	//history命令
	else if(myargv[0] != NULL && strcmp(myargv[0],"history") == 0) {
		return myhistory(log_path);
	}
	//echo命令
	else if(myargv[0] == NULL || strcmp(myargv[0], "echo") == 0) {
		if(myargv[1] && strcmp(myargv[1],"$?") == 0) {
			printf("%d,%d\n", lastcode, lastsig);
		} else {
			for (int i = 1; myargv[i]; ++i) {
				printf("%s ", myargv[i]);
			}
			printf("\n");
		}
		return 0;
	}
	//ls命令
	else if(myargv[0] && strcmp(myargv[0],"ls") == 0) {
		if(myargv[1] != NULL) {
			//选择打开路径
			for (int i = 1; myargv[i] != NULL; ++i) {
				printf("%s:\n",myargv[i]);
				myls(myargv[i]);
			}
		} else {
			myls(cwd);
		}
		return 0;
	}
	//pwd命令
	else if(myargv[0] && strcmp(myargv[0],"pwd") == 0) {
		printf("%s\n", cwd);
		return 0;
	}
	//rm命令
	else if(myargv[0] && strcmp(myargv[0],"rm") == 0) {
		return myrm();
	}
	//ps指令
	else if((myargv[0] != NULL && strcmp(myargv[0],"ps") == 0)) {
		return myps();
	}
	//tree指令
	else if (myargv[0] != NULL && strcmp(myargv[0], "tree") == 0) {
		int lastdir[100]= {0};
		if(myargv[1]!=NULL) {
			return mytree(myargv[1], 1, lastdir);
		} else {
			return mytree(cwd, 1, lastdir);
		}
	}
	//sleep指令，用于测试后台进程
	else if((myargv[0] != NULL && strcmp(myargv[0],"sleep") == 0)) {
		int time = atoi(myargv[1]);
		sleep(time);
		printf("after sleep\n");
	}
	//mv命令
	else if((myargv[0] != NULL && strcmp(myargv[0], "mv") == 0)) {
		return mymv();
	}
	//cp命令
	else if((myargv[0] != NULL && strcmp(myargv[0], "cp") == 0)) {
		return mycp();
	}
	//error命令
	else if(myargv[0] && strcmp(myargv[0], "error") == 0) {
		return -1;
	}
	//没有匹配命令
	else {
		if(execvp(myargv[0], myargv) == -1){
			printf("no command exist, please input 'help -all'to see our commands.\n");
		}
		return -1;
	}
}

int sheller(int* bg, int* bgid) {
	if(getcwd(cwd, sizeof(cwd)) == NULL) {//getcwd将当前工作目录的绝对路径复制到cwd
		perror("getcwd() error");
		return 1;
	}
	char lineCommand[MAX_INPUT_SIZE];//一行输入的命令
	printf(ANSI_COLOR_CYAN "Conchshell:%s" ANSI_COLOR_RESET "@ ", cwd);//输出提示符
	fflush(stdout);//清空输出缓冲区
	char* s = fgets(lineCommand,sizeof(lineCommand) - 1, stdin);//获取用户输入的命令
	assert(s);
	(void)s;//避免Linux认为s变量未使用,导致警告
	lineCommand[strlen(lineCommand) - 1] = 0;//清除最后一个\n
	history_log(lineCommand, log_path);
	int t = 0;
	if (strlen(lineCommand) == 0){
		return 1;
	}
	else if (lineCommand[0] == 'h' && lineCommand[1] == 'e') {
		commands[0] = lineCommand;
		commands[1] = NULL;
	}
	else {
		for (commands[t++] = strtok(lineCommand, "|"); commands[t++] = strtok(NULL, "|"););//分割管道之间的命令
	}
	int fds[OPT_NUM][2];
	pid_t pids[OPT_NUM];
	int command_son_num = 0;
	for (command_son_num = 1; commands[command_son_num]; ++command_son_num) {
		if (pipe(fds[command_son_num-1]) < 0) {
			printf("Pipe falied\n");
			return 1;
		}
	}
	int background[command_son_num];
	//printf("command_son_num%d\n", command_son_num);
	for (int j = 0; j < command_son_num; ++j) {
		//printf("command: %s!!!\n", commands[j]);
		redirType = NONE_REDIR;
		redirFile = NULL;//重定向
		background[j] = command_back(commands[j]);
		command_redi(commands[j]);
		//printf("after: %s!!!\n", commands[j]);
		myargv[0] = strtok(commands[j], " ");//strtok字符串分割,获取命令
		//退出命令
		if(myargv[0] && strcmp(myargv[0], "q") == 0) {
			return 0;
		}
		int end = 1;
		while(myargv[end++] = strtok(NULL," "));//如果没有子串了,strtok会返回NULL，即myargv[end] = NULL
		//cd命令
		if(myargv[0] && strcmp(myargv[0],"cd") == 0) {
			background[j] = 1;
			if(myargv[1] != NULL){
				if(chdir(myargv[1]) == -1){
					printf("directory not exist!\n");
				}
			} else {
				if(chdir("/") == -1){
					perror("chdir");
				}
			}
			continue;
		}
		int bgID;
		if(background[j])bgID = ++*bgid;
		pids[j] = fork();
		if(pids[j] != 0){
			if(background[j]) {
				bg[bgID] = pids[j];
				for(int i =1; i <= *bgid; ++i) {
					if(bg[i] > 1) {
						printf("[%d] %d\n", i, bg[i]);
					}
					else if(bg[i] == 1){
						printf("[%d] done\n", i);
						bg[i] = 0;
					}
				}
			}
		}
		assert(pids[j] != -1);
		if (pids[j] == 0) {//子进程
			if (command_son_num == 1);
			else if (j == 0) {
				// First process
				dup2(fds[j][1], STDOUT_FILENO);
				close(fds[j][0]);
				close(fds[j][1]);
			}else if (j == command_son_num - 1) {
				// Last process
				dup2(fds[j - 1][0], STDIN_FILENO);
				close(fds[j - 1][0]);
				close(fds[j - 1][1]);
			}else {
				// Middle process
				dup2(fds[j - 1][0], STDIN_FILENO);
				dup2(fds[j][1], STDOUT_FILENO);
				close(fds[j - 1][0]);
				close(fds[j - 1][1]);
				close(fds[j][0]);
				close(fds[j][1]);
			}
			switch (redirType)
			{
			case NONE_REDIR:
				// 什么都不做
				break;
			case INPUT_REDIR: {
				int fd = open(redirFile, O_RDONLY);
				if (fd < 0) {
					perror("input open");
					exit(errno);
				}
				// 重定向的文件已经成功打开了
				dup2(fd, 0);
				break;
			}
			case OUTPUT_REDIR:
			case APPEND_REDIR: {
				umask(0);
				int flags = O_WRONLY | O_CREAT;
				if (redirType == APPEND_REDIR) flags |= O_APPEND;
				else flags |= O_TRUNC;
				int fd = open(redirFile, flags, 0666);
				if (fd < 0)
				{
					perror("append open");
					exit(errno);
				}
				dup2(fd, 1);
				break;
			}
			default:
				printf("bug when redir?\n");
				break;
			}
			if (redirType == INPUT_REDIR || j != 0) {
				char tempbuf[OPT_NUM];
				fgets(tempbuf,sizeof(tempbuf) - 1, stdin);
				tempbuf[strlen(tempbuf) - 1] = 0;
				myargv[end-1] = strtok(tempbuf, " ");
				while(myargv[end++] = strtok(NULL," "));
			}
			if(myrun() < 0) {//执行命令
				perror("Not a valid command");
				exit(1);
			}
			if(background[j]) bg[bgID] = 1;
			exit(0);
		}
	}
	for (int i = 0; i < command_son_num - 1; i++) {
		close(fds[i][0]);
		close(fds[i][1]);
	}
	for (int i = 0; i < command_son_num; i++) {
		if(background[i] == 0){
			int status;
			pid_t ret = waitpid(pids[i], &status, 0);
			assert(ret > 0);
			(void) ret;
			lastcode = (status >> 8) & 0xFF;
			lastsig = status & 0x7F;
		}
	}
	return 1;
}

int main()
{
	if (getcwd(log_path, sizeof(log_path)) == NULL) {
		perror("getcwd() error");
	}
	strcat(log_path, "/history.log");
	printf(ANSI_COLOR_MAGENTA "history: " ANSI_COLOR_RESET "%s\n" , log_path);
	void *shared_memory = mmap(NULL, SHARED_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	
	if (shared_memory == MAP_FAILED) {
		perror("mmap failed");
		exit(EXIT_FAILURE);
   	}
   	int *shared_bgid = (int*)(shared_memory + sizeof(int));
		int *shared_bg = (int*)(shared_memory + 2 * sizeof(int));
		*shared_bgid = 0;
	while(sheller(shared_bg, shared_bgid));
	munmap(shared_memory, SHARED_MEMORY_SIZE);
	return 0;
}
