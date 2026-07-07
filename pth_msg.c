/*
**  GNU Pth - The GNU Portable Threads
**  Copyright (c) 1999-2006 Ralf S. Engelschall <rse@engelschall.com>
**
**  This file is part of GNU Pth, a non-preemptive thread scheduling
**  library which can be found at http://www.gnu.org/software/pth/.
**
**  This library is free software; you can redistribute it and/or
**  modify it under the terms of the GNU Lesser General Public
**  License as published by the Free Software Foundation; either
**  version 2.1 of the License, or (at your option) any later version.
**
**  This library is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
**  Lesser General Public License for more details.
**
**  You should have received a copy of the GNU Lesser General Public
**  License along with this library; if not, write to the Free Software
**  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
**  USA, or contact Ralf S. Engelschall <rse@engelschall.com>.
**
**  pth_msg.c: Pth message port facility
*/
                             /* ``Those who do not understand Unix
                                  are condemned to reinvent it, poorly.''
                                                   -- Henry Spencer      */
#include "pth_p.h"

#if cpp

/* message port structure */
struct pth_msgport_st {
    pth_ringnode_t mp_node;  /* maintainance node handle */
    const char    *mp_name;  /* optional name of message port */
    pth_t          mp_tid;   /* corresponding thread */
    pth_ring_t     mp_queue; /* queue of messages pending on port */
    volatile int   mp_lock;  /* per-port spinlock (multi-scheduler) */
};

#endif /* cpp */

static pth_ring_t   pth_msgport      = PTH_RING_INIT;
static volatile int pth_msgport_lock = 0;   /* guards the registry ring */

/* create a new message port */
pth_msgport_t pth_msgport_create(const char *name)
{
    pth_msgport_t mp;

    /* Notice: "name" is allowed to be NULL */

    /* allocate message port structure */
    if ((mp = (pth_msgport_t)malloc(sizeof(struct pth_msgport_st))) == NULL)
        return pth_error((pth_msgport_t)NULL, ENOMEM);

    /* initialize structure */
    mp->mp_name  = name;
    mp->mp_tid   = pth_current;
    pth_ring_init(&mp->mp_queue);
    pth_spin_init(&mp->mp_lock);

    /* insert into list of existing message ports */
    pth_spin_lock(&pth_msgport_lock);
    pth_ring_append(&pth_msgport, &mp->mp_node);
    pth_spin_unlock(&pth_msgport_lock);

    return mp;
}

/* delete a message port */
void pth_msgport_destroy(pth_msgport_t mp)
{
    pth_message_t *m;

    /* check input */
    if (mp == NULL)
        return;

    /* first reply to all pending messages */
    while ((m = pth_msgport_get(mp)) != NULL)
        pth_msgport_reply(m);

    /* remove from list of existing message ports */
    pth_spin_lock(&pth_msgport_lock);
    pth_ring_delete(&pth_msgport, &mp->mp_node);
    pth_spin_unlock(&pth_msgport_lock);

    /* deallocate message port structure */
    free(mp);

    return;
}

/* find a known message port through name */
pth_msgport_t pth_msgport_find(const char *name)
{
    pth_msgport_t mp, mpf;

    /* check input */
    if (name == NULL)
        return pth_error((pth_msgport_t)NULL, EINVAL);

    /* iterate over message ports */
    pth_spin_lock(&pth_msgport_lock);
    mp = mpf = (pth_msgport_t)pth_ring_first(&pth_msgport);
    while (mp != NULL) {
        if (mp->mp_name != NULL)
            if (strcmp(mp->mp_name, name) == 0)
                break;
        mp = (pth_msgport_t)pth_ring_next(&pth_msgport, (pth_ringnode_t *)mp);
        if (mp == mpf) {
            mp = NULL;
            break;
        }
    }
    pth_spin_unlock(&pth_msgport_lock);
    return mp;
}

/* number of messages on a port */
int pth_msgport_pending(pth_msgport_t mp)
{
    int n;
    if (mp == NULL)
        return pth_error(-1, EINVAL);
    pth_spin_lock(&mp->mp_lock);
    n = pth_ring_elements(&mp->mp_queue);
    pth_spin_unlock(&mp->mp_lock);
    return n;
}

/* put a message on a port */
int pth_msgport_put(pth_msgport_t mp, pth_message_t *m)
{
    if (mp == NULL)
        return pth_error(FALSE, EINVAL);
    pth_spin_lock(&mp->mp_lock);
    pth_ring_append(&mp->mp_queue, (pth_ringnode_t *)m);
    pth_spin_unlock(&mp->mp_lock);
    /* a thread parked on PTH_EVENT_MSG for this port may live on another
       scheduler asleep in poll(2); nudge the other schedulers so they
       re-evaluate the (now non-empty) port on their next loop */
    pth_gsched_kick_others();
    return TRUE;
}

/* get top message from a port */
pth_message_t *pth_msgport_get(pth_msgport_t mp)
{
    pth_message_t *m;

    if (mp == NULL)
        return pth_error((pth_message_t *)NULL, EINVAL);
    pth_spin_lock(&mp->mp_lock);
    m = (pth_message_t *)pth_ring_pop(&mp->mp_queue);
    pth_spin_unlock(&mp->mp_lock);
    return m;
}

/* reply message to sender */
int pth_msgport_reply(pth_message_t *m)
{
    if (m == NULL)
        return pth_error(FALSE, EINVAL);
    return pth_msgport_put(m->m_replyport, m);
}

