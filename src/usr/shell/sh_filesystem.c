

/**********************************************************
 *                       Includers                        *
 **********************************************************/
#include "sh_utils.h"
#include "sh_command.h"
#include "sh_filesystem.h"

/**********************************************************
 *                         Macro                          *
 **********************************************************/

/**********************************************************
 *                  Extern Declareation                   *
 **********************************************************/


/**********************************************************
 *                    Global Variables                    *
 **********************************************************/

/**********************************************************
 *                    Static Variables                    *
 **********************************************************/

/**********************************************************
 *                       Implements                       *
 **********************************************************/
static int sh_ls_cmd(int argc, char *argv[])
{
	return ls_main(argc, argv);
}

static int sh_cat_cmd(int argc, char *argv[])
{
	return cat_main(argc, argv);
}

static int sh_cd_cmd(int argc, char *argv[])
{
	return cd_main(argc, argv);
}

static int sh_pwd_cmd(int argc, char *argv[])
{
	return pwd_main(argc, argv);
}

static int sh_mkdir_cmd(int argc, char *argv[])
{
	return mkdir_main(argc, argv);
}

static int sh_mv_cmd(int argc, char *argv[])
{
	return mv_main(argc, argv);
}

static int sh_rm_cmd(int argc, char *argv[])
{
	return rm_main(argc, argv);
}

static int sh_creat_cmd(int argc, char *argv[])
{
	return creat_main(argc, argv);
}

void _initialize_filesystem_cmds(void)
{
	register_shell_command("ls", sh_ls_cmd, 
		"List the file names of directory", 
		"ls [dirname|filename]", 
		"This command lists the file names of current directory \n\t"
		"or specified directory.", 
		sh_filename_completer);

	register_shell_command("cat", sh_cat_cmd, 
		"type the file content", 
		"cat [dirname|filename]", 
		"This command cats the file conent\n",
		sh_filename_completer);

	register_shell_command("cd", sh_cd_cmd,
		"Change the current directory", 
		"cd [dirname]", 
		"This command changes the specified directory as current directory.",
		sh_directory_completer);

	register_shell_command("pwd", sh_pwd_cmd,
		"Print the current directory", 
		"pwd", 
		"This command prints the current directory.",
		sh_noop_completer);

	register_shell_command("mkdir", sh_mkdir_cmd,
		"Create a new directory", 
		"mkdir dirname", 
		"This command creates a new directory.",
		sh_noop_completer);

	register_shell_command("mv", sh_mv_cmd,
		"rename file", 
		"rename name", 
		"This command rename a file.",
		sh_noop_completer);

	register_shell_command("rm", sh_rm_cmd,
		"remove file", 
		"remove name", 
		"This command remove a file.",
		sh_noop_completer);

	register_shell_command("creat", sh_creat_cmd,
		"Create a new file", 
		"creat filename", 
		"This command creates a new file.",
		sh_noop_completer);

	register_shell_command("xby_test", xby_test_cmd,
			"xby_test command", 
			"xby_test command", 
			"xby_test command",
			sh_filename_completer);
	
	return;
}

