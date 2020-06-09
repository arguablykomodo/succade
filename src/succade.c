#define CFG_IMPLEMENTATION

#include <stdlib.h>    // NULL, size_t, EXIT_SUCCESS, EXIT_FAILURE, ...
#include <string.h>    // strlen(), strcmp(), ...
#include <signal.h>    // sigaction(), ... 
#include <float.h>     // DBL_MAX
#include <sys/epoll.h> // epoll_wait(), ... 
#include <sys/types.h> // pid_t
#include <sys/wait.h>  // waitpid()
#include <errno.h>     // errno
#include "ini.h"       // https://github.com/benhoyt/inih
#include "cfg.h"
#include "succade.h"   // defines, structs, all that stuff
#include "options.c"   // Command line args/options parsing
#include "helpers.c"   // Helper functions, mostly for strings
#include "execute.c"   // Execute child processes
#include "loadini.c"   // Handles loading/processing of INI cfg file

static volatile int running;   // Used to stop main loop in case of SIGINT
static volatile int handled;   // The last signal that has been handled 

/*
 * Frees all members of the given bar that need freeing.
 */
static void free_lemon(lemon_s *lemon)
{
	free(lemon->sid);
	cfg_free(&lemon->lemon_cfg);
	cfg_free(&lemon->block_cfg);

	char *arg = kita_child_get_arg(lemon->child);
	free(arg);
}

/*
 * Frees all members of the given block that need freeing.
 */
static void free_block(block_s *block)
{
	free(block->sid);
	free(block->output);
	cfg_free(&block->block_cfg);
	//free_click_cfg(&block->click_cfg);

	char *arg = kita_child_get_arg(block->child);
	free(arg);
}

void free_spark(spark_s *spark)
{
	free(spark->output);

	char *arg = kita_child_get_arg(spark->child);
	free(arg);
}

/*
 * Command line options and arguments string for lemonbar.
 * Allocated with malloc(), so please free() it at some point.
 */
char *lemon_arg(lemon_s *lemon)
{
	cfg_s *lcfg = &lemon->lemon_cfg;
	cfg_s *bcfg = &lemon->block_cfg;

	char w[8]; // TODO hardcoded (8 is what we want tho) 
	char h[8];

	snprintf(w, 8, "%d", cfg_get_int(lcfg, LEMON_OPT_WIDTH));
	snprintf(h, 8, "%d", cfg_get_int(lcfg, LEMON_OPT_HEIGHT));

	char *block_font = optstr('f', cfg_get_str(lcfg, LEMON_OPT_BLOCK_FONT), 0);
	char *label_font = optstr('f', cfg_get_str(lcfg, LEMON_OPT_LABEL_FONT), 0);
	char *affix_font = optstr('f', cfg_get_str(lcfg, LEMON_OPT_AFFIX_FONT), 0);
	char *name_str   = optstr('n', cfg_get_str(lcfg, LEMON_OPT_NAME), 0);

	char *fg = cfg_get_str(bcfg, BLOCK_OPT_BLOCK_FG);
	char *bg = cfg_get_str(lcfg, LEMON_OPT_BG);
	char *lc = cfg_get_str(bcfg, BLOCK_OPT_LC);

	char *arg = malloc(sizeof(char) * BUFFER_LEMON_ARG); 

	snprintf(arg, 1024,
		"-g %sx%s+%d+%d -F%s -B%s -U%s -u%d %s %s %s %s %s %s",
		cfg_has(lcfg, LEMON_OPT_WIDTH) ? w : "",     // max 8
		cfg_has(lcfg, LEMON_OPT_HEIGHT) ? h : "",    // max 8
		cfg_get_int(lcfg, LEMON_OPT_X),              // max 8
		cfg_get_int(lcfg, LEMON_OPT_Y),              // max 8
		fg ? fg : "-",                               // strlen, max 9
		bg ? bg : "-",                               // strlen, max 9
		lc ? lc : "-",                               // strlen, max 9
		cfg_get_int(lcfg, LEMON_OPT_LW),             // max 4
		cfg_get_int(lcfg, LEMON_OPT_BOTTOM) ? "-b" : "",   // max 2
		cfg_get_int(lcfg, LEMON_OPT_FORCE)  ? "-d" : "",   // max 2
		block_font,                                  // strlen, max 255
		label_font,                                  // strlen, max 255
		affix_font,                                  // strlen, max 255
		name_str                                     // strlen
	);

	free(block_font);
	free(label_font);
	free(affix_font);
	free(name_str);

	return arg;
}

/*
 * Runs the bar process and opens file descriptors for reading and writing.
 * Returns 0 on success, -1 if bar could not be started.
 */
int open_lemon(lemon_s *lemon)
{
	char *old_arg = kita_child_get_arg(lemon->child);
	free(old_arg);

	kita_child_set_arg(lemon->child, lemon_arg(lemon));
	if (kita_child_open(lemon->child) == 0)
	{
		return kita_child_set_buf_type(lemon->child, KITA_IOS_IN, KITA_BUF_LINE);
	}

	return -1;
}

/*
 *
 */
int open_block(block_s *block)
{
	if (kita_child_open(block->child) == 0)
	{
		block->last_open = get_time();
		block->alive = 1;
		return 0;
	}
	return -1;
}

int read_block(block_s *block)
{
	char *old = block->output ? strdup(block->output) : NULL;

	free(block->output); // just in case, free'ing NULL is fine
	block->output = kita_child_read(block->child, KITA_IOS_OUT);
	block->last_read = get_time();
	
	int same = (old && equals(old, block->output));
	free(old);

	return !same;
}

int read_spark(spark_s *spark)
{
	free(spark->output); // just in case, free'ing NULL is fine
	spark->output = kita_child_read(spark->child, KITA_IOS_OUT);
	spark->last_read = get_time();

	return !empty(spark->output);
}

/*
 * Send a kill signal to the lemon's child process.
 */
void close_lemon(lemon_s *lemon)
{
	kita_child_term(lemon->child);
}

/*
 * Send a kill signal to the block's child process.
 */
void close_block(block_s *block)
{
	kita_child_term(block->child);
}

/*
 * Send a kill signal to the spark's child process.
 */
void close_spark(spark_s *spark)
{
	kita_child_term(spark->child);
}

/*
 * Convenience function: simply runs close_block() for all blocks.
 */
void close_blocks(state_s *state)
{
	for (size_t i = 0; i < state->num_blocks; ++i)
	{
		close_block(&state->blocks[i]);
	}
}

int open_spark(spark_s *spark)
{
	if (kita_child_open(spark->child) == 0)
	{
		spark->last_open = get_time();
		spark->alive = 1;
		return 0;
	}
	return -1;
}

/*
 * Convenience function: simply opens all given triggers.
 * Returns the number of successfully opened triggers.
 */ 
size_t open_sparks(state_s *state)
{
	size_t num_sparks_opened = 0;
	for (size_t i = 0; i < state->num_sparks; ++i)
	{
		num_sparks_opened += (open_spark(&state->sparks[i]) == 0);
	}
	return num_sparks_opened;
}

/*
 * Convenience function: simply closes all given triggers.
 */
void close_sparks(state_s *state)
{
	for (size_t i = 0; i < state->num_sparks; ++i)
	{
		close_spark(&state->sparks[i]);
	}
}

/*
 * Convenience function: simply frees all given blocks.
 */
void free_blocks(state_s *state)
{
	for (size_t i = 0; i < state->num_blocks; ++i)
	{
		free_block(&state->blocks[i]);
	}
}

/*
 * Convenience function: simply frees all given triggers.
 */
void free_sparks(state_s *state)
{
	for (size_t i = 0; i < state->num_sparks; ++i)
	{
		free_spark(&state->sparks[i]);
	}
}

int block_can_consume(block_s *block)
{
	return block->type == BLOCK_SPARKED
		&& cfg_get_int(&block->block_cfg, BLOCK_OPT_CONSUME) 
		&& !empty(block->spark->output);
}

double block_due_in(block_s *block, double now)
{
	float reload = cfg_get_float(&block->block_cfg, BLOCK_OPT_RELOAD);

	return block->type == BLOCK_TIMED ? 
		reload - (now - block->last_open) : 
		DBL_MAX;
}

int block_is_due(block_s *block, double now, double tolerance)
{
	// block is currently running
	if (block->alive)
	{
		return 0;
	}

	// One-shot blocks are due if they have never been run before
	if (block->type == BLOCK_ONCE)
	{
		return block->last_open == 0.0;
	}

	// Timed blocks are due if their reload time has elapsed
	// or if they've never been run before
	if (block->type == BLOCK_TIMED)
	{
		if (block->last_open == 0.0)
		{
			return 1;
		}
		double due_in = block_due_in(block, now);
		return due_in < tolerance;
	}

	// Sparked blocks are due if their spark has new output, or if 
	// they don't consume output and have never been run
	if (block->type == BLOCK_SPARKED)
	{
		// spark missing
		if (block->spark == NULL)
		{
			return 0;
		}

		// spark has output waiting to be processed
		if (block->spark->output)
		{
			return 1;
		}

		// doesn't consume and has never been run before
		if (cfg_get_int(&block->block_cfg, BLOCK_OPT_CONSUME) == 0)
		{
			return block->last_open == 0.0;
		}
	}

	// Live blocks are due if they haven't been run yet 
	if (block->type == BLOCK_LIVE)
	{	
		return block->last_open == 0.0;
	}

	// Unknown block type (WTF?)
	return 0;
}

char *prefixstr(const char *affix, const char *fg, const char *bg)
{
	size_t len = empty(affix) ? 0 : strlen(affix) + 32;
	char *prefix = malloc(len * sizeof(char) + 1);

	if (len == 0)
	{
		prefix = malloc(1);
		prefix[0] = 0;
		return prefix;
	}

	snprintf(prefix, len, "%%{T3 F%s B%s}%s", fg ? fg : "-", bg ? bg : "-", affix);
	return prefix;
}

/*
 * Given a block, it returns a pointer to a string that is the formatted result 
 * of this block's script output, ready to be fed to Lemonbar, including prefix,
 * label and suffix. The string is malloc'd and should be free'd by the caller.
 * If `len` is positive, it will be used as buffer size for the result string.
 * This means that `len` needs to be big enough to contain the fully formatted 
 * string this function is putting together, otherwise truncation will happen.
 * Alternatively, set `len` to 0 to let this function calculate the buffer.
 */
char *blockstr(const lemon_s *bar, const block_s *block, size_t len)
{
	char action_start[(5 * strlen(block->sid)) + 56]; // ... + (5 * 11) + 1
	action_start[0] = 0;
	char action_end[21]; // (5 * 4) + 1
	action_end[0] = 0;

	if (cfg_has(&block->block_cfg, BLOCK_OPT_CMD_LMB))
	{
		strcat(action_start, "%{A1:");
		strcat(action_start, block->sid);
		strcat(action_start, "_lmb:}");
		strcat(action_end, "%{A}");
	}
	if (cfg_has(&block->block_cfg, BLOCK_OPT_CMD_MMB))
	{
		strcat(action_start, "%{A2:");
		strcat(action_start, block->sid);
		strcat(action_start, "_mmb:}");
		strcat(action_end, "%{A}");
	}
	if (cfg_has(&block->block_cfg, BLOCK_OPT_CMD_RMB))
	{
		strcat(action_start, "%{A3:");
		strcat(action_start, block->sid);
		strcat(action_start, "_rmb:}");
		strcat(action_end, "%{A}");
	}
	if (cfg_has(&block->block_cfg, BLOCK_OPT_CMD_SUP))
	{
		strcat(action_start, "%{A4:");
		strcat(action_start, block->sid);
		strcat(action_start, "_sup:}");
		strcat(action_end, "%{A}");
	}
	if (cfg_has(&block->block_cfg, BLOCK_OPT_CMD_SDN))
	{
		strcat(action_start, "%{A5:");
		strcat(action_start, block->sid);
		strcat(action_start, "_sdn:}");
		strcat(action_end, "%{A}");
	}

	size_t diff;
	char *result = escape(block->output, '%', &diff);
	int padding = cfg_get_int(&block->block_cfg, BLOCK_OPT_WIDTH) + diff;

	size_t buf_len;

	if (len > 0)
	{
		// If len is given, we use that as buffer size
		buf_len = len;
	}
	else
	{
		char *bar_prefix = cfg_get_str(&bar->block_cfg, BLOCK_OPT_PREFIX);
		char *bar_suffix = cfg_get_str(&bar->block_cfg, BLOCK_OPT_SUFFIX);
		char *block_label = cfg_get_str(&block->block_cfg, BLOCK_OPT_LABEL);

		// Required buffer mainly depends on the result and name of a block
		buf_len = 209;   // format str = 70, known stuff = 138, '\0' = 1
		buf_len += strlen(action_start);
		buf_len += bar_prefix ? strlen(bar_prefix) : 0;
		buf_len += bar_suffix ? strlen(bar_suffix) : 0;
		buf_len += block_label ? strlen(block_label) : 0;
		buf_len += strlen(result);
	}

	char *bar_block_bg   = cfg_get_str(&bar->block_cfg, BLOCK_OPT_BLOCK_BG);
	char *bar_label_fg   = cfg_get_str(&bar->block_cfg, BLOCK_OPT_LABEL_FG);
	char *bar_label_bg   = cfg_get_str(&bar->block_cfg, BLOCK_OPT_LABEL_BG);
	char *bar_affix_fg   = cfg_get_str(&bar->block_cfg, BLOCK_OPT_AFFIX_FG);
	char *bar_affix_bg   = cfg_get_str(&bar->block_cfg, BLOCK_OPT_AFFIX_BG);
	int bar_block_offset = cfg_get_int(&bar->block_cfg, BLOCK_OPT_OFFSET);
	int bar_block_ol     = cfg_get_int(&bar->block_cfg, BLOCK_OPT_OL);
	int bar_block_ul     = cfg_get_int(&bar->block_cfg, BLOCK_OPT_UL);

	char *block_fg = cfg_get_str(&block->block_cfg, BLOCK_OPT_BLOCK_FG);
	char *block_bg = cfg_get_str(&block->block_cfg, BLOCK_OPT_BLOCK_BG);
	char *block_label_fg = cfg_get_str(&block->block_cfg, BLOCK_OPT_LABEL_FG);
	char *block_label_bg = cfg_get_str(&block->block_cfg, BLOCK_OPT_LABEL_BG);
	char *block_affix_fg = cfg_get_str(&block->block_cfg, BLOCK_OPT_AFFIX_FG);
	char *block_affix_bg = cfg_get_str(&block->block_cfg, BLOCK_OPT_AFFIX_BG);
	char *block_lc = cfg_get_str(&block->block_cfg, BLOCK_OPT_LC);
	int block_ol = cfg_get_int(&block->block_cfg, BLOCK_OPT_OL);
	int block_ul = cfg_get_int(&block->block_cfg, BLOCK_OPT_UL);
	int block_offset = cfg_get_int(&block->block_cfg, BLOCK_OPT_OFFSET);

	// TODO bug! if the user decides to set underline TRUE for the bar
	//      buf turn it FALSE for individual blocks, it will not work.
	//	similarly, the 'offset' option currently only works when set 
	//	on a block, not when set for the entire bar
	const char *fg = strsel(block_fg, NULL, NULL);
	const char *bg = strsel(block_bg, bar_block_bg, NULL);
	const char *lc = strsel(block_lc, NULL, NULL);
	const char *label_fg = strsel(block_label_fg, bar_label_fg, fg);
	const char *label_bg = strsel(block_label_bg, bar_label_bg, bg);
	const char *affix_fg = strsel(block_affix_fg, bar_affix_fg, fg);
	const char *affix_bg = strsel(block_affix_bg, bar_affix_bg, bg);
        const int offset = (block_offset >= 0) ? block_offset : bar_block_offset;
	const int ol = block_ol ? 1 : (bar_block_ol ? 1 : 0);
	const int ul = block_ul ? 1 : (bar_block_ul ? 1 : 0);

	//const char *prefix = prefixstr(bar->block_cfg.prefix, affix_fg, affix_bg);

	// TODO currently we are adding the format thingies for label, 
	//      prefix and suffix, even if those are empty anyway, which
	//      makes the string much longer than it needs to be, hence 
	//      also increasing the parsing workload for lemonbar.
	//      but of course, replacing a couple bytes with lots of malloc
	//      would not be great either, so... not sure about it yet.

	char *block_prefix = cfg_get_str(&bar->block_cfg, BLOCK_OPT_PREFIX);
	char *block_suffix = cfg_get_str(&bar->block_cfg, BLOCK_OPT_SUFFIX);
	char *block_label  = cfg_get_str(&block->block_cfg, BLOCK_OPT_LABEL);

	char *str = malloc(buf_len);
	snprintf(str, buf_len,
		"%s%%{O%d F%s B%s U%s %co %cu}"                   // 14
		"%%{T3 F%s B%s}%s"                                //  9
		"%%{T2 F%s B%s}%s"                                //  9
		"%%{T1 F%s B%s}%*s"                               //  9
		"%%{T3 F%s B%s}%s"                                //  9
		"%%{T- F- B- U- -o -u}%s",                        // 20
		// Start
		action_start,                                     // strlen
		offset,                                           // max 4
		fg ? fg : "-",                                    // strlen, max 9
		bg ? bg : "-",                                    // strlen, max 9
		lc ? lc : "-",                                    // strlen, max 9
		ol ? '+' : '-',                                   // 1
		ul ? '+' : '-',                                   // 1
		// Prefix
		affix_fg ? affix_fg : "-",                        // strlen, max 9
		affix_bg ? affix_bg : "-",		          // strlen, max 9
		block_prefix ? block_prefix : "",    // strlen
		// Label
		label_fg ? label_fg : "-",                        // strlen, max 9
		label_bg ? label_bg : "-",                        // strlen, max 9
		block_label ? block_label : "",  // strlen
		// Block
		fg ? fg : "-",                                    // strlen, max 9
		bg ? bg : "-",                                    // strlen, max 9
		padding,                                          // max 4
		result,                                           // strlen
		// Suffix
		affix_fg ? affix_fg : "-",                        // strlen, max 9
		affix_bg ? affix_bg : "-",                        // strlen, max 9
		block_suffix ? block_suffix : "",    // strlen
		// End
		action_end                                        // 5*4
	);

	free(result);
	return str;
}

/*
 * Returns 'l', 'c' or 'r' for input values -1, 0 and 1 respectively.
 * For other input values, the behavior is undefined.
 */
char get_align(const int align)
{
	char a[] = {'l', 'c', 'r'};
	return a[align+1]; 
}

/*
 * Combines the results of all given blocks into a single string that can be fed
 * to Lemonbar. Returns a pointer to the string, allocated with malloc().
 */
char *barstr(const state_s *state)
{
	// For convenience...
	const lemon_s *bar = &state->lemon;
	size_t num_blocks = state->num_blocks;

	// Short blocks like temperature, volume or battery, will usually use 
	// something in the range of 130 to 200 byte. So let's go with 256 byte.
	size_t bar_str_len = 256 * num_blocks; // TODO hardcoded value
	char *bar_str = malloc(bar_str_len);
	bar_str[0] = '\0';

	char align[5];
	int last_align = -1;

	const block_s *block = NULL;
	for (int i = 0; i < num_blocks; ++i)
	{
		block = &state->blocks[i];

		// Live blocks might not have a result available
		if (block->output == NULL)
		{
			continue;
		}

		int block_align = cfg_get_int(&block->block_cfg, BLOCK_OPT_ALIGN);

		char *block_str = blockstr(bar, block, 0);
		size_t block_str_len = strlen(block_str);
		if (block_align != last_align)
		{
			last_align = block_align;
			snprintf(align, 5, "%%{%c}", get_align(last_align));
			strcat(bar_str, align);
		}
		// Let's check if this block string can fit in our buffer
		size_t free_len = bar_str_len - (strlen(bar_str) + 1);
		if (block_str_len > free_len)
		{
			// Let's make space for approx. two more blocks
			bar_str_len += 256 * 2; 
			bar_str = realloc(bar_str, bar_str_len);
		}
		strcat(bar_str, block_str);
		free(block_str);
	}
	strcat(bar_str, "\n");
	bar_str = realloc(bar_str, strlen(bar_str) + 1);
	return bar_str;
}

/*
 * Parses the format string for the bar, which should contain block names 
 * separated by whitespace and, optionally, up to two vertical bars to indicate 
 * alignment of blocks. For every block name found, the callback function `cb` 
 * will be run. Returns the number of block names found.
 */
size_t parse_format(const char *format, create_block_callback cb, void *data)
{
	if (format == NULL)
	{
		return 0;
	}

	size_t format_len = strlen(format) + 1;
	char block_name[BUFFER_BLOCK_NAME];
	block_name[0] = '\0';
	size_t block_name_len = 0;
	int block_align = -1;
	int num_blocks = 0;

	for (size_t i = 0; i < format_len; ++i)
	{
		switch (format[i])
		{
		case '|':
			// Next align
			block_align += block_align < 1;
		case ' ':
		case '\0':
			if (block_name_len)
			{
				// Block name complete, inform the callback
				cb(block_name, block_align, num_blocks++, data);
				// Prepare for the next block name
				block_name[0] = '\0';
				block_name_len = 0;
			}
			break;
		default:
			// Add the char to the current's block name
			block_name[block_name_len++] = format[i];
			block_name[block_name_len]   = '\0';
		}
	}

	// Return the number of blocks found
	return num_blocks;
}

kita_child_s* make_child(state_s *state, const char *cmd, int in, int out, int err)
{
	// Create child process
	kita_child_s *child = kita_child_new(cmd, in, out, err);
	if (child == NULL)
	{
		return NULL;
	}

	// Add the child to the kita 
	if (kita_child_add(state->kita, child) == -1)
	{
		kita_child_free(&child);
		return NULL;
	}

	kita_child_set_context(child, state);
	return child;
}

/*
 * Finds and returns the block with the given `sid` -- or NULL.
 */
block_s *get_block(const state_s *state, const char *sid)
{
	// Iterate over all existing blocks and check for a name match
	for (size_t i = 0; i < state->num_blocks; ++i)
	{
		// If names match, return a pointer to this block
		if (equals(state->blocks[i].sid, sid))
		{
			return &state->blocks[i];
		}
	}
	return NULL;
}

/*
 * Add the block with the given SID to the collection of blocks, unless there 
 * is already a block with that SID present. 
 * Returns a pointer to the added (or existing) block or NULL in case of error.
 */
block_s *add_block(state_s *state, const char *sid)
{
	// See if there is an existing block by this name (and return, if so)
	block_s *eb = get_block(state, sid);
	if (eb)
	{
		return eb;
	}

	// Resize the block container to be able to hold one more block
	size_t current  =   state->num_blocks;
	size_t new_size = ++state->num_blocks * sizeof(block_s);
	block_s *blocks = realloc(state->blocks, new_size);
	if (blocks == NULL)
	{
		fprintf(stderr, "add_block(): realloc() failed!\n");
		--state->num_blocks;
		return NULL;
	}
	state->blocks = blocks;

	// Create the block, setting its name and default values
	state->blocks[current] = (block_s) { 0 };
	state->blocks[current].sid = strdup(sid);
	cfg_init(&state->blocks[current].block_cfg, "default", BLOCK_OPT_COUNT);

	// Return a pointer to the new block
	return &state->blocks[current];
}

/*
 * inih doc: "Handler should return nonzero on success, zero on error."
 */
int lemon_cfg_handler(void *data, const char *section, const char *name, const char *value)
{
	state_s *state = (state_s*) data;

	// Only process if section is empty or specificially for bar
	if (empty(section) || equals(section, state->lemon.sid))
	{
		return lemon_ini_handler(&state->lemon, section, name, value);
	}

	return 1;
}

/*
 * inih doc: "Handler should return nonzero on success, zero on error."
 */
int block_cfg_handler(void *data, const char *section, const char *name, const char *value)
{
	state_s *state = (state_s*) data;

	// Abort if section is empty or specifically for bar
	if (empty(section) || (equals(section, state->lemon.sid)))
	{
		return 1;
	}

	// Find the block whose name fits the section name
	block_s *block = get_block(state, section);

	// Abort if we couldn't find that block
	if (block == NULL)
	{
		return 1;
	}

	// Process via the appropriate handler
	return block_ini_handler(block, section, name, value);
}

/*
 * Load the config and parse the section for the bar, ignoring other sections.
 * Returns 0 on success, -1 on file open error, -2 on memory allocation error, 
 * -3 if no config file path was given in the preferences, or the line number 
 * of the first encountered parse error.
 */
static int load_lemon_cfg(state_s *state)
{
	// Abort if config file path empty or NULL
	if (empty(state->prefs.config))
	{
		return -3;
	}

	// Fire up the INI parser
	return ini_parse(state->prefs.config, lemon_cfg_handler, state);
}

/*
 * Load the config and parse all sections apart from the bar section.
 * Returns 0 on success, -1 on file open error, -2 on memory allocation error, 
 * -3 if no config file path was given in the preferences, or the line number 
 * of the first encountered parse error.
 */
static int load_block_cfg(state_s *state)
{
	// Abort if config file path empty or NULL
	if (empty(state->prefs.config))
	{
		return -3;
	}

	return ini_parse(state->prefs.config, block_cfg_handler, state);
}

spark_s *get_spark(state_s *state, void *block, const char *cmd)
{
	for (size_t i = 0; i < state->num_sparks; ++i)
	{
		if (state->sparks[i].block != block)
		{
			continue;
		}
		if (!equals(state->sparks[i].child->cmd, cmd))
		{
			continue;
		}
		return &state->sparks[i];
	}
	return NULL;
}

spark_s *add_spark(state_s *state, block_s *block, const char *cmd)
{
	// See if there is an existing spark that matches the given params
	spark_s *es = get_spark(state, block, cmd);
	if (es)
	{
		return es;
	}

	// Resize the spark array to be able to hold one more spark
	size_t current  =   state->num_sparks;
	size_t new_size = ++state->num_sparks * sizeof(spark_s);
	spark_s *sparks = realloc(state->sparks, new_size);
	if (sparks == NULL)
	{
		fprintf(stderr, "add_spark(): realloc() failed!\n");
		--state->num_sparks;
		return NULL;
	}
	state->sparks = sparks; 
	 
	state->sparks[current] = (spark_s) { 0 };
	state->sparks[current].block = block;

	// Add a reference of this spark to the block we've created it for
	block->spark = &state->sparks[current];

	// Return a pointer to the new spark
	return &state->sparks[current];
}

size_t create_sparks(state_s *state)
{
	block_s *block = NULL;
	for (size_t i = 0; i < state->num_blocks; ++i)
	{
		block = &state->blocks[i];

		if (block->type != BLOCK_SPARKED)
		{
			continue;
		}

		char *trigger = cfg_get_str(&block->block_cfg, BLOCK_OPT_TRIGGER);
		if (empty(trigger))
		{
			fprintf(stderr, "create_sparks(): missing trigger for sparked block '%s'\n", block->sid);
			continue;
		}

		add_spark(state, block, trigger);
	}

	for (size_t i = 0; i < state->num_sparks; ++i)
	{
		char *trigger = cfg_get_str(&state->sparks[i].block->block_cfg, BLOCK_OPT_TRIGGER);
		state->sparks[i].child = make_child(state, trigger, 0, 1, 0);
	}

	return state->num_sparks;
}

/*
 * Takes a string that might represent an action that was registered with one 
 * of the blocks and tries to find the associated block. If found, the command
 * associated with the action will be executed.
 * Returns 0 on success, -1 if the string was not a recognized action command
 * or the block that the action belongs to could not be found.
 */
int process_action(const state_s *state, const char *action)
{
	size_t len = strlen(action);
	if (len < 5)
	{
		return -1;	// Can not be an action command, too short
	}

	// A valid action command should have the format <blockname>_<cmd-type>
	// For example, for a block named `datetime` that was clicked with the 
	// left mouse button, `action` should be "datetime_lmb"

	char types[5][5] = {"_lmb", "_mmb", "_rmb", "_sup", "_sdn"};

	// Extract the type suffix, including the underscore
	char type[5]; 
	snprintf(type, 5, "%s", action + len - 4);

	// Extract everything _before_ the suffix (this is the block name)
	char block[len-3];
	snprintf(block, len - 3, "%s", action); 

	// We check if the action type is valid (see types)
	int b = 0;
	int found = 0;
	for (; b < 5; ++b)
	{
		if (equals(type, types[b]))
		{
			found = 1;
			break;
		}
	}

	// Not a recognized action type
	if (!found)
	{
		return -1;
	}

	// Find the source block of the action
	block_s *source = get_block(state, block);
	if (source == NULL)
	{
		return -1;
	}

	// Now to fire the right command for the action type
	switch (b) {
		case 0:
			run_cmd(cfg_get_str(&source->block_cfg, BLOCK_OPT_CMD_LMB));
			return 0;
		case 1:
			run_cmd(cfg_get_str(&source->block_cfg, BLOCK_OPT_CMD_MMB));
			return 0;
		case 2:
			run_cmd(cfg_get_str(&source->block_cfg, BLOCK_OPT_CMD_RMB));
			return 0;
		case 3:
			run_cmd(cfg_get_str(&source->block_cfg, BLOCK_OPT_CMD_SUP));
			return 0;
		case 4:
			run_cmd(cfg_get_str(&source->block_cfg, BLOCK_OPT_CMD_SDN));
			return 0;
		default:
			// Should never happen...
			return -1;
	}
}

/*
 * This callback is supposed to be called for every block name that is being 
 * extracted from the config file's 'format' option for the bar itself, which 
 * lists the blocks to be displayed on the bar. `name` should contain the name 
 * of the block as read from the format string, `align` should be -1, 0 or 1, 
 * meaning left, center or right, accordingly (indicating where the block is 
 * supposed to be displayed on the bar).
 */
static void on_block_found(const char *name, int align, int n, void *data)
{
	// 'Unpack' the data
	state_s *state = (state_s*) data;
	
	// Find or add the block with the given name
	block_s *block = add_block(state, name);
	
	if (block == NULL)
	{
		fprintf(stderr, "on_block_found(): add_block() failed!\n");
		return;
	}
	// Set the block's align to the given one
	cfg_set_int(&block->block_cfg, BLOCK_OPT_ALIGN, align);
}

/*
 * Handles SIGINT (CTRL+C) and similar signals by setting the static variable
 * `running` to 0, effectively ending the main loop, so that clean-up happens.
 */
void on_signal(int sig)
{
	running = 0;
	handled = sig;
}

lemon_s *lemon_by_child(state_s *state, kita_child_s *child)
{
	return (state->lemon.child == child) ? &state->lemon : NULL;
}

block_s *block_by_child(state_s *state, kita_child_s *child)
{
	for (size_t i = 0; i < state->num_blocks; ++i)
	{
		if (state->blocks[i].child == child)
		{
			return &state->blocks[i];
		}
	}
	return NULL;
}

spark_s *spark_by_child(state_s *state, kita_child_s *child)
{
	for (size_t i = 0; i < state->num_sparks; ++i)
	{
		if (state->sparks[i].child == child)
		{
			return &state->sparks[i];
		}
	}
	return NULL;
}

void on_child_error(kita_state_s *ks, kita_event_s *ke)
{
	//fprintf(stderr, "on_child_error(): %s\n", ke->child->cmd);
	// TODO possibly log this to a file or something
}

void on_child_feedok(kita_state_s *ks, kita_event_s *ke)
{
	//fprintf(stderr, "on_child_feedok(): %s\n", ke->child->cmd);
}

void on_child_readok(kita_state_s *ks, kita_event_s *ke)
{
	//fprintf(stderr, "on_child_readok(): %s\n", ke->child->cmd);

	state_s *state = (state_s*) kita_child_get_context(ke->child);
	lemon_s *lemon = NULL;
	block_s *block = NULL;
	spark_s *spark = NULL;

	if ((lemon = lemon_by_child(state, ke->child)))
	{
		if (ke->ios == KITA_IOS_OUT)
		{
			char *output = kita_child_read(ke->child, ke->ios);
			process_action(state, output);
			free(output);
		}
		else
		{
			// TODO user should be able to choose whether this ...
			//      - ... will be ignored
			//      - ... will be printed to stderr
			//      - ... will be logged to a file
			fprintf(stderr, "%s\n", kita_child_read(ke->child, ke->ios));
		}
		return;
	}

	if ((block = block_by_child(state, ke->child)))
	{
		if (ke->ios == KITA_IOS_OUT)
		{
			// schedule an update if the block's output was
			// different from its previous output
			if (read_block(block))
			{
				state->due = 1;
			}
		}
		else
		{
			// TODO implement block mode switch logic

		}
		return;
	}

	if ((spark = spark_by_child(state, ke->child)))
	{
		if (ke->ios == KITA_IOS_OUT)
		{
			read_spark(spark);
		}
		return;
	}
}

void on_child_exited(kita_state_s *ks, kita_event_s *ke)
{
	//fprintf(stderr, "on_child_exited(): %s\n", ke->child->cmd);
	
	state_s *state = (state_s*) kita_child_get_context(ke->child);
	lemon_s *lemon = NULL;
	block_s *block = NULL;
	spark_s *spark = NULL;
	
	if ((lemon = lemon_by_child(state, ke->child)))
	{
		running = 0;
		return;
	}

	if ((block = block_by_child(state, ke->child)))
	{
		block->alive = 0;
		return;
	}
	
	if ((spark = spark_by_child(state, ke->child)))
	{
		spark->alive = 0;
		return;
	}
}

void on_child_closed(kita_state_s *ks, kita_event_s *ke)
{
	//fprintf(stderr, "on_child_closed(): %s\n", ke->child->cmd);
}

void on_child_reaped(kita_state_s *ks, kita_event_s *ke)
{
	//fprintf(stderr, "on_child_reaped(): %s\n", ke->child->cmd);
	on_child_exited(ks, ke);
}

void on_child_remove(kita_state_s *ks, kita_event_s *ke)
{
	//fprintf(stderr, "on_child_remove()\n");
}

// http://courses.cms.caltech.edu/cs11/material/general/usage.html
void help(const char *invocation, FILE *where)
{
	fprintf(where, "USAGE\n");
	fprintf(where, "\t%s [OPTIONS...]\n", invocation);
	fprintf(where, "\n");
	fprintf(where, "OPTIONS\n");
	fprintf(where, "\t-e\tRun bar even if it is empty (no blocks).\n");
	fprintf(where, "\t-h\tPrint this help text and exit.\n");
	fprintf(where, "\t-s\tINI section name for the bar.\n");
}

int main(int argc, char **argv)
{
	//
	// SIGNAL HANDLING
	//

	struct sigaction sa_int = { .sa_handler = &on_signal };

	sigaction(SIGINT,  &sa_int, NULL);
	sigaction(SIGQUIT, &sa_int, NULL);
	sigaction(SIGTERM, &sa_int, NULL);
	sigaction(SIGPIPE, &sa_int, NULL);
	
	//
	// CHECK FOR X 
	//

	if (!x_is_running())
	{
		fprintf(stderr, "Failed to detect X\n");
		return EXIT_FAILURE;
	}

	//
	// SUCCADE STATE
	//

	state_s  state = { 0 };
	prefs_s *prefs = &(state.prefs); // For convenience
	lemon_s *lemon = &(state.lemon); // For convenience

	//
	// KITA STATE
	//

	state.kita = kita_init();
	if (state.kita == NULL)
	{
		fprintf(stderr, "Failed to initialize kita state\n");
		return EXIT_FAILURE;
	}

	kita_state_s *kita = state.kita; // For convenience
	kita_set_option(kita, KITA_OPT_NO_NEWLINE, 1);

	// 
	// KITA CALLBACKS 
	//

	kita_set_callback(kita, KITA_EVT_CHILD_CLOSED, on_child_closed);
	kita_set_callback(kita, KITA_EVT_CHILD_REAPED, on_child_reaped);
	kita_set_callback(kita, KITA_EVT_CHILD_HANGUP, on_child_exited);
	kita_set_callback(kita, KITA_EVT_CHILD_EXITED, on_child_exited);
	kita_set_callback(kita, KITA_EVT_CHILD_REMOVE, on_child_remove);
	kita_set_callback(kita, KITA_EVT_CHILD_FEEDOK, on_child_feedok);
	kita_set_callback(kita, KITA_EVT_CHILD_READOK, on_child_readok);
	kita_set_callback(kita, KITA_EVT_CHILD_ERROR,  on_child_error);

	//
	// COMMAND LINE ARGUMENTS
	//

	parse_args(argc, argv, prefs);
	char *default_cfg_path = NULL;

	//
	// PRINT HELP AND EXIT, MAYBE
	//

	if (prefs->help)
	{
		help(argv[0], stdout);
		return EXIT_SUCCESS;
	}

	//
	// PREFERENCES / DEFAULTS
	//

	// if no custom config file given, set it to the default
	if (prefs->config == NULL)
	{
		// we use an additional variable for consistency with free()
		default_cfg_path = config_path(DEFAULT_CFG_FILE, SUCCADE_NAME);
		prefs->config = default_cfg_path; 
	}

	// if no custom INI section for bar given, set it to default
	if (prefs->section == NULL)
	{
		prefs->section = DEFAULT_LEMON_SECTION;
	}

	//
	// BAR
	//

	// copy the section ID from the config for convenience and consistency
	lemon->sid = strdup(prefs->section);
	cfg_init(&lemon->lemon_cfg, "lemon", LEMON_OPT_COUNT);
	cfg_init(&lemon->block_cfg, "lemon", BLOCK_OPT_COUNT);

	// read the config file and parse bar's section
	if (load_lemon_cfg(&state) < 0)
	{
		fprintf(stderr, "Failed to load config file: %s\n", prefs->config);
		return EXIT_FAILURE;
	}
	
	// if no `bin` option was present in the config, set it to the default
	//if (empty(lemon->lemon_cfg.bin))
	if (!cfg_has(&lemon->lemon_cfg, LEMON_OPT_BIN))
	{
		// We use strdup() for consistency with free() later on
		cfg_set_str(&lemon->lemon_cfg, LEMON_OPT_BIN, strdup(DEFAULT_LEMON_BIN));
	}

	// if no `name` option was present in the config, set it to the default
	//if (empty(lemon->lemon_cfg.name))
	if (!cfg_has(&lemon->lemon_cfg, LEMON_OPT_NAME))
	{
		// We use strdup() for consistency with free() later on
		cfg_set_str(&lemon->lemon_cfg, LEMON_OPT_NAME, strdup(DEFAULT_LEMON_NAME));
	}

	// create the child process and add it to the kita state
	char *lemon_bin = cfg_get_str(&lemon->lemon_cfg, LEMON_OPT_BIN);
	lemon->child = make_child(&state, lemon_bin, 1, 1, 1);
	if (lemon->child == NULL)
	{
		fprintf(stderr, "Failed to create bar process: %s\n", lemon_bin);
		return EXIT_FAILURE;
	}

	// open (run) the lemon
	if (open_lemon(lemon) == -1)
	{
		fprintf(stderr, "Failed to open bar: %s\n", lemon->sid);
		return EXIT_FAILURE;
	}

	//
	// BLOCKS
	//

	// create blocks by parsing the format string
	// TODO I'd like to make this into a two-step thing:
	//      1. parse the format string, creating an array of requested block names
	//      2. iterate through the requested block names, creating blocks as we go
	char *lemon_format = cfg_get_str(&lemon->lemon_cfg, LEMON_OPT_FORMAT);
	parse_format(lemon_format, on_block_found, &state);
	
	// exit if no blocks could be loaded and 'empty' option isn't present
	if (state.num_blocks == 0 && prefs->empty == 0)
	{
		fprintf(stderr, "Failed to load any blocks\n");
		return EXIT_FAILURE;
	}

	// parse the config again, this time processing block sections
	if (load_block_cfg(&state) < 0)
	{
		fprintf(stderr, "Failed to load config file: %s\n", prefs->config);
		return EXIT_FAILURE;
	}

	// create child processes and add them to the kita state
	block_s *block = NULL;
	for (size_t i = 0; i < state.num_blocks; ++i)
	{
		block = &state.blocks[i];
		char *block_bin = cfg_get_str(&block->block_cfg, BLOCK_OPT_BIN);
		char *block_cmd = block_bin ? block_bin : block->sid;
		block->child = make_child(&state, block_cmd, 0, 1, 1);
	}

	//
	// SPARKS
	//

	create_sparks(&state);
	open_sparks(&state);
	
	//
	// MAIN LOOP
	//

	double now;
	double before = get_time();
	double delta;
	double wait = 0.0; 

	running = 1;
	
	while (running)
	{
		now    = get_time();
		delta  = now - before;
		before = now;

		//fprintf(stderr, "> now = %f, wait = %f, delta = %f\n", now, wait, delta);

		// open all blocks that are due
		block_s *block = NULL;
		for (size_t i = 0; i < state.num_blocks; ++i)
		{
			block = &state.blocks[i];
			if (block_is_due(block, now, BLOCK_WAIT_TOLERANCE))
			{
				//fprintf(stderr, "> open_block() '%s'\n", block->sid);
				if (block_can_consume(block))
				{
					kita_child_set_arg(block->child, block->spark->output);
					open_block(block);
					kita_child_set_arg(block->child, NULL);
				}
				else
				{
					open_block(block);
				}
				if (block->type == BLOCK_SPARKED)
				{
					free(block->spark->output);
					block->spark->output = NULL;

				}
			}
		}

		// feed the bar, if there is any new block output 
		if (state.due)
		{
			char *input = barstr(&state);
			//fprintf(stderr, "_ %s\n", input);
			kita_child_feed(lemon->child, input);
			free(input);
			state.due = 0;
		}

		// let kita check for child events
		kita_tick(kita, (wait == -1 ? wait : wait * MILLISEC_PER_SEC));

		// figure out how long we can idle, based on timed blocks
		double lemon_due = DBL_MAX;
		double thing_due = DBL_MAX;
		for (size_t i = 0; i < state.num_blocks; ++i)
		{
			block = &state.blocks[i];
			thing_due = block_due_in(block, now);

			if (thing_due < lemon_due)
			{
				lemon_due = thing_due;
			}
		}

		// Update `wait` accordingly (-1 = not waiting on any blocks)
		wait = lemon_due == DBL_MAX ? -1 : lemon_due;
	}

	//
	// CLEAN UP
	//

	fprintf(stderr, "Performing clean-up ...\n");

	free(default_cfg_path);

	// Close triggers - it's important we free these first as they might
	// point to instances of bar and/or blocks, which will lead to errors
	free_sparks(&state);
	free(state.sparks);
	
	// Close blocks
	free_blocks(&state);
	free(state.blocks);

	// Close bar
	free_lemon(&state.lemon);

	kita_free(&kita);
	kita = NULL;

	fprintf(stderr, "Clean-up finished, see you next time!\n");

	return EXIT_SUCCESS;
}

