// chiaharvestgraph.c
// 
// (c)2021 by Abraham Stolk.
// XCH Donations: xch1zfgqfqfdse3e2x2z9lscm6dx9cvd5j2jjc7pdemxjqp0xp05xzps602592

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <math.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <time.h>
#include <limits.h>
#include <errno.h>
#include <error.h>
#include <string.h>
#include <termios.h>

#include "grapher.h"
#include "colourmaps.h"


#define MAXLINESZ		1024

#define WAIT_BETWEEN_SELECT_US	500000L

#define	MAXHIST			( 8 * 24 * 7 )	// A week's worth of octet-hours.
#define MAXENTR			( 120 )		// We expect 32 per 600s, worst-case: 64 per 600s, 48 per octet-hr

typedef struct octethr
{
	time_t	stamps[ MAXENTR ];
	int	height[ MAXENTR ];
	int	sz;
	time_t	timelo;
	time_t	timehi;
} octethr_t;


octethr_t octets[ MAXHIST ];

static int entries_added=0;	// How many log entries have we added in total?

static time_t newest_stamp=0;	// The stamp of the latest entry.

static time_t refresh_stamp=0;	// When did we update the image, last?

static struct termios orig_termios;

static const rgb_t* ramp=0;

static time_t oldeststamp;


static void init_octets( time_t now )
{
	time_t q = now / 450;
	time_t q_lo = (q+0) * 450;
	time_t q_hi = (q+1) * 450;
	for ( int i=MAXHIST-1; i>=0; --i )	// [0..MAXHIST)
	{
		const int ir = MAXHIST-1-i;	// [MAXHIST-1..0]
		octets[i].sz = 0;
		octets[i].timelo = q_lo - 450 * ir;
		octets[i].timehi = q_hi - 450 * ir;
	}
}


static void shift_octets( void )
{
	fprintf( stderr, "Shifting octets...\n" );
	const int last = MAXHIST-1;
	for ( int i=0; i<last; ++i )
		octets[i] = octets[i+1];
	memset( octets+last, 0, sizeof(octethr_t) );
	octets[ last ].timelo = octets[ last-1 ].timelo + 450;
	octets[ last ].timehi = octets[ last-1 ].timehi + 450;
}


static int too_old( time_t t )
{
	return t <= octets[ 0 ].timelo;
}


static int too_new( time_t t )
{
	const int last = MAXHIST-1;
	return t >= octets[last].timehi;
}


static int octetslot( time_t tim )
{
	const int last = MAXHIST-1;
	const time_t d = tim - octets[last].timehi;
	if ( d >= 0 )
		return INT_MAX;
	const int slot = (int) ( MAXHIST - 1 + ( d / 450 ) );
	if ( slot < 0 )
	{
		fprintf
		(
			stderr,
			"ERROR - UNEXPECTED TIME VALUE.\n"
			"tim=%zd lasttimehi=%zd d=%zd slot=%d\n"
			"REPORT THIS MESSAGE TO %s\n",
			tim, octets[last].timehi, d, slot,
			"https://github.com/stolk/chiaharvestgraph/issues/12"
		);
	}
	return slot;
}


static int add_entry( time_t t, int height )
{
	static int virgin=1;
	while ( too_new( t ) )
		shift_octets();
	if ( too_old( t ) )
		return 0;	// signal not adding.
	int s = octetslot( t );
	if ( s < 0 || s >= MAXHIST )
		return -1;	// signal failure.
	const int i = octets[s].sz;
	assert( i < MAXENTR );
	octets[s].stamps[i] = t;
	octets[s].height[i] = height;
	octets[s].sz += 1;

	if ( virgin )
	{
		oldeststamp = t;
		virgin = 0;
	}
	oldeststamp = t < oldeststamp ? t : oldeststamp;
	return 1;
}


static FILE* f_log = 0;


static FILE* open_log_file(const char* dirname, const char* logname)
{
	if ( f_log )
		fclose( f_log );
	if ( !logname )
		logname = "debug.log";

	char fname[PATH_MAX+1];
	snprintf( fname, sizeof(fname), "%s/%s", dirname, logname );
	f_log = fopen( fname, "rb" );
	if ( !f_log )
	{
		fprintf( stderr, "Failed to open log file '%s'\n", fname );
		return 0;
	}

	return f_log;
}

// Parses log entries that look like this:
// 🌱 Updated peak to height 305755, weight 26951920, hh 0bec5b24604b76d6665c4e2daae1772bc37186d91e9146cf75f1ce2f747996f7, forked at 305754, rh: f55d8d2daa85b4d6399e79101d9bfefa2d8a8921fef1cd705fb9b583d3bbfa26, total iters: 989354550375, overflow: False, deficit: 11, difficulty: 484, sub slot iters: 114819072, Generator size: 1667, Generator ref list size: 1

static void analyze_line(const char* line, ssize_t length)
{
	if ( length > 60 )
	{
		const char* s = strstr( line+24, "Updated peak to height " );
		if ( s )
		{
			s += 23;
			int year=-1;
			int month=-1;	
			int day=-1;
			int hours=-1;
			int minut=-1;
			float secon=-1;
			const int num = sscanf
			(
				line,
				"%04d-%02d-%02dT%02d:%02d:%f ",
				&year,
				&month,
				&day,
				&hours,
				&minut,
				&secon
			);
			if ( num == 6 )
			{
				struct tm tim =
				{
					(int)secon,	// seconds 0..60
					minut,		// minutes 0..59
					hours,		// hours 0..23
					day,		// day 1..31
					month-1,	// month 0..11
					year-1900,	// year - 1900
					-1,
					-1,
					-1
				};
				const time_t logtim = mktime( &tim );
				assert( logtim != (time_t) -1 );
				const int height = atoi( s );

				if ( logtim > newest_stamp )
				{
					const int added = add_entry( logtim, height );
					if ( added < 0)
					{
						fprintf( stderr, "OFFENDING LOG LINE: %s\n", line );
						exit(3); // Stop right there, so the user can see the message.
					}
					if ( added > 0)
					{
						newest_stamp = logtim;
						entries_added += added;
					}
				}
				else
				{
					// Got a new height, in the same second as the previous one?
					//fprintf(stderr, "Spurious entry: %s", line);
				}
			}
		}
	}
}


static int read_log_file(void)
{
	assert( f_log );
	static char* line = 0;
	static size_t linesz=MAXLINESZ;
	if ( !line )
		line = (char*)malloc(MAXLINESZ);

	int linesread = 0;

	do
	{
		struct timeval tv = { 0L, WAIT_BETWEEN_SELECT_US };
		fd_set rdset;
		FD_ZERO(&rdset);
		int log_fds = fileno( f_log );
		FD_SET( log_fds, &rdset );
		const int ready = select( log_fds+1, &rdset, NULL, NULL, &tv);

		if ( ready < 0 )
			error( EXIT_FAILURE, errno, "select() failed" );

		if ( ready == 0 )
		{
			//fprintf( stderr, "No descriptors ready for reading.\n" );
			return linesread;
		}

		const ssize_t ll = getline( &line, &linesz, f_log );
		if ( ll <= 0 )
		{
			//fprintf( stderr, "getline() returned %zd\n", ll );
			clearerr( f_log );
			return linesread;
		}

		linesread++;
		analyze_line( line, ll );
	} while(1);
}


static void draw_column( int nr, uint32_t* img, int h, time_t now )
{
	const int q = MAXHIST-1-nr;
	if ( q<0 )
		return;
	const time_t qlo = octets[q].timelo;
	const int sz = octets[q].sz;
	const int band = ( ( qlo / 450 / 8 ) & 1 );

	const float expected_per_second = 32 / 600.0f;
	const float expected_per_octet  = 450 * expected_per_second;

	int h0=-1;
	int h1=-1;
	int count=0;
	for ( int i=0; i<sz; ++i )
	{
		//const time_t t = octets[q].stamps[i];
		const int hx = octets[q].height[i];
		if (!i)
			h0 = h1 = hx;
		else
		{
			h0 = hx < h0 ? hx : h0;
			h1 = hx > h1 ? hx : h1;
		}
		count++;
	}
	if ( q>0 )
	{
		int j = octets[q-1].sz-1;
		if ( j > 0 )
			h0 = octets[q-1].height[ j ];
	}
	const int deltah = h1-h0;
	const int nom = (int) ( roundf(expected_per_octet * h / 50 ) ); 
	const int lvl = (int) ( roundf(deltah * h / 50) );

	for ( int y=0; y<h; ++y )
	{
		uint32_t red = 0x36;
		uint32_t grn = 0x36;
		uint32_t blu = 0x36;
		const int yinv = (h-1-y);

		if ( y == nom || y == nom-1 )
		{
			red = 0xa0;
			grn = 0xa0;
		}

		if ( y == lvl && nr == 0 )
		{
			red = grn = blu = 0xcc;
		}
		else if ( nr == 0 )
		{
		}
		else if ( y < nom )
		{
			if ( (y > lvl) && count )
			{
				red = 0xc0;
				grn = blu = 0x36;
			}
		}
		else if ( y >= nom )
		{
			if ( y < lvl && count )
			{
				blu = 0xc0;
				grn = 0x80;
				red = 0x36;
			}
		}
		else if ( y == nom || y == nom-1 )
		{
			red = 0xa0;
			grn = 0xa0;
		}

		if ( band )
		{
			red = red * 200 / 255;
			grn = grn * 200 / 255;
			blu = blu * 200 / 255;
		}
		const uint32_t c = (0xff<<24) | (blu<<16) | (grn<<8) | (red<<0);
		img[ yinv*imw ] = c;
	}
}


static void setup_postscript(void)
{
	uint8_t c0[3] = {0xc0,0x36,0x36};
	uint8_t c1[3] = {0x36,0x80,0xc0};
	uint8_t c2[3] = {0xc0,0xc0,0x36};
	const char* l0 = "BLOCK DEFICIT  ";
	const char* l1 = "BLOCK EXCESS  ";
	const char* l2 = "NOMINAL ";

	snprintf
	(
		postscript,
		sizeof(postscript),

		SETFG "%d;%d;%dm" SETBG "%d;%d;%dm%s"
		SETFG "%d;%d;%dm" SETBG "%d;%d;%dm%s"
		SETFG "%d;%d;%dm" SETBG "%d;%d;%dm%s"
		SETFG "255;255;255m",

		c0[0],c0[1],c0[2], 0,0,0, l0,
		c1[0],c1[1],c1[2], 0,0,0, l1,
		c2[0],c2[1],c2[2], 0,0,0, l2
	);
}


static void setup_scale(void)
{
	strncpy( overlay + imw - 4, "NOW", 4 );

	int x = imw - 8;
	int h = 0;
	while( x >= imw/2 )
	{
		char lab[8] = {0,0,0,0, 0,0,0,0};

		if ( h<12 )
			snprintf( overlay+x, sizeof(lab), "%2dh",  h+1);
		else if ( h%24==0 )
			snprintf( overlay+x, sizeof(lab), "%dDAY", h/24);

		x -= 4;
		h += 1;
	}
}


static void place_stats_into_overlay(void)
{
#if 0
	double avg = total_response_time_eligible / total_eligible_responses;
	int avgms   = (int)round(avg * 1000);
	int worstms = (int)round(worst_response_time_eligible * 1000);

	const char* q_av = 0;
	if ( avgms < 80 )
		q_av = "fast";
	else if ( avgms < 300 )
		q_av = "ok";
	else
		q_av = "slow";

	const char* q_wo = 0;
	if ( worstms < 1000 )
		q_wo = "fast";
	else if ( worstms < 2000 )
		q_wo = "ok";
	else if ( worstms < 5000 )
		q_wo = "slow";
	else
		q_wo = "too-slow";

	snprintf
	(
		overlay+0,
		imw,
		"PLOTS:%d  AVG-CHECK:%dms[%s]  SLOWEST-CHECK:%dms[%s]   ",
		plotcount,
		avgms, q_av,
		worstms, q_wo
	);
#endif
}


static int update_image(void)
{
	int redraw=0;
	const char* skipdraw = getenv("SKIPDRAW");

	if ( grapher_resized )
	{
		grapher_adapt_to_new_size();
		setup_scale();
		redraw=1;
	}

	// Compose the image.
	if ( newest_stamp > refresh_stamp )
		redraw=1;

	if (redraw)
	{
		time_t now = time(0);
		for ( int col=0; col<imw-2; ++col )
		{
			draw_column( col, im + (2*imw) + (imw-2-col), imh-2, now );
		}
		place_stats_into_overlay();
		if ( !skipdraw )
			grapher_update();
		refresh_stamp = newest_stamp;
	}
	return 0;
}


static void disableRawMode()
{
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}


static void enableRawMode()
{
	tcgetattr(STDIN_FILENO, &orig_termios);
	atexit(disableRawMode);
	struct termios raw = orig_termios;
	raw.c_lflag &= ~(ECHO);				// Don't echo key presses.
	raw.c_lflag &= ~(ICANON);			// Read by char, not by line.
	raw.c_cc[VMIN] = 0;				// No minimum nr of chars.
	raw.c_cc[VTIME] = 0;				// No waiting time.
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}


int main(int argc, char *argv[])
{
	const char* dirname = 0;

	if (argc != 2)
	{
		fprintf( stderr, "Usage: %s ~/.chia/mainnet/log\n", argv[0] );
		exit( 1 );
	}
	else
	{
		dirname = argv[ 1 ];
	}
	fprintf( stderr, "Monitoring directory %s\n", dirname );

	const int viridis = ( getenv( "CMAP_VIRIDIS" ) != 0 );
	const int magma   = ( getenv( "CMAP_MAGMA"   ) != 0 );
	const int plasma  = ( getenv( "CMAP_PLASMA"  ) != 0 );
	const int heat    = ( getenv( "CMAP_HEAT"    ) != 0 );

	ramp = cmap_viridis;
	if ( viridis ) ramp = cmap_viridis;
	if ( magma   ) ramp = cmap_magma;
	if ( plasma  ) ramp = cmap_plasma;
	if ( heat    ) ramp = cmap_heat;

	init_octets( time(0) );

	setup_postscript();

	int numdebuglogs=8;
	const char* str = getenv("NUM_DEBUG_LOGS");
	if ( str )
	{
		numdebuglogs=atoi(str);
		assert(numdebuglogs>0);
	}
	for ( int i=numdebuglogs-1; i>=0; --i )
	{
		char logfilename[80];
		if ( i )
			snprintf( logfilename, sizeof(logfilename), "debug.log.%d", i );
		else
			snprintf( logfilename, sizeof(logfilename), "debug.log" );
		if ( open_log_file( dirname, logfilename ) )
		{
			// Log file exists, we should read what is in it, currently.
			const int numl = read_log_file();
			fprintf( stderr, "read %d lines from log %s\n", numl, logfilename );
		}
	}

	int fd;
	if ( (fd = inotify_init()) < 0 )
		error( EXIT_FAILURE, errno, "failed to initialize inotify instance" );

	int wd;
	if ( (wd = inotify_add_watch ( fd, dirname, IN_MODIFY | IN_CREATE | IN_DELETE ) ) < 0 )
		error( EXIT_FAILURE, errno, "failed to add inotify watch for '%s'", dirname );


	int result = grapher_init(1);
	if ( result < 0 )
	{
		fprintf( stderr, "Failed to intialize grapher(), maybe we are not running in a terminal?\n" );
		exit(2);
	}

	enableRawMode();
	update_image();

	// Read notifications.
	char buf[ sizeof(struct inotify_event) + PATH_MAX ];
	int done=0;

	do
	{
		int len = read( fd, buf, sizeof(buf) );
		if ( len <= 0 && errno != EINTR )
		{
			error( EXIT_FAILURE, len == 0 ? 0 : errno, "failed to read inotify event" );
		}
		int i=0;
		while (i < len)
		{
			struct inotify_event *ie = (struct inotify_event*) &buf[i];
			if ( ie->mask & IN_CREATE )
			{
				// A file got created. It could be our new log file!
				if ( !strcmp( ie->name, "debug.log" ) )
				{
					fprintf( stderr, "Reopening logfile.\n" );
					open_log_file( dirname, 0 );
					const int numl = read_log_file();
					fprintf( stderr, "read %d lines from log.\n", numl );
				}
			}
			else if ( ie->mask & IN_MODIFY )
			{
				if ( !strcmp( ie->name, "debug.log" ) )
				{
					//fprintf( stderr, "Modified.\n" );
					const int numl = read_log_file();
					(void) numl;
					//fprintf( stderr, "read %d lines from log.\n", numl );
				}
			}
			else if (ie->mask & IN_DELETE)
			{
				printf("%s was deleted\n",  ie->name);
			}

			i += sizeof(struct inotify_event) + ie->len;
		}

		update_image();

		char c=0;
		const int numr = read( STDIN_FILENO, &c, 1 );
		if ( numr == 1 && ( c == 27 || c == 'q' || c == 'Q' ) )
			done=1;
	} while (!done);

	grapher_exit();
	exit(0);
}

