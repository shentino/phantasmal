#include <kernel/kernel.h>

#include <phantasmal/log.h>
#include <phantasmal/channel.h>
#include <phantasmal/lpc_names.h>

#include <type.h>

mixed*  channels;
int     num_channels;
mixed*  channel_attributes;

#define ATTRIB_ADMIN       1

/* TODO:  Move this to /usr/game, or make it more configurable from
   that location */

/* Prototypes */
void upgraded(varargs int clone);
int is_subscribed(object user, int channel);


static void create(varargs int clone) {
  if(clone) {
    error("Can't clone CHANNELD!");
  }

  upgraded();
}

void upgraded(varargs int clone) {
  int ctr;

  if(!SYSTEM() && !COMMON())
    return;

  /* Note:  these must mesh with include/channel.h.  Don't just modify
     stuff here. */
  channel_attributes = ({ ({ "OOC", 0 }),
			    ({ "Log", ATTRIB_ADMIN }),
			    ({ "Error", ATTRIB_ADMIN }),
			    });

  if(!channels) {
    num_channels = sizeof(channel_attributes);
    channels = allocate(num_channels);

    for(ctr = 0; ctr < num_channels; ctr++) {
      channels[ctr] = ([ ]);
    }
  }

  if(num_channels != sizeof(channel_attributes)) {
    LOGD->write_syslog("Warning!  ChannelD doesn't fully upgrade yet"
		       + " when recompiled!", LOG_ERR);
  }
}

mixed* channel_list(object user) {
  mixed* ret, *tmp;
  int    ctr, is_admin;

  if(!SYSTEM() && !COMMON() && !GAME())
    return nil;

  ret = ({ });
  is_admin = user->is_admin();

  for(ctr = 0; ctr < num_channels; ctr++) {
    if(is_admin || !(channel_attributes[ctr][1] & ATTRIB_ADMIN)) {
      tmp = channel_attributes[ctr];

      /* Add entry with or without extra log/err info */
      if(ctr == CHANNEL_LOG || ctr == CHANNEL_ERR) {
	if(channels[ctr][user->get_name()]) {
	  ret += ({ ({ PHRASED->new_simple_english_phrase(tmp[0]), ctr,
			 channels[ctr][user->get_name()][1]}) });
	} else {
	  ret += ({ ({ PHRASED->new_simple_english_phrase(tmp[0]), ctr, 0 })
		      });
	}
      } else {
	ret += ({ ({ PHRASED->new_simple_english_phrase(tmp[0]), ctr, 0 }) });
      }
    }
  }

  return ret;
}

/* The user will be used for the locale eventually. */
int get_channel_by_name(string name, object user) {
  int ctr;

  if(!SYSTEM() && !COMMON() && !GAME())
    return -1;

  for(ctr = 0; ctr < sizeof(channel_attributes); ctr++) {
    if(!STRINGD->stricmp(channel_attributes[ctr][0], name)) {
      return ctr;
    }
  }

  return -1;
}

void string_to_channel(int channel, string str, varargs int modifiers) {
  int    ctr, do_write;
  mixed* keys;
  int    sub_dat;

  if(!SYSTEM() && !COMMON() && !GAME())
    return;

  keys = map_indices(channels[channel]);
  for(ctr = 0; ctr < sizeof(keys); ctr++) {
    sub_dat = channels[channel][keys[ctr]][1];

    do_write = 1;
    switch(channel) {
    case CHANNEL_ERR:
    case CHANNEL_LOG:
      if(modifiers && sub_dat) {
	if(modifiers < sub_dat)
	  do_write = 0;
      }
      break;
    }

    if(do_write)
      channels[channel][keys[ctr]][0]->message(str);
  }
}

void chat_to_channel(int channel, object phrase, varargs int modifiers) {
  int    ctr, do_write;
  mixed *keys, *user_stuff;
  int    sub_dat;

  if(!SYSTEM() && !COMMON() && !GAME())
    return;

  keys = map_indices(channels[channel]);
  for(ctr = 0; ctr < sizeof(keys); ctr++) {
    user_stuff = channels[channel][keys[ctr]];
    sub_dat = user_stuff[1];

    do_write = 1;
    switch(channel) {
    case CHANNEL_ERR:
    case CHANNEL_LOG:
      if(modifiers && sub_dat) {
	if(modifiers < sub_dat)
	  do_write = 0;
      }
      break;
    }

    if(do_write) {
      string name;

      name = previous_object()->get_Name();
      if(!name) { name = "Somebody"; }
      user_stuff[0]->send_system_phrase("(OOC)");
      user_stuff[0]->message(" " + name + " ");
      user_stuff[0]->send_system_phrase("chats");
      user_stuff[0]->message(": ");
      user_stuff[0]->send_phrase(phrase);
      user_stuff[0]->message("\r\n");
    }
  }
}

int subscribe_user(object user, int channel, varargs int arg) {
  int    attrib, ctr;

  if(!SYSTEM() && !COMMON() && !GAME())
    error("Only privileged code may subscribe users to channels!");

  if(channel < 0 || channel >= num_channels) {
    error("You may only subscribe to an existing channel!");
  }
  attrib = channel_attributes[channel][1];

  if((attrib & ATTRIB_ADMIN) && !user->is_admin()) {
    return -1;
  }

  if(is_subscribed(user, channel))
    return -1;

  channels[channel][user->query_name()] = ({ user, arg });
  return 1;
}

int unsubscribe_user(mixed user, int channel) {
  string name;

  if(!SYSTEM() && !COMMON() && !GAME())
    return -1;

  if(typeof(user) == T_STRING) {
    name = user;
  } else if (typeof(user) == T_OBJECT) {
    name = user->query_name();
  }

  if(channels[channel][name] ) {
    /* Remove user's entry */
    channels[channel][name] = nil;

    if(channels[channel][name]) {
      LOGD->write_syslog("Failed unsubscribe attempt!", LOG_WARNING);
    }

    return 1;
  }

  return -1;
}

void unsubscribe_user_from_all(object user) {
  int ctr, chan;

  if(!SYSTEM() && !COMMON() && !GAME())
    return;

  for(chan = 0; chan < num_channels; chan++) {
    unsubscribe_user(user, chan);
  }
}

int is_subscribed(object user, int channel) {
  if(!SYSTEM() && !COMMON() && !GAME())
    return -1;

  if(channels[channel][user->query_name()]) {
    return 1;
  }

  return 0;
}

int sub_data_level(object user, int channel) {
  if(!SYSTEM() && !COMMON() && !GAME())
    return -1;

  if(channels[channel][user->query_name()]) {
    return channels[channel][user->query_name()][1];
  }

}
