/*
 * Copyright (C) 2006-2011 David Robillard <d@drobilla.net>
 * Copyright (C) 2006-2017 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2007-2011 Carl Hetherington <carl@carlh.net>
 * Copyright (C) 2016-2019 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2017 Julien "_FrnchFrgg_" RIVAUD <frnchfrgg@free.fr>
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

#pragma once

#include <string>
#include <vector>
#include <cmath>

#include <glibmm/threads.h>

#include "pbd/fastlog.h"
#include "pbd/undo.h"
#include "pbd/rcu.h"
#include "pbd/statefuldestructible.h"
#include "pbd/controllable.h"
#include "pbd/enum_convert.h"

#include "ardour/ardour.h"
#include "ardour/automation_control.h"
#include "ardour/bundle.h"
#include "ardour/chan_count.h"
#include "ardour/data_type.h"
#include "ardour/latent.h"
#include "ardour/port_set.h"
#include "ardour/session_object.h"
#include "ardour/libardour_visibility.h"
#include "ardour/types.h"
#include "ardour/utils.h"
#include "ardour/buffer_set.h"

class XMLNode;

namespace ARDOUR {

class Amp;
class AudioEngine;
class AudioPort;
class Bundle;
class MidiPort;
class PeakMeter;
class Port;
class Processor;
class Session;
class UserBundle;

/** A collection of ports (all input or all output) with connections.
 *
 * An IO can contain ports of varying types, making routes/inserts/etc with
 * varied combinations of types (eg MIDI and audio) possible.
 */
class LIBARDOUR_API IO : public SessionObject
{
public:
	static const std::string state_node_name;

	enum Direction {
		Input,
		Output
	};

	IO (Session&, const std::string& name, Direction, DataType default_type = DataType::AUDIO, bool sendish = false);
	IO (Session&, const XMLNode&, DataType default_type = DataType::AUDIO, bool sendish = false);

	virtual ~IO();

	Direction direction() const { return _direction; }

	DataType default_type() const         { return _default_type; }
	void     set_default_type(DataType t) { _default_type = t; }

	bool active() const { return _active; }
	void set_active(bool yn) { _active = yn; }

	bool set_name (const std::string& str);
	void set_pretty_name (const std::string& str);
	std::string pretty_name () const { return _pretty_name_prefix; }

	void set_audio_channel_names (std::vector<std::string> const& acn) {
		_audio_channel_names = acn;
	}

	virtual void silence (samplecnt_t);

	int ensure_io (ChanCount cnt, bool clear, void *src);

	int connect_ports_to_bundle (std::shared_ptr<Bundle>, bool exclusive, void *);
	int connect_ports_to_bundle (std::shared_ptr<Bundle>, bool, bool, void *);
	int disconnect_ports_from_bundle (std::shared_ptr<Bundle>, void *);

	BundleList bundles_connected ();

	std::shared_ptr<Bundle> bundle () { return _bundle; }

	bool can_add_port (DataType) const;

	int add_port (std::string connection, void *src, DataType type = DataType::NIL);
	int remove_port (std::shared_ptr<Port>, void *src);
	int connect (std::shared_ptr<Port> our_port, std::string other_port, void *src);
	int disconnect (std::shared_ptr<Port> our_port, std::string other_port, void *src);
	int disconnect (void *src);
	bool connected_to (std::shared_ptr<const IO>) const;
	bool connected_to (const std::string&) const;
	bool connected () const;
	bool physically_connected () const;
	bool has_ext_connection () const;

	samplecnt_t latency () const;
	samplecnt_t connected_latency (bool for_playback) const;

	void set_private_port_latencies (samplecnt_t value, bool playback);
	void set_public_port_latencies (samplecnt_t value, bool playback) const;
	void set_public_port_latency_from_connections () const;

	std::shared_ptr<PortSet> ports ();
	std::shared_ptr<PortSet const> ports () const;

	bool has_port (std::shared_ptr<Port>) const;

	std::shared_ptr<Port> nth (uint32_t n) const;
	std::shared_ptr<Port> port_by_name (const std::string& str) const;

	std::shared_ptr<AudioPort> audio(uint32_t n) const;
	std::shared_ptr<MidiPort>  midi(uint32_t n) const;

	const ChanCount& n_ports () const;

	/* The process lock will be held on emission of this signal if
	 * IOChange contains ConfigurationChanged.  In other cases,
	 * the process lock status is undefined.
	 */
	PBD::Signal<void(IOChange, void *)> changed;

	XMLNode& get_state () const;

	int set_state (const XMLNode&, int version);
	int set_state_2X (const XMLNode&, int, bool);
	static void prepare_for_reset (XMLNode&, const std::string&);

	/* We'd like this to use bool, but there are unexplained issues using
	 * bool with a PBD::StackAllocator. They may arise from stdlib's
	 * specialiation of std::list<bool> and/or std::vector<bool>.
	 *
	 * So we use int instead, with the same semantics.
	 */

	class IntBoolCombiner {
	public:

		typedef int result_type;

		template <typename Iter>
		result_type operator() (Iter first, Iter last) const {
			int r = 0;
			while (first != last) {
				if (*first > 0) {
					r = 1;
				}
				++first;
			}

			return r;
		}
	};

	/** Emitted when the port count is about to change.  Objects
	 *  can attach to this, and return a non-zero if they want to prevent
	 *  the change from happening.
	 */
	PBD::SignalWithCombiner<IntBoolCombiner, int(ChanCount)> PortCountChanging;

	static PBD::Signal<void(ChanCount)> PortCountChanged; // emitted when the number of ports changes

	static std::string name_from_state (const XMLNode&);
	static void set_name_in_state (XMLNode&, const std::string&);

	/* three utility functions - this just seems to be simplest place to put them */

	void collect_input (BufferSet& bufs, pframes_t nframes, ChanCount offset);
	void copy_to_outputs (BufferSet& bufs, DataType type, pframes_t nframes, samplecnt_t offset);
	void flush_buffers (pframes_t nframes);

	/* AudioTrack::deprecated_use_diskstream_connections() needs these */

	int set_ports (const std::string& str);

protected:
	virtual XMLNode& state () const;

	Direction _direction;
	DataType _default_type;
	bool     _active;
	bool     _sendish;

private:
	SerializedRCUManager<PortSet> _ports;

	void reestablish_port_subscriptions ();
	PBD::ScopedConnectionList _port_connections;

	std::shared_ptr<Bundle> _bundle; ///< a bundle representing our ports

	std::vector<std::string> _audio_channel_names;

	struct UserBundleInfo {
		UserBundleInfo (IO*, std::shared_ptr<UserBundle> b);
		std::shared_ptr<UserBundle> bundle;
		PBD::ScopedConnection changed;
	};

	static int parse_io_string (const std::string&, std::vector<std::string>& chns);
	static int parse_gain_string (const std::string&, std::vector<std::string>& chns);

	int ensure_ports (ChanCount, bool clear, void *src);

	void bundle_changed (Bundle::Change);
	int set_port_state_2X (const XMLNode& node, int version, bool in);

	int get_port_counts (const XMLNode& node, int version, ChanCount& n, std::shared_ptr<Bundle>& c);
	int get_port_counts_2X (const XMLNode& node, int version, ChanCount& n, std::shared_ptr<Bundle>& c);
	int create_ports (const XMLNode&, int version);

	std::shared_ptr<Bundle> find_possible_bundle (const std::string &desired_name);

	int ensure_ports_locked (ChanCount, bool clear, bool& changed);

	std::string build_legal_port_name (std::shared_ptr<PortSet const>, DataType type);
	int32_t find_port_hole (std::shared_ptr<PortSet const>, const char* base);

	void setup_bundle ();
	std::string bundle_channel_name (uint32_t, uint32_t, DataType) const;

	void apply_pretty_name ();
	std::string _pretty_name_prefix;
	BufferSet _buffers;
	void connection_change (std::shared_ptr<ARDOUR::Port>, std::shared_ptr<ARDOUR::Port>);
};

} // namespace ARDOUR

namespace PBD {
	DEFINE_ENUM_CONVERT (ARDOUR::IO::Direction)
}

