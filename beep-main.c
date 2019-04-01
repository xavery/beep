/* beep - beep the pc speaker any number of ways
 * Copyright (C) 2000-2010 Johnathan Nightingale
 * Copyright (C) 2010-2013 Gerfried Fuchs
 * Copyright (C) 2013-2018 Hans Ulrich Niedermann
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

/*
 * For more documentation on beep, see the beep-usage.txt and
 * beep.1.in files.
 */


#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/kd.h>
#include <linux/input.h>

#include "beep-drivers.h"
#include "beep-drivers.h"
#include "beep-driver-console.h"
#include "beep-driver-evdev.h"
#include "beep-driver-noop.h"
#include "beep-library.h"
#include "beep-log.h"
#include "beep-usage.h"


static
const char version_message[] =
    PACKAGE_TARNAME " " PACKAGE_VERSION "\n"
    "Copyright (C) 2002-2016 Johnathan Nightingale\n"
    "Copyright (C) 2013-2018 Hans Ulrich Niedermann\n"
    "Use and Distribution subject to GPL.\n"
    "For information: http://www.gnu.org/copyleft/.\n";


typedef enum
    {
     END_DELAY_NO = 0,
     END_DELAY_YES = 1,
    } end_delay_E;

typedef enum
    {
     STDIN_BEEP_NONE = 0,
     STDIN_BEEP_LINE = 1,
     STDIN_BEEP_CHAR = 2,
    } stdin_beep_E;

typedef struct _beep_parms_T beep_parms_T;

/* Meaningful Defaults */
#define DEFAULT_FREQ       440   /* Middle A */
#define DEFAULT_LENGTH     200   /* milliseconds */
#define DEFAULT_REPS       1
#define DEFAULT_DELAY      100   /* milliseconds */
#define DEFAULT_END_DELAY  END_DELAY_NO
#define DEFAULT_STDIN_BEEP STDIN_BEEP_NONE

struct _beep_parms_T
{
    unsigned int freq; /* tone frequency (Hz)      */
    unsigned int length;     /* tone length    (ms)      */
    unsigned int reps;       /* # of repetitions         */
    unsigned int delay;      /* delay between reps  (ms) */
    end_delay_E  end_delay;  /* do we delay after last rep? */
    stdin_beep_E stdin_beep; /* are we using stdin triggers?  We have three options:
		     - just beep and terminate (default)
		     - beep after a line of input
		     - beep after a character of input
		     In the latter two cases, pass the text back out again,
		     so that beep can be tucked appropriately into a text-
		     processing pipe.
		  */
    beep_parms_T *next;  /* in case -n/--new is used. */
};

static void
handle_signal(int unused_signum __attribute__(( unused )))
{
}

/* print usage and leave exit code up to the caller */
static
void print_usage(void)
{
    fputs(beep_usage, stdout);
}


/* print usage and exit */
static
void usage_bail(void)
    __attribute__(( noreturn ));

static
void usage_bail(void)
{
    print_usage();
    exit(EXIT_FAILURE);
}


/* Global. Written by parse_command_line(), read by main() initialization. */
static char *param_device_name = NULL;


/* Parse the command line.  argv should be untampered, as passed to main.
 * Beep parameters returned in result, subsequent parameters in argv will over-
 * ride previous ones.
 *
 * Currently valid parameters:
 *  "-f <frequency in Hz>"
 *  "-l <tone length in ms>"
 *  "-r <repetitions>"
 *  "-d <delay in ms>"
 *  "-D <delay in ms>" (similar to -d, but delay after last repetition as well)
 *  "-s" (beep after each line of input from stdin, echo line to stdout)
 *  "-c" (beep after each char of input from stdin, echo char to stdout)
 *  "--verbose/--debug"
 *  "-h/--help"
 *  "-v/-V/--version"
 *  "-n/--new"
 *
 * March 29, 2002 - Daniel Eisenbud points out that ch should be int, not char,
 * for correctness on platforms with unsigned chars.
 */
static
void parse_command_line(const int argc, char *const argv[], beep_parms_T *result)
{
    int ch;

    static const
        struct option opt_list[] =
        { {"help",    no_argument,       NULL, 'h'},
          {"version", no_argument,       NULL, 'V'},
          {"new",     no_argument,       NULL, 'n'},
          {"verbose", no_argument,       NULL, 'X'},
          {"debug",   no_argument,       NULL, 'X'},
          {"device",  required_argument, NULL, 'e'},
          {NULL,      0,                 NULL,  0 }
        };

    while ((ch = getopt_long(argc, argv, "f:l:r:d:D:schvVne:", opt_list, NULL))
           != EOF) {
        /* handle parsed numbers for various arguments */
        int          argval_i = -1;
        unsigned int argval_u = ~0U;
        float        argval_f = -1.0f;

        switch (ch) {
        case 'f':  /* freq */
            if (sscanf(optarg, "%f", &argval_f) != 1) {
                usage_bail();
            }
            if ((0.0f > argval_f) || (argval_f > 20000.0f)) {
                usage_bail();
            }
            argval_i = (int) (argval_f + 0.5f);
            argval_u = (unsigned int) argval_i;
            if (result->freq != 0) {
                log_warning("multiple -f values given, only last one is used.");
            }
            result->freq = argval_u;
            break;
        case 'l' : /* length */
            if (sscanf(optarg, "%u", &argval_u) != 1) {
                usage_bail();
            }
            if (argval_u > 300000U) {
                usage_bail();
            }
            result->length = argval_u;
            break;
        case 'r' : /* repetitions */
            if (sscanf(optarg, "%u", &argval_u) != 1) {
                usage_bail();
            }
            if (argval_u > 300000U) {
                usage_bail();
            }
            result->reps = argval_u;
            break;
        case 'd' : /* delay between reps - WITHOUT delay after last beep*/
            if (sscanf(optarg, "%u", &argval_u) != 1) {
                usage_bail();
            }
            if (argval_u > 300000U) {
                usage_bail();
            }
            result->delay = argval_u;
            result->end_delay = END_DELAY_NO;
            break;
        case 'D' : /* delay between reps - WITH delay after last beep */
            if (sscanf(optarg, "%u", &argval_u) != 1) {
                usage_bail();
            }
            if (argval_u > 300000U) {
                usage_bail();
            }
            result->delay = argval_u;
            result->end_delay = END_DELAY_YES;
            break;
        case 's' :
            result->stdin_beep = STDIN_BEEP_LINE;
            break;
        case 'c' :
            result->stdin_beep = STDIN_BEEP_CHAR;
            break;
        case 'v' :
        case 'V' : /* also --version */
            fputs(version_message, stdout);
            exit(EXIT_SUCCESS);
            /* break; unreachable */
        case 'n' : /* also --new - create another beep */
            if (result->freq == 0) {
                result->freq = DEFAULT_FREQ;
            }
            result->next = (beep_parms_T *)malloc(sizeof(beep_parms_T));
            result->next->freq       = 0;
            result->next->length     = DEFAULT_LENGTH;
            result->next->reps       = DEFAULT_REPS;
            result->next->delay      = DEFAULT_DELAY;
            result->next->end_delay  = DEFAULT_END_DELAY;
            result->next->stdin_beep = DEFAULT_STDIN_BEEP;
            result->next->next       = NULL;
            result = result->next; /* yes, I meant to do that. */
            break;
        case 'X' : /* --debug / --verbose */
            if (log_level < 999) {
                /* just limit to some finite value */
                log_level++;
            }
            break;
        case 'e' : /* also --device */
            if (param_device_name) {
                log_error("You cannot give the --device parameter more than once.");
                exit(EXIT_FAILURE);
            }
            param_device_name = optarg;
            break;
        case 'h': /* also --help */
            print_usage();
            exit(EXIT_SUCCESS);
            /* break; unreachable */
        default:
            usage_bail();
            /* break; unreachable */
        }
    }
    if (optind < argc) {
        log_error("non-option arguments left on command line");
        usage_bail();
    }
    if (result->freq == 0) {
        result->freq = DEFAULT_FREQ;
    }
}


static
int sleep_ms(beep_driver *driver, unsigned int milliseconds)
{
    const time_t seconds = milliseconds / 1000U;
    const long   nanoseconds = (milliseconds % 1000UL) * 1000UL * 1000UL;
    const struct timespec request =
        { seconds,
          nanoseconds };
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
    const int retcode = nanosleep(&request, NULL);
    if (retcode == -1 && errno == EINTR) {
        beep_drivers_end_tone(driver);
        beep_drivers_fini(driver);
        exit(EXIT_FAILURE);
    }
    signal(SIGINT,  SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    return retcode;
}


static
void play_beep(beep_driver *driver, beep_parms_T parms)
{
    log_verbose("%d times %d ms beeps (%d ms delay between, "
                "%d ms delay after) @ %d Hz",
                parms.reps, parms.length, parms.delay, parms.end_delay,
                parms.freq);

    /* repeat the beep */
    for (unsigned int i = 0; i < parms.reps; i++) {
        beep_drivers_begin_tone(driver, parms.freq & 0xffff);
        sleep_ms(driver, parms.length);
        beep_drivers_end_tone(driver);
        if ((parms.end_delay == END_DELAY_YES) || ((i+1) < parms.reps)) {
            sleep_ms(driver, parms.delay);
        }
    }
}


/* If stdout is a TTY, print a bell character to stdout as a fallback. */
static
void fallback_beep(void)
{
    /* Printing '\a' can only beep if we print it to a tty */
    if (isatty(STDOUT_FILENO)) {
        fputc('\a', stdout);
    }
}


int main(const int argc, char *const argv[])
{
    log_init(argc, argv);

    /* Bail out if running setuid or setgid.
     *
     * It is near impossible to make beep setuid-safe:
     *
     *   * We open files for writing, and may even write to them.
     *
     *   * Checking the device file with realpath leaks information.
     *
     * So we refuse running setuid or setgid.
     */
    if ((getuid() != geteuid()) || (getgid() != getegid())) {
        log_error("Running setuid or setgid, "
                  "which is not supported for security reasons.");
        log_error("Set up permissions for the pcspkr evdev device file instead.");
        exit(EXIT_FAILURE);
    }

    /* Bail out if running under sudo.
     *
     * For the reasoning, see the setuid comment above.
     */
    if (getenv("SUDO_COMMAND") || getenv("SUDO_USER") || getenv("SUDO_UID") || getenv("SUDO_GID")) {
        log_error("Running under sudo, "
                  "which is not supported for security reasons.");
        log_error("Set up permissions for the pcspkr evdev device file instead.");
        exit(EXIT_FAILURE);
    }

    /* Parse command line */
    beep_parms_T *parms = (beep_parms_T *)malloc(sizeof(beep_parms_T));
    if (NULL == parms) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    parms->freq       = 0;
    parms->length     = DEFAULT_LENGTH;
    parms->reps       = DEFAULT_REPS;
    parms->delay      = DEFAULT_DELAY;
    parms->end_delay  = DEFAULT_END_DELAY;
    parms->stdin_beep = DEFAULT_STDIN_BEEP;
    parms->next       = NULL;

    parse_command_line(argc, argv, parms);

    /* Register drivers.  If we do that after parse_command_line, we may
     * have set the logging verbosity.  If we do that before
     * parse_command_line, parse_command_line might use some driver
     * functions.  Not sure yet which option we prefer.
     */
    /* beep_drivers_register(&noop_driver); */
    beep_drivers_register(&console_driver);
    beep_drivers_register(&evdev_driver);

    beep_driver *driver = NULL;

    if (param_device_name) {
        driver = beep_drivers_detect(param_device_name);
        if (!driver) {
            const int saved_errno = errno;
            log_error("Could not open %s for writing: %s",
                      param_device_name, strerror(saved_errno));
            exit(EXIT_FAILURE);
        }
    } else {
        driver = beep_drivers_detect(NULL);
        if (!driver) {
            log_error("Could not open any device");
            /* Output the only beep we can, in an effort to fall back on usefulness */
            fallback_beep();
            exit(EXIT_FAILURE);
        }
    }

    log_verbose("beep: using driver %p (name=%s, fd=%d, dev=%s)",
                (void *)driver, driver->name,
                driver->device_fd, driver->device_name);

    /* this outermost while loop handles the possibility that -n/--new
       has been used, i.e. that we have multiple beeps specified. Each
       iteration will play, then free() one parms instance. */
    while (parms) {
        beep_parms_T *next = parms->next;

        if (parms->stdin_beep != STDIN_BEEP_NONE) {
            /* In this case, beep is probably part of a pipe, in which
               case POSIX says stdin and out should be fuly buffered.
               This however means very laggy performance with beep
               just twiddling it's thumbs until a buffer fills. Thus,
               kill the buffering.  In some situations, this too won't
               be enough, namely if we're in the middle of a long
               pipe, and the processes feeding us stdin are buffered,
               we'll have to wait for them, not much to be done about
               that. */
            setvbuf(stdin, NULL, _IONBF, 0);
            setvbuf(stdout, NULL, _IONBF, 0);

            char sin[4096];
            while (fgets(sin, 4096, stdin)) {
                if (parms->stdin_beep == STDIN_BEEP_CHAR) {
                    for (char *ptr=sin; *ptr; ptr++) {
                        putchar(*ptr);
                        fflush(stdout);
                        play_beep(driver, *parms);
                    }
                } else { /* STDIN_BEEP_LINE */
                    fputs(sin, stdout);
                    play_beep(driver, *parms);
                }
            }
        } else {
            play_beep(driver, *parms);
        }

        /* Junk each parms struct after playing it */
        free(parms);
        parms = next;
    }

    beep_drivers_end_tone(driver);
    beep_drivers_fini(driver);

    return EXIT_SUCCESS;
}


/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
