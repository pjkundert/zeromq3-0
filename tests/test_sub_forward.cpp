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

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../include/zmq.h"
#include "../include/zmq_utils.h"

int main (int argc, char *argv [])
{
    void *sock[10];
    int socknum = 0;

    void *ctx = zmq_init (1);
    assert (ctx);

    //  First, create an intermediate device.
    void *xpub = zmq_socket (ctx, ZMQ_XPUB);
    assert (xpub);
    sock[socknum++] = xpub;
    int rc = zmq_bind (xpub, "tcp://127.0.0.1:5560");
    assert (rc == 0);

    void *xsub = zmq_socket (ctx, ZMQ_XSUB);
    assert (xsub);
    sock[socknum++] = xsub;
    rc = zmq_connect (xsub, "tcp://127.0.0.1:5561");
    assert (rc == 0);

    //  Create a publisher.
    void *pub = zmq_socket (ctx, ZMQ_PUB);
    assert (pub);
    sock[socknum++] = pub;
    rc = zmq_bind (pub, "tcp://127.0.0.1:5561");
    assert (rc == 0);

    //  Create a subscriber.
    void *sub = zmq_socket (ctx, ZMQ_SUB);
    assert (sub);
    sock[socknum++] = sub;
    rc = zmq_connect (sub, "tcp://127.0.0.1:5560");
    assert (rc == 0);

    //  Confirm no subs yet, either in general (using zmq_subs), or
    // specificially (using getsockopt w/ ZMQ_SUBSCRIBE)
    rc = zmq_subs (pub, "", 0);
    assert (rc == 0);
    unsigned char term[10];
    size_t termsiz = 0;
    rc = zmq_getsockopt(pub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 0);

    //  Subscribe for all messages.
    rc = zmq_setsockopt (sub, ZMQ_SUBSCRIBE, "", 0);
    assert (rc == 0);

    //  Pass the subscription upstream through the device.
    char buff [32];
    rc = zmq_recv (xpub, buff, sizeof (buff), 0);
    assert (rc >= 0);
    rc = zmq_send (xsub, buff, rc, 0);
    assert (rc >= 0);

    //  Wait a bit till the subscription gets to the publisher.
    zmq_sleep (1);

    //  Confirm subs; won't reach the PUB 'til it is activated at first send
    rc = zmq_subs (xpub, "", 0);
    assert (rc == 1);

    //  Send an empty message.
    rc = zmq_send (pub, NULL, 0, 0);
    assert (rc == 0);

    rc = zmq_subs (pub, "", 0);
    assert (rc == 1);

    //  Pass the message downstream through the device.
    rc = zmq_recv (xsub, buff, sizeof (buff), 0);
    assert (rc >= 0);
    rc = zmq_send (xpub, buff, rc, 0);
    assert (rc >= 0);

    //  Receive the message in the subscriber.
    rc = zmq_recv (sub, buff, sizeof (buff), 0);
    assert (rc == 0);

    //  Confirm subs
    rc = zmq_subs (pub, "", 0);
    assert (rc == 1);
    rc = zmq_subs (xpub, "", 0);
    assert (rc == 1);

    termsiz = 0;
    rc = zmq_getsockopt(pub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 1);

    termsiz = 0;
    rc = zmq_getsockopt(xpub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 1);

    memcpy(term, "BOOP", 4);
    termsiz = 1; // "B"
    rc = zmq_getsockopt(pub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 1);

    // 
    //  Next, create a few new subscribers, at various levels, with different
    //  but overlapping filters.
    // 
    //                         +--> subl2b: "BOO"
    //                         |
    //                         | "BOO"
    //                         |
    // pub .--> xsub .--> xpub .--> sub:     ""
    //     |                   |
    //     | "BO"              | "B"
    //     |                   |
    //     +-> subl1a: "BO"    +--> subl2a: "B"
    //

    void *subl2a = zmq_socket (ctx, ZMQ_SUB);
    assert (subl2a);
    sock[socknum++] = subl2a;
    rc = zmq_connect (subl2a, "tcp://127.0.0.1:5560");
    assert (rc == 0);
    rc = zmq_setsockopt (subl2a, ZMQ_SUBSCRIBE, "B", 1);
    assert (rc == 0);
    int timeout = 250;
    rc = zmq_setsockopt (subl2a, ZMQ_RCVTIMEO, &timeout, sizeof timeout);
    assert (rc == 0);

    void *subl2b = zmq_socket (ctx, ZMQ_SUB);
    assert (subl2b);
    sock[socknum++] = subl2b;
    rc = zmq_connect (subl2b, "tcp://127.0.0.1:5560");
    assert (rc == 0);
    rc = zmq_setsockopt (subl2b, ZMQ_SUBSCRIBE, "BOO", 3);
    assert (rc == 0);
    timeout = 250;
    rc = zmq_setsockopt (subl2b, ZMQ_RCVTIMEO, &timeout, sizeof timeout);
    assert (rc == 0);

    void *subl1a = zmq_socket (ctx, ZMQ_SUB);
    assert (subl1a);
    sock[socknum++] = subl1a;
    rc = zmq_connect (subl1a, "tcp://127.0.0.1:5561");
    assert (rc == 0);
    rc = zmq_setsockopt (subl1a, ZMQ_SUBSCRIBE, "BO", 2);
    assert (rc == 0);
    timeout = 250;
    rc = zmq_setsockopt (subl1a, ZMQ_RCVTIMEO, &timeout, sizeof timeout);
    assert (rc == 0);

    // Now, run the xsub/xpub to completion to transfer pubs
    timeout = 250;
    rc = zmq_setsockopt (xpub, ZMQ_RCVTIMEO, &timeout, sizeof timeout);
    assert (rc == 0);
    size_t msgs = 0;
    while (( rc = zmq_recv (xpub, buff, sizeof (buff), 0)) >= 0 ) {
        rc = zmq_send (xsub, buff, rc, 0);
        assert (rc >= 0);
        ++msgs;
    }

    // We should only see 2 upstream subscriptions; "B" and "BOO"
    assert (msgs == 2);

    // No new subscriptions activated 'til we publish something...
    termsiz = 0;
    rc = zmq_getsockopt(pub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 1);


    //  Send a message that should satisfy all subscriber.
    rc = zmq_send (pub, "BOOP", 4, 0);
    assert (rc == 4);

    rc = zmq_subs (pub, "", 0);
    assert (rc == 1);
    rc = zmq_subs (xpub, "", 0);
    assert (rc == 1);

    //  Pass the message downstream through the device.
    timeout = 250;
    rc = zmq_setsockopt (xsub, ZMQ_RCVTIMEO, &timeout, sizeof timeout);
    assert (rc == 0);
    msgs = 0;
    while (( rc = zmq_recv (xsub, buff, sizeof (buff), 0)) >= 0 ) {
        rc = zmq_send (xpub, buff, rc, 0);
        assert (rc >= 0);
        ++msgs;
    }

    //  Receive the message in the subscribers.
    rc = zmq_recv (sub, buff, sizeof (buff), 0);
    assert (rc == 4);
    rc = zmq_recv (subl1a, buff, sizeof (buff), 0);
    assert (rc == 4);
    rc = zmq_recv (subl2a, buff, sizeof (buff), 0);
    assert (rc == 4);
    rc = zmq_recv (subl2b, buff, sizeof (buff), 0);
    assert (rc == 4);

    //  Confirm subs using the simple zmq_subs interface
    rc = zmq_subs (pub, "", 0);
    assert (rc == 1);
    rc = zmq_subs (xpub, "", 0);
    assert (rc == 1);

    // "A" matches the "" (any) sub
    rc = zmq_subs (pub, "A", 1);
    assert (rc == 1);
    rc = zmq_subs (xpub, "A", 1);
    assert (rc == 1);

    rc = zmq_subs (pub, "B", 1);
    assert (rc == 1);
    rc = zmq_subs (xpub, "B", 1);
    assert (rc == 1);

    //  Get counts using the getsockopt/ZMQ_SUBSCRIBE interface
    termsiz = 0; // ""
    rc = zmq_getsockopt(pub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 1);

    termsiz = 0;
    rc = zmq_getsockopt(xpub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 1);

    termsiz = 1; // "B"
    rc = zmq_getsockopt(pub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 2);

    termsiz = 1;
    rc = zmq_getsockopt(xpub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 2);

    termsiz = 2; // "BO"
    rc = zmq_getsockopt(pub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 3);

    termsiz = 2;
    rc = zmq_getsockopt(xpub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 2);

    termsiz = 3; // "BOO"
    rc = zmq_getsockopt(pub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 4);

    termsiz = 3;
    rc = zmq_getsockopt(xpub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 3);

    //  Remove the "" subscriber, and run the xsub/xpub to completion to
    //  transfer unsubs
    rc = zmq_setsockopt(sub, ZMQ_UNSUBSCRIBE, "", 0);
    assert (rc == 0);
    rc = zmq_setsockopt (xpub, ZMQ_RCVTIMEO, &timeout, sizeof timeout);
    assert (rc == 0);
    msgs = 0;
    while (( rc = zmq_recv (xpub, buff, sizeof (buff), 0)) >= 0 ) {
        rc = zmq_send (xsub, buff, rc, 0);
        assert (rc >= 0);
        ++msgs;
    }

    // We should only see 1 upstream unsubscribe, the ""
    assert (msgs == 1);
    
    // No old subscriptions deactivated 'til we publish something...
    termsiz = 0;
    rc = zmq_getsockopt(pub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 1UL);
    
    //  Send a message that should satisfy all subscribers.
    rc = zmq_send (pub, "BOOP", 4, 0);
    assert (rc == 4);

    
    //  Get counts using the getsockopt/ZMQ_SUBSCRIBE interface
    termsiz = 0; // ""
    rc = zmq_getsockopt(pub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 0UL);
    
    termsiz = 0;
    rc = zmq_getsockopt(xpub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 0UL);
    
    termsiz = 1; // "B"
    rc = zmq_getsockopt(pub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 1UL);
    
    termsiz = 1;
    rc = zmq_getsockopt(xpub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 1UL);
    
    termsiz = 2; // "BO"
    rc = zmq_getsockopt(pub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 2UL);
    
    termsiz = 2;
    rc = zmq_getsockopt(xpub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 1UL);
    
    termsiz = 3; // "BOO"
    rc = zmq_getsockopt(pub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 3UL);
    
    termsiz = 3;
    rc = zmq_getsockopt(xpub, ZMQ_SUBSCRIBE, term, &termsiz);
    assert (rc == 0);
    assert (termsiz == 2UL);


    //  Pass the message downstream through the device.
    msgs = 0;
    while (( rc = zmq_recv (xsub, buff, sizeof (buff), 0)) >= 0 ) {
        rc = zmq_send (xpub, buff, rc, 0);
        assert (rc >= 0);
        ++msgs;
    }
    assert (msgs == 1);

    //  Receive the message in the subscribers.
    //rc = zmq_recv (sub, buff, sizeof (buff), 0);
    //assert (rc == 4);
    rc = zmq_recv (subl1a, buff, sizeof (buff), 0);
    assert (rc == 4);
    rc = zmq_recv (subl2a, buff, sizeof (buff), 0);
    assert (rc == 4);
    rc = zmq_recv (subl2b, buff, sizeof (buff), 0);
    assert (rc == 4);


    //  Send a message that should satisfy no subscribers.
    rc = zmq_send (pub, "", 0, 0);
    assert (rc == 0);

    //  Pass the message downstream through the device.
    msgs = 0;
    while (( rc = zmq_recv (xsub, buff, sizeof (buff), 0)) >= 0 ) {
        rc = zmq_send (xpub, buff, rc, 0);
        assert (rc >= 0);
        ++msgs;
    }
    assert (msgs == 0);

    rc = zmq_recv (subl1a, buff, sizeof (buff), 0);
    assert (rc == -1);
    rc = zmq_recv (subl2a, buff, sizeof (buff), 0);
    assert (rc == -1);
    rc = zmq_recv (subl2b, buff, sizeof (buff), 0);
    assert (rc == -1);


    //  Send a message that should satisfy 2/3 subscribers.
    rc = zmq_send (pub, "BO", 2, 0);
    assert (rc == 2);

    //  Pass the message downstream through the device.
    msgs = 0;
    while (( rc = zmq_recv (xsub, buff, sizeof (buff), 0)) >= 0 ) {
        rc = zmq_send (xpub, buff, rc, 0);
        assert (rc >= 0);
        ++msgs;
    }
    assert (msgs == 1);

    rc = zmq_recv (subl1a, buff, sizeof (buff), 0);
    assert (rc == 2);
    rc = zmq_recv (subl2a, buff, sizeof (buff), 0);
    assert (rc == 2);
    rc = zmq_recv (subl2b, buff, sizeof (buff), 0);
    assert (rc == -1);

    //  Clean up.
    for (int i = 0
             ; i < socknum
             ; ++i) {
        rc = zmq_close (sock[i]);
        assert (rc == 0);
    }
    rc = zmq_term (ctx);
    assert (rc == 0);

    return 0 ;
}
