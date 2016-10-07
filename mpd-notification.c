/*
 * (C) 2011-2016 by Christian Hesse <mail@eworm.de>
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 */

#include "mpd-notification.h"

const static char optstring[] = "hH:m:op:s:t:vV";
const static struct option options_long[] = {
	/* name		has_arg			flag	val */
	{ "help",	no_argument,		NULL,	'h' },
	{ "host",	required_argument,	NULL,	'H' },
	{ "music-dir",	required_argument,	NULL,	'm' },
	{ "oneline",	no_argument,		NULL,	'o' },
	{ "port",	required_argument,	NULL,	'p' },
	{ "scale",	required_argument,	NULL,	's' },
	{ "timeout",	required_argument,	NULL,	't' },
	{ "verbose",	no_argument,		NULL,	'v' },
	{ "version",	no_argument,		NULL,	'V' },
	{ "notification-file-workaround",
			no_argument,		NULL,	OPT_FILE_WORKAROUND },
	{ 0, 0, 0, 0 }
};

/* global variables */
char *program = NULL;
NotifyNotification * notification = NULL;
struct mpd_connection * conn = NULL;
uint8_t doexit = 0;
uint8_t verbose = 0;
uint8_t oneline = 0;

/*** received_signal ***/
void received_signal(int signal) {
	GError * error = NULL;

	switch (signal) {
		case SIGINT:
		case SIGTERM:
			if (verbose > 0)
				printf("%s: Received signal %s, preparing exit.\n", program, strsignal(signal));

			doexit++;
			mpd_send_noidle(conn);
			break;

		case SIGHUP:
		case SIGUSR1:
			if (verbose > 0)
				printf("%s: Received signal %s, showing last notification again.\n", program, strsignal(signal));

			if (notify_notification_show(notification, &error) == FALSE) {
				g_printerr("%s: Error \"%s\" while trying to show notification again.\n", program, error->message);
				g_error_free(error);
			}
			break;
		default:
			fprintf(stderr, "%s: Reveived signal %s (%d), no idea what to do...\n", program, strsignal(signal), signal);
	}
}

/*** retrieve_artwork ***/
GdkPixbuf * retrieve_artwork(const char * music_dir, const char * uri) {
	GdkPixbuf * pixbuf = NULL;
	char * uri_path = NULL, * imagefile = NULL;
	DIR * dir;
	struct dirent * entry;
	regex_t regex;

#ifdef HAVE_LIBAV
	int i;
	magic_t magic = NULL;
	const char *magic_mime;
	AVFormatContext * pFormatCtx = NULL;
	GdkPixbufLoader * loader;

	/* try album artwork first */
	uri_path = malloc(strlen(music_dir) + strlen(uri) + 2);
	sprintf(uri_path, "%s/%s", music_dir, uri);

	if ((magic = magic_open(MAGIC_MIME_TYPE)) == NULL) {
		fprintf(stderr, "%s: unable to initialize magic library\n", program);
		goto image;
	}

	if (magic_load(magic, NULL) != 0) {
		fprintf(stderr, "%s: cannot load magic database: %s\n", program, magic_error(magic));
		magic_close(magic);
		goto image;
	}

	if ((magic_mime = magic_file(magic, uri_path)) == NULL) {
		fprintf(stderr, "%s: We did not get a MIME type...\n", program);
		goto image;
	}

	if (verbose > 0)
		printf("%s: MIME type for %s is: %s\n", program, uri_path, magic_mime);

	if (strcmp(magic_mime, "audio/mpeg") != 0)
		goto image;

	pFormatCtx = avformat_alloc_context();

	if (avformat_open_input(&pFormatCtx, uri_path, NULL, NULL) != 0) {
		fprintf(stderr, "%s: avformat_open_input() failed", program);
		goto image;
	}

	if (pFormatCtx->iformat->read_header(pFormatCtx) < 0) {
		fprintf(stderr, "%s: could not read the format header\n", program);
		goto image;
	}

	/* find the first attached picture, if available */
	for (i = 0; i < pFormatCtx->nb_streams; i++) {
		if (pFormatCtx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
			AVPacket pkt;

			if (verbose > 0)
				printf("%s: Found artwork in media file.\n", program);

			pkt = pFormatCtx->streams[i]->attached_pic;

			loader = gdk_pixbuf_loader_new();
			gdk_pixbuf_loader_write(loader, pkt.data, pkt.size, NULL);
			pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);

			gdk_pixbuf_loader_close(loader, NULL);
			goto done;
		}
	}

image:
#endif

	/* cut the file name from path for current directory */
	*strrchr(uri_path, '/') = 0;

	if ((dir = opendir(uri_path)) == NULL) {
		fprintf(stderr, "%s: Can not read directory '%s': ", program, uri_path);
		goto fail;
	}

	if (regcomp(&regex, REGEX_ARTWORK, REG_NOSUB + REG_ICASE) != 0) {
		fprintf(stderr, "%s: Could not compile regex\n", program);
		goto fail;
	}

	while ((entry = readdir(dir))) {
		if (*entry->d_name == '.')
			continue;

		if (regexec(&regex, entry->d_name, 0, NULL, 0) == 0) {
			if (verbose > 0)
				printf("%s: Found image file: %s\n", program, entry->d_name);

			imagefile = malloc(strlen(uri_path) + strlen(entry->d_name) + 2);
			sprintf(imagefile, "%s/%s", uri_path, entry->d_name);
			pixbuf = gdk_pixbuf_new_from_file(imagefile, NULL);
			free(imagefile);
			break;
		}
	}

	regfree(&regex);
	closedir(dir);

done:
fail:
	if (pFormatCtx != NULL) {
		avformat_close_input(&pFormatCtx);
		avformat_free_context(pFormatCtx);
	}

#ifdef HAVE_LIBAV
	if (magic != NULL)
		magic_close(magic);
#endif

	free(uri_path);

	return pixbuf;
}

/*** append_string ***/
char * append_string(char * string, const char * format, const char delim, const char * s) {
	char * tmp, * offset;

	tmp = g_markup_escape_text(s, -1);

	string = realloc(string, strlen(string) + strlen(format) + strlen(tmp) + 2 /* delim + line break */);

	offset = string + strlen(string);

	if (delim > 0) {
		*offset = delim;
		offset++;
	}

	sprintf(offset, format, tmp);

	free(tmp);

	return string;
}

/*** main ***/
int main(int argc, char ** argv) {
	dictionary * ini = NULL;
	const char * title = NULL, * artist = NULL, * album = NULL;
	char * notifystr = NULL;
	GdkPixbuf * pixbuf = NULL;
	GError * error = NULL;
	unsigned short int errcount = 0, state = MPD_STATE_UNKNOWN;
	const char * mpd_host, * mpd_port_str, * music_dir, * uri = NULL;
	unsigned mpd_port = MPD_PORT, mpd_timeout = MPD_TIMEOUT, notification_timeout = NOTIFICATION_TIMEOUT;
	struct mpd_song * song = NULL;
	unsigned int i, version = 0, help = 0, scale = 0, file_workaround = 0;

	program = argv[0];

	if ((mpd_host = getenv("MPD_HOST")) == NULL)
		mpd_host = MPD_HOST;

	if ((mpd_port_str = getenv("MPD_PORT")) == NULL)
		mpd_port = MPD_PORT;
	else
		mpd_port = atoi(mpd_port_str);

	music_dir = getenv("XDG_MUSIC_DIR");

	/* parse config file */
	if (chdir(getenv("HOME")) == 0 && access(".config/mpd-notification.conf", R_OK) == 0 &&
			(ini = iniparser_load(".config/mpd-notification.conf")) != NULL) {
		file_workaround = iniparser_getboolean(ini, ":notification-file-workaround", file_workaround);
		mpd_host = iniparser_getstring(ini, ":host", mpd_host);
		mpd_port = iniparser_getint(ini, ":port", mpd_port);
		notification_timeout = iniparser_getint(ini, ":timeout", notification_timeout);
		music_dir = iniparser_getstring(ini, ":music-dir", music_dir);
		scale = iniparser_getint(ini, ":scale", scale);
	}

	/* get the verbose status */
	while ((i = getopt_long(argc, argv, optstring, options_long, NULL)) != -1) {
		switch (i) {
			case 'h':
				help++;
				break;
			case 'o':
				oneline++;
				break;
			case 'v':
				verbose++;
				break;
			case 'V':
				verbose++;
				version++;
				break;
		}
	}

	/* reinitialize getopt() by resetting optind to 0 */
	optind = 0;

	/* say hello */
	if (verbose > 0)
		printf("%s: %s v%s (compiled: " __DATE__ ", " __TIME__ ")\n", program, PROGNAME, VERSION);

	if (help > 0)
		fprintf(stderr, "usage: %s [-h] [-H HOST] [-p PORT] [-m MUSIC-DIR] [-s PIXELS] [-t TIMEOUT] [-v] [-V]\n", program);

	if (version > 0 || help > 0)
		return EXIT_SUCCESS;

	/* get command line options */
	while ((i = getopt_long(argc, argv, optstring, options_long, NULL)) != -1) {
		switch (i) {
			case 'p':
				mpd_port = atoi(optarg);
				if (verbose > 0)
					printf("%s: using port %d\n", program, mpd_port);
				break;
			case 'm':
				music_dir = optarg;
				if (verbose > 0)
					printf("%s: using music-dir %s\n", program, music_dir);
				break;
			case 'H':
				mpd_host = optarg;
				if (verbose > 0)
					printf("%s: using host %s\n", program, mpd_host);
				break;
			case 's':
				scale = atof(optarg);
				break;
			case 't':
				notification_timeout = atof(optarg) * 1000;
				if (verbose > 0)
					printf("%s: using notification-timeout %d\n", program, notification_timeout);
				break;
			case OPT_FILE_WORKAROUND:
				file_workaround++;
				break;
		}
	}

	/* disable artwork stuff if we are connected to a foreign host */
	if (mpd_host != NULL && mpd_host[0] != '/')
		music_dir = NULL;

	/* change directory to music base directory */
	if (music_dir != NULL) {
		if (chdir(music_dir) == -1) {
			fprintf(stderr, "%s: Can not change directory to '%s'.\n", program, music_dir);
			music_dir = NULL;
		}
	}

#ifdef HAVE_LIBAV
	/* libav */
	av_register_all();

	/* only fatal messages from libav */
	if (verbose == 0)
		av_log_set_level(AV_LOG_FATAL);
#endif

	conn = mpd_connection_new(mpd_host, mpd_port, mpd_timeout);

	if (mpd_connection_get_error(conn) != MPD_ERROR_SUCCESS) {
		fprintf(stderr,"%s: %s\n", program, mpd_connection_get_error_message(conn));
		mpd_connection_free(conn);
		exit(EXIT_FAILURE);
	}

	if(notify_init(PROGNAME) == FALSE) {
		fprintf(stderr, "%s: Can't create notify.\n", program);
		exit(EXIT_FAILURE);
	}

	notification =
#		if NOTIFY_CHECK_VERSION(0, 7, 0)
		notify_notification_new(TEXT_TOPIC, TEXT_NONE, ICON_AUDIO_X_GENERIC);
#		else
		notify_notification_new(TEXT_TOPIC, TEXT_NONE, ICON_AUDIO_X_GENERIC, NULL);
#		endif
	notify_notification_set_category(notification, PROGNAME);
	notify_notification_set_urgency (notification, NOTIFY_URGENCY_NORMAL);

	signal(SIGHUP, received_signal);
	signal(SIGINT, received_signal);
	signal(SIGTERM, received_signal);
	signal(SIGUSR1, received_signal);

	while(doexit == 0 && mpd_run_idle_mask(conn, MPD_IDLE_PLAYER)) {
		mpd_command_list_begin(conn, true);
		mpd_send_status(conn);
		mpd_send_current_song(conn);
		mpd_command_list_end(conn);

		state = mpd_status_get_state(mpd_recv_status(conn));
		if (state == MPD_STATE_PLAY) {
			mpd_response_next(conn);

			song = mpd_recv_song(conn);

			title = mpd_song_get_tag(song, MPD_TAG_TITLE, 0);

			/* ignore if we have no title */
			if (title == NULL)
				goto nonotification;

			/* initial allocation and string termination */
			notifystr = strdup("");

			notifystr = append_string(notifystr, TEXT_PLAY_TITLE, 0, title);

			if ((artist = mpd_song_get_tag(song, MPD_TAG_ARTIST, 0)) != NULL)
				notifystr = append_string(notifystr, TEXT_PLAY_ARTIST, oneline ? ' ' : '\n', artist);

			if ((album = mpd_song_get_tag(song, MPD_TAG_ALBUM, 0)) != NULL)
				notifystr = append_string(notifystr, TEXT_PLAY_ALBUM, oneline ? ' ' : '\n', album);

			uri = mpd_song_get_uri(song);

			if (music_dir != NULL && uri != NULL) {
				GdkPixbuf * copy;

				pixbuf = retrieve_artwork(music_dir, uri);

				if (scale > 0) {
					int x, y;

					x = gdk_pixbuf_get_width(pixbuf);
					y = gdk_pixbuf_get_height(pixbuf);

					if ((copy = gdk_pixbuf_scale_simple (pixbuf,
							(x > y ? scale : scale * x / y),
							(y > x ? scale : scale * y / x),
							GDK_INTERP_BILINEAR)) != NULL) {
						g_object_unref(pixbuf);
						pixbuf = copy;
					}
				}


			}

			mpd_song_free(song);
		} else if (state == MPD_STATE_PAUSE)
			notifystr = strdup(TEXT_PAUSE);
		else if (state == MPD_STATE_STOP)
			notifystr = strdup(TEXT_STOP);
		else
			notifystr = strdup(TEXT_UNKNOWN);

		if (verbose > 0)
			printf("%s: %s\n", program, notifystr);

		/* Some notification daemons do not support handing pixbuf data. Write a PNG
		 * file and give the path. */
		if (file_workaround > 0 && pixbuf != NULL) {
			gdk_pixbuf_save(pixbuf, "/tmp/.mpd-notification-artwork.png", "png", NULL, NULL);

			notify_notification_update(notification, TEXT_TOPIC, notifystr, "/tmp/.mpd-notification-artwork.png");
		} else
			notify_notification_update(notification, TEXT_TOPIC, notifystr, ICON_AUDIO_X_GENERIC);

		/* Call this unconditionally! When pixbuf is NULL this clears old image. */
		notify_notification_set_image_from_pixbuf(notification, pixbuf);

		notify_notification_set_timeout(notification, notification_timeout);

		while(notify_notification_show(notification, &error) == FALSE) {
			if (errcount > 1) {
				fprintf(stderr, "%s: Looks like we can not reconnect to notification daemon... Exiting.\n", program);
				exit(EXIT_FAILURE);
			} else {
				g_printerr("%s: Error \"%s\" while trying to show notification. Trying to reconnect.\n", program, error->message);
				errcount++;

				g_error_free(error);
				error = NULL;

				notify_uninit();

				usleep(500 * 1000);

				if(notify_init(PROGNAME) == FALSE) {
					fprintf(stderr, "%s: Can't create notify.\n", program);
					exit(EXIT_FAILURE);
				}
			}
		}
		errcount = 0;

nonotification:
		if (notifystr != NULL) {
			free(notifystr);
			notifystr = NULL;
		}
		if (pixbuf != NULL) {
			g_object_unref(pixbuf);
			pixbuf = NULL;
		}
		mpd_response_finish(conn);
	}

	if (verbose > 0)
		printf("%s: Exiting...\n", program);

	mpd_connection_free(conn);

	g_object_unref(G_OBJECT(notification));
	notify_uninit();

	if (ini != NULL)
		iniparser_freedict(ini);

	return EXIT_SUCCESS;
}
