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

#include <stdlib.h>

#include <new>
#include <algorithm>
#include <string>
#include "platform.hpp"
#if defined ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#endif

#include "err.hpp"
#include "pipe.hpp"
#include "mtrie.hpp"

zmq::mtrie_t::mtrie_t () :
    refs (1),
    min (0),
    count (0)
{
}

zmq::mtrie_t::mtrie_t (const mtrie_t &rhs) :
    refs (1),
    pipes (rhs.pipes),
    min (rhs.min),
    count (rhs.count)
{
    //  Performs a shallow copy, just incrementing the reference count on all
    //  the sub-mtrie_t's
    if ( count == 1 ) {
        next.node = rhs.next.node;
        next.node->refs++;
    } else if ( count > 1 ) {
        next.table = (mtrie_t**)malloc (count * sizeof *next.table);
        for (unsigned short i = 0; i < count; i++) {
            next.table[i] = rhs.next.table[i];
            if (next.table[i])
                next.table[i]->refs++;
        }
    }
}

zmq::mtrie_t::~mtrie_t ()
{
    zmq_assert (refs == 0);
    if (count == 1)
        delete next.node;
    else if (count > 1) {
        for (unsigned short i = 0; i != count; ++i)
            if (next.table [i])
                delete next.table [i];
        free (next.table);
    }
}

bool zmq::mtrie_t::add (const unsigned char *prefix_, size_t size_,
    pipe_t *pipe_)
{
    return add_helper (prefix_, size_, pipe_);
}

size_t zmq::mtrie_t::recognize (const unsigned char *prefix_, size_t size_,
    value_t *pat_, count_t *patsiz_)
{
    zmq_assert (size_ > 0);
    //  Recognize single-char literal; skips 1 char of prefix_
    *patsiz_ = 1;
    *pat_ = *prefix_;
    return 1;
}

bool zmq::mtrie_t::add_helper (const unsigned char *prefix_, size_t size_,
    pipe_t *pipe_)
{
    //  We are at the node corresponding to the prefix. We are done.
    if (!size_) {
        bool result = pipes.empty ();
        pipes.insert (pipe_);
        return result;
    }

    //  Obtain the pattern of characters specified (sorted from low to high
    //  value) consuming the next 'skip' chars.
    unsigned char pat[1<<8];
    count_t patsiz;
    size_t preskip = recognize(prefix_, size_, pat, &patsiz);
    zmq_assert (preskip == 1);  // for now...

    unsigned char c = *pat;     // use pat[0], pat[patsiz-1] for char range...
    if (c < min || c >= min + count) {
        //  The character is out of range of currently handled charcters.
        if (!count) {
            //  We have just a single node.
            min = c;
            count = 1;
            next.node = NULL;
        } else {
            //  We have to extend the table.  Compute the new character range
            //  (lo,hi]; note that, worst case, we need to extend 1 value beyond
            //  the capacity of value_t, so we must use count_t.  Create a new
            //  (empty) table, copy old contents into place, and free old (if it
            //  was allocated).
            mtrie_t **oldtable;
            if (count > 0) {
                if (count > 1)
                    oldtable = next.table;
                else
                    oldtable = &next.node;
            } else
                oldtable = 0;
            count_t hi = std::max (min + count, c + count_t(1));
            count_t lo = std::min (min, c);
            mtrie_t **newtable = (mtrie_t **)calloc (hi - lo, sizeof *newtable);
            zmq_assert (newtable);
            if (count > 0) {
                memmove (newtable + (c - lo), oldtable, count * sizeof *newtable);
                if (count > 1)
                    free ((void*)oldtable);
            }
            min = c;
            count = hi - lo;
        }
    }

    //  If next node does not exist, create one.
    if (count == 1) {
        if (!next.node) {
            next.node = new (std::nothrow) mtrie_t;
            zmq_assert (next.node);
        }
        return next.node->add_helper (prefix_ + 1, size_ - 1, pipe_);
    }
    else {
        if (!next.table [c - min]) {
            next.table [c - min] = new (std::nothrow) mtrie_t;
            zmq_assert (next.table [c - min]);
        }
        return next.table [c - min]->add_helper (prefix_ + 1, size_ - 1, pipe_);
    }
}


void zmq::mtrie_t::rm (pipe_t *pipe_,
    void (*func_) (const unsigned char *data_, size_t size_, void *arg_),
    void *arg_)
{
    unsigned char *buff = NULL;
    rm_helper (pipe_, &buff, 0, 0, func_, arg_);
    free (buff);
}

void zmq::mtrie_t::rm_helper (pipe_t *pipe_, unsigned char **buff_,
    size_t buffsize_, size_t maxbuffsize_,
    void (*func_) (const unsigned char *data_, size_t size_, void *arg_),
    void *arg_)
{
    //  Remove the subscription from this node.
    if (pipes.erase (pipe_) && pipes.empty ())
        func_ (*buff_, buffsize_, arg_);

    //  Adjust the buffer.
    if (buffsize_ >= maxbuffsize_) {
        maxbuffsize_ = buffsize_ + 256;
        *buff_ = (unsigned char*) realloc (*buff_, maxbuffsize_);
        alloc_assert (*buff_);
    }

    //  If there are no subnodes in the trie, return.
    if (count == 0)
        return;

    //  If there's one subnode (optimisation).
    if (count == 1) {
        (*buff_) [buffsize_] = min;
        buffsize_++;
        next.node->rm_helper (pipe_, buff_, buffsize_, maxbuffsize_,
            func_, arg_);
        return;
    }

    //  If there are multiple subnodes.
    for (unsigned char c = 0; c != count; c++) {
        (*buff_) [buffsize_] = min + c;
        if (next.table [c])
            next.table [c]->rm_helper (pipe_, buff_, buffsize_ + 1,
                maxbuffsize_, func_, arg_);
    }  
}

bool zmq::mtrie_t::rm (const unsigned char *prefix_, size_t size_, pipe_t *pipe_)
{
    return rm_helper (prefix_, size_, pipe_);
}

bool zmq::mtrie_t::rm_helper (const unsigned char *prefix_, size_t size_,
    pipe_t *pipe_)
{
    if (!size_) {
        pipes_t::size_type erased = pipes.erase (pipe_);
        zmq_assert (erased == 1);
        return pipes.empty ();
    }

    const unsigned char c = *prefix_;
    if (!count || c < min || c >= min + count)
        return false;

    mtrie_t *next_node =
        count == 1 ? next.node : next.table [c - min];

    if (!next_node)
        return false;

    return next_node->rm_helper (prefix_ + 1, size_ - 1, pipe_);
}

size_t zmq::mtrie_t::match (const unsigned char *data_, size_t size_,
    void (*func_) (pipe_t *pipe_, void *arg_), void *arg_, size_t max_)
{
    //  Search down the mtrie_t, reporting all nodes that match at each level of
    //  the search term.  The very first level (matching the empty term) matches
    //  any search term.
    size_t total = 0;
    mtrie_t *current = this;
    while (true) {
        //  Signal the pipes attached to this node; empty data matches!  Process
        //  and count nodes, stopping at max_.
        for (pipes_t::iterator it = current->pipes.begin ();
             it != current->pipes.end (); ++it) {
            if (func_)
                func_ (*it, arg_);
            if (++total == max_) // if max_ == 0, no limit!
                break;
        }

        if (!size_)
            break;
        if (current->count == 0)
            break;

        //  If there's one subnode (optimisation).
        if (current->count == 1) {
            if (data_ [0] != current->min)
                break;
            current = current->next.node;
            data_++;
            size_--;
            continue;
        }

        //  If there are multiple subnodes, check that there is a matching
        //  non-empty entry for this data entry; if so, advance and loop.
        if (data_ [0] < current->min || data_ [0] >=
            current->min + current->count)
            break;
        if (!current->next.table [data_ [0] - current->min])
            break;
        current = current->next.table [data_ [0] - current->min];
        data_++;
        size_--;
    }
    return total;
}

