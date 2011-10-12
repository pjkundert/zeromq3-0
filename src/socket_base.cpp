/*
    Copyright (c) 2007-2011 iMatix Corporation
    Copyright (c) 2007-2011 Other contributors as noted in the AUTHORS file

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <new>
#include <string>
#include <algorithm>

#include "platform.hpp"

#if defined ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#if defined _MSC_VER
#include <intrin.h>
#endif
#else
#include <unistd.h>
#endif

#include "socket_base.hpp"
#include "zmq_listener.hpp"
#include "zmq_connecter.hpp"
#include "io_thread.hpp"
#include "connect_session.hpp"
#include "config.hpp"
#include "clock.hpp"
#include "pipe.hpp"
#include "err.hpp"
#include "ctx.hpp"
#include "platform.hpp"
#include "likely.hpp"
#include "uuid.hpp"
#include "msg.hpp"

#include "pair.hpp"
#include "pub.hpp"
#include "sub.hpp"
#include "req.hpp"
#include "rep.hpp"
#include "pull.hpp"
#include "push.hpp"
#include "xreq.hpp"
#include "xrep.hpp"
#include "xpub.hpp"
#include "xsub.hpp"
#include "router.hpp"
#include "dealer.hpp"

bool zmq::socket_base_t::check_tag ()
{
    return tag == 0xbaddecaf;
}

zmq::socket_base_t *zmq::socket_base_t::create (int type_, class ctx_t *parent_,
    uint32_t tid_)
{
    socket_base_t *s = NULL;
    switch (type_) {

    case ZMQ_PAIR:
        s = new (std::nothrow) pair_t (parent_, tid_);
        break;
    case ZMQ_PUB:
        s = new (std::nothrow) pub_t (parent_, tid_);
        break;
    case ZMQ_SUB:
        s = new (std::nothrow) sub_t (parent_, tid_);
        break;
    case ZMQ_REQ:
        s = new (std::nothrow) req_t (parent_, tid_);
        break;
    case ZMQ_REP:
        s = new (std::nothrow) rep_t (parent_, tid_);
        break;
    case ZMQ_XREQ:
        s = new (std::nothrow) xreq_t (parent_, tid_);
        break;
    case ZMQ_XREP:
        s = new (std::nothrow) xrep_t (parent_, tid_);
        break;
    case ZMQ_PULL:
        s = new (std::nothrow) pull_t (parent_, tid_);
        break;
    case ZMQ_PUSH:
        s = new (std::nothrow) push_t (parent_, tid_);
        break;
    case ZMQ_XPUB:
        s = new (std::nothrow) xpub_t (parent_, tid_);
        break;
    case ZMQ_XSUB:
        s = new (std::nothrow) xsub_t (parent_, tid_);
        break;
    case ZMQ_ROUTER:
        s = new (std::nothrow) router_t (parent_, tid_);
        break;
    case ZMQ_DEALER:
        s = new (std::nothrow) dealer_t (parent_, tid_);
        break;
    default:
        errno = EINVAL;
        return NULL;
    }
    alloc_assert (s);
    return s;
}

zmq::socket_base_t::socket_base_t (ctx_t *parent_, uint32_t tid_) :
    own_t (parent_, tid_),
    tag (0xbaddecaf),
    ctx_terminated (false),
    destroyed (false),
    last_tsc (0),
    ticks (0),
    rcvlabel (false),
    rcvmore (false)
{
}

zmq::socket_base_t::~socket_base_t ()
{
    zmq_assert (destroyed);

    //  Check whether there are no session leaks.
    sessions_sync.lock ();
    zmq_assert (sessions.empty ());
    sessions_sync.unlock ();

    //  Mark the socket as dead.
    tag = 0xdeadbeef;
}

zmq::mailbox_t *zmq::socket_base_t::get_mailbox ()
{
    return &mailbox;
}

void zmq::socket_base_t::stop ()
{
    //  Called by ctx when it is terminated (zmq_term).
    //  'stop' command is sent from the threads that called zmq_term to
    //  the thread owning the socket. This way, blocking call in the
    //  owner thread can be interrupted.
    send_stop ();
}

int zmq::socket_base_t::parse_uri (const char *uri_,
                        std::string &protocol_, std::string &address_)
{
    zmq_assert (uri_ != NULL);

    std::string uri (uri_);
    std::string::size_type pos = uri.find ("://");
    if (pos == std::string::npos) {
        errno = EINVAL;
        return -1;
    }
    protocol_ = uri.substr (0, pos);
    address_ = uri.substr (pos + 3);
    if (protocol_.empty () || address_.empty ()) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int zmq::socket_base_t::check_protocol (const std::string &protocol_)
{
    //  First check out whether the protcol is something we are aware of.
    if (protocol_ != "inproc" && protocol_ != "ipc" && protocol_ != "tcp" &&
          protocol_ != "pgm" && protocol_ != "epgm" && protocol_ != "sys") {
        errno = EPROTONOSUPPORT;
        return -1;
    }

    //  If 0MQ is not compiled with OpenPGM, pgm and epgm transports
    //  are not avaialble.
#if !defined ZMQ_HAVE_OPENPGM
    if (protocol_ == "pgm" || protocol_ == "epgm") {
        errno = EPROTONOSUPPORT;
        return -1;
    }
#endif

    //  IPC transport is not available on Windows and OpenVMS.
#if defined ZMQ_HAVE_WINDOWS || defined ZMQ_HAVE_OPENVMS
    if (protocol_ == "ipc") {
        //  Unknown protocol.
        errno = EPROTONOSUPPORT;
        return -1;
    }
#endif

    //  Check whether socket type and transport protocol match.
    //  Specifically, multicast protocols can't be combined with
    //  bi-directional messaging patterns (socket types).
    if ((protocol_ == "pgm" || protocol_ == "epgm") &&
          options.type != ZMQ_PUB && options.type != ZMQ_SUB &&
          options.type != ZMQ_XPUB && options.type != ZMQ_XSUB) {
        errno = ENOCOMPATPROTO;
        return -1;
    }

    //  Protocol is available.
    return 0;
}

void zmq::socket_base_t::attach_pipe (pipe_t *pipe_,
    const blob_t &peer_identity_)
{
    //  First, register the pipe so that we can terminate it later on.
    pipe_->set_event_sink (this);
    pipes.push_back (pipe_);

    //  Then, pass the pipe to the specific socket type.
    //  If the peer haven't specified it's identity, let's generate one.
    if (peer_identity_.size ()) {
        xattach_pipe (pipe_, peer_identity_);
    }
    else {
        blob_t identity (17, 0);
        generate_uuid ((unsigned char*) identity.data () + 1);
        xattach_pipe (pipe_, identity);
    }

    //  If the socket is already being closed, ask any new pipes to terminate
    //  straight away.
    if (is_terminating ()) {
        register_term_acks (1);
        pipe_->terminate (false);
    }
}

int zmq::socket_base_t::setsockopt (int option_, const void *optval_,
    size_t optvallen_)
{
    if (unlikely (ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    //  Invoke handler; specific socket type overloads the option.
    return xsetsockopt (option_, optval_, optvallen_);
}

int zmq::socket_base_t::getsockopt (int option_, void *optval_,
    size_t *optvallen_)
{
    if (unlikely (ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    //  Invoke handler; specific socket type overloads the option.
    return xgetsockopt (option_, optval_, optvallen_);
}

int zmq::socket_base_t::bind (const char *addr_)
{
    if (unlikely (ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    //  Parse addr_ string.
    std::string protocol;
    std::string address;
    int rc = parse_uri (addr_, protocol, address);
    if (rc != 0)
        return -1;

    rc = check_protocol (protocol);
    if (rc != 0)
        return -1;

    if (protocol == "inproc" || protocol == "sys") {
        endpoint_t endpoint = {this, options};
        return register_endpoint (addr_, endpoint);
    }

    if (protocol == "tcp" || protocol == "ipc") {

        //  Choose I/O thread to run the listerner in.
        io_thread_t *io_thread = choose_io_thread (options.affinity);
        if (!io_thread) {
            errno = EMTHREAD;
            return -1;
        }

        //  Create and run the listener.
        zmq_listener_t *listener = new (std::nothrow) zmq_listener_t (
            io_thread, this, options);
        alloc_assert (listener);
        int rc = listener->set_address (protocol.c_str(), address.c_str ());
        if (rc != 0) {
            delete listener;
            return -1;
        }
        launch_child (listener);

        return 0;
    }

    if (protocol == "pgm" || protocol == "epgm") {

        //  For convenience's sake, bind can be used interchageable with
        //  connect for PGM and EPGM transports.
        return connect (addr_);
    }

    zmq_assert (false);
    return -1;
}

int zmq::socket_base_t::connect (const char *addr_)
{
    if (unlikely (ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    //  Parse addr_ string.
    std::string protocol;
    std::string address;
    int rc = parse_uri (addr_, protocol, address);
    if (rc != 0)
        return -1;

    //  Checks that protocol is valid and supported on this system
    rc = check_protocol (protocol);
    if (rc != 0)
        return -1;

    //  Parsed address for validation
    sockaddr_storage addr;
    socklen_t addr_len;

    if (protocol == "tcp")
        rc = resolve_ip_hostname (&addr, &addr_len, address.c_str ());
    else
    if (protocol == "ipc")
        rc = resolve_local_path (&addr, &addr_len, address.c_str ());
    if (rc != 0)
        return -1;

    if (protocol == "inproc" || protocol == "sys") {

        //  TODO: inproc connect is specific with respect to creating pipes
        //  as there's no 'reconnect' functionality implemented. Once that
        //  is in place we should follow generic pipe creation algorithm.

        //  Find the peer endpoint.
        endpoint_t peer = find_endpoint (addr_);
        if (!peer.socket)
            return -1;

        // The total HWM for an inproc connection should be the sum of
        // the binder's HWM and the connector's HWM.
        int  sndhwm;
        int  rcvhwm;
        if (options.sndhwm == 0 || peer.options.rcvhwm == 0)
            sndhwm = 0;
        else
            sndhwm = options.sndhwm + peer.options.rcvhwm;
        if (options.rcvhwm == 0 || peer.options.sndhwm == 0)
            rcvhwm = 0;
        else
            rcvhwm = options.rcvhwm + peer.options.sndhwm;

        //  Create a bi-directional pipe to connect the peers.
        object_t *parents [2] = {this, peer.socket};
        pipe_t *pipes [2] = {NULL, NULL};
        int hwms [2] = {sndhwm, rcvhwm};
        bool delays [2] = {options.delay_on_disconnect, options.delay_on_close};
        int rc = pipepair (parents, pipes, hwms, delays);
        errno_assert (rc == 0);

        //  Attach local end of the pipe to this socket object.
        attach_pipe (pipes [0], peer.options.identity);

        //  Attach remote end of the pipe to the peer socket. Note that peer's
        //  seqnum was incremented in find_endpoint function. We don't need it
        //  increased here.
        send_bind (peer.socket, pipes [1], options.identity, false);

        return 0;
    }

    //  Choose the I/O thread to run the session in.
    io_thread_t *io_thread = choose_io_thread (options.affinity);
    if (!io_thread) {
        errno = EMTHREAD;
        return -1;
    }

    //  Create session.
    connect_session_t *session = new (std::nothrow) connect_session_t (
        io_thread, this, options, protocol.c_str (), address.c_str ());
    alloc_assert (session);

    //  If 'immediate connect' feature is required, we'll create the pipes
    //  to the session straight away. Otherwise, they'll be created by the
    //  session once the connection is established.
    if (options.immediate_connect) {

        //  Create a bi-directional pipe.
        object_t *parents [2] = {this, session};
        pipe_t *pipes [2] = {NULL, NULL};
        int hwms [2] = {options.sndhwm, options.rcvhwm};
        bool delays [2] = {options.delay_on_disconnect, options.delay_on_close};
        int rc = pipepair (parents, pipes, hwms, delays);
        errno_assert (rc == 0);

        //  Attach local end of the pipe to the socket object.
        attach_pipe (pipes [0], blob_t ());

        //  Attach remote end of the pipe to the session object later on.
        session->attach_pipe (pipes [1]);
    }

    //  Activate the session. Make it a child of this socket.
    launch_child (session);

    return 0;
}

int zmq::socket_base_t::send (msg_t *msg_, int flags_)
{
    //  Check whether the library haven't been shut down yet.
    if (unlikely (ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    //  Check whether message passed to the function is valid.
    if (unlikely (!msg_->check ())) {
        errno = EFAULT;
        return -1;
    }

    //  Process pending commands, if any.
    int rc = process_commands (0, true);
    if (unlikely (rc != 0))
        return -1;

    //  At this point we impose the LABEL & MORE flags on the message.
    if (flags_ & ZMQ_SNDLABEL)
        msg_->set_flags (msg_t::label);
    if (flags_ & ZMQ_SNDMORE)
        msg_->set_flags (msg_t::more);

    //  Try to send the message.
    rc = xsend (msg_, flags_);
    if (rc == 0)
        return 0;
    if (unlikely (errno != EAGAIN))
        return -1;

    //  In case of non-blocking send we'll simply propagate
    //  the error - including EAGAIN - up the stack.
    if (flags_ & ZMQ_DONTWAIT || options.sndtimeo == 0)
        return -1;

    //  Compute the time when the timeout should occur.
    //  If the timeout is infite, don't care.
    clock_t clock ;
    int timeout = options.sndtimeo;
    uint64_t end = timeout < 0 ? 0 : (clock.now_ms () + timeout);

    //  Oops, we couldn't send the message. Wait for the next
    //  command, process it and try to send the message again.
    //  If timeout is reached in the meantime, return EAGAIN.
    while (true) {
        if (unlikely (process_commands (timeout, false) != 0))
            return -1;
        rc = xsend (msg_, flags_);
        if (rc == 0)
            break;
        if (unlikely (errno != EAGAIN))
            return -1;
        if (timeout > 0) {
            timeout = (int) (end - clock.now_ms ());
            if (timeout <= 0) {
                errno = EAGAIN;
                return -1;
            }
        }
    }
    return 0;
}

int zmq::socket_base_t::recv (msg_t *msg_, int flags_)
{
    //  Check whether the library haven't been shut down yet.
    if (unlikely (ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    //  Check whether message passed to the function is valid.
    if (unlikely (!msg_->check ())) {
        errno = EFAULT;
        return -1;
    }

    //  Get the message.
    int rc = xrecv (msg_, flags_);
    if (unlikely (rc != 0 && errno != EAGAIN))
        return -1;

    //  Once every inbound_poll_rate messages check for signals and process
    //  incoming commands. This happens only if we are not polling altogether
    //  because there are messages available all the time. If poll occurs,
    //  ticks is set to zero and thus we avoid this code.
    //
    //  Note that 'recv' uses different command throttling algorithm (the one
    //  described above) from the one used by 'send'. This is because counting
    //  ticks is more efficient than doing RDTSC all the time.
    if (++ticks == inbound_poll_rate) {
        if (unlikely (process_commands (0, false) != 0))
            return -1;
        ticks = 0;
    }

    //  If we have the message, return immediately.
    if (rc == 0) {
        rcvlabel = msg_->flags () & msg_t::label;
        rcvmore = msg_->flags () & msg_t::more || rcvlabel;
        if (rcvlabel)
            msg_->reset_flags (msg_t::label);
        if (rcvmore)
            msg_->reset_flags (msg_t::more);
        return 0;
    }

    //  If the message cannot be fetched immediately, there are two scenarios.
    //  For non-blocking recv, commands are processed in case there's an
    //  activate_reader command already waiting int a command pipe.
    //  If it's not, return EAGAIN.
    if (flags_ & ZMQ_DONTWAIT || options.rcvtimeo == 0) {
        if (unlikely (process_commands (0, false) != 0))
            return -1;
        ticks = 0;

        rc = xrecv (msg_, flags_);
        if (rc < 0)
            return rc;
        rcvlabel = msg_->flags () & msg_t::label;
        rcvmore = msg_->flags () & msg_t::more || rcvlabel;
        if (rcvlabel)
            msg_->reset_flags (msg_t::label);
        if (rcvmore)
            msg_->reset_flags (msg_t::more);
        return 0;
    }

    //  Compute the time when the timeout should occur.
    //  If the timeout is infite, don't care.
    clock_t clock ;
    int timeout = options.rcvtimeo;
    uint64_t end = timeout < 0 ? 0 : (clock.now_ms () + timeout);

    //  In blocking scenario, commands are processed over and over again until
    //  we are able to fetch a message.
    bool block = (ticks != 0);
    while (true) {
        if (unlikely (process_commands (block ? timeout : 0, false) != 0))
            return -1;
        rc = xrecv (msg_, flags_);
        if (rc == 0) {
            ticks = 0;
            break;
        }
        if (unlikely (errno != EAGAIN))
            return -1;
        block = true;
        if (timeout > 0) {
            timeout = (int) (end - clock.now_ms ());
            if (timeout <= 0) {
                errno = EAGAIN;
                return -1;
            }
        }
    }
    rcvlabel = msg_->flags () & msg_t::label;
    rcvmore = msg_->flags () & msg_t::more || rcvlabel;
    if (rcvlabel)
        msg_->reset_flags (msg_t::label);
    if (rcvmore)
        msg_->reset_flags (msg_t::more);
    return 0;
}

int zmq::socket_base_t::close ()
{
    //  Transfer the ownership of the socket from this application thread
    //  to the reaper thread which will take care of the rest of shutdown
    //  process.
    send_reap (this);

    return 0;
}

bool zmq::socket_base_t::has_in ()
{
    return xhas_in ();
}

bool zmq::socket_base_t::has_out ()
{
    return xhas_out ();
}

bool zmq::socket_base_t::has_subs (const void *data_, size_t size_)
{
    return xhas_subs (data_, size_);
}

bool zmq::socket_base_t::register_session (const blob_t &name_,
    session_t *session_)
{
    sessions_sync.lock ();
    bool registered = sessions.insert (
        sessions_t::value_type (name_, session_)).second;
    sessions_sync.unlock ();
    return registered;
}

void zmq::socket_base_t::unregister_session (const blob_t &name_)
{
    sessions_sync.lock ();
    sessions_t::iterator it = sessions.find (name_);
    zmq_assert (it != sessions.end ());
    sessions.erase (it);
    sessions_sync.unlock ();
}

zmq::session_t *zmq::socket_base_t::find_session (const blob_t &name_)
{
    sessions_sync.lock ();
    sessions_t::iterator it = sessions.find (name_);
    if (it == sessions.end ()) {
        sessions_sync.unlock ();
        return NULL;
    }
    session_t *session = it->second;

    //  Prepare the session for subsequent attach command.
    //  Note the connect sessions have NULL pointers registered here.
    if (session)
        session->inc_seqnum ();

    sessions_sync.unlock ();
    return session;
}

void zmq::socket_base_t::start_reaping (poller_t *poller_)
{
    //  Plug the socket to the reaper thread.
    poller = poller_;
    handle = poller->add_fd (mailbox.get_fd (), this);
    poller->set_pollin (handle);

    //  Initialise the termination and check whether it can be deallocated
    //  immediately.
    terminate ();
    check_destroy ();
}

int zmq::socket_base_t::process_commands (int timeout_, bool throttle_)
{
    int rc;
    command_t cmd;
    if (timeout_ != 0) {

        //  If we are asked to wait, simply ask mailbox to wait.
        rc = mailbox.recv (&cmd, timeout_);
    }
    else {

        //  If we are asked not to wait, check whether we haven't processed
        //  commands recently, so that we can throttle the new commands.

        //  Get the CPU's tick counter. If 0, the counter is not available.
        uint64_t tsc = zmq::clock_t::rdtsc ();

        //  Optimised version of command processing - it doesn't have to check
        //  for incoming commands each time. It does so only if certain time
        //  elapsed since last command processing. Command delay varies
        //  depending on CPU speed: It's ~1ms on 3GHz CPU, ~2ms on 1.5GHz CPU
        //  etc. The optimisation makes sense only on platforms where getting
        //  a timestamp is a very cheap operation (tens of nanoseconds).
        if (tsc && throttle_) {

            //  Check whether TSC haven't jumped backwards (in case of migration
            //  between CPU cores) and whether certain time have elapsed since
            //  last command processing. If it didn't do nothing.
            if (tsc >= last_tsc && tsc - last_tsc <= max_command_delay)
                return 0;
            last_tsc = tsc;
        }

        //  Check whether there are any commands pending for this thread.
        rc = mailbox.recv (&cmd, 0);
    }

    //  Process all the commands available at the moment.
    while (true) {
        if (rc == -1 && errno == EAGAIN)
            break;
        if (rc == -1 && errno == EINTR)
            return -1;
        errno_assert (rc == 0);
        cmd.destination->process_command (cmd);
        rc = mailbox.recv (&cmd, 0);
     }

    if (ctx_terminated) {
        errno = ETERM;
        return -1;
    }

    return 0;
}

void zmq::socket_base_t::process_stop ()
{
    //  Here, someone have called zmq_term while the socket was still alive.
    //  We'll remember the fact so that any blocking call is interrupted and any
    //  further attempt to use the socket will return ETERM. The user is still
    //  responsible for calling zmq_close on the socket though!
    ctx_terminated = true;
}

void zmq::socket_base_t::process_bind (pipe_t *pipe_,
    const blob_t &peer_identity_)
{
    attach_pipe (pipe_, peer_identity_);
}

void zmq::socket_base_t::process_unplug ()
{
}

void zmq::socket_base_t::process_term (int linger_)
{
    //  Unregister all inproc endpoints associated with this socket.
    //  Doing this we make sure that no new pipes from other sockets (inproc)
    //  will be initiated.
    unregister_endpoints (this);

    //  Ask all attached pipes to terminate.
    for (pipes_t::size_type i = 0; i != pipes.size (); ++i)
        pipes [i]->terminate (false);
    register_term_acks ((int) pipes.size ());

    //  Continue the termination process immediately.
    own_t::process_term (linger_);
}

void zmq::socket_base_t::process_destroy ()
{
    destroyed = true;
}

int zmq::socket_base_t::xgetsockopt (int option_, void *optval_,
    size_t *optvallen_)
{
    int rc;
    switch (option_) {
    case ZMQ_RCVLABEL:
        if (*optvallen_ < sizeof (int)) {
            errno = EINVAL;
            rc = -1;
            break;
        }
        *((int*) optval_) = rcvlabel ? 1 : 0;
        *optvallen_ = sizeof (int);
        rc = 0;
        break;
    case ZMQ_RCVMORE:
        if (*optvallen_ < sizeof (int)) {
            errno = EINVAL;
            rc = -1;
            break;
        }
        *((int*) optval_) = rcvmore ? 1 : 0;
        *optvallen_ = sizeof (int);
        rc = 0;
        break;
    case ZMQ_FD:
        if (*optvallen_ < sizeof (fd_t)) {
            errno = EINVAL;
            rc = -1;
            break;
        }
        *((fd_t*) optval_) = mailbox.get_fd ();
        *optvallen_ = sizeof (fd_t);
        rc = 0;
        break;
    case ZMQ_EVENTS:
        if (*optvallen_ < sizeof (int)) {
            errno = EINVAL;
            rc = -1;
            break;
        }
        rc = process_commands (0, false);
        if (rc != 0 && (errno == EINTR || errno == ETERM))
            break;
        errno_assert (rc == 0);
        *((int*) optval_) = 0;
        if (has_out ())
            *((int*) optval_) |= ZMQ_POLLOUT;
        if (has_in ())
            *((int*) optval_) |= ZMQ_POLLIN;
        *optvallen_ = sizeof (int);
        rc = 0;
        break;
    default:
        //  If the socket type doesn't support the option, pass it along
        //  to the generic option parser.
        rc = options.getsockopt (option_, optval_, optvallen_);
        break;
    }
    return rc;
}

int zmq::socket_base_t::xsetsockopt (int option_, const void *optval_,
    size_t optvallen_)
{
    //  If the socket type doesn't support the option, pass it to
    //  the generic option parser.
    return options.setsockopt (option_, optval_, optvallen_);
}

bool zmq::socket_base_t::xhas_out ()
{
    return false;
}

bool zmq::socket_base_t::xhas_subs (const void *, size_t)
{
    return xhas_out ();
}

int zmq::socket_base_t::xsend (msg_t *msg_, int flags_)
{
    errno = ENOTSUP;
    return -1;
}

bool zmq::socket_base_t::xhas_in ()
{
    return false;
}

int zmq::socket_base_t::xrecv (msg_t *msg_, int flags_)
{
    errno = ENOTSUP;
    return -1;
}

void zmq::socket_base_t::xread_activated (pipe_t *pipe_)
{
    zmq_assert (false);
}
void zmq::socket_base_t::xwrite_activated (pipe_t *pipe_)
{
    zmq_assert (false);
}

void zmq::socket_base_t::xhiccuped (pipe_t *pipe_)
{
    zmq_assert (false);
}

void zmq::socket_base_t::in_event ()
{
    //  This function is invoked only once the socket is running in the context
    //  of the reaper thread. Process any commands from other threads/sockets
    //  that may be available at the moment. Ultimately, the socket will
    //  be destroyed.
    process_commands (0, false);
    check_destroy ();
}

void zmq::socket_base_t::out_event ()
{
    zmq_assert (false);
}

void zmq::socket_base_t::timer_event (int id_)
{
    zmq_assert (false);
}

void zmq::socket_base_t::check_destroy ()
{
    //  If the object was already marked as destroyed, finish the deallocation.
    if (destroyed) {

        //  Remove the socket from the reaper's poller.
        poller->rm_fd (handle);

        //  Remove the socket from the context.
        destroy_socket (this);

        //  Notify the reaper about the fact.
        send_reaped ();

        //  Deallocate.
        own_t::process_destroy ();
    }
}

void zmq::socket_base_t::read_activated (pipe_t *pipe_)
{
    xread_activated (pipe_);
}

void zmq::socket_base_t::write_activated (pipe_t *pipe_)
{
    xwrite_activated (pipe_);
}

void zmq::socket_base_t::hiccuped (pipe_t *pipe_)
{
    xhiccuped (pipe_);
}

void zmq::socket_base_t::terminated (pipe_t *pipe_)
{
    //  Notify the specific socket type about the pipe termination.
    xterminated (pipe_);

    //  Remove the pipe from the list of attached pipes and confirm its
    //  termination if we are already shutting down.
    pipes.erase (pipe_);
    if (is_terminating ())
        unregister_term_ack ();
}

