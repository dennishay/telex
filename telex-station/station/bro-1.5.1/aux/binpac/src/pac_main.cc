// $Id: pac_main.cc 3320 2006-06-20 21:19:33Z rpang $

#include <unistd.h>
#include <ctype.h>

#include "pac_common.h"
#include "pac_decl.h"
#include "pac_exttype.h"
#include "pac_id.h"
#include "pac_output.h"
#include "pac_parse.h"
#include "pac_type.h"
#include "pac_utils.h"
#include "pac_exception.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

extern int yydebug;
extern int yyparse();
extern void switch_to_file(FILE* fp_input);
string input_filename;

bool FLAGS_pac_debug = false;
string FLAGS_output_directory;
vector<string> FLAGS_include_directories;

Output* header_output = 0;
Output* source_output = 0;

void add_to_include_directories(string dirs)
	{
	unsigned int dir_begin = 0, dir_end;
	while ( dir_begin < dirs.length() )
		{
		for ( dir_end = dir_begin; dir_end < dirs.length(); ++dir_end)
			if ( dirs[dir_end] == ':' )
				break;

		string dir = dirs.substr(dir_begin, dir_end - dir_begin);

		// Add a trailing '/' if necessary
		if ( dir.length() > 0 && *(dir.end() - 1) != '/' )
			dir += '/';
			
		FLAGS_include_directories.push_back(dir);
		dir_begin = dir_end + 1;
		}
	}

void pac_init()
	{
	init_builtin_identifiers();
	Type::init();
	}

void insert_comments(Output* out, const char* source_filename)
	{
	out->println("// This file is automatically generated from %s.\n",
		source_filename);
	}

void insert_basictype_defs(Output* out)
	{
	out->println("#ifndef pac_type_defs");
	out->println("#define pac_type_defs");
	out->println("");
	out->println("typedef char int8;");
	out->println("typedef short int16;");
	out->println("typedef long int32;");
	out->println("typedef unsigned char uint8;");
	out->println("typedef unsigned short uint16;");
	out->println("typedef unsigned long uint32;");
	out->println("");
	out->println("#endif /* pac_type_defs */");
	out->println("");
	}

void insert_byteorder_macros(Output* out)
	{
	out->println("#define FixByteOrder16(x)	(byteorder == HOST_BYTEORDER ? (x) : pac_swap16(x))");
	out->println("#define FixByteOrder32(x)	(byteorder == HOST_BYTEORDER ? (x) : pac_swap32(x))");
	out->println("");
	}

const char* to_id(const char* s)
	{
	static char t[1024];
	int i;
	for ( i = 0; s[i] && i < (int) sizeof(t) - 1; ++i )
		t[i] = isalnum(s[i]) ? s[i] : '_';
	if ( isdigit(t[0]) )
		t[0] = '_'; 
	t[i] = '\0';
	return t;
	}

int compile(const char* filename)
	{
	FILE* fp_input = fopen(filename, "r");
	if ( ! fp_input )
		{
		perror(fmt("Error in opening %s", filename));
		return -1;
		}
	input_filename = filename;

	string basename;

	if ( ! FLAGS_output_directory.empty() )
		{
		// Strip leading directories of filename
		const char *last_slash = strrchr(filename, '/');
		if ( last_slash )
			basename = last_slash + 1;
		else
			basename = filename;
		basename = FLAGS_output_directory + "/" + basename;
		}
	else
		basename = filename;

	// If the file name ends with ".pac"
	if ( basename.length() > 4 &&
	     basename.substr(basename.length() - 4) == ".pac" )
		{
		basename = basename.substr(0, basename.length() - 4);
		}

	basename += "_pac";

	DEBUG_MSG("Output file: %s.{h,cc}\n", basename.c_str());

	int ret = 0;

	try 
		{
		switch_to_file(fp_input);
		if ( yyparse() )
			return 1;

		Output out_h(fmt("%s.h", basename.c_str()));
		Output out_cc(fmt("%s.cc", basename.c_str()));

		header_output = &out_h;
		source_output = &out_cc;

		insert_comments(&out_h, filename);
		insert_comments(&out_cc, filename);

		const char* filename_id = to_id(filename);

		out_h.println("#ifndef %s_h", filename_id);
		out_h.println("#define %s_h", filename_id);
		out_h.println("");
		out_h.println("#include <vector>");
		out_h.println("");
		out_h.println("#include \"binpac.h\"");
		out_h.println("");

		out_cc.println("#include \"%s.h\"\n", basename.c_str());

		Decl::ProcessDecls(&out_h, &out_cc);

		out_h.println("#endif /* %s_h */", filename_id);
		} 
	catch ( OutputException& e ) 
		{
		fprintf(stderr, "Error in compiling %s: %s\n",
			filename, e.errmsg());
		ret = 1;
		}
	catch ( Exception& e )
		{
		fprintf(stderr, "%s\n", e.msg());
		exit(1);
		}

	header_output = 0;
	source_output = 0;
	input_filename = "";
	fclose(fp_input);

	return ret;
	}

void usage()
	{
#ifdef VERSION
	fprintf(stderr, "binpac version %s\n", VERSION);
#endif
	fprintf(stderr, "usage: binpac [options] <pac files>\n");
	fprintf(stderr, "     <pac files>           | pac-language input files\n");
	fprintf(stderr, "     -d <dir>              | use given directory for compiler output\n");
	fprintf(stderr, "     -D                    | enable debugging output\n");
	fprintf(stderr, "     -h                    | show command line help\n");
	fprintf(stderr, "     -I <dir>              | include <dir> in input file search path\n");
	exit(1);
	}

int main(int argc, char* argv[])
	{
#ifdef HAVE_MALLOC_OPTIONS
	extern char *malloc_options;
#endif
	int o;
	while ( (o = getopt(argc, argv, "DI:d:h")) != -1 )
		{
		switch(o)
			{
			case 'D': 
				yydebug = 1;
				FLAGS_pac_debug = true;
#ifdef HAVE_MALLOC_OPTIONS
				malloc_options = "A";
#endif
				break;

			case 'I':
				// Add to FLAGS_include_directories
				add_to_include_directories(optarg);
				break;

			case 'd': 
				FLAGS_output_directory = optarg;
				break;

			case 'h':
				usage();
				break;
			}
		}

	// Strip the trailing '/'s
	while ( ! FLAGS_output_directory.empty() &&
	        *(FLAGS_output_directory.end() - 1) == '/' )
		{
		FLAGS_output_directory.erase(FLAGS_output_directory.end()-1);
		}

	// Add the current directory to FLAGS_include_directories
	add_to_include_directories(".");

	pac_init();

	argc -= optind;
	argv += optind;
	if ( argc == 0 )
		compile("-");

	int ret = 0;
	for ( int i = 0; i < argc; ++i )
		if ( compile(argv[i]) )
			ret = 1;

	return ret;
	}

