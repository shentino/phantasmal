/* $Header: /cvsroot/phantasmal/mudlib/usr/System/open/lib/userlib.c,v 1.20 2005/08/19 21:41:47 angelbob Exp $ */

#include <kernel/kernel.h>
#include <kernel/user.h>
#include <kernel/rsrc.h>

#include <phantasmal/log.h>
#include <phantasmal/phrase.h>
#include <phantasmal/channel.h>
#include <phantasmal/map.h>
#include <phantasmal/search_locations.h>
#include <phantasmal/lpc_names.h>

#include <type.h>

inherit COMMON_AUTO;
inherit user API_USER;
inherit rsrc API_RSRC;
inherit io   SYSTEM_USER_IO;

/* Duplicated in user object */
#define STATE_NORMAL            0
#define STATE_LOGIN             1
#define STATE_OLDPASSWD         2
#define STATE_NEWPASSWD1        3
#define STATE_NEWPASSWD2        4

/* Directory to store user passwords in */
#define SYSTEM_USER_DIR     "/usr/game/users"

/* Saved by save_object */
string name;	                /* user filename and login name */
string password;		/* user password */
string Name;                    /* human-readable user name */
int    channel_subs;            /* user channel subscriptions */
int    log_chan_level;          /* Level of output on CHANNEL_LOG */
int    err_chan_level;          /* Level of output on CHANNEL_ERR */
int    body_num;                /* number of body object */


/* Random unsaved */
static string newpasswd;	/* new password */
static object wiztool;		/* command handler */
static int nconn;		/* # of connections */
static int timestamp;           /* Last network input */
static string hostname;         /* Hostname they're logging in from */
static mapping state;		/* state for a connection object */
static int first_login;         /* whether this was the first login */

/* Cached vars */
static object body;             /* Body object */
static object mobile;           /* Mobile for body object */
static object location;         /* Location of body */

/* Prototypes */
        void   upgraded(varargs int clone);
static  int    process_message(string str);
static  void   print_prompt(void);

/* Macros */
#define NEW_PHRASE(x) PHRASED->new_simple_english_phrase(x)


/*
 * NAME:	create()
 * DESCRIPTION:	initialize user object
 */
static void create(int clone)
{
  io::create(clone);
  if (clone) {
    user::create();
    rsrc::create();

    state = ([ ]);
  } else {
    upgraded(clone);
  }
}

void upgraded(varargs int clone) {
  if(SYSTEM() || GAME()) {
    io::upgraded(clone);
  } else
    error("Non-System code called upgrade!");
}


/****** Accessor functions *******/

string get_name(void) {
  return name;
}

string get_Name(void) {
  return Name;
}

object get_location(void) {
  return location;
}

object get_body(void) {
  return body;
}

int get_idle_time(void) {
  return time() - timestamp;
}

int is_admin(void)  {

  /* Can't just check wiztool's existence because we need to be able to
     restore subscribed admin-only channels using the
     restore_user_from_file functionality.  That happens before the
     wiztool is created for this player object. */

  /* Check if an immort */
  if (sizeof(rsrc::query_owners() & ({ name }))) {
    return 1;
  }

  return 0;
}

/****************/

/* This currently extracts only alphabetic characters from a name, and
   converts it to lowercase.  If this changes, change it in the
   WIZTOOLLIB as well. */
static string username_to_filename(string str) {
  int iter;
  int len;
  string ret;

  ret = "";
  if(!str) return nil;

  if(str == "Ecru" || str == "System") return str;

  len = strlen(str);
  for(iter = 0; iter < len; iter++) {
    if(str[iter] >= 'a' && str[iter] <= 'z')
      ret += str[iter..iter];
    else if(str[iter] >= 'A' && str[iter] <= 'Z') {
      str[iter] += 'a' - 'A';
      ret += str[iter..iter];
    }
  }
  return ret;
}

static void save_user_to_file() {
  string  user_filename;
  mixed*  chanlist;
  int     subcode, ctr;

  user_filename = SYSTEM_USER_DIR + "/" + name + ".pwd";

  chanlist = CHANNELD->channel_list(this_object());
  subcode = 0;
  for(ctr = 0; ctr < sizeof(chanlist); ctr++) {
    if(CHANNELD->is_subscribed(this_object(), chanlist[ctr][1])) {
      if(chanlist[ctr][1] == CHANNEL_LOG) {
	log_chan_level = CHANNELD->sub_data_level(this_object(),
						  chanlist[ctr][1]);
      } else if (chanlist[ctr][1] == CHANNEL_ERR) {
	err_chan_level = CHANNELD->sub_data_level(this_object(),
						  chanlist[ctr][1]);
      }

      subcode += 1 << chanlist[ctr][1];
    }
  }

  channel_subs = subcode;
  save_object(user_filename);
}

static int restore_user_from_file(string str) {
  object* chanlist;
  int     chan_code, channel, subcode;

  if(!restore_object(SYSTEM_USER_DIR + "/" + username_to_filename(str)
		     + ".pwd")) {
    /* No such file, can't restore */
    CHANNELD->unsubscribe_user_from_all(this_object());
    return 0;
  }

  CHANNELD->unsubscribe_user_from_all(this_object());

  chan_code = 1;
  channel = 0;
  subcode = channel_subs;
  while(subcode && subcode >= chan_code) {
    /* If this user should be subbed to this channel */
    if(subcode & chan_code) {
      int ret;

      subcode -= chan_code;

      switch(channel) {
      case CHANNEL_LOG:
	ret = CHANNELD->subscribe_user(this_object(), channel, log_chan_level);
	break;
      case CHANNEL_ERR:
	ret = CHANNELD->subscribe_user(this_object(), channel, err_chan_level);
	break;
      default:
	ret = CHANNELD->subscribe_user(this_object(), channel);
      }

      if(ret < 0) {
	LOGD->write_syslog("Couldn't sub user " + Name + " to channel #"
			   + channel + "!", LOG_ERROR);
      }
    }

    /* Next channel... */
    chan_code += chan_code;
    channel++;
  }
  if(subcode != 0) {
    LOGD->write_syslog("Subcode " + subcode + " means user didn't sub to all "
		       + "appropriate channels!", LOG_WARNING);
  }

  return 1;
}


void user_state_data(mixed data) {
  if(data == nil && (SYSTEM() || COMMON())) {
    /* Passing nil means "done now, print a prompt". */
    print_prompt();
    return;
  }
}


/*
 * NAME:	receive_message()
 * DESCRIPTION:	process a message from the user
 */
nomask int receive_message(string str)
{
  if (previous_program() == LIB_CONN) {
    if(peek_state()) {
      mixed tmp;

      tmp = state_receive_message(str);
      if(!peek_state()) {
	/* We started with a user_state active, but it's stopped now. */
	print_prompt();
      }
      return tmp;
    }

    return process_message(str);
  }
  error("receive_message called by illegal sender");
}


/*
 * NAME:	show_room_to_player()
 * DESCRIPTION:	give room desc to current player
 */
static void show_room_to_player(object ROOM location) {
  string tmp;
  object PHRASE phr;
  int    ctr;
  mixed* objs;

  if(!SYSTEM() && !COMMON() && !GAME())
    error("Only privileged code may call show_room_to_player!");

  if(!location) {
    send_system_phrase("you are nowhere");
    message("\n");
    return;
  }

  phr = location->get_brief();
  if(phr)
    tmp = phr->to_string(this_object());
  if(tmp) {
    message(tmp);
    if(is_admin())
      message(" [" + location->get_number() + "]");
    message("\n");
  } else {
    send_system_phrase("(unnamed location)");
    message("\n");
  }

  phr = location->get_look();
  tmp = phr ? phr->to_string(this_object()) : nil;
  if(tmp) {
    message(tmp);
    message("\n");
  } else {
    send_system_phrase("(no room desc)");
    message("\n");
  }

  message("*****\n");

  objs = location->objects_in_container();
  for(ctr = 0; ctr < sizeof(objs); ctr++) {
    if(objs[ctr] != body) {
      message("- ");
      send_phrase(objs[ctr]->get_brief());
      message("\n");
    }
  }

  message("\n");

  if(function_object("num_exits", location)) {
    send_system_phrase("Exits");
    message(": ");
    for(ctr = 0; ctr < location->num_exits(); ctr++) {
      object exit;

      exit = location->get_exit_num(ctr);
      phr = EXITD->get_short_for_dir(exit->get_direction());
      message(phr->to_string(this_object()) + " ");
    }
    message("\n");
  }
}


/* This is just a utility function */
static string* string_to_words(string str) {
  string *words;
  int     ctr;

  words = explode(str, " ");

  /* Trim */
  for(ctr = 0; ctr < sizeof(words); ctr++) {
    if(!words[ctr] || STRINGD->is_whitespace(words[ctr])) {
      words = words[..ctr-1] + words[ctr+1..];
    } else {
      words[ctr] = STRINGD->to_lower(STRINGD->trim_whitespace(words[ctr]));
    }
  }

  return words;
}


/* This function takes the supplied object list and searches it
   for objects that match the string.  This includes string matching,
   but also includes things like searching through open containers for any
   possible matches.  If only_details is true then objects will have
   their details searched, but not objects inside those details.
*/
static object* search_contained_objects(object* objs, string str,
					varargs int only_details) {
  object *ret, *contents, *details, temp;
  string *words, err;
  int ctr, temp2;

  words = string_to_words(str);
  temp2 = 0;

  ret = ({ });
  if (sizeof(objs)) {
    temp = objs[0];
    temp2 = 1;
  }

  while(sizeof(objs)) {
    if(objs[0] == location
       || (!only_details
	   && objs[0]->is_container() && objs[0]->is_open())) {
      contents = objs[0]->objects_in_container();
      if(contents)
	objs += contents;
    }

    if(objs[0]->match_words(this_object(), words)) {
      ret += ({ objs[0] });
    }

    details = objs[0]->get_details();
    if(details && sizeof(details)) {
      objs += details;
    }

    objs = objs[1..];
  }

  return sizeof(ret) ? ret : nil;
}

private object* find_objects_in_loc(int loc, string str) {
  object *objs;
  int ctr;

  objs = ({ });

  if(!location &&
     (loc == LOC_CURRENT_ROOM || loc == LOC_IMMEDIATE_CURRENT_ROOM
      || loc == LOC_DETAIL_CURRENT_ROOM || LOC_CURRENT_EXITS))
    return nil;

  if(!body &&
     (loc == LOC_INVENTORY || loc == LOC_IMMEDIATE_INVENTORY
      || loc == LOC_BODY))
    return nil;

  switch(loc) {

  case LOC_IMMEDIATE_CURRENT_ROOM:
    return location->find_contained_objects(this_object(), str);

  case LOC_IMMEDIATE_INVENTORY:
    return body->find_contained_objects(this_object(), str);

  case LOC_CURRENT_ROOM:
    /* Pass location directly so its details will be searched */
    return search_contained_objects( ({ location }), str);

  case LOC_CURRENT_EXITS:
    /* Pass location directly so its details will be searched */
    if(function_object("num_exits", location)) {
      for(ctr = 0; ctr < location->num_exits(); ctr++) {
        objs += ({ location->get_exit_num(ctr) });
      }
    }
    return search_contained_objects( objs, str);

  case LOC_INVENTORY:
    /* Pass objects in body object so that body's details won't be
       searched */
    return search_contained_objects(body->objects_in_container(), str);

  case LOC_DETAIL_CURRENT_ROOM:
    return search_contained_objects( ({ location }), str, 1);

  case LOC_BODY:
    return search_contained_objects( ({ body }), str, 1);

  default:
    error("Unrecognized location (" + loc
	  + ") when finding objects!");
  }

  /* Never used */
  return nil;
}

object* find_objects(string str, int locations...) {
  int     ctr;
  object* objs;

  if(!SYSTEM() && !COMMON() && !GAME())
    error("Only Phantasmal objects are allowed to call find_objects()!");

  objs = ({ });
  for(ctr = 0; ctr < sizeof(locations); ctr++) {
    objs += find_objects_in_loc(locations[ctr], str);
  }

  return sizeof(objs) ? objs : nil;
}


object* find_first_objects(string str, int locations...) {
  int     ctr;
  object* objs;

  if(!SYSTEM() && !COMMON() && !GAME())
    error("Only Phantasmal objects are allowed to call find_first_objects()!");

  for(ctr = 0; ctr < sizeof(locations); ctr++) {
    objs = find_objects_in_loc(locations[ctr], str);
    if(objs && sizeof(objs)) {
      return objs;
    }
  }

  return nil;
}


nomask void notify_moved(object obj) {
  if(function_object("set_number", previous_object()) != MOBILE) {
    error("Only MOBILEs can notify the User object that its body moved.");
  }

  location = body->get_location();
}


static void set_state(object key_obj, int new_state) {
  state[key_obj] = new_state;
}

static int get_state(object key_obj) {
  return state[key_obj];
}

/*
 * NAME:	login()
 * DESCRIPTION:	login a new user
 */
int login(string str)
{
  if (previous_program() == LIB_CONN) {
    /* Note:  'filename', as used here, isn't the actual filename
       with extension.  It's just the alpha characters from the
       supplied name, in lowercase */
    string filename, check_name;

    filename = username_to_filename(str);

    /* Check for security problems */
    if(filename == "" || filename == nil

       /* Bad ideas for security reasons */
       || filename == "game"
       || filename == "Ecru"
       || filename == "System"
       || (sscanf(filename, "%*scommon%*s") == 2)
       || (sscanf(filename, "%*ssystem%*s") == 2)

       /* No trailing spaces or slashes in names allowed */
       || (sscanf(str, "%*s ") != 0)
       || (sscanf(str, "%*s/") != 0)

       /* And let the game code add more restrictions if it likes */
       || this_object()->name_is_forbidden(str)
       || this_object()->filename_is_forbidden(filename)) {

      previous_object()->message("\nThat name is forbidden.\n"
				 + "Please log in with a different one.\n");
      return MODE_DISCONNECT;
    }

    if (nconn == 0) {
      ::login(str);
    }
    nconn++;

    name = filename;
    Name = str;

    /* Capitalize first letter of Name (but not name) */
    if (Name[0] >= 'a' && Name[0] <= 'z') {
      Name[0] -= 'a' - 'A';
    }

    timestamp = time();
    hostname = query_ip_name(this_object());

    name = nil;
    restore_user_from_file(filename);
    check_name = username_to_filename(name);
    if(name && STRINGD->strcmp(filename, check_name)) {
      LOGD->write_syslog("Internal error restoring player from file!",
			 LOG_FATAL);
      previous_object()->message("Something went wrong logging you in.\n"
			       + "Try again, maybe?\n");
      return MODE_DISCONNECT;
    }

    if(!name) {
      first_login = 1;
      name = filename;
    } else
      first_login = 0;

    if (password) {
      object phr;

      /* check password */
      phr = PHRASED->file_phrase(SYSTEM_PHRASES, "Password: ");
      previous_object()->message(phr->to_string(this_object()));

      set_state(previous_object(), STATE_LOGIN);
    } else {
      /* no password; login immediately */

      /* Set our connection object to the one that just called us */
      connection(previous_object());

      message_all_users(Name + " ");
      system_phrase_all_users("logs in.");
      message_all_users("\n");

      send_system_phrase("choose new password");
      set_state(previous_object(), STATE_NEWPASSWD1);

      /* Check if an immort */
      if (sizeof(rsrc::query_owners() & ({ str })) == 0) {
	return MODE_NOECHO;
      }

      /* If so, create a wiztool */
      if (!wiztool) {
	wiztool = SYSTEM_USER_OBJ->clone_wiztool_as(str);
	if(!wiztool)
	  error("Can't clone wiztool!");
      }
    }
    return MODE_NOECHO;
  }

}


/*
 * NAME:	logout()
 * DESCRIPTION:	logout user
 */
void logout(int quit)
{
  if (previous_program() == LIB_CONN && --nconn == 0) {
    if (query_conn()) {
      if (quit) {
	message_all_users(Name + " ");
	system_phrase_all_users("logs out.");
	message_all_users("\n");
      } else {
	message_all_users(Name + " disconnected.\n");
      }
    }
    this_object()->player_logout();
    ::logout();
    if (wiztool) {
      destruct_object(wiztool);
    }
    destruct_object(this_object());
  }
}


static int process_message(string str)
{
  string cmd;
  int    ctr, size, retcode;
  int    force_command;
  mixed* command;

  timestamp = time();

  switch (get_state(previous_object())) {
  case STATE_NORMAL:
    retcode = this_object()->process_command(str);
    if(retcode >= 0)
      return retcode;

    /* If retcode was -1, everything's fine and no changes need to
       happen.  Just break, print a prompt and smile. */
    break;

  case STATE_LOGIN:
    if (crypt(str, password) != password) {
      object phr;

      previous_object()->message("\n");
      phr = PHRASED->file_phrase(SYSTEM_PHRASES, "Bad password.");
      previous_object()->message(phr->to_string(this_object()));
      previous_object()->message("\n");
      return MODE_DISCONNECT;
    }
    connection(previous_object());
    message("\n");
    message_all_users(Name + " ");
    system_phrase_all_users("logs in.");
    message_all_users("\n");
    if (!wiztool && sizeof(rsrc::query_owners() & ({ name })) != 0) {
      wiztool = SYSTEM_USER_OBJ->clone_wiztool_as(name);
      if(!wiztool)
	error("Can't clone wiztool!");
    }

    catch {
      this_object()->player_login(first_login);
    } : {
      message("Error logging user in!\n");
      return MODE_DISCONNECT;
    }
    break;

  case STATE_OLDPASSWD:
    if (crypt(str, password) != password) {
      message("\n");
      send_system_phrase("Bad password.");
      message("\n");
      break;
    }
    message("\n");
    send_system_phrase("New password: ");
    set_state(previous_object(), STATE_NEWPASSWD1);
    return MODE_NOECHO;

  case STATE_NEWPASSWD1:
    if(strlen(str) == 0) {
      message("\n");
      send_system_phrase("Looks like no password");
      message("\n");
      if(password && strlen(password)) {
	send_system_phrase("Password change cancelled");
	return MODE_NOECHO;
      }

      return MODE_DISCONNECT;
    }
    if(strlen(str) < 4) {
      message("\n");
      send_system_phrase("must be four characters");
      message("\n");
      send_system_phrase("New password: ");
      return MODE_NOECHO;
    }
    newpasswd = str;
    message("\n");
    send_system_phrase("Retype new password: ");
    set_state(previous_object(), STATE_NEWPASSWD2);
    return MODE_NOECHO;

  case STATE_NEWPASSWD2:
    if (newpasswd == str) {
      password = crypt(str);
      save_user_to_file();
      message("\n");
      send_system_phrase("Password changed.");
      message("\n");
    } else {
      message("\n");
      send_system_phrase("Mismatch; password not changed.");
      message("\n");

      set_state(previous_object(), STATE_NEWPASSWD1);
      send_system_phrase("New password: ");
      return MODE_NOECHO;
    }
    newpasswd = nil;

    if(!location) {
      catch {
	this_object()->player_login(first_login);
      } : {
	return MODE_DISCONNECT;
      }
    }

    break;
  }

  /* Don't print a prompt if we've pushed a state_stack -- they
     print their own prompts. */
  if(!peek_state()) {
    print_prompt();
  }

  set_state(previous_object(), STATE_NORMAL);
  return MODE_ECHO;
}

static void print_prompt(void) {
  string str;

  str = (wiztool) ? query_editor(wiztool) : nil;
  if (str) {
    message((str == "insert") ? "*\b" : ":");
  } else {
    message(is_admin() ? "# " : "> ");
  }
}


/************** User-level commands *************************/

static void cmd_help(object user, string cmd, string str) {
  mixed *hlp, *kw;
  int index;
  int exact;

  if(str)
    str = STRINGD->trim_whitespace(str);

  exact = 0;
  if (str && sscanf(str, "%d %s", index, str) == 2) {
    if(index < 1) {
      send_system_phrase("Usage: ");
      message(cmd + " <word>\n");
      message("   or  " + cmd + " <num> <word>\n");
      return;
    }
    exact = 1;
    index = index - 1;  /* User sees as 1-indexed, we see 0-indexed */
  } else if (!str || STRINGD->is_whitespace(str)) {

    if(wiztool) {
      str = "main_admin";
    } else {
      str = "main";
    }

    index = 0;
  } else if (str) {
    index = 0;
  }

  if(wiztool) {
    kw = ({ "admin" });
  } else {
    kw = ({ });
  }
  hlp = HELPD->query_exact_with_keywords(str, this_object(), kw);
  if(hlp) {
    if((exact && (sizeof(hlp) <= index)) || (sizeof(hlp) < 0)
       || (index < 0)) {
      message("Only " + sizeof(hlp) + " help files on \""
	      + str + "\".\n");
    } else {
      if(sizeof(hlp) > 1) {
	message("Help on " + str + ":    [" + sizeof(hlp) + " entries]\n");
      }
      message_scroll(hlp[index][1]->to_string(this_object()) + "\n");
    }
    return;
  }

  if(!exact) {
    string sdx_key;

    sdx_key = SOUNDEXD->get_key(str);
    hlp = HELPD->query_soundex_with_keywords(sdx_key, this_object(), ({ }));

    if(hlp && sizeof(hlp)) {
      if(index) {
	if(index < sizeof(hlp)) {
	  message("Help on " + hlp[index][0] + ":\n");
	  message(hlp[index][1]->to_string(this_object()));
	  message("\n");
	} else {
	  message("There are only " + sizeof(hlp)
		  + " help entries that sound like " + str + ".\n");
	}
      } else if(sizeof(hlp) == 1) {
	message_scroll("Help on " + hlp[0][0] + ":\n"
		       + hlp[0][1]->to_string(this_object())
		       + "\n");
      } else {
	message("\nWhich do you want help on:\n");
	for(index = 0; index < sizeof(hlp); index++) {
	  message("     " + hlp[index][0] + "\n");
	}
	message("(type \"help <topic>\" for the topic you want)\n");
      }
      return;
    }
  }

  message("No help on \"" + str + "\".\n");
}


private void __sub_unsub_channels(object user, string cmd, int chan,
				  string channelname, string subval,
				  string sublevel) {
  /* Sub or unsub the user */
  if(!STRINGD->stricmp(subval, "on")
     || !STRINGD->stricmp(subval, "sub")
     || !STRINGD->stricmp(subval, "subscribe")) {

    if(sublevel) {
      int level;

      /* Subscribe with extra sub info */
      if((chan != CHANNEL_ERR && chan != CHANNEL_LOG)
	 || !wiztool) {
	user->message("You can't subscribe to any channels that use"
		      + " extra subscription info.\n");
	user->message("Usage: " + cmd + " <channel> [on|off]\n");
      }

      if(!sscanf(sublevel, "%d", level)) {
	/* Try and parse sublevel as a string */
	if(chan == CHANNEL_LOG || chan == CHANNEL_ERR
	   && LOGD->get_level_by_name(sublevel)) {
	  level = LOGD->get_level_by_name(sublevel);
	} else {
	  user->message("Not sure what level to use for '" + sublevel
			+ "'.\n");
	  return;
	}
      }

      /* Subscribe with extra info 'level' */
      if(CHANNELD->subscribe_user(user, chan, level) < 0) {
	  user->message("You can't subscribe to that channel.\n");
      } else {
	user->message("Subscribed to " + channelname + ", level " + level
		      + ".\n");
      }
    } else {

      /* Subscribe with no extra info */
      if(CHANNELD->subscribe_user(user, chan) < 0) {
	user->message("You can't subscribe to that channel.\n");
      } else {
	user->message("Subscribed to " + channelname + ".\n");
      }
    }

    /* Save new subscriptions, if any */
    save_user_to_file();

  } else if(!STRINGD->stricmp(subval, "off")
	    || !STRINGD->stricmp(subval, "unsub")
	    || !STRINGD->stricmp(subval, "unsubscribe")) {

    if(CHANNELD->unsubscribe_user(user, chan) < 0) {
      user->message("You can't unsub from that.  "
		    + "Are you sure you're subscribed?\n");
    } else {
      user->message("Unsubscribed from " + channelname + ".\n");
      save_user_to_file();
    }
  } else {
    user->message("Huh?  Try using 'on' or 'off' for the third value.\n");
  }
}

static void cmd_channels(object user, string cmd, string str) {
  int    ctr, chan;
  mixed* chanlist;
  string channelname, subval, sublevel;

  if(str)
    str = STRINGD->trim_whitespace(str);
  if(!str || str == "") {
    chanlist = CHANNELD->channel_list(user);
    user->message("Channels:\n");
    for(ctr = 0; ctr < sizeof(chanlist); ctr++) {
      if(CHANNELD->is_subscribed(user, ctr)) {
	user->message("* ");
      } else {
	user->message("  ");
      }
      user->send_phrase(chanlist[ctr][0]);

      if(chanlist[ctr][2] > 0) {
	user->message("  " + chanlist[ctr][2]);
      }

      user->message("\n");
    }
    user->message("-----\n");
    return;
  }

  if(!str || str == "" || sscanf(str, "%*s %*s %*s %*s") == 4) {
    if(wiztool) {
      send_system_phrase("Usage: ");
      message(cmd + " [<channel name> [on|off]] [extra info]\n");
    } else {
      send_system_phrase("Usage: ");
      message(cmd + " [<channel name> [on|off]]\n");
    }
    return;
  }

  if((sscanf(str, "%s %s %s", channelname, subval, sublevel) != 3)
     && (sscanf(str, "%s %s", channelname, subval) != 2)
     && (sscanf(str, "%s", channelname) != 1)) {
    user->message("Parsing error!\n");
    return;
  }

  chan = CHANNELD->get_channel_by_name(channelname, user);

  if(chan < 0) {
    user->message("You don't know any channel named '" + channelname
		  + "'.  Type 'channels' for a list of names.\n");
    return;
  }

  if(subval) {
    __sub_unsub_channels(user, cmd, chan, channelname, subval, sublevel);
    return;
  }

  /* Check whether you're subbed and whether the channel is available
     here. */
  user->message("Channel: " + channelname + "\n");
  if(CHANNELD->is_subscribed(user, chan)) {
    user->message("You are currently subscribed to that channel.\n");
  } else {
    user->message("You are not currently subscribed to that channel.\n");
  }
  user->message("That channel is available in this area.\n");
}
