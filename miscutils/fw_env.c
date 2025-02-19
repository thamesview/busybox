/*
 * u-boot flash environment variable get/set commands
 *
 * (C) Copyright 2000-2008
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * (C) Copyright 2008
 * Guennadi Liakhovetski, DENX Software Engineering, lg@denx.de.
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

//config:config UBOOT_ENV_FW_PRINTENV
//config:	bool "uboot environment fw_printenv"
//config:	default n
//config:	help
//config:	  Enables setting and printing u-boot environment varaibles.
//config:	  The fw_printenv will be included.
//config:
//config:config UBOOT_ENV_FW_SETENV
//config:	bool "uboot environment fw_setenv"
//config:	default n
//config:	help
//config:	  Enables setting and printing u-boot environment varaibles.
//config:	  The fw_setenv will be included.

//applet:IF_UBOOT_ENV_FW_PRINTENV(APPLET(fw_setenv, BB_DIR_USR_SBIN, BB_SUID_DROP))
//applet:IF_UBOOT_ENV_FW_SETENV(APPLET_ODDNAME(fw_printenv, fw_setenv, BB_DIR_USR_SBIN, BB_SUID_DROP, fw_printenv))

//kbuild:lib-$(CONFIG_UBOOT_ENV_FW_PRINTENV) += fw_env.o
//kbuild:lib-$(CONFIG_UBOOT_ENV_FW_SETENV) += fw_env.o

#include "libbb.h"
#include "common_bufsiz.h"

#include <mtd/mtd-user.h>
#include "fw_env.h"

#define	CMD_GETENV	"fw_printenv"
#define	CMD_SETENV	"fw_setenv"

#define min(x, y) ({				\
	typeof(x) _min1 = (x);			\
	typeof(y) _min2 = (y);			\
	(void) (&_min1 == &_min2);		\
	_min1 < _min2 ? _min1 : _min2; })

struct envdev_s {
	char devname[16];		/* Device name */
	ulong devoff;			/* Device offset */
	ulong env_size;			/* environment size */
	ulong erase_size;		/* device erase size */
	ulong env_sectors;		/* number of environment sectors */
	uint8_t mtd_type;		/* type of the MTD device */
};

static struct envdev_s envdevices[2] =
{
	{
		.mtd_type = MTD_ABSENT,
	}, {
		.mtd_type = MTD_ABSENT,
	},
};
static int dev_current;

#define DEVNAME(i)    envdevices[(i)].devname
#define DEVOFFSET(i)  envdevices[(i)].devoff
#define ENVSIZE(i)    envdevices[(i)].env_size
#define DEVESIZE(i)   envdevices[(i)].erase_size
#define ENVSECTORS(i) envdevices[(i)].env_sectors
#define DEVTYPE(i)    envdevices[(i)].mtd_type

#define CONFIG_ENV_SIZE ENVSIZE(dev_current)

#define ENV_SIZE      getenvsize()

struct env_image_single {
	uint32_t	crc;	/* CRC32 over data bytes    */
	char		data[];
};

struct env_image_redundant {
	uint32_t	crc;	/* CRC32 over data bytes    */
	unsigned char	flags;	/* active or obsolete */
	char		data[];
};

enum flag_scheme {
	FLAG_NONE,
	FLAG_BOOLEAN,
	FLAG_INCREMENTAL,
};

struct environment {
	void			*image;
	uint32_t		*crc;
	unsigned char		*flags;
	char			*data;
	enum flag_scheme	flag_scheme;
};

static struct environment environment = {
	.flag_scheme = FLAG_NONE,
};

static int HaveRedundEnv = 0;

static unsigned char active_flag = 1;
/* obsolete_flag must be 0 to efficiently set it on NOR flash without erasing */
static unsigned char obsolete_flag = 0;


#define XMK_STR(x)	#x
#define MK_STR(x)	XMK_STR(x)

static unsigned	long  crc32 (unsigned long, const unsigned char *, unsigned);
static int flash_io (int mode);
static char *envmatch (char * s1, char * s2);
#define ENV_CRC_BAD (-5)
static int env_init (void);
static int parse_config (void);

#if defined(CONFIG_FILE)
static int get_config (const char *);
#endif
static inline ulong getenvsize (void)
{
	ulong rc = CONFIG_ENV_SIZE - sizeof (uint32_t);

	if (HaveRedundEnv)
		rc -= sizeof (char);
	return rc;
}

unsigned long crc32 (unsigned long crc, const unsigned char *buf, unsigned len)
{
	return crc32_block_endian0( crc ^ 0xffffffffL, buf, len, global_crc32_table) ^ 0xffffffffL;
}

/*
 * Search the environment for a variable.
 * Return the value, if found, or NULL, if not found.
 */
char *fw_getenv (char *name)
{
	char *env, *nxt;

	if (env_init ())
		return NULL;

	for (env = environment.data; *env; env = nxt + 1) {
		char *val;

		for (nxt = env; *nxt; ++nxt) {
			if (nxt >= &environment.data[ENV_SIZE]) {
				fprintf (stderr, "## Error: "
					"environment not terminated\n");
				return NULL;
			}
		}
		val = envmatch (name, env);
		if (!val)
			continue;
		return val;
	}
	return NULL;
}

//usage:#define fw_printenv_trivial_usage
//usage:       "[[ -n name ] | [ name ... ]]"
//usage:#define fw_printenv_full_usage "\n\n"
//usage:       "Print u-boot environment variables\n"
//usage:     "\n	-n	Print only the value"

/*
 * Print the current definition of one, or more, or all
 * environment variables
 */
int fw_printenv_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int fw_printenv_main(int argc UNUSED_PARAM, char **argv)
{
	char *env, *nxt;
	int i, n_flag;
	int rc = 0;

	rc = env_init ();
	if ( rc == ENV_CRC_BAD) {
		fprintf (stderr, "## Error: "
			 "Unable to display variables when flash environment "
			 "is corrupted.\n");
		fprintf (stderr, "## Error: "
			 "Use u-boot 'saveenv' CLI to update variables with defaults.\n");
		return -1;
	}
	else if (rc) {
		return -1;
	}

	if (argc == 1) {		/* Print all env variables  */
		for (env = environment.data; *env; env = nxt + 1) {
			for (nxt = env; *nxt; ++nxt) {
				if (nxt >= &environment.data[ENV_SIZE]) {
					fprintf (stderr, "## Error: "
						"environment not terminated\n");
					return -1;
				}
			}

			printf ("%s\n", env);
		}
		return 0;
	}

	if (strcmp (argv[1], "-n") == 0) {
		n_flag = 1;
		++argv;
		--argc;
		if (argc != 2) {
			fprintf (stderr, "## Error: "
				"`-n' option requires exactly one argument\n");
			bb_show_usage();
			return -1;
		}
	} else {
		n_flag = 0;
	}

	for (i = 1; i < argc; ++i) {	/* print single env variables   */
		char *name = argv[i];
		char *val = NULL;

		for (env = environment.data; *env; env = nxt + 1) {

			for (nxt = env; *nxt; ++nxt) {
				if (nxt >= &environment.data[ENV_SIZE]) {
					fprintf (stderr, "## Error: "
						"environment not terminated\n");
					return -1;
				}
			}
			val = envmatch (name, env);
			if (val) {
				if (!n_flag) {
					fputs (name, stdout);
					putc ('=', stdout);
				}
				puts (val);
				break;
			}
		}
		if (!val) {
			fprintf (stderr, "## Error: \"%s\" not defined\n", name);
			rc = -1;
		}
	}

	return rc;
}

/*
 * Prompts user for a yes/no "are you sure?" message.
 * If 'yes' return 1;
 * If the force variable is true return 1;
 * Otherwise return 0;
 *
 */
static int are_you_sure(const bool force)
{
	char buf[2];

	if (force) {
		return 1;
	}

	fprintf(stderr, "Proceed with update [N/y]? ");
	fgets( buf, 2, stdin);

	if ( (buf[0] == 'y') || (buf[0] == 'Y')) {
		return 1;
	}
	else {
		return 0;
	}
}

/*
** Processes (argc, argv) from a command line.
**
** Inputs:
**
** argv[0] contains the variable name.
**
** argv[1] .. argv[argc-1] are concatenated together to form the
** variable value.
**
** Returns:
**   -1 on error
**    1 on variable modification, either delete or update.
**    0 no updates made.
**
*/
static int process_one_line(int argc, char **argv, const bool force)
{
	int i, len;
	int val_argc = 0;
	char *env, *nxt;
	char *oldval = NULL;
	char *name, *new_var;

	if ( argc < 1) {
		fprintf( stderr, "## Error: "
			 "Expecting variable name, but none found.\n");
		bb_show_usage();
		errno = EINVAL;
		return -1;
	}

	name = argv[0];
	val_argc = 1;

	/*
	 * search if variable with this name already exists
	 */
	for (nxt = env = environment.data; *env; env = nxt + 1) {
		for (nxt = env; *nxt; ++nxt) {
			if (nxt >= &environment.data[ENV_SIZE]) {
				fprintf (stderr, "## Error: "
					"environment not terminated\n");
				errno = EINVAL;
				return -1;
			}
		}
		if ((oldval = envmatch (name, env)) != NULL)
			break;
	}

	/*
	 * Delete any existing definition
	 */
	if (oldval) {
		if (*++nxt == '\0') {
			*env = '\0';
		} else {
			for (;;) {
				*env = *nxt++;
				if ((*env == '\0') && (*nxt == '\0'))
					break;
				++env;
			}
		}
		*++env = '\0';
	}

	/* Delete only ? */
	if (argc < (val_argc + 1)) {
		if (oldval) {
			if (!force) {
				fprintf(stderr, "Deleting environment variable: `%s'\n", name);
			}
			return 1;
		}
		else {
			// trying to delete non-existant variable
			return 0;
		}
	}

	/*
	 * Append new definition at the end
	 */
	for (env = environment.data; *env || *(env + 1); ++env);
	if (env > environment.data)
		++env;
	/*
	 * Overflow when:
	 * "name" + "=" + "val" +"\0\0"  > CONFIG_ENV_SIZE - (env-environment)
	 */
	len = strlen (name) + 2;
	/* add '=' for first arg, ' ' for all others */
	for (i = val_argc; i < argc; ++i) {
		len += strlen (argv[i]) + 1;
	}
	if (len > (&environment.data[ENV_SIZE] - env)) {
		fprintf( stderr, "## Error: "
			"Environment overflow.  Unable to process \"%s\".\n",
			name);
		errno = ENOMEM;
		return -1;
	}
	new_var = env;
	while ((*env = *name++) != '\0')
		env++;
	for (i = val_argc; i < argc; ++i) {
		char *val = argv[i];

		*env = (i == val_argc) ? '=' : ' ';
		while ((*++env = *val++) != '\0');
	}

	/* end is marked with double '\0' */
	*++env = '\0';

	if (!force) {
		fprintf(stderr, "Updating environment variable: `%s'\n", new_var);
	}
	return 1;
}

static int process_file( const char* fname, const bool force)
{
	FILE* f;
	char* buf = bb_common_bufsiz1;
	char* p;
	char* name;
	char* val;
	char* argv[2];
	int len;
	int rc;
	int modified = 0;

	if ( strcmp(fname, "-") == 0) {
		f = stdin;
	}
	else {
		f = fopen(fname, "r");
	}
	if ( NULL == f) {
		fprintf(stderr, "Unable to open file %s for reading...\n", fname);
		return -1;
	}

	while (fgets(buf, COMMON_BUFSIZE, f)) {
		// trim leading whitespace
		for (p = buf; *p && isspace(*p); p++);

		if (*p == '\0') {
			// line is completely whitespace
			continue;
		}

		if (*p == '#') {
			// skip lines starting with comment character
			continue;
		}

		len = strlen(p);
		// if line ends with a newline, replace it with \0
		if (p[len-1] == '\n') {
			p[len-1] = '\0';
			len--;
		}

		name = p;
		// Look for first white space character.  That
		// marks the end of the variable name.
		for (; *p && !isspace(*p); p++);
		*p = '\0';
		p++;

		// trim leading whitespace from value
		for (; *p && isspace(*p); p++);
		val = p;

		// pack name/value into argv[] to reuse the existing
		// one line parser.
		argv[0] = name;
		argv[1] = val;
		rc = process_one_line( *val ? 2 : 1, argv, force);
		if (rc < 0) {
			return rc;
		}
		else {
			modified += rc;
		}
	}

	fclose(f);
	return modified;
}

//usage:#define fw_setenv_trivial_usage
//usage:       "[-f] name [ value ... ] OR "
//usage:       "[-s] file name"
//usage:#define fw_setenv_full_usage "\n\n"
//usage:       "Set u-boot environemnt variable to value.\n"
//usage:       "If a name without any values is given, the variable\n"
//usage:       "with this name is deleted from the environment;\n"
//usage:       "otherwise, all 'value' arguments are concatenated,\n"
//usage:       "separated by single blank characters, and the\n"
//usage:       "resulting string is assigned to the environment\n"
//usage:       "variable 'name'.\n"
//usage:     "\n	-f	Force update.  Assume 'yes'."
//usage:     "\n	-s	Read name/value pairs from a file."
//usage:     "\n		Use '-' for stdin and '-s' option."
//usage:       "\n"

/*
 * Deletes or sets environment variables. Returns -1 and sets errno error codes:
 * 0	  - OK
 * EINVAL - need at least 1 argument
 * EROFS  - certain variables ("ethaddr", "serial#") cannot be
 *	    modified or deleted
 *
 */

/* Must match getopt32 option string order */
enum {
	OPT_f = 1 << 0,
	OPT_s = 1 << 1,
};

int fw_setenv_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int fw_setenv_main(int argc, char **argv)
{
	int rc;
	unsigned opt;
	const char *str_s;

	if (argc < 2) {
		errno = EINVAL;
		bb_show_usage();
		return -1;
	}

	rc = env_init ();
	if ( rc == ENV_CRC_BAD) {
		fprintf (stderr, "## Error: "
			 "Unable to update variables when flash environment "
			 "is corrupted.\n");
		fprintf (stderr, "## Error: "
			 "Use u-boot 'saveenv' CLI to update variables with defaults.\n");
		return -1;
	}
	else if (rc) {
		return -1;
	}

	opt = getopt32(argv, "fs:", &str_s);

	// skip command name in argument list
	argc--;
	argv++;

	if (opt & OPT_f) {
		argc--;
		argv++;
	}

	if (opt & OPT_s) {
		// name / value pairs given in a file, one set per line.
		if ( (strcmp(str_s, "-") == 0) && !(opt & OPT_f)) {
			// Can't ask interactive questions while also
			// reading input from stdin.
			fprintf (stderr, "## Error: "
				 "Using stdin as the input file also requires the '-f' option.\n");
			return -1;
		}
		rc = process_file(str_s, opt & OPT_f);
	}
	else {
		// name / value pair given on command line
		rc = process_one_line(argc, argv, opt & OPT_f);
	}

	if (rc < 0) {
		return -1;
	}

	if ( rc > 0 ) {
		if ( !are_you_sure(opt & OPT_f)) {
			return 0;
		}

		/*
		 * Update CRC
		 */
		*environment.crc = crc32 (0, (uint8_t *) environment.data, ENV_SIZE);

		/* write environment back to flash */
		if (flash_io (O_RDWR)) {
			fprintf (stderr, "Error: can't write fw_env to flash\n");
			return -1;
		}
	}

	return 0;
}

/*
 * Test for bad block on NAND, just returns 0 on NOR, on NAND:
 * 0	- block is good
 * > 0	- block is bad
 * < 0	- failed to test
 */
static int flash_bad_block (int fd, uint8_t mtd_type, loff_t *blockstart)
{
	if (mtd_type == MTD_NANDFLASH) {
		int badblock = ioctl (fd, MEMGETBADBLOCK, blockstart);

		if (badblock < 0) {
			perror ("Cannot read bad block mark");
			return badblock;
		}

		if (badblock) {
#ifdef DEBUG
			fprintf (stderr, "Bad block at 0x%llx, "
				 "skipping\n", *blockstart);
#endif
			return badblock;
		}
	}

	return 0;
}

/*
 * Read data from flash at an offset into a provided buffer. On NAND it skips
 * bad blocks but makes sure it stays within ENVSECTORS (dev) starting from
 * the DEVOFFSET (dev) block. On NOR the loop is only run once.
 */
static int flash_read_buf (int dev, int fd, void *buf, size_t count,
			   off_t offset, uint8_t mtd_type)
{
	size_t blocklen;	/* erase / write length - one block on NAND,
				   0 on NOR */
	size_t processed = 0;	/* progress counter */
	size_t readlen = count;	/* current read length */
	off_t top_of_range;	/* end of the last block we may use */
	off_t block_seek;	/* offset inside the current block to the start
				   of the data */
	loff_t blockstart;	/* running start of the current block -
				   MEMGETBADBLOCK needs 64 bits */
	int rc;

	/*
	 * Start of the first block to be read, relies on the fact, that
	 * erase sector size is always a power of 2
	 */
	blockstart = offset & ~(DEVESIZE (dev) - 1);

	/* Offset inside a block */
	block_seek = offset - blockstart;

	if (mtd_type == MTD_NANDFLASH) {
		/*
		 * NAND: calculate which blocks we are reading. We have
		 * to read one block at a time to skip bad blocks.
		 */
		blocklen = DEVESIZE (dev);

		/*
		 * To calculate the top of the range, we have to use the
		 * global DEVOFFSET (dev), which can be different from offset
		 */
		top_of_range = (DEVOFFSET (dev) & ~(blocklen - 1)) +
			ENVSECTORS (dev) * blocklen;

		/* Limit to one block for the first read */
		if (readlen > blocklen - block_seek)
			readlen = blocklen - block_seek;
	} else {
		blocklen = 0;
		top_of_range = offset + count;
	}

	/* This only runs once on NOR flash */
	while (processed < count) {
		rc = flash_bad_block (fd, mtd_type, &blockstart);
		if (rc < 0)		/* block test failed */
			return -1;

		if (blockstart + block_seek + readlen > top_of_range) {
			/* End of range is reached */
			fprintf (stderr,
				 "Too few good blocks within range\n");
			return -1;
		}

		if (rc) {		/* block is bad */
			blockstart += blocklen;
			continue;
		}

		/*
		 * If a block is bad, we retry in the next block at the same
		 * offset - see common/env_nand.c::writeenv()
		 */
		lseek (fd, blockstart + block_seek, SEEK_SET);

		rc = read (fd, buf + processed, readlen);
		if (rc != readlen) {
			fprintf (stderr, "Read error on %s: %s\n",
				 DEVNAME (dev), strerror (errno));
			return -1;
		}
#ifdef DEBUG
		fprintf (stderr, "Read 0x%x bytes at 0x%llx\n",
			 rc, blockstart + block_seek);
#endif
		processed += readlen;
		readlen = min (blocklen, count - processed);
		block_seek = 0;
		blockstart += blocklen;
	}

	return processed;
}

/*
 * Write count bytes at offset, but stay within ENVSETCORS (dev) sectors of
 * DEVOFFSET (dev). Similar to the read case above, on NOR we erase and write
 * the whole data at once.
 */
static int flash_write_buf (int dev, int fd, void *buf, size_t count,
			    off_t offset, uint8_t mtd_type)
{
	void *data;
	struct erase_info_user erase;
	size_t blocklen;	/* length of NAND block / NOR erase sector */
	size_t erase_len;	/* whole area that can be erased - may include
				   bad blocks */
	size_t erasesize;	/* erase / write length - one block on NAND,
				   whole area on NOR */
	size_t processed = 0;	/* progress counter */
	size_t write_total;	/* total size to actually write - excludinig
				   bad blocks */
	off_t erase_offset;	/* offset to the first erase block (aligned)
				   below offset */
	off_t block_seek;	/* offset inside the erase block to the start
				   of the data */
	off_t top_of_range;	/* end of the last block we may use */
	loff_t blockstart;	/* running start of the current block -
				   MEMGETBADBLOCK needs 64 bits */
	int rc;

	blocklen = DEVESIZE (dev);

	/* Erase sector size is always a power of 2 */
	top_of_range = (DEVOFFSET (dev) & ~(blocklen - 1)) +
		ENVSECTORS (dev) * blocklen;

	erase_offset = offset & ~(blocklen - 1);

	/* Maximum area we may use */
	erase_len = top_of_range - erase_offset;

	blockstart = erase_offset;
	/* Offset inside a block */
	block_seek = offset - erase_offset;

	/*
	 * Data size we actually have to write: from the start of the block
	 * to the start of the data, then count bytes of data, and to the
	 * end of the block
	 */
	write_total = (block_seek + count + blocklen - 1) & ~(blocklen - 1);

	/*
	 * Support data anywhere within erase sectors: read out the complete
	 * area to be erased, replace the environment image, write the whole
	 * block back again.
	 */
	if (write_total > count) {
		data = malloc (erase_len);
		if (!data) {
			fprintf (stderr,
				 "Cannot malloc %u bytes: %s\n",
				 erase_len, strerror (errno));
			return -1;
		}

		rc = flash_read_buf (dev, fd, data, write_total, erase_offset,
				     mtd_type);
		if (write_total != rc)
			return -1;

		/* Overwrite the old environment */
		memcpy (data + block_seek, buf, count);
	} else {
		/*
		 * We get here, iff offset is block-aligned and count is a
		 * multiple of blocklen - see write_total calculation above
		 */
		data = buf;
	}

	if (mtd_type == MTD_NANDFLASH) {
		/*
		 * NAND: calculate which blocks we are writing. We have
		 * to write one block at a time to skip bad blocks.
		 */
		erasesize = blocklen;
	} else {
		erasesize = erase_len;
	}

	erase.length = erasesize;

	/* This only runs once on NOR flash */
	while (processed < write_total) {
		rc = flash_bad_block (fd, mtd_type, &blockstart);
		if (rc < 0)		/* block test failed */
			return rc;

		if (blockstart + erasesize > top_of_range) {
			fprintf (stderr, "End of range reached, aborting\n");
			return -1;
		}

		if (rc) {		/* block is bad */
			blockstart += blocklen;
			continue;
		}

		erase.start = blockstart;
		ioctl (fd, MEMUNLOCK, &erase);

		if (ioctl (fd, MEMERASE, &erase) != 0) {
			fprintf (stderr, "MTD erase error on %s: %s\n",
				 DEVNAME (dev),
				 strerror (errno));
			return -1;
		}

		if (lseek (fd, blockstart, SEEK_SET) == -1) {
			fprintf (stderr,
				 "Seek error on %s: %s\n",
				 DEVNAME (dev), strerror (errno));
			return -1;
		}

#ifdef DEBUG
		printf ("Write 0x%x bytes at 0x%llx\n", erasesize, blockstart);
#endif
		if (write (fd, data + processed, erasesize) != erasesize) {
			fprintf (stderr, "Write error on %s: %s\n",
				 DEVNAME (dev), strerror (errno));
			return -1;
		}

		ioctl (fd, MEMLOCK, &erase);

		processed  += blocklen;
		block_seek = 0;
		blockstart += blocklen;
	}

	if (write_total > count)
		free (data);

	return processed;
}

/*
 * Set obsolete flag at offset - NOR flash only
 */
static int flash_flag_obsolete (int dev, int fd, off_t offset)
{
	int rc;

	/* This relies on the fact, that obsolete_flag == 0 */
	rc = lseek (fd, offset, SEEK_SET);
	if (rc < 0) {
		fprintf (stderr, "Cannot seek to set the flag on %s \n",
			 DEVNAME (dev));
		return rc;
	}
	rc = write (fd, &obsolete_flag, sizeof (obsolete_flag));
	if (rc < 0)
		perror ("Could not set obsolete flag");

	return rc;
}

static int flash_write (int fd_current, int fd_target, int dev_target)
{
	int rc;

	switch (environment.flag_scheme) {
	case FLAG_NONE:
		break;
	case FLAG_INCREMENTAL:
		(*environment.flags)++;
		break;
	case FLAG_BOOLEAN:
		*environment.flags = active_flag;
		break;
	default:
		fprintf (stderr, "Unimplemented flash scheme %u \n",
			 environment.flag_scheme);
		return -1;
	}

#ifdef DEBUG
	printf ("Writing new environment at 0x%lx on %s\n",
		DEVOFFSET (dev_target), DEVNAME (dev_target));
#endif
	rc = flash_write_buf (dev_target, fd_target, environment.image,
			      CONFIG_ENV_SIZE, DEVOFFSET (dev_target),
			      DEVTYPE(dev_target));
	if (rc < 0)
		return rc;

	if (environment.flag_scheme == FLAG_BOOLEAN) {
		/* Have to set obsolete flag */
		off_t offset = DEVOFFSET (dev_current) +
			offsetof (struct env_image_redundant, flags);
#ifdef DEBUG
		printf ("Setting obsolete flag in environment at 0x%lx on %s\n",
			DEVOFFSET (dev_current), DEVNAME (dev_current));
#endif
		flash_flag_obsolete (dev_current, fd_current, offset);
	}

	return 0;
}

static int flash_read (int fd)
{
	struct mtd_info_user mtdinfo;
	int rc;

	rc = ioctl (fd, MEMGETINFO, &mtdinfo);
	if (rc < 0) {
		perror ("Cannot get MTD information");
		return -1;
	}

	if (mtdinfo.type != MTD_NORFLASH && mtdinfo.type != MTD_NANDFLASH) {
		fprintf (stderr, "Unsupported flash type %u\n", mtdinfo.type);
		return -1;
	}

	DEVTYPE(dev_current) = mtdinfo.type;

	rc = flash_read_buf (dev_current, fd, environment.image, CONFIG_ENV_SIZE,
			     DEVOFFSET (dev_current), mtdinfo.type);

	return (rc != CONFIG_ENV_SIZE) ? -1 : 0;
}

static int flash_io (int mode)
{
	int fd_current, fd_target, rc, dev_target;

	/* dev_current: fd_current, erase_current */
	fd_current = open (DEVNAME (dev_current), mode);
	if (fd_current < 0) {
		fprintf (stderr,
			 "Can't open %s: %s\n",
			 DEVNAME (dev_current), strerror (errno));
		return -1;
	}

	if (mode == O_RDWR) {
		if (HaveRedundEnv) {
			/* switch to next partition for writing */
			dev_target = !dev_current;
			/* dev_target: fd_target, erase_target */
			fd_target = open (DEVNAME (dev_target), mode);
			if (fd_target < 0) {
				fprintf (stderr,
					 "Can't open %s: %s\n",
					 DEVNAME (dev_target),
					 strerror (errno));
				rc = -1;
				goto exit;
			}
		} else {
			dev_target = dev_current;
			fd_target = fd_current;
		}

		rc = flash_write (fd_current, fd_target, dev_target);

		if (HaveRedundEnv) {
			if (close (fd_target)) {
				fprintf (stderr,
					"I/O error on %s: %s\n",
					DEVNAME (dev_target),
					strerror (errno));
				rc = -1;
			}
		}
	} else {
		rc = flash_read (fd_current);
	}

exit:
	if (close (fd_current)) {
		fprintf (stderr,
			 "I/O error on %s: %s\n",
			 DEVNAME (dev_current), strerror (errno));
		return -1;
	}

	return rc;
}

/*
 * s1 is either a simple 'name', or a 'name=value' pair.
 * s2 is a 'name=value' pair.
 * If the names match, return the value of s2, else NULL.
 */

static char *envmatch (char * s1, char * s2)
{

	while (*s1 == *s2++)
		if (*s1++ == '=')
			return s2;
	if (*s1 == '\0' && *(s2 - 1) == '=')
		return s2;
	return NULL;
}

/*
 * Prevent confusion if running from erased flash memory
 */
static int env_init (void)
{
	int crc0, crc0_ok;
	char flag0;
	void *addr0;

	int crc1, crc1_ok;
	char flag1;
	void *addr1;

	struct env_image_single *single;
	struct env_image_redundant *redundant;

	if (!global_crc32_table) {
		global_crc32_table = crc32_filltable(NULL, 0);
	}

	if (parse_config ())		/* should fill envdevices */
		return -1;

	addr0 = calloc (1, CONFIG_ENV_SIZE);
	if (addr0 == NULL) {
		fprintf (stderr,
			"Not enough memory for environment (%ld bytes)\n",
			CONFIG_ENV_SIZE);
		return -1;
	}

	/* read environment from FLASH to local buffer */
	environment.image = addr0;

	if (HaveRedundEnv) {
		redundant = addr0;
		environment.crc		= &redundant->crc;
		environment.flags	= &redundant->flags;
		environment.data	= redundant->data;
	} else {
		single = addr0;
		environment.crc		= &single->crc;
		environment.flags	= NULL;
		environment.data	= single->data;
	}

	dev_current = 0;
	if (flash_io (O_RDONLY))
		return -1;

	crc0 = crc32 (0, (uint8_t *) environment.data, ENV_SIZE);
	crc0_ok = (crc0 == *environment.crc);
	if (!HaveRedundEnv) {
		if (!crc0_ok) {
			fprintf (stderr, "## Error: "
				"Bad environment CRC.  Environment unusable.\n");
			return ENV_CRC_BAD;
		}
	} else {
		flag0 = *environment.flags;

		dev_current = 1;
		addr1 = calloc (1, CONFIG_ENV_SIZE);
		if (addr1 == NULL) {
			fprintf (stderr,
				"Not enough memory for environment (%ld bytes)\n",
				CONFIG_ENV_SIZE);
			return -1;
		}
		redundant = addr1;

		/*
		 * have to set environment.image for flash_read(), careful -
		 * other pointers in environment still point inside addr0
		 */
		environment.image = addr1;
		if (flash_io (O_RDONLY))
			return -1;

		/* Check flag scheme compatibility */
		if (DEVTYPE(dev_current) == MTD_NORFLASH &&
		    DEVTYPE(!dev_current) == MTD_NORFLASH) {
			environment.flag_scheme = FLAG_BOOLEAN;
		} else if (DEVTYPE(dev_current) == MTD_NANDFLASH &&
			   DEVTYPE(!dev_current) == MTD_NANDFLASH) {
			environment.flag_scheme = FLAG_INCREMENTAL;
		} else {
			fprintf (stderr, "Incompatible flash types!\n");
			return -1;
		}

		crc1 = crc32 (0, (uint8_t *) redundant->data, ENV_SIZE);
		crc1_ok = (crc1 == redundant->crc);
		flag1 = redundant->flags;

		if (crc0_ok && !crc1_ok) {
			dev_current = 0;
		} else if (!crc0_ok && crc1_ok) {
			dev_current = 1;
		} else if (!crc0_ok && !crc1_ok) {
			fprintf (stderr, "## Error: "
				"Bad environment CRC.  Environment unusable.\n");
			return ENV_CRC_BAD;
		} else {
			switch (environment.flag_scheme) {
			case FLAG_BOOLEAN:
				if (flag0 == active_flag &&
				    flag1 == obsolete_flag) {
					dev_current = 0;
				} else if (flag0 == obsolete_flag &&
					   flag1 == active_flag) {
					dev_current = 1;
				} else if (flag0 == flag1) {
					dev_current = 0;
				} else if (flag0 == 0xFF) {
					dev_current = 0;
				} else if (flag1 == 0xFF) {
					dev_current = 1;
				} else {
					dev_current = 0;
				}
				break;
			case FLAG_INCREMENTAL:
				if ((flag0 == 255 && flag1 == 0) ||
				    flag1 > flag0)
					dev_current = 1;
				else if ((flag1 == 255 && flag0 == 0) ||
					 flag0 > flag1)
					dev_current = 0;
				else /* flags are equal - almost impossible */
					dev_current = 0;
				break;
			default:
				fprintf (stderr, "Unknown flag scheme %u \n",
					 environment.flag_scheme);
				return -1;
			}
		}

		/*
		 * If we are reading, we don't need the flag and the CRC any
		 * more, if we are writing, we will re-calculate CRC and update
		 * flags before writing out
		 */
		if (dev_current) {
			environment.image	= addr1;
			environment.crc		= &redundant->crc;
			environment.flags	= &redundant->flags;
			environment.data	= redundant->data;
			free (addr0);
		} else {
			environment.image	= addr0;
			/* Other pointers are already set */
			free (addr1);
		}
	}
	return 0;
}


static int parse_config ( void)
{
	struct stat st;

#if defined(CONFIG_FILE)
	/* Fills in DEVNAME(), ENVSIZE(), DEVESIZE(). Or don't. */
	if (get_config (CONFIG_FILE)) {
		fprintf (stderr,
			"Cannot parse config file: %s\n", strerror (errno));
		return -1;
	}
#else
	strcpy (DEVNAME (0), DEVICE1_NAME);
	DEVOFFSET (0) = DEVICE1_OFFSET;
	ENVSIZE (0) = ENV1_SIZE;
	DEVESIZE (0) = DEVICE1_ESIZE;
	ENVSECTORS (0) = DEVICE1_ENVSECTORS;
#ifdef HAVE_REDUND
	strcpy (DEVNAME (1), DEVICE2_NAME);
	DEVOFFSET (1) = DEVICE2_OFFSET;
	ENVSIZE (1) = ENV2_SIZE;
	DEVESIZE (1) = DEVICE2_ESIZE;
	ENVSECTORS (1) = DEVICE2_ENVSECTORS;
	HaveRedundEnv = 1;
#endif
#endif
	if (stat (DEVNAME (0), &st)) {
		fprintf (stderr,
			"Cannot access MTD device %s: %s\n",
			DEVNAME (0), strerror (errno));
		return -1;
	}

	if (HaveRedundEnv && stat (DEVNAME (1), &st)) {
		fprintf (stderr,
			"Cannot access MTD device %s: %s\n",
			DEVNAME (1), strerror (errno));
		return -1;
	}
	return 0;
}

#if defined(CONFIG_FILE)
static int get_config (const char *fname)
{
	FILE *fp;
	int i = 0;
	int rc;
	char dump[128];

	fp = fopen (fname, "r");
	if (fp == NULL)
		return -1;

	while (i < 2 && fgets (dump, sizeof (dump), fp)) {
		/* Skip incomplete conversions and comment strings */
		if (dump[0] == '#')
			continue;

		rc = sscanf (dump, "%s %lx %lx %lx %lx",
			     DEVNAME (i),
			     &DEVOFFSET (i),
			     &ENVSIZE (i),
			     &DEVESIZE (i),
			     &ENVSECTORS (i));

		if (rc < 4)
			continue;

		if (rc < 5)
			/* Default - 1 sector */
			ENVSECTORS (i) = 1;

		i++;
	}
	fclose (fp);

	HaveRedundEnv = i - 1;
	if (!i) {			/* No valid entries found */
		errno = EINVAL;
		return -1;
	} else
		return 0;
}
#endif
