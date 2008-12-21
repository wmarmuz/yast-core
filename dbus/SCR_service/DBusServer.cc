
/*

  DBus server

 */

#include "DBusServer.h"
#include "DBusMsg.h"

#ifdef HAVE_POLKIT
#include "PolKit.h"
#endif

#include <ycp/y2log.h>

extern "C"
{
// nanosleep()
#include <time.h>
// stat()
#include <sys/stat.h>
}

// ostringstream
#include <sstream>

// use atomic type in signal handler (see bnc#434509)
static sig_atomic_t finish = 0;


DBusServer::DBusServer()
{
    dbus_threads_init_default();
    sa = new ScriptingAgent();
}

DBusServer::~DBusServer()
{
    if (sa != NULL)
    {
	delete sa;
    }
}


bool DBusServer::connect()
{
    // connect to DBus, request a service name
    return connection.connect(DBUS_BUS_SYSTEM, "org.opensuse.yast.SCR");
}

// set 30 second timer
void DBusServer::resetTimer()
{
    ::alarm(30);
}

// NOTE: this is a signal handler, do only really necessary tasks here!
// be aware of non-reentrant functions!
void sig_timer(int signal, siginfo_t *info, void *data)
{
    if (signal == SIGALRM)
    {
	// set the finish flag for the main loop
	finish = true;
    }
}

void DBusServer::registerSignalHandler()
{
    struct sigaction new_action, old_action;

    // use sa_sigaction parameter
    new_action.sa_flags = SA_SIGINFO;
    new_action.sa_sigaction = &sig_timer;
    ::sigemptyset(&new_action.sa_mask);

    if (::sigaction(SIGALRM, &new_action, &old_action))
    {
	y2error("Cannot register SIGALRM handler!");
    }
}

bool DBusServer::canFinish()
{
    // check if clients are still running,
    // remove finished clients
    for(Clients::iterator it = clients.begin();
	it != clients.end();)
    {
	pid_t pid = it->second;

	if (!isProcessRunning(pid))
	{
	    Clients::iterator remove_it(it);

	    // move the current iterator
	    it++;

	    y2milestone("Removing client %s (pid %d) from list", (remove_it->first).c_str(), pid);

	    clients.erase(remove_it);
	}
	else
	{
	    // the process is still running
	    // no need to check the other clients
	    // we have to still run for at least this client
	    y2debug("Client %s PID %d is still running", (it->first).c_str(), pid);
	    break;
	}
    }

    // if there is no client the server can be finished
    return clients.size() == 0;
}

/**
 * Server that exposes a method call and waits for it to be called
 */
void DBusServer::run(bool forever) 
{
    y2milestone("Listening for incoming DBus messages...");

    if (forever)
	y2milestone("Timer disabled");
    else
	registerSignalHandler();

    // mainloop
    while (true)
    {
	// the time is over
	if (finish)
	{
	    y2milestone("Timout signal received");

	    if (canFinish())
	    {
		break;
	    }
	    else
	    {
		// reset the flag
		finish = false;

		// set a new timer
		resetTimer();
	    }
	}

	// set 5 seconds timeout
	connection.setTimeout(5000);
	// try reading a message from DBus
	DBusMsg request(connection.receive());

	// check if a message was received
	if (request.empty())
	{ 
	    continue; 
	}

	// reset the timer when a message is received
	if (! forever)
	    resetTimer();

	// create a reply to the message
	DBusMsg reply;

	y2milestone("Received request from %s: interface: %s, method: %s", request.sender().c_str(),
	    request.interface().c_str(), request.method().c_str());
      
	// check this is a method call for the right object, interface & method
	if (request.type() == DBUS_MESSAGE_TYPE_METHOD_CALL && request.interface() == "org.opensuse.yast.SCR.Methods" && request.path() == "/SCR")
	{
	    std::string method(request.method());

	    YCPValue arg0;
	    YCPPath pth;

	    bool check_ok = false;

	    // check missing arguments
	    if (method == "Read" || method == "Write" || method == "Execute" ||
		method == "Dir" || method == "Error" || method == "UnregisterAgent" ||
		method == "UnmountAgent" || method == "RegisterAgent")
	    {
		if (request.arguments() == 0)
		{
		    // return an ERROR
		    reply.createError(request, "Missing arguments", DBUS_ERROR_INVALID_ARGS);
		}
		else
		{
		    arg0 = request.getYCPValue(0);

		    if (arg0.isNull() || !arg0->isPath())
		    {
			// return an ERROR
			reply.createError(request, "Expecting YCPPath as the first argument", DBUS_ERROR_INVALID_ARGS);
		    }
		    else
		    {
			pth = arg0->asPath();
			check_ok = true;
		    }
		}
	    }
	    else if (method == "UnregisterAllAgents" || method != "RegisterNewAgents")
	    {
		check_ok = true;
	    }

	    if (check_ok)
	    {
		YCPValue arg = request.getYCPValue(1);
		YCPValue opt = request.getYCPValue(2);

		std::string caller(request.sender());

#ifdef HAVE_POLKIT
		std::string arg_str, opt_str;
		
		if (!arg.isNull())
		{
		    arg_str = arg->toString();
		}

		if (!opt.isNull())
		{
		    opt_str = opt->toString();
		}

		// PolicyKit check
		if (!isActionAllowed(caller, pth->toString(), method, arg_str, opt_str))
		{
		    // access denied
		    reply.createError(request, "System policy does not allow you to do the action", DBUS_ERROR_ACCESS_DENIED);
		}
		else
#endif
		{
		    y2debug("Request from: %s", caller.c_str());
		    // remember the client
		    if (clients.find(caller) == clients.end())
		    {
			// insert the dbus name and PID
			Clients::value_type new_client(caller, callerPid(caller));
			y2milestone("Added new client %s (pid %d)", caller.c_str(), new_client.second);
			clients.insert(new_client);
		    }


		    YCPValue ret;

		    if (method == "Read")
			ret = sa->Read(pth, arg, opt);
		    else if (method == "Write")
			ret = sa->Write(pth, arg, opt);
		    else if (method == "Execute")
			ret = sa->Execute(pth, arg, opt);
		    else if (method == "Dir")
		    {
			ret = sa->Dir(pth);
			if (ret.isNull())
			    ret = YCPList();
		    }
		    else if (method == "Error")
			ret = sa->Error(pth);
		    else if (method == "UnregisterAgent")
			ret = sa->UnregisterAgent(pth);
		    else if (method == "UnregisterAllAgents")
			ret = sa->UnregisterAllAgents();
		    else if (method == "UnmountAgent")
			ret = sa->UnmountAgent(pth);
		    else if (method == "RegisterNewAgents")
			ret = sa->RegisterNewAgents();
		    else if (method == "RegisterAgent")
			ret = sa->RegisterAgent(pth, arg);
		    else
			y2internal("Unhandled method %s", method.c_str());

		    reply.createReply(request);

		    if (!ret.isNull())
		    {
			y2milestone("Result: %s", ret->toString().c_str());
			reply.addYCPValue(ret);
		    }
		    else
			reply.addYCPValue(YCPVoid());
		}
	    }
	}
	// handle Introspection request from "org.freedesktop.DBus.Introspectable" "Introspect"
	else if (request.isMethodCall(DBUS_INTERFACE_INTROSPECTABLE, "Introspect"))
	{
	    y2milestone("Requesting path: %s", request.path().c_str());
	    // define all exported methods here
	    const char *introspect = (request.path() != "/SCR") ?
// introcpection data for the root node
DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE
"<node>"
" <interface name='org.freedesktop.DBus.Introspectable'>"
"  <method name='Introspect'>"
"   <arg name='xml_data' type='s' direction='out'/>"
"  </method>"
" </interface>"
" <node name='SCR'/>"
"</node>" :

// introcpection data for SCR node
DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE
"<node>"
" <interface name='org.opensuse.yast.SCR.Methods'>"
"  <method name='Read'>"
"   <arg name='path' type='(bsv)' direction='in'/>"
"   <arg name='arg' type='(bsv)' direction='in'/>"
"   <arg name='opt' type='(bsv)' direction='in'/>"
"   <arg name='ret' type='(bsv)' direction='out'/>"
"  </method>"
"  <method name='Write'>"
"   <arg name='path' type='(bsv)' direction='in'/>"
"   <arg name='arg' type='(bsv)' direction='in'/>"
"   <arg name='opt' type='(bsv)' direction='in'/>"
"   <arg name='ret' type='(bsv)' direction='out'/>"
"  </method>"
"  <method name='Execute'>"
"   <arg name='path' type='(bsv)' direction='in'/>"
"   <arg name='arg' type='(bsv)' direction='in'/>"
"   <arg name='opt' type='(bsv)' direction='in'/>"
"   <arg name='ret' type='(bsv)' direction='out'/>"
"  </method>"
"  <method name='Dir'>"
"   <arg name='path' type='(bsv)' direction='in'/>"
"   <arg name='ret' type='(bsv)' direction='out'/>"
"  </method>"
"  <method name='Error'>"
"   <arg name='path' type='(bsv)' direction='in'/>"
"   <arg name='ret' type='(bsv)' direction='out'/>"
"  </method>"
"  <method name='UnregisterAgent'>"
"   <arg name='path' type='(bsv)' direction='in'/>"
"   <arg name='ret' type='(bsv)' direction='out'/>"
"  </method>"
"  <method name='UnregisterAllAgents'>"
"   <arg name='ret' type='(bsv)' direction='out'/>"
"  </method>"
"  <method name='RegisterNewAgents'>"
"   <arg name='ret' type='(bsv)' direction='out'/>"
"  </method>"
"  <method name='RegisterAgent'>"
"   <arg name='path' type='(bsv)' direction='in'/>"
"   <arg name='arg' type='(bsv)' direction='in'/>"
"   <arg name='ret' type='(bsv)' direction='out'/>"
"  </method>"
"  <method name='UnmountAgent'>"
"   <arg name='path' type='(bsv)' direction='in'/>"
"   <arg name='ret' type='(bsv)' direction='out'/>"
"  </method>"
" <interface name='org.freedesktop.DBus.Introspectable'>"
"  <method name='Introspect'>"
"   <arg name='xml_data' type='s' direction='out'/>"
"  </method>"
" </interface>"
" </interface>"
"</node>";

	    // create a reply to the request
	    reply.createReply(request);
	    reply.addString(introspect);
	}
	else if (request.type() == DBUS_MESSAGE_TYPE_METHOD_CALL)
	{
	    y2warning("Ignoring unknown interface or method call: interface: %s, method: %s",
		request.interface().c_str(), request.method().c_str());

	    // report error
	    reply.createError(request, "Unknown object, interface or method", DBUS_ERROR_UNKNOWN_METHOD);
	}
	else if (request.type() == DBUS_MESSAGE_TYPE_ERROR)
	{
	    DBusError error;
	    dbus_error_init(&error);

	    dbus_set_error_from_message(&error, request.getMessage());

	    if (dbus_error_is_set(&error))
	    {
		y2error("Received an error: %s: %s", error.name, error.message);
	    }

	    dbus_error_free(&error);
	}
	else if (request.type() == DBUS_MESSAGE_TYPE_SIGNAL)
	{
	    y2error("Received a signal: interface: %s method: %s", request.interface().c_str(), request.method().c_str());
	}

	// was a reply set?
	if (!reply.empty())
	{
	    // send the reply
	    if (!connection.send(reply))
	    {
		y2error("Cannot send the result");
	    }
	    else
	    {
		y2milestone("Flushing connection...");
		connection.flush();
		y2milestone("...done");
	    }
	}

	y2milestone("Message processed");
    }

    y2milestone("Finishing the DBus service");
}

#ifdef HAVE_POLKIT
bool DBusServer::isActionAllowed(const std::string &caller, const std::string &path, const std::string &method,
	    const std::string &arg, const std::string &opt)
{
    // create actionId
    static const char *polkit_prefix = "org.opensuse.yast.scr";

    // check the access right to all methods at first (see bnc#449794)
    std::string action_id(PolKit::createActionId(polkit_prefix, "", method, "", ""));

    if (policykit.isDBusUserAuthorized(action_id, caller, connection.getConnection()))
    {
	y2security("User is authorized to do action %s", action_id.c_str());
	return true;
    }
    else
    {
	y2debug("User is NOT authorized to do action %s", action_id.c_str());
    }

    action_id = PolKit::createActionId(polkit_prefix, path, method, arg, opt);

    bool ret = false;

    // check the policy here
    if (policykit.isDBusUserAuthorized(action_id, caller, connection.getConnection()))
    {
	y2security("User is authorized to do action %s", action_id.c_str());
	ret = true;
    }
    else
    {
	y2security("User is NOT authorized to do action %s", action_id.c_str());
    }

    return ret;
}
#endif


bool DBusServer::isProcessRunning(pid_t pid)
{
    ostringstream sstr;
    sstr << "/proc/" << pid;

    struct stat stat_result;
    bool ret = ::stat(sstr.str().c_str(), &stat_result) == 0;

    y2milestone("Process /proc/%d is running: %s", pid, ret ? "true" : "false");
    return ret;
}

pid_t DBusServer::callerPid(const std::string &bus_name)
{
    pid_t pid;
    DBusMsg query;

    // ask the DBus server for the PID of the caller
    query.createCall("org.freedesktop.DBus", "/org/freedesktop/DBus/Bus",
	"org.freedesktop.DBus", "GetConnectionUnixProcessID");

    query.addString(bus_name);

    // send the request
    DBusMsg reply(connection.call(query));
    
    // read the answer
    DBusMessageIter iter;
    dbus_message_iter_init(reply.getMessage(), &iter);

    int type = dbus_message_iter_get_arg_type(&iter);
    y2debug("Message type: %d, %c", type, (char)type);

    if (type == DBUS_TYPE_UINT32)
    {
	dbus_message_iter_get_basic(&iter, &pid);
    }
    else
    {
	y2internal("Unexpected type in PID reply %d (%c)", type, (char)type);
    }

    y2milestone("Message from PID %d", pid);

    return pid;
}