#include <kernel/kernel.h>
#include <kernel/access.h>
#include <kernel/rsrc.h>
#include <kernel/version.h>
#include <type.h>
#include <config.h>
#include <log.h>

inherit access API_ACCESS;
inherit rsrc   API_RSRC;

/* How many objects can be saved to file in a single call_out? */
#define SAVE_CHUNK   10

private string pending_callback;
private int __sys_suspended;

/* Prototypes */
private void suspend_system();
private void release_system();
static void __co_write_rooms(object user, int* objects, int ctr,
			     string roomfile,
			     string zonefile);
static void __co_write_zones(object user, int* objects, int ctr,
			     string zonefile);
static void __reboot_callback(void);
static void __shutdown_callback(void);


static void create(varargs int clone)
{
  object driver, obj, the_void;
  string zone_file, mapd_dtd, help_dtd, objs_file;
  string bind_dtd;
  int major, minor, patch, rooms_loaded;

  /* First things first -- this release needs one of the
     latest versions of DGD, so let's make sure. */
  if(!sscanf(KERNEL_LIB_VERSION, "%d.%d.%d", major, minor, patch)) {
    error("Don't recognize Kernel Library version as being of the"
	  + " form X.Y.ZZ!");
  }
  if((major == 1 && minor < 2)
     || (major == 1 && minor == 2 && patch < 13)) {
    error("Need to upgrade to DGD version 1.2.41 or higher!");
  } else if (major == 1 && minor == 2 && patch > 16) {
    DRIVER->message("This is a very new Kernel Library version, or at\n");
    DRIVER->message("  least newer than this version of Phantasmal.  If\n");
    DRIVER->message("  you have problems, please upgrade!\n");
  } else if (major > 1 || (major == 1 && minor > 2)) {
    DRIVER->message("This version of Phantasmal is not tested\n");
    DRIVER->message("with DGD beyond 1.2.XX.  Please upgrade!\n");
  }

  access::create();
  rsrc::create();

  add_user("common");
  add_owner("common");
  set_global_access("common", READ_ACCESS);

  driver = find_object(DRIVER);

  /* Start LOGD and log MUD startup */
  if(!find_object(LOGD)) { compile_object(LOGD); }
  /* Channels aren't set yet... */
  LOGD->write_syslog("\n-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-\n"
		     + "Starting MUD...\n"
		     + "-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-");

  /* Compile, find and install the Errord */
  if(!find_object(ERRORD)) { compile_object(ERRORD); }
  driver->set_error_manager(find_object(ERRORD));

  /* Compile, find and install the Objectd */
  if(!find_object(OBJECTD)) { compile_object(OBJECTD); }
  driver->set_object_manager(find_object(OBJECTD));
  OBJECTD->do_initial_obj_setup();

  /* Compile the String manager (used as a std library) */
  if(!find_object(STRINGD)) { compile_object(STRINGD); }

  /* Start up logging channels in the LOGD */
  LOGD->start_channels();

  /* Compile, find and install the Telnetd */
  if(!find_object(TELNETD)) { compile_object(TELNETD); }
  "/kernel/sys/userd"->set_telnet_manager(0,find_object(TELNETD));

  /* Compile the Phrase manager (before Helpd) */
  if(!find_object(PHRASED)) { compile_object(PHRASED); }

  /* Set up online help */
  if(!find_object(HELPD)) { compile_object(HELPD); }

  help_dtd = read_file(HELP_DTD);
  if(!help_dtd)
    error("Can't load file " + HELP_DTD + "!");

  HELPD->load_help_dtd(help_dtd);
  HELPD->new_help_directory("/data/help");

  /* Compile the Objnumd */
  if(!find_object(OBJNUMD)) { compile_object(OBJNUMD); }

  /* Set up ZoneD, Mapd & Exitd */
  if(!find_object(ZONED)) { compile_object(ZONED); }
  if(!find_object(MAPD)) { compile_object(MAPD); }
  if(!find_object(EXITD)) { compile_object(EXITD); }

  bind_dtd = read_file(BIND_DTD);
  if (!bind_dtd) {
    error("Can't read file " + BIND_DTD + "!");
  }

  mapd_dtd = read_file(MAPD_ROOM_DTD);
  if(!mapd_dtd)
    error("Can't read file " + MAPD_ROOM_DTD + "!");
  MAPD->init(mapd_dtd, bind_dtd);

  /* Set up The Void (room #0) */
  if(!find_object(THE_VOID)) { compile_object(THE_VOID); }
  the_void = clone_object(THE_VOID);
  if(!the_void)
    error("Can't clone void object!");
  MAPD->add_room_number(the_void, 0);

  /* Load stuff into MAPD and EXITD */
  objs_file = read_file(ROOM_FILE);
  if(objs_file) {
    MAPD->add_unq_text_rooms(objs_file, ROOM_FILE);
    EXITD->add_deferred_exits();
    rooms_loaded = 1;
  } else {
    DRIVER->message("Can't read room file!\n");
    LOGD->write_syslog("Can't read room file!  Starting blank!", LOG_WARN);
    rooms_loaded = 0;
  }

  /* Load zone/segment mapping information */
  zone_file = read_file(ZONE_FILE);
  ZONED->init_from_file(zone_file);

  /* Start up ChannelD and ConfigD */
  if(!find_object(CHANNELD)) compile_object(CHANNELD);
  if(!find_object(CONFIGD)) compile_object(CONFIGD);
}

void save_mud_data(object user, string room_filename,
		   string zone_filename, string callback) {
  int*   objects;
  int    cohandle;
  mixed  tmp;

  if(!SYSTEM()) {
    error("Only privileged code can call save_mud_data!");
    return;
  }

  if(pending_callback) {
    error("Somebody else is already saving!");
  }
  pending_callback = callback;

  if(__sys_suspended) {
    LOGD->write_syslog("System was still suspended! (unsuspending)",
		       LOG_ERROR);
    release_system();
  }

  LOGD->write_syslog("Writing World Data to files...", LOG_NORMAL);

  remove_file(room_filename + ".old");
  rename_file(room_filename, room_filename + ".old");
  remove_file(room_filename);

  remove_file(zone_filename + ".old");
  rename_file(zone_filename, zone_filename + ".old");
  remove_file(zone_filename);

  if(sizeof(get_dir(room_filename)[0])) {
    LOGD->write_syslog("Can't remove old roomfile -- trying to append!",
		       LOG_FATAL);
  }

  if(sizeof(get_dir(zone_filename)[0])) {
    LOGD->write_syslog("Can't remove old zonefile -- trying to append!",
		       LOG_FATAL);
  }

  LOGD->write_syslog("Writing rooms to file", LOG_VERBOSE);
  objects = MAPD->rooms_in_zone(0) - ({ 0 });

  cohandle = call_out("__co_write_rooms", 0, user, objects, 0,
		      room_filename, zone_filename);
  if(cohandle < 1) {
    error("Can't schedule call_out to save objects!");
  } else {
    suspend_system();
  }
}

void prepare_reboot(void)
{
  if(previous_program() != SYSTEM_WIZTOOLLIB
     && previous_program() != DRIVER)
    error("Can't call prepare_reboot from there!");

  if(find_object(LOGD)) {
    LOGD->write_syslog("Preparing to reboot MUD...", LOG_NORMAL);
  }

  /* save_mud_data(nil, "__reboot_callback"); */
}

void prepare_shutdown(void)
{
  if(previous_program() != SYSTEM_WIZTOOLLIB
     && previous_program() != DRIVER)
    error("Can't call prepare_shutdown from there!");

  if(find_object(LOGD)) {
    LOGD->write_syslog("Shutting down MUD...", LOG_NORMAL);
  }

  save_mud_data(this_user(), ROOM_FILE, ZONE_FILE,
		"__shutdown_callback");
}

void reboot(void)
{
  if(previous_program() != SYSTEM_WIZTOOLLIB)
    error("Can't call reboot from there!");

  if(find_object(LOGD)) {
    LOGD->write_syslog("Rebooting!", LOG_NORMAL);
  }
}



/********* Helper and callout functions ***********************/

/* suspend_system and release_system based on
   /usr/System/sys/objectd.c */

/*
  Suspend_system suspends network input, new logins and callouts
  except in this object.  (idea stolen from Geir Harald Hansen's
  ObjectD).  This will need to be copied to any and every object
  that suspends callouts -- the RSRCD checks previous_object()
  to find out who *isn't* suspended.  TelnetD only suspends
  new incoming network activity.
*/
private void suspend_system() {
  if(__sys_suspended) {
    LOGD->write_syslog("System already suspended...", LOG_ERROR);
  }
  __sys_suspended = 1;
  RSRCD->suspend_callouts();
  TELNETD->suspend_input(0);  /* 0 means "not shutdown" */
}

/*
  Releases everything that suspend_system suspends.
*/
private void release_system() {
  if(!__sys_suspended) {
    LOGD->write_syslog("System not suspended, won't unsuspend.", LOG_ERROR);
    return;
  }
  __sys_suspended = 0;
  RSRCD->release_callouts();
  TELNETD->release_input();
  pending_callback = nil;
}


static void __co_write_rooms(object user, int* objects, int ctr,
			     string roomfile,
			     string zonefile) {
  string unq_str;
  object obj;
  int    chunk_ctr;

  catch {
    for(chunk_ctr = 0; ctr < sizeof(objects) && chunk_ctr < SAVE_CHUNK;
	ctr++, chunk_ctr++) {
      obj = MAPD->get_room_by_num(objects[ctr]);

      unq_str = obj->to_unq_text();

      if(!write_file(roomfile, unq_str)) {
	DRIVER->message("Couldn't write rooms to file!  Fix or kill driver!");
	error("Couldn't write rooms to file " + roomfile + "!");
      }
    }

    if(ctr < sizeof(objects)) {
      /* Still saving rooms... */
      if(call_out("__co_write_rooms", 0, user, objects, ctr, roomfile,
		  zonefile) < 1) {
	release_system();
	error("Can't schedule call_out to continue writing rooms!");
      }
      return;
    }

    /* Done with portables, start on zones */
    LOGD->write_syslog("Writing zones to file", LOG_VERBOSE);

    if(call_out("__co_write_zones", 0, user, objects, 0, zonefile) < 1) {
       release_system();
       error("Can't schedule call_out to start writing portables!");
     }
  } : {
    release_system();
    user->message("Error writing rooms!\n");
    error("Error writing rooms!");
  }
}

static void __co_write_zones(object user, int* objects, int ctr,
			     string zonefile) {
  string unq_str;

  catch {
    unq_str = ZONED->to_unq_text();
    if(!unq_str) {
      LOGD->write_syslog(ZONED->get_parse_error_stack());
      release_system();
      error("Can't serialize ZONED to UNQ!");
    }
    write_file(zonefile, unq_str);
  } : {
    release_system();
    user->message("Error writing zones!\n");
    error("Error writing zones!");
  }

  catch {
    if(pending_callback) {
      call_other(this_object(), pending_callback);
      pending_callback = nil;
    }
  } : {
    release_system();
    user->message("Error calling callback!\n");
    error("Error calling callback!");
  }

  release_system();
  LOGD->write_syslog("Finished writing saved data...", LOG_NORMAL);
  if(user)
    user->message("Finished writing data.\n");
}


static void __shutdown_callback(void) {
  ::shutdown();
}

static void __reboot_callback(void) {
  /* Dumping of state starts happening before we get this notification,
     so don't explicitly dump state again... */
}
