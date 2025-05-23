/*
 * Copyright (C) 2000-2017 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2006-2014 David Robillard <d@drobilla.net>
 * Copyright (C) 2006 Jesse Chappell <jesse@essej.net>
 * Copyright (C) 2007-2011 Carl Hetherington <carl@carlh.net>
 * Copyright (C) 2013-2015 John Emmas <john@creativepost.co.uk>
 * Copyright (C) 2013-2016 Tim Mayberry <mojofunk@gmail.com>
 * Copyright (C) 2013-2019 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2015 GZharun <grygoriiz@wavesglobal.com>
 * Copyright (C) 2016-2017 Julien "_FrnchFrgg_" RIVAUD <frnchfrgg@free.fr>
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
 */

#include <algorithm>
#include <cmath>
#include <vector>

#include <unistd.h>
#include <locale.h>
#include <errno.h>

#include <glibmm.h>
#include <glibmm/threads.h>

#include "pbd/xml++.h"
#include "pbd/replace_all.h"
#include "pbd/unknown_type.h"
#include "pbd/enumwriter.h"
#include "pbd/locale_guard.h"
#include "pbd/types_convert.h"

#include "ardour/audioengine.h"
#include "ardour/buffer.h"
#include "ardour/buffer_set.h"
#include "ardour/debug.h"
#include "ardour/io.h"
#include "ardour/port.h"
#include "ardour/profile.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/types_convert.h"
#include "ardour/user_bundle.h"

#include "pbd/i18n.h"

#define BLOCK_PROCESS_CALLBACK() Glib::Threads::Mutex::Lock em (AudioEngine::instance()->process_lock())

using namespace std;
using namespace ARDOUR;
using namespace PBD;

const string                 IO::state_node_name = "IO";
PBD::Signal<void(ChanCount)> IO::PortCountChanged;

static std::string
legalize_io_name (std::string n)
{
	replace_all (n, ":", "-");
	return n;
}

/** @param default_type The type of port that will be created by ensure_io
 * and friends if no type is explicitly requested (to avoid breakage).
 */
IO::IO (Session& s, const string& name, Direction dir, DataType default_type, bool sendish)
	: SessionObject (s, legalize_io_name (name))
	, _direction (dir)
	, _default_type (default_type)
	, _sendish (sendish)
	, _ports (new PortSet)
{
	_active = true;
	setup_bundle ();
}

IO::IO (Session& s, const XMLNode& node, DataType dt, bool sendish)
	: SessionObject(s, "unnamed io")
	, _direction (Input)
	, _default_type (dt)
	, _sendish (sendish)
	, _ports (new PortSet)
{
	_active = true;

	set_state (node, Stateful::loading_state_version);
	setup_bundle ();
}

IO::~IO ()
{
	DEBUG_TRACE (DEBUG::Ports, string_compose ("IO %1 unregisters %2 ports\n", name(), ports()->num_ports()));

	BLOCK_PROCESS_CALLBACK ();

	for (auto const& p : *ports()) {
		_session.engine().unregister_port (p);
	}
}

std::shared_ptr<PortSet>
IO::ports () {
	return std::const_pointer_cast<PortSet> (_ports.reader ());
}

std::shared_ptr<PortSet const>
IO::ports () const {
	return _ports.reader ();
}

void
IO::connection_change (std::shared_ptr<Port> a, std::shared_ptr<Port> b)
{
	if (_session.deletion_in_progress ()) {
		return;
	}
	/* Note:
	 * this could be called from within our own ::disconnect() method(s)
	 * or from somewhere that operates directly on a port. 
	 */
	std::shared_ptr<PortSet const> ports = _ports.reader();
	if (ports->contains (a) || ports->contains (b)) {
		changed (IOChange (IOChange::ConnectionsChanged), this); /* EMIT SIGNAL */
	}
}

void
IO::silence (samplecnt_t nframes)
{
	/* io_lock, not taken: function must be called from Session::process() calltree */

	for (auto const& p : *ports ()) {
		if (p->port_handle ()) {
			p->get_buffer (nframes).silence (nframes);
		}
	}
}

int
IO::disconnect (std::shared_ptr<Port> our_port, string other_port, void* src)
{
	if (other_port.length() == 0 || our_port == 0) {
		return 0;
	}

	/* check that our_port is really one of ours */
	if (!ports()->contains (our_port)) {
		return -1;
	}

	/* disconnect it from the source */

	DEBUG_TRACE (DEBUG::PortConnectIO,
	             string_compose("IO::disconnect %1 from %2\n", our_port->name(), other_port));

	if (our_port->disconnect (other_port)) {
		error << string_compose(_("IO: cannot disconnect port %1 from %2"), our_port->name(), other_port) << endmsg;
		return -1;
	}

	changed (IOChange (IOChange::ConnectionsChanged), src); /* EMIT SIGNAL */

	_session.set_dirty ();

	return 0;
}

int
IO::connect (std::shared_ptr<Port> our_port, string other_port, void* src)
{
	if (other_port.length() == 0 || our_port == 0) {
		return 0;
	}

	/* check that our_port is really one of ours */
	if (!ports()->contains (our_port)) {
		return -1;
	}

	/* connect it to the source */
	DEBUG_TRACE (DEBUG::PortConnectIO,
	             string_compose("IO::connect %1 to %2\n", our_port->name(), other_port));

	if (our_port->connect (other_port)) {
		return -1;
	}

	changed (IOChange (IOChange::ConnectionsChanged), src); /* EMIT SIGNAL */
	_session.set_dirty ();
	return 0;
}

bool
IO::can_add_port (DataType type) const
{
	switch (type) {
		case DataType::NIL:
			return false;
		case DataType::AUDIO:
			return true;
		case DataType::MIDI:
			return ports()->count ().n_midi() < 1;
	}
	abort(); /*NOTREACHED*/
	return false;
}

int
IO::remove_port (std::shared_ptr<Port> port, void* src)
{
	ChanCount before = ports()->count ();
	ChanCount after = before;
	after.set (port->type(), after.get (port->type()) - 1);

	std::optional<int> const r = PortCountChanging (after); /* EMIT SIGNAL */
	if (r.value_or (0)) {
		return -1;
	}

	IOChange change;

	{
		BLOCK_PROCESS_CALLBACK ();

		{
			RCUWriter<PortSet> writer (_ports);
			std::shared_ptr<PortSet> p = writer.get_copy ();

			if (p->remove (port)) {
				change.type = IOChange::Type (change.type | IOChange::ConfigurationChanged);
				change.before = before;
				change.after = p->count ();

				if (port->connected()) {
					change.type = IOChange::Type (change.type | IOChange::ConnectionsChanged);
				}

			}
			_session.engine().unregister_port (port);
		}

		PortCountChanged (n_ports()); /* EMIT SIGNAL */

		if (change.type != IOChange::NoChange) {
			changed (change, src);
			std::shared_ptr<PortSet const> ports = _ports.reader();
			_buffers.attach_buffers (*ports);
		}
	}

	if (change.type & IOChange::ConfigurationChanged) {
		setup_bundle ();
	}

	if (change.type == IOChange::NoChange) {
		return -1;
	}

	_session.set_dirty ();

	return 0;
}

/** Add a port.
 *
 * @param destination Name of port to connect new port to.
 * @param src Source for emitted ConfigurationChanged signal.
 * @param type Data type of port.  Default value (NIL) will use this IO's default type.
 */
int
IO::add_port (string destination, void* src, DataType type)
{
	std::shared_ptr<Port> our_port;

	if (type == DataType::NIL) {
		type = _default_type;
	}

	if (!can_add_port (type)) {
		return -1;
	}

	ChanCount before = ports()->count ();
	ChanCount after = before;
	after.set (type, after.get (type) + 1);

	std::optional<int> const r = PortCountChanging (after); /* EMIT SIGNAL */
	if (r.value_or (0)) {
		return -1;
	}

	IOChange change;

	{
		BLOCK_PROCESS_CALLBACK ();

		/* Create a new port */
		{
			RCUWriter<PortSet> writer (_ports);
			std::shared_ptr<PortSet> p = writer.get_copy ();
			change.before = p->count ();

			string portname = build_legal_port_name (p, type);

			if (_direction == Input) {
				if ((our_port = _session.engine().register_input_port (type, portname)) == 0) {
					error << string_compose(_("IO: cannot register input port %1"), portname) << endmsg;
					return -1;
				}
			} else {
				if ((our_port = _session.engine().register_output_port (type, portname)) == 0) {
					error << string_compose(_("IO: cannot register output port %1"), portname) << endmsg;
					return -1;
				}
			}

			p->add (our_port);
			change.after = p->count ();
		}

		PortCountChanged (n_ports()); /* EMIT SIGNAL */

		change.type = IOChange::ConfigurationChanged;
		changed (change, src); /* EMIT SIGNAL */
		_buffers.attach_buffers (*ports());
	}

	if (!destination.empty()) {
		if (our_port->connect (destination)) {
			return -1;
		}
	}

	apply_pretty_name ();
	setup_bundle ();
	_session.set_dirty ();

	return 0;
}

int
IO::disconnect (void* src)
{
	for (auto const& p : *ports()) {
		p->disconnect_all ();
	}

	changed (IOChange (IOChange::ConnectionsChanged), src); /* EMIT SIGNAL */

	return 0;
}

/** Caller must hold process lock */
int
IO::ensure_ports_locked (ChanCount count, bool clear, bool& changed)
{
#ifndef PLATFORM_WINDOWS
	assert (!AudioEngine::instance()->process_lock().trylock());
#endif

	std::shared_ptr<Port> port;

	changed = false;

	{
		RCUWriter<PortSet> writer (_ports);
		std::shared_ptr<PortSet> p = writer.get_copy ();

		for (DataType::iterator t = DataType::begin(); t != DataType::end(); ++t) {

			const size_t n = count.get (*t);

			const ChanCount n_ports = p->count ();

			/* remove unused ports */
			vector<std::shared_ptr<Port> > deleted_ports;
			for (size_t i = n_ports.get (*t); i > n; --i) {
					port = p->port (*t, i-1);

					assert (port);
					p->remove (port);

					/* hold a reference to the port so that we can ensure
					 * that this thread, and not a JACK notification thread,
					 * holds the final reference.
					 */

					deleted_ports.push_back (port);
					_session.engine().unregister_port (port);

					changed = true;
			}

			/* this will drop the final reference to the deleted ports,
			 * which will in turn call their destructors, which will in
			 * turn call the backend to unregister them.
			 *
			 * There will no connect/disconnect or register/unregister
			 * callbacks from the backend until we get here, because
			 * they are driven by the Port destructor. The destructor
			 * will not execute until we drop the final reference,
			 * which all happens right .... here.
			 */
			deleted_ports.clear ();

			/* create any necessary new ports */
			while (p->count ().get(*t) < n) {

				string portname = build_legal_port_name (p, *t);

				try {

					if (_direction == Input) {
						if ((port = _session.engine().register_input_port (*t, portname)) == 0) {
							error << string_compose(_("IO: cannot register input port %1"), portname) << endmsg;
							return -1;
						}
					} else {
						if ((port = _session.engine().register_output_port (*t, portname)) == 0) {
							error << string_compose(_("IO: cannot register output port %1"), portname) << endmsg;
							return -1;
						}
					}
				} catch (AudioEngine::PortRegistrationFailure& err) {
					/* pass it on */
					throw;
				}

				p->add (port);
				changed = true;
			}
		}
		/* end of RCUWriter scope */
	}


	if (changed) {
		const ChanCount n_ports = ports ()->count ();
		PortCountChanged (n_ports); /* EMIT SIGNAL */
		_session.set_dirty ();
		_ports.flush ();
	}

	if (clear) {
		/* disconnect all existing ports so that we get a fresh start */
		for (auto const& p : *ports ()) {
			p->disconnect_all ();
		}
	}

	return 0;
}

/** Caller must hold process lock */
int
IO::ensure_ports (ChanCount count, bool clear, void* src)
{
#ifndef PLATFORM_WINDOWS
	assert (!AudioEngine::instance()->process_lock().trylock());
#endif

	if (count == n_ports() && !clear) {
		return 0;
	}

	bool changed = false;
	IOChange change;

	change.before = ports()->count ();

	if (ensure_ports_locked (count, clear, changed)) {
		return -1;
	}

	if (changed) {
		change.after = ports()->count ();
		change.type = IOChange::ConfigurationChanged;
		this->changed (change, src); /* EMIT SIGNAL */
		_buffers.attach_buffers (*ports());
		setup_bundle ();
		_session.set_dirty ();
	}

	return 0;
}

void
IO::reestablish_port_subscriptions ()
{
	_port_connections.drop_connections ();
	for (auto const& p : *ports ()) {
		p->ConnectedOrDisconnected.connect_same_thread (*this, std::bind (&IO::connection_change, this, _1, _2));
	}
}

/** Caller must hold process lock */
int
IO::ensure_io (ChanCount count, bool clear, void* src)
{
#ifndef PLATFORM_WINDOWS
	assert (!AudioEngine::instance()->process_lock().trylock());
#endif

	return ensure_ports (count, clear, src);
}

XMLNode&
IO::get_state () const
{
	return state ();
}

XMLNode&
IO::state () const
{
	XMLNode* node = new XMLNode (state_node_name);

	node->set_property ("name", name());
	node->set_property ("id", id ());
	node->set_property ("direction", _direction);
	node->set_property ("default-type", _default_type);

	if (!_pretty_name_prefix.empty ()) {
		node->set_property("pretty-name", _pretty_name_prefix);
	}

	for (auto const& p : *_ports.reader ()) {
		node->add_child_nocopy (p->get_state ());
	}

	return *node;
}

int
IO::set_state (const XMLNode& node, int version)
{
	/* callers for version < 3000 need to call set_state_2X directly, as A3 IOs
	 * are input OR output, not both, so the direction needs to be specified
	 * by the caller.
	 */
	assert (version >= 3000);

	/* force use of non-localized representation of decimal point,
	   since we use it a lot in XML files and so forth.
	*/

	if (node.name() != state_node_name) {
		error << string_compose(_("incorrect XML node \"%1\" passed to IO object"), node.name()) << endmsg;
		return -1;
	}

	bool ignore_name = node.property ("ignore-name");
	std::string name;
	if (node.get_property ("name", name) && !ignore_name) {
		set_name (name);
	}

	if (node.get_property (X_("default-type"), _default_type)) {
		assert(_default_type != DataType::NIL);
	}

	set_id (node);

	node.get_property ("direction", _direction);

	if (create_ports (node, version)) {
		return -1;
	}

	if (_sendish && _direction == Output) {
		/* ignore <Port name="...">  from XML for sends, but use the names
		 * ::ensure_ports_locked() creates port using ::build_legal_port_name()
		 * This is needed to properly restore connections when creating
		 * external sends from templates because the IO name changes.
		 */
	std::shared_ptr<PortSet const> ports = _ports.reader ();

		PortSet::const_iterator i = ports->begin();
		XMLNodeConstIterator    x = node.children().begin();
		for (; i != ports->end() && x != node.children().end(); ++i, ++x) {
			if ((*x)->name() == "Port") {
				(*x)->remove_property (X_("name"));
				(*x)->set_property (X_("name"), i->name());
			}
		}
	}

	// after create_ports, updates names

	if (node.get_property ("pretty-name", name)) {
		set_pretty_name (name);
	}

	/* now set port state (this will *not* connect them, but will store
	   the names of connected ports).
	 */

	if (version < 3000) {
		return set_port_state_2X (node, version, false);
	}

	XMLProperty const * prop;

	for (XMLNodeConstIterator i = node.children().begin(); i != node.children().end(); ++i) {

		if ((*i)->name() == "Port") {

			prop = (*i)->property (X_("name"));

			if (!prop) {
				continue;
			}

			std::shared_ptr<Port> p = port_by_name (prop->value());

			if (p) {
				p->set_state (**i, version);

				if (!_session.inital_connect_or_deletion_in_progress ()) {
					/* re-apply connection if create_ports(), ensure_ports()
					 * disconnected the port
					 */
					p->reconnect ();
				}
			}
		}
	}

	return 0;
}

int
IO::set_state_2X (const XMLNode& node, int version, bool in)
{
	XMLProperty const * prop;
	LocaleGuard lg;

	/* force use of non-localized representation of decimal point,
	   since we use it a lot in XML files and so forth.
	*/

	if (node.name() != state_node_name) {
		error << string_compose(_("incorrect XML node \"%1\" passed to IO object"), node.name()) << endmsg;
		return -1;
	}

	if ((prop = node.property ("name")) != 0) {
		set_name (prop->value());
	}

	if ((prop = node.property (X_("default-type"))) != 0) {
		_default_type = DataType(prop->value());
		assert(_default_type != DataType::NIL);
	}

	set_id (node);

	_direction = in ? Input : Output;

	if (create_ports (node, version)) {
		return -1;
	}

	if (set_port_state_2X (node, version, in)) {
		return -1;
	}

	return 0;
}

std::shared_ptr<Bundle>
IO::find_possible_bundle (const string &desired_name)
{
	static const string digits = "0123456789";
	const string &default_name = (_direction == Input ? _("in") : _("out"));
	const string &bundle_type_name = (_direction == Input ? _("input") : _("output"));

	std::shared_ptr<Bundle> c = _session.bundle_by_name (desired_name);

	if (!c) {
		int bundle_number, mask;
		string possible_name;
		bool stereo = false;
		string::size_type last_non_digit_pos;
		std::string bundle_number_str;

		error << string_compose(_("Unknown bundle \"%1\" listed for %2 of %3"), desired_name, bundle_type_name, _name)
		      << endmsg;

		// find numeric suffix of desired name
		bundle_number = 0;

		last_non_digit_pos = desired_name.find_last_not_of(digits);

		if (last_non_digit_pos != string::npos) {
			bundle_number_str = desired_name.substr(last_non_digit_pos);
			bundle_number = string_to<int32_t>(bundle_number_str);
		}

		// see if it's a stereo connection e.g. "in 3+4"

		if (last_non_digit_pos > 1 && desired_name[last_non_digit_pos] == '+') {
			string::size_type left_last_non_digit_pos;

			left_last_non_digit_pos = desired_name.find_last_not_of(digits, last_non_digit_pos-1);

			if (left_last_non_digit_pos != string::npos) {
				int left_bundle_number = 0;
				bundle_number_str = desired_name.substr(left_last_non_digit_pos, last_non_digit_pos-1);
				left_bundle_number = string_to<int32_t>(bundle_number_str);

				if (left_bundle_number > 0 && left_bundle_number + 1 == bundle_number) {
					bundle_number--;
					stereo = true;
				}
			}
		}

		// make 0-based
		if (bundle_number)
			bundle_number--;

		// find highest set bit
		mask = 1;
		while ((mask <= bundle_number) && (mask <<= 1)) {}

		// "wrap" bundle number into largest possible power of 2
		// that works...

		while (mask) {

			if (bundle_number & mask) {
				bundle_number &= ~mask;

				std::string possible_name = default_name + " " + to_string(bundle_number + 1);

				if (stereo) {
					possible_name += "+" + to_string(bundle_number + 2);
				}

				if ((c = _session.bundle_by_name (possible_name)) != 0) {
					break;
				}
			}
			mask >>= 1;
		}
		if (c) {
			info << string_compose (_("Bundle %1 was not available - \"%2\" used instead"), desired_name, possible_name)
			     << endmsg;
		} else {
			error << string_compose(_("No %1 bundles available as a replacement"), bundle_type_name)
			      << endmsg;
		}

	}

	return c;

}

int
IO::get_port_counts_2X (XMLNode const & node, int /*version*/, ChanCount& n, std::shared_ptr<Bundle>& /*c*/)
{
	XMLProperty const * prop;
	XMLNodeList children = node.children ();

	uint32_t n_audio = 0;

	for (XMLNodeIterator i = children.begin(); i != children.end(); ++i) {

		if ((prop = node.property ("inputs")) != 0 && _direction == Input) {
			n_audio = count (prop->value().begin(), prop->value().end(), '{');
		} else if ((prop = node.property ("input-connection")) != 0 && _direction == Input) {
			n_audio = 1;
		} else if ((prop = node.property ("outputs")) != 0 && _direction == Output) {
			n_audio = count (prop->value().begin(), prop->value().end(), '{');
		} else if ((prop = node.property ("output-connection")) != 0 && _direction == Output) {
			n_audio = 2;
		}
	}

	ChanCount cnt;
	cnt.set_audio (n_audio);
	n = ChanCount::max (n, cnt);

	return 0;
}

int
IO::get_port_counts (const XMLNode& node, int version, ChanCount& n, std::shared_ptr<Bundle>& c)
{
	if (version < 3000) {
		return get_port_counts_2X (node, version, n, c);
	}

	XMLProperty const * prop;
	XMLNodeConstIterator iter;
	uint32_t n_audio = 0;
	uint32_t n_midi = 0;
	ChanCount cnt;

	n = n_ports();

	if ((prop = node.property ("connection")) != 0) {

		if ((c = find_possible_bundle (prop->value())) != 0) {
			n = ChanCount::max (n, c->nchannels());
		}
		return 0;
	}

	for (iter = node.children().begin(); iter != node.children().end(); ++iter) {

		if ((*iter)->name() == X_("Bundle")) {
			prop = (*iter)->property ("name");
			if ((c = find_possible_bundle (prop->value())) != 0) {
				n = ChanCount::max (n, c->nchannels());
				return 0;
			} else {
				return -1;
			}
		}

		if ((*iter)->name() == X_("Port")) {
			prop = (*iter)->property (X_("type"));

			if (!prop) {
				continue;
			}

			if (prop->value() == X_("audio")) {
				cnt.set_audio (++n_audio);
			} else if (prop->value() == X_("midi")) {
				cnt.set_midi (++n_midi);
			}
		}
	}

	n = ChanCount::max (n, cnt);
	return 0;
}

int
IO::create_ports (const XMLNode& node, int version)
{
	ChanCount n;
	std::shared_ptr<Bundle> c;

	get_port_counts (node, version, n, c);

	{
		Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock ());

		if (ensure_ports (n, !_session.inital_connect_or_deletion_in_progress (), this)) {
			error << string_compose(_("%1: cannot create I/O ports"), _name) << endmsg;
			return -1;
		}
	}

	/* XXX use c */

	return 0;
}

int
IO::set_port_state_2X (const XMLNode& node, int /*version*/, bool in)
{
	XMLProperty const * prop;

	/* XXX: bundles ("connections" as was) */

	if ((prop = node.property ("inputs")) != 0 && in) {

		string::size_type ostart = 0;
		string::size_type start = 0;
		string::size_type end = 0;
		int i = 0;
		int n;
		vector<string> ports;

		string const str = prop->value ();

		while ((start = str.find_first_of ('{', ostart)) != string::npos) {
			start += 1;

			if ((end = str.find_first_of ('}', start)) == string::npos) {
				error << string_compose(_("IO: badly formed string in XML node for inputs \"%1\""), str) << endmsg;
				return -1;
			}

			if ((n = parse_io_string (str.substr (start, end - start), ports)) < 0) {
				error << string_compose(_("bad input string in XML node \"%1\""), str) << endmsg;

				return -1;

			} else if (n > 0) {


				for (int x = 0; x < n; ++x) {
					/* XXX: this is a bit of a hack; need to check if it's always valid */
					string::size_type const p = ports[x].find ("/out");
					if (p != string::npos) {
						ports[x].replace (p, 4, "/audio_out");
					}
					if (NULL != nth(i).get())
						nth(i)->connect (ports[x]);
				}
			}

			ostart = end+1;
			i++;
		}

	}

	if ((prop = node.property ("outputs")) != 0 && !in) {

		string::size_type ostart = 0;
		string::size_type start = 0;
		string::size_type end = 0;
		int i = 0;
		int n;
		vector<string> ports;

		string const str = prop->value ();

		while ((start = str.find_first_of ('{', ostart)) != string::npos) {
			start += 1;

			if ((end = str.find_first_of ('}', start)) == string::npos) {
				error << string_compose(_("IO: badly formed string in XML node for outputs \"%1\""), str) << endmsg;
				return -1;
			}

			if ((n = parse_io_string (str.substr (start, end - start), ports)) < 0) {
				error << string_compose(_("IO: bad output string in XML node \"%1\""), str) << endmsg;

				return -1;

			} else if (n > 0) {

				for (int x = 0; x < n; ++x) {
					/* XXX: this is a bit of a hack; need to check if it's always valid */
					string::size_type const p = ports[x].find ("/in");
					if (p != string::npos) {
						ports[x].replace (p, 3, "/audio_in");
					}
					if (NULL != nth(i).get())
						nth(i)->connect (ports[x]);
				}
			}

			ostart = end+1;
			i++;
		}
	}

	return 0;
}

void
IO::prepare_for_reset (XMLNode& node, const std::string& name)
{
	/* reset name */
	node.set_property ("name", name);

	/* now find connections and reset the name of the port
	   in one so that when we re-use it it will match
	   the name of the thing we're applying it to.
	*/

	XMLProperty * prop;
	XMLNodeList children = node.children();

	for (XMLNodeIterator i = children.begin(); i != children.end(); ++i) {

		if ((*i)->name() == "Port") {

			prop = (*i)->property (X_("name"));

			if (prop) {
				string new_name;
				string old = prop->value();
				string::size_type slash = old.find ('/');

				if (slash != string::npos) {
					/* port name is of form: <IO-name>/<port-name> */

					new_name = name;
					new_name += old.substr (old.find ('/'));

					prop->set_value (new_name);
				}
			}
		}
	}
}



int
IO::set_ports (const string& str)
{
	vector<string> ports;
	int n;
	uint32_t nports;

	if ((nports = count (str.begin(), str.end(), '{')) == 0) {
		return 0;
	}

	{
		Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock ());

		// FIXME: audio-only
		if (ensure_ports (ChanCount(DataType::AUDIO, nports), true, this)) {
			return -1;
		}
	}

	string::size_type start  = 0;
	string::size_type end    = 0;
	string::size_type ostart = 0;
	for (int i = 0; (start = str.find_first_of ('{', ostart)) != string::npos; ++i) {
		start += 1;

		if ((end = str.find_first_of ('}', start)) == string::npos) {
			error << string_compose(_("IO: badly formed string in XML node for inputs \"%1\""), str) << endmsg;
			return -1;
		}

		if ((n = parse_io_string (str.substr (start, end - start), ports)) < 0) {
			error << string_compose(_("bad input string in XML node \"%1\""), str) << endmsg;

			return -1;

		} else if (n > 0) {

			for (int x = 0; x < n; ++x) {
				connect (nth (i), ports[x], this);
			}
		}

		ostart = end+1;
	}

	return 0;
}

int
IO::parse_io_string (const string& str, vector<string>& ports)
{
	string::size_type pos, opos;

	if (str.length() == 0) {
		return 0;
	}

	opos = 0;

	ports.clear ();

	while ((pos = str.find_first_of (',', opos)) != string::npos) {
		ports.push_back (str.substr (opos, pos - opos));
		opos = pos + 1;
	}

	if (opos < str.length()) {
		ports.push_back (str.substr(opos));
	}

	return ports.size();
}

int
IO::parse_gain_string (const string& str, vector<string>& ports)
{
	string::size_type pos, opos;

	opos = 0;
	ports.clear ();

	while ((pos = str.find_first_of (',', opos)) != string::npos) {
		ports.push_back (str.substr (opos, pos - opos));
		opos = pos + 1;
	}

	if (opos < str.length()) {
		ports.push_back (str.substr(opos));
	}

	return ports.size();
}

bool
IO::set_name (const string& requested_name)
{
	string name = requested_name;

	if (_name == name) {
		return true;
	}

	/* replace all colons in the name. i wish we didn't have to do this */

	name = legalize_io_name (name);

	for (auto const& p : *ports ()) {
		string current_name = p->name();
		assert (current_name.find (_name) != std::string::npos);
		current_name.replace (current_name.find (_name), _name.val().length(), name);
		p->set_name (current_name);
	}

	bool const r = SessionObject::set_name (name);

	setup_bundle ();

	return r;
}

void
IO::set_pretty_name (const std::string& str)
{
	if (_pretty_name_prefix == str) {
		return;
	}
	_pretty_name_prefix = str;
	apply_pretty_name ();
}

void
IO::apply_pretty_name ()
{
	uint32_t pn = 1;
	if (_pretty_name_prefix.empty ()) {
		return;
	}
	for (auto const& p : *ports ()) {
		p->set_pretty_name (string_compose (("%1/%2 %3"),
		                                    _pretty_name_prefix,
		                                    _direction == Output ? S_("IO|Out") : S_("IO|In"),
		                                    pn++));
	}
}

void
IO::set_private_port_latencies (samplecnt_t value, bool playback)
{
	LatencyRange lat;
	lat.min = lat.max = value;
	for (auto const& p : *ports ()) {
		 p->set_private_latency_range (lat, playback);
	}
}

void
IO::set_public_port_latency_from_connections () const
{
	/* get min/max of connected up/downstream ports */
	bool connected = false;
	bool playback = _direction == Output;
	LatencyRange lr;
	lr.min = ~((pframes_t) 0);
	lr.max = 0;

	std::shared_ptr<PortSet const> ps = ports ();

	for (auto const& p : *ps) {
		if (p->connected()) {
			connected = true;
		}
		p->collect_latency_from_backend (lr, playback);
	}

	if (!connected) {
		/* if output is not connected to anything, use private latency */
		lr.min = lr.max = latency ();
	}

	for (auto const& p : *ps) {
		 p->set_public_latency_range (lr, playback);
	}
}

void
IO::set_public_port_latencies (samplecnt_t value, bool playback) const
{
	LatencyRange lat;
	lat.min = lat.max = value;
	for (auto const& p : *_ports.reader ()) {
		 p->set_public_latency_range (lat, playback);
	}
}

samplecnt_t
IO::latency () const
{
	samplecnt_t max_latency = 0;

	for (auto const& p : *_ports.reader ()) {
		samplecnt_t latency;
		if ((latency = p->private_latency_range (_direction == Output).max) > max_latency) {
			DEBUG_TRACE (DEBUG::LatencyIO, string_compose ("port %1 has %2 latency of %3 - use\n",
			                                               name(),
			                                               ((_direction == Output) ? "PLAYBACK" : "CAPTURE"),
			                                               latency));
			max_latency = latency;
		}
	}

	DEBUG_TRACE (DEBUG::LatencyIO, string_compose ("%1: max %4 latency from %2 ports = %3\n",
	                                               name(), ports()->num_ports(), max_latency,
	                                               ((_direction == Output) ? "PLAYBACK" : "CAPTURE")));
	return max_latency;
}

#if 0 // not used, but may some day be handy for debugging
samplecnt_t
IO::public_latency () const
{
	samplecnt_t max_latency = 0;

	/* io lock not taken - must be protected by other means */

	for (PortSet::const_iterator i = _ports.begin(); i != _ports.end(); ++i) {
		samplecnt_t latency;
		if ((latency = i->public_latency_range (_direction == Output).max) > max_latency) {
			DEBUG_TRACE (DEBUG::LatencyIO, string_compose ("port %1 has %2 latency of %3 - use\n",
			                                               name(),
			                                               ((_direction == Output) ? "PLAYBACK" : "CAPTURE"),
			                                               latency));
			max_latency = latency;
		}
	}

	DEBUG_TRACE (DEBUG::LatencyIO, string_compose ("%1: max %4 public latency from %2 ports = %3\n",
	                                               name(), _ports.num_ports(), max_latency,
	                                               ((_direction == Output) ? "PLAYBACK" : "CAPTURE")));
	return max_latency;
}
#endif

samplecnt_t
IO::connected_latency (bool for_playback) const
{
	/* may be called concurrently with processing via
	 *
	 * Session::auto_connect_thread_run ()
	 * -> Session::update_latency_compensation ()
	 * -> Session::update_route_latency ()
	 * -> Route::update_signal_latency ()
	 * -> IO::connected_latency ()
	 */
	std::shared_ptr<PortSet const> ps = ports ();

	samplecnt_t max_latency = 0;
	bool connected = false;

	/* if output is not connected to anything, use private latency */
	for (auto const& p : *ps) {
		if (p->connected()) {
			connected = true;
			max_latency = 0;
			break;
		}
		samplecnt_t latency;
		if ((latency = p->private_latency_range (for_playback).max) > max_latency) {
			max_latency = latency;
		}
	}
	if (connected) {
		for (auto const& p : *ps) {
			LatencyRange lr;
			p->get_connected_latency_range (lr, for_playback);
			if (lr.max > max_latency) {
				max_latency = lr.max;
			}
		}
	}
	return max_latency;
}

int
IO::connect_ports_to_bundle (std::shared_ptr<Bundle> c, bool exclusive, void* src) {
	return connect_ports_to_bundle(c, exclusive, false, src);
}

int
IO::connect_ports_to_bundle (std::shared_ptr<Bundle> c, bool exclusive,
                             bool allow_partial, void* src)
{
	BLOCK_PROCESS_CALLBACK ();

	if (exclusive) {
		for (auto const& p : *ports ()) {
			p->disconnect_all ();
		}
	}

	c->connect (_bundle, _session.engine(), allow_partial);

	changed (IOChange (IOChange::ConnectionsChanged), src); /* EMIT SIGNAL */
	return 0;
}

int
IO::disconnect_ports_from_bundle (std::shared_ptr<Bundle> c, void* src)
{
	BLOCK_PROCESS_CALLBACK ();

	c->disconnect (_bundle, _session.engine());

	/* If this is a UserBundle, make a note of what we've done */

	changed (IOChange (IOChange::ConnectionsChanged), src); /* EMIT SIGNAL */
	return 0;
}

void
IO::bundle_changed (Bundle::Change /*c*/)
{
}


string
IO::build_legal_port_name (std::shared_ptr<PortSet const> ports, DataType type)
{
	int limit;
	string suffix;

	if (type == DataType::AUDIO) {
		suffix = X_("audio");
	} else if (type == DataType::MIDI) {
		suffix = X_("midi");
	} else {
		throw unknown_type();
	}

	/* note that if "in" or "out" are translated it will break a session
	   across locale switches because a port's connection list will
	   show (old) translated names, but the current port name will
	   use the (new) translated name.
	*/

	if (_sendish) {
		if (_direction == Input) {
			suffix += X_("_return");
		} else {
			suffix += X_("_send");
		}
	} else {
		if (_direction == Input) {
			suffix += X_("_in");
		} else {
			suffix += X_("_out");
		}
	}

	// allow up to 4 digits for the output port number, plus the slash, suffix and extra space

	uint32_t name_size = AudioEngine::instance()->port_name_size();
	limit = name_size - AudioEngine::instance()->my_name().length() - (suffix.length() + 5);

	++name_size; // allow for \0

	std::unique_ptr<char[]> buf1 (new char[name_size]);
	std::unique_ptr<char[]> buf2 (new char[name_size]);

	/* colons are illegal in port names, so fix that */

	string nom = legalize_io_name (_name.val());

	std::snprintf (buf1.get(), name_size, ("%.*s/%s"), limit, nom.c_str(), suffix.c_str());

	int port_number = find_port_hole (ports, buf1.get ());
	std::snprintf (buf2.get(), name_size, "%s %d", buf1.get (), port_number);

	return string (buf2.get ());
}

int32_t
IO::find_port_hole (std::shared_ptr<PortSet const> ports, const char* base)
{
	/* CALLER MUST HOLD IO LOCK */

	uint32_t n;

	if (ports->empty()) {
		return 1;
	}

	uint32_t const name_size = AudioEngine::instance()->port_name_size() + 1;

	/* we only allow up to 4 characters for the port number */
	for (n = 1; n < 9999; ++n) {
		PortSet::const_iterator i = ports->begin ();

		std::unique_ptr<char[]> buf (new char[name_size]);
		std::snprintf (buf.get (), name_size, "%s %u", base, n);

		for ( ; i != ports->end (); ++i) {
			if (string (i->name()) == string (buf.get ())) {
				break;
			}
		}

		if (i == ports->end()) {
			break;
		}
	}
	return n;
}

std::shared_ptr<AudioPort>
IO::audio(uint32_t n) const
{
	return ports()->nth_audio_port (n);
}

std::shared_ptr<MidiPort>
IO::midi(uint32_t n) const
{
	return ports()->nth_midi_port (n);
}

/**
 *  Setup a bundle that describe our inputs or outputs. Also creates the bundle if necessary.
 */
void
IO::setup_bundle ()
{
	if (!_bundle) {
		_bundle.reset (new Bundle (_direction == Input));
	}

	_bundle->suspend_signals ();

	_bundle->remove_channels ();

	_bundle->set_name (string_compose ("%1 %2", _name, _direction == Input ? _("in") : _("out")));

	std::shared_ptr<PortSet const> ports = _ports.reader();

	int c = 0;
	for (DataType::iterator i = DataType::begin(); i != DataType::end(); ++i) {
		uint32_t const N = ports->count().get (*i);
		for (uint32_t j = 0; j < N; ++j) {
			_bundle->add_channel (bundle_channel_name (j, N, *i), *i);
			_bundle->set_port (c, _session.engine().make_port_name_non_relative (ports->port (*i, j)->name()));
			++c;
		}
	}

	reestablish_port_subscriptions ();

	_bundle->resume_signals ();
}

/** @return Bundles connected to our ports */
BundleList
IO::bundles_connected ()
{
	BundleList bundles;

	/* Session bundles */
	std::shared_ptr<ARDOUR::BundleList const> b = _session.bundles ();
	for (auto const& i : *b) {
		if (i->connected_to (_bundle, _session.engine())) {
			bundles.push_back (i);
		}
	}

	/* Route bundles */

	std::shared_ptr<ARDOUR::RouteList const> r = _session.get_routes ();

	if (_direction == Input) {
		for (auto const& i : *r) {
			if (i->output()->bundle()->connected_to (_bundle, _session.engine())) {
				bundles.push_back (i->output()->bundle());
			}
		}
	} else {
		for (auto const& i : *r) {
			if (i->input()->bundle()->connected_to (_bundle, _session.engine())) {
				bundles.push_back (i->input()->bundle());
			}
		}
	}

	return bundles;
}


IO::UserBundleInfo::UserBundleInfo (IO* io, std::shared_ptr<UserBundle> b)
{
	bundle = b;
	b->Changed.connect_same_thread (changed, std::bind (&IO::bundle_changed, io, _1));
}

std::string
IO::bundle_channel_name (uint32_t c, uint32_t n, DataType t) const
{
	char buf[32];

	if (t == DataType::AUDIO) {

		if (n == _audio_channel_names.size () && c < _audio_channel_names.size ()) {
			return _audio_channel_names.at (c);
		}

		switch (n) {
		case 1:
			return _("mono");
		case 2:
			return c == 0 ? _("L") : _("R");
		default:
			std::snprintf (buf, sizeof(buf), "%d", (c + 1));
			return buf;
		}

	} else {

		std::snprintf (buf, sizeof(buf), "%d", (c + 1));
		return buf;

	}

	return "";
}

string
IO::name_from_state (const XMLNode& node)
{
	XMLProperty const * prop;

	if ((prop = node.property ("name")) != 0) {
		return prop->value();
	}

	return string();
}

void
IO::set_name_in_state (XMLNode& node, const string& new_name)
{
	node.set_property (X_("name"), new_name);
	XMLNodeList children = node.children ();
	for (XMLNodeIterator i = children.begin(); i != children.end(); ++i) {
		if ((*i)->name() == X_("Port")) {
			string const old_name = (*i)->property(X_("name"))->value();
			string const old_name_second_part = old_name.substr (old_name.find_first_of ("/") + 1);
			(*i)->set_property (X_("name"), string_compose ("%1/%2", new_name, old_name_second_part));
		}
	}
}

bool
IO::connected () const
{
	for (auto const& p : *_ports.reader ()) {
		if (p->connected()) {
			return true;
		}
	}

	return false;
}

bool
IO::connected_to (std::shared_ptr<const IO> other) const
{
	if (!other) {
		return connected ();
	}

	assert (_direction != other->direction());

	uint32_t i, j;
	uint32_t no = n_ports().n_total();
	uint32_t ni = other->n_ports ().n_total();

	for (i = 0; i < no; ++i) {
		for (j = 0; j < ni; ++j) {
			std::shared_ptr<Port> pa (nth(i));
			std::shared_ptr<Port> pb (other->nth(j));
			if (pa && pb && pa->connected_to (pb->name())) {
				return true;
			}
		}
	}

	return false;
}

bool
IO::connected_to (const string& str) const
{
	for (auto const& p : *_ports.reader ()) {
		if (p->connected_to (str)) {
			return true;
		}
	}
	return false;
}

void
IO::collect_input (BufferSet& bufs, pframes_t nframes, ChanCount offset)
{
	std::shared_ptr<PortSet> ps = ports ();

	assert (bufs.available() >= ps->count());

	if (ps->count() == ChanCount::ZERO) {
		return;
	}

	bufs.set_count (ps->count());

	for (DataType::iterator t = DataType::begin(); t != DataType::end(); ++t) {
		PortSet::iterator   i = ps->begin (*t);
		BufferSet::iterator b = bufs.begin (*t);

		for (uint32_t off = 0; off < offset.get(*t); ++off, ++b) {
			if (b == bufs.end(*t)) {
				continue;
			}
		}

		for ( ; i != ps->end (*t); ++i, ++b) {
			const Buffer& bb (i->get_buffer (nframes));
			b->read_from (bb, nframes);
		}
	}
}

void
IO::copy_to_outputs (BufferSet& bufs, DataType type, pframes_t nframes, samplecnt_t offset)
{
	std::shared_ptr<PortSet> ps = ports ();

	PortSet::iterator   o = ps->begin (type);
	BufferSet::iterator i = bufs.begin (type);
	BufferSet::iterator prev = i;

	assert(i != bufs.end(type)); // or second loop will crash

	/* Copy any buffers 1:1 to outputs */
	while (i != bufs.end (type) && o != ps->end (type)) {
		Buffer& port_buffer (o->get_buffer (nframes));
		port_buffer.read_from (*i, nframes, offset);
		prev = i;
		++i;
		++o;
	}

	/* Copy last buffer to any extra outputs */
	while (o != ps->end (type)) {
		Buffer& port_buffer (o->get_buffer (nframes));
		port_buffer.read_from (*prev, nframes, offset);
		++o;
	}
}

void
IO::flush_buffers (pframes_t nframes)
{
	/* when port is both externally and internally connected,
	 * make data available to downstream internal ports */
	for (auto const& p : *ports ()) {
		p->flush_buffers (nframes);
	}
}

std::shared_ptr<Port>
IO::port_by_name (const std::string& str) const
{
	/* to be called only from ::set_state() - no locking */

	for (auto const& p : *_ports.reader ()) {
		if (p->name() == str) {
			return std::const_pointer_cast<Port> (p);
		}
	}
	return std::shared_ptr<Port> ();
}

bool
IO::physically_connected () const
{
	for (auto const& p : *_ports.reader ()) {
		if (p->physically_connected()) {
			return true;
		}
	}

	return false;
}

bool
IO::has_ext_connection () const
{
	for (auto const& p : *_ports.reader ()) {
		if (p->has_ext_connection()) {
			return true;
		}
	}
	return false;
}

bool
IO::has_port (std::shared_ptr<Port> p) const
{
	return ports()->contains (p);
}

std::shared_ptr<Port>
IO::nth (uint32_t n) const {
	std::shared_ptr<PortSet const> ports = _ports.reader ();
	if (n < ports->num_ports ()) {
		return ports->port (n);
	} else {
		return std::shared_ptr<Port> ();
	}
}

const ChanCount&
IO::n_ports () const {
	return ports()->count();
}
