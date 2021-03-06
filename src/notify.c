/*
 * cmusfm - notify.c
 * Copyright (c) 2014 Arkadiusz Bokowy
 *
 * This file is a part of a cmusfm.
 *
 * cmusfm is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * cmusfm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * If you want to read full version of the GNU General Public License
 * see <http://www.gnu.org/licenses/>.
 */

#include "notify.h"

#include <stdlib.h>
#include <string.h>
#include <libnotify/notify.h>


// global notification handler
static NotifyNotification *cmus_notify;


// Show track information via the notification system.
void cmusfm_notify_show(const scrobbler_trackinfo_t *sb_tinf, const char *icon) {

	char *body;
	size_t art_len, alb_len;

	if(cmus_notify) {
		// forcefully close previous notification
		notify_notification_close(cmus_notify, NULL);
		g_object_unref(G_OBJECT(cmus_notify));
	}

	// concatenate artist and album (when applicable)
	art_len = strlen(sb_tinf->artist);
	alb_len = strlen(sb_tinf->album);
	body = (char*)malloc(art_len + alb_len + sizeof(" ()") + 1);
	strcpy(body, sb_tinf->artist);
	if(alb_len > 0)
		sprintf(&body[art_len], " (%s)", sb_tinf->album);

	cmus_notify = notify_notification_new(sb_tinf->track, body, icon);
	notify_notification_show(cmus_notify, NULL);
	free(body);
}

// Initialize notification system.
void cmusfm_notify_initialize() {
	cmus_notify = NULL;
	notify_init("cmusfm");
}

// Free notification system resources.
void cmusfm_notify_free() {
	if(cmus_notify)
		g_object_unref(G_OBJECT(cmus_notify));
	notify_uninit();
}
