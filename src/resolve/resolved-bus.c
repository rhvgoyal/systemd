/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include "bus-errors.h"
#include "bus-util.h"

#include "resolved.h"
#include "resolved-dns-domain.h"

static int reply_query_state(DnsQuery *q) {
        _cleanup_free_ char *ip = NULL;
        const char *name;
        int r;

        if (q->request_hostname)
                name = q->request_hostname;
        else {
                r = in_addr_to_string(q->request_family, &q->request_address, &ip);
                if (r < 0)
                        return r;

                name = ip;
        }

        switch (q->state) {

        case DNS_QUERY_NO_SERVERS:
                return sd_bus_reply_method_errorf(q->request, BUS_ERROR_NO_NAME_SERVERS, "No appropriate name servers or networks for name found");

        case DNS_QUERY_TIMEOUT:
                return sd_bus_reply_method_errorf(q->request, SD_BUS_ERROR_TIMEOUT, "Query timed out");

        case DNS_QUERY_ATTEMPTS_MAX:
                return sd_bus_reply_method_errorf(q->request, SD_BUS_ERROR_TIMEOUT, "All attempts to contact name servers or networks failed");

        case DNS_QUERY_RESOURCES:
                return sd_bus_reply_method_errorf(q->request, BUS_ERROR_NO_RESOURCES, "Not enough resources");

        case DNS_QUERY_INVALID_REPLY:
                return sd_bus_reply_method_errorf(q->request, BUS_ERROR_INVALID_REPLY, "Received invalid reply");

        case DNS_QUERY_FAILURE: {
                _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;

                if (q->answer_rcode == DNS_RCODE_NXDOMAIN)
                        sd_bus_error_setf(&error, _BUS_ERROR_DNS "NXDOMAIN", "'%s' not found", name);
                else {
                        const char *rc, *n;
                        char p[3]; /* the rcode is 4 bits long */

                        rc = dns_rcode_to_string(q->answer_rcode);
                        if (!rc) {
                                sprintf(p, "%i", q->answer_rcode);
                                rc = p;
                        }

                        n = strappenda(_BUS_ERROR_DNS, rc);
                        sd_bus_error_setf(&error, n, "Could not resolve '%s', server or network returned error %s", name, rc);
                }

                return sd_bus_reply_method_error(q->request, &error);
        }

        case DNS_QUERY_NULL:
        case DNS_QUERY_PENDING:
        case DNS_QUERY_SUCCESS:
        default:
                assert_not_reached("Impossible state");
        }
}

static int append_address(sd_bus_message *reply, DnsResourceRecord *rr, int ifindex) {
        int r;

        assert(reply);
        assert(rr);

        r = sd_bus_message_open_container(reply, 'r', "iayi");
        if (r < 0)
                return r;

        if (rr->key->type == DNS_TYPE_A) {
                r = sd_bus_message_append(reply, "i", AF_INET);
                if (r < 0)
                        return r;

                r = sd_bus_message_append_array(reply, 'y', &rr->a.in_addr, sizeof(struct in_addr));

        } else if (rr->key->type == DNS_TYPE_AAAA) {
                r = sd_bus_message_append(reply, "i", AF_INET6);
                if (r < 0)
                        return r;

                r = sd_bus_message_append_array(reply, 'y', &rr->aaaa.in6_addr, sizeof(struct in6_addr));
        } else
                return -EAFNOSUPPORT;

        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "i", ifindex);
        if (r < 0)
                return r;

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        return 0;
}

static void bus_method_resolve_hostname_complete(DnsQuery *q) {
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *cname = NULL, *canonical = NULL;
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        _cleanup_(dns_answer_unrefp) DnsAnswer *answer = NULL;
        unsigned added = 0, i;
        int r, ifindex;

        assert(q);

        if (q->state != DNS_QUERY_SUCCESS) {
                r = reply_query_state(q);
                goto finish;
        }

        r = sd_bus_message_new_method_return(q->request, &reply);
        if (r < 0)
                goto finish;

        r = sd_bus_message_open_container(reply, 'a', "(iayi)");
        if (r < 0)
                goto finish;

        answer = dns_answer_ref(q->answer);
        ifindex = q->answer_ifindex;

        for (i = 0; i < answer->n_rrs; i++) {
                r = dns_question_matches_rr(q->question, answer->rrs[i]);
                if (r < 0)
                        goto parse_fail;
                if (r == 0) {
                        /* Hmm, if this is not an address record,
                           maybe it's a cname? If so, remember this */
                        r = dns_question_matches_cname(q->question, answer->rrs[i]);
                        if (r < 0)
                                goto parse_fail;
                        if (r > 0)
                                cname = dns_resource_record_ref(answer->rrs[i]);

                        continue;
                }

                r = append_address(reply, answer->rrs[i], ifindex);
                if (r < 0)
                        goto finish;

                if (!canonical)
                        canonical = dns_resource_record_ref(answer->rrs[i]);

                added ++;
        }

        if (added <= 0) {
                if (!cname) {
                        r = sd_bus_reply_method_errorf(q->request, BUS_ERROR_NO_SUCH_RR, "'%s' does not have any RR of requested type", q->request_hostname);
                        goto finish;
                }

                /* This has a cname? Then update the query with the
                 * new cname. */
                r = dns_query_cname_redirect(q, cname->cname.name);
                if (r < 0) {
                        if (r == -ELOOP)
                                r = sd_bus_reply_method_errorf(q->request, BUS_ERROR_CNAME_LOOP, "CNAME loop on '%s'", q->request_hostname);
                        else
                                r = sd_bus_reply_method_errno(q->request, -r, NULL);

                        goto finish;
                }

                /* Before we restart the query, let's see if any of
                 * the RRs we already got already answers our query */
                for (i = 0; i < answer->n_rrs; i++) {
                        r = dns_question_matches_rr(q->question, answer->rrs[i]);
                        if (r < 0)
                                goto parse_fail;
                        if (r == 0)
                                continue;

                        r = append_address(reply, answer->rrs[i], ifindex);
                        if (r < 0)
                                goto finish;

                        if (!canonical)
                                canonical = dns_resource_record_ref(answer->rrs[i]);

                        added++;
                }

                /* If we didn't find anything, then let's restart the
                 * query, this time with the cname */
                if (added <= 0) {
                        r = dns_query_go(q);
                        if (r == -ESRCH) {
                                r = sd_bus_reply_method_errorf(q->request, BUS_ERROR_NO_NAME_SERVERS, "No appropriate name servers or networks for name found");
                                goto finish;
                        }
                        if (r < 0) {
                                r = sd_bus_reply_method_errno(q->request, -r, NULL);
                                goto finish;
                        }
                        return;
                }
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                goto finish;

        /* Return the precise spelling and uppercasing reported by the server */
        assert(canonical);
        r = sd_bus_message_append(reply, "s", DNS_RESOURCE_KEY_NAME(canonical->key));
        if (r < 0)
                goto finish;

        r = sd_bus_send(q->manager->bus, reply, NULL);
        goto finish;

parse_fail:
        r = sd_bus_reply_method_errorf(q->request, BUS_ERROR_INVALID_REPLY, "Received invalid reply");

finish:
        if (r < 0)
                log_error("Failed to send bus reply: %s", strerror(-r));

        dns_query_free(q);
}

static int bus_method_resolve_hostname(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        Manager *m = userdata;
        const char *hostname;
        int family;
        DnsQuery *q;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "si", &hostname, &family);
        if (r < 0)
                return r;

        if (!IN_SET(family, AF_INET, AF_INET6, AF_UNSPEC))
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Unknown address family %i", family);

        if (!hostname_is_valid(hostname))
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid hostname '%s'", hostname);

        question = dns_question_new(family == AF_UNSPEC ? 2 : 1);
        if (!question)
                return -ENOMEM;

        if (family != AF_INET6) {
                _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

                key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, hostname);
                if (!key)
                        return -ENOMEM;

                r = dns_question_add(question, key);
                if (r < 0)
                        return r;
        }

        if (family != AF_INET) {
                _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

                key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_AAAA, hostname);
                if (!key)
                        return -ENOMEM;

                r = dns_question_add(question, key);
                if (r < 0)
                        return r;
        }

        r = dns_query_new(m, &q, question);
        if (r < 0)
                return r;

        q->request = sd_bus_message_ref(message);
        q->request_family = family;
        q->request_hostname = hostname;
        q->complete = bus_method_resolve_hostname_complete;

        r = dns_query_go(q);
        if (r < 0) {
                dns_query_free(q);

                if (r == -ESRCH)
                        sd_bus_error_setf(error, BUS_ERROR_NO_NAME_SERVERS, "No appropriate name servers or networks for name found");

                return r;
        }

        return 1;
}

static void bus_method_resolve_address_complete(DnsQuery *q) {
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        _cleanup_(dns_answer_unrefp) DnsAnswer *answer = NULL;
        unsigned added = 0, i;
        int r;

        assert(q);

        if (q->state != DNS_QUERY_SUCCESS) {
                r = reply_query_state(q);
                goto finish;
        }

        r = sd_bus_message_new_method_return(q->request, &reply);
        if (r < 0)
                goto finish;

        r = sd_bus_message_open_container(reply, 'a', "s");
        if (r < 0)
                goto finish;

        answer = dns_answer_ref(q->answer);

        for (i = 0; i < answer->n_rrs; i++) {
                r = dns_question_matches_rr(q->question, answer->rrs[i]);
                if (r < 0)
                        goto parse_fail;
                if (r == 0)
                        continue;

                r = sd_bus_message_append(reply, "s", answer->rrs[i]->ptr.name);
                if (r < 0)
                        goto finish;

                added ++;
        }

        if (added <= 0) {
                _cleanup_free_ char *ip = NULL;

                in_addr_to_string(q->request_family, &q->request_address, &ip);

                r = sd_bus_reply_method_errorf(q->request, BUS_ERROR_NO_SUCH_RR, "Address '%s' does not have any RR of requested type", ip);
                goto finish;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                goto finish;

        r = sd_bus_send(q->manager->bus, reply, NULL);
        goto finish;

parse_fail:
        r = sd_bus_reply_method_errorf(q->request, BUS_ERROR_INVALID_REPLY, "Received invalid reply");

finish:
        if (r < 0)
                log_error("Failed to send bus reply: %s", strerror(-r));

        dns_query_free(q);
}

static int bus_method_resolve_address(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_free_ char *reverse = NULL;
        Manager *m = userdata;
        int family, ifindex;
        const void *d;
        DnsQuery *q;
        size_t sz;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "i", &family);
        if (r < 0)
                return r;

        if (!IN_SET(family, AF_INET, AF_INET6))
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Unknown address family %i", family);

        r = sd_bus_message_read_array(message, 'y', &d, &sz);
        if (r < 0)
                return r;

        if (sz != FAMILY_ADDRESS_SIZE(family))
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid address size");

        r = sd_bus_message_read(message, "i", &ifindex);
        if (r < 0)
                return r;
        if (ifindex < 0)
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid interface index");

        r = dns_name_reverse(family, d, &reverse);
        if (r < 0)
                return r;

        question = dns_question_new(1);
        if (!question)
                return -ENOMEM;

        key = dns_resource_key_new_consume(DNS_CLASS_IN, DNS_TYPE_PTR, reverse);
        if (!key)
                return -ENOMEM;

        reverse = NULL;

        r = dns_question_add(question, key);
        if (r < 0)
                return r;

        r = dns_query_new(m, &q, question);
        if (r < 0)
                return r;

        q->request = sd_bus_message_ref(message);
        q->request_family = family;
        memcpy(&q->request_address, d, sz);
        q->complete = bus_method_resolve_address_complete;

        r = dns_query_go(q);
        if (r < 0) {
                dns_query_free(q);

                if (r == -ESRCH)
                        sd_bus_error_setf(error, BUS_ERROR_NO_NAME_SERVERS, "No appropriate name servers or networks for name found");

                return r;
        }

        return 1;
}

static const sd_bus_vtable resolve_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("ResolveHostname", "si", "a(iayi)s", bus_method_resolve_hostname, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("ResolveAddress", "iayi", "as", bus_method_resolve_address, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END,
};

static int on_bus_retry(sd_event_source *s, usec_t usec, void *userdata) {
        Manager *m = userdata;

        assert(s);
        assert(m);

        m->bus_retry_event_source = sd_event_source_unref(m->bus_retry_event_source);

        manager_connect_bus(m);
        return 0;
}

int manager_connect_bus(Manager *m) {
        int r;

        assert(m);

        if (m->bus)
                return 0;

        r = sd_bus_default_system(&m->bus);
        if (r < 0) {
                /* We failed to connect? Yuck, we must be in early
                 * boot. Let's try in 5s again. As soon as we have
                 * kdbus we can stop doing this... */

                log_debug("Failed to connect to bus, trying again in 5s: %s", strerror(-r));

                r = sd_event_add_time(m->event, &m->bus_retry_event_source, CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 5*USEC_PER_SEC, 0, on_bus_retry, m);
                if (r < 0) {
                        log_error("Failed to install bus reconnect time event: %s", strerror(-r));
                        return r;
                }

                return 0;
        }

        r = sd_bus_add_object_vtable(m->bus, NULL, "/org/freedesktop/resolve1", "org.freedesktop.resolve1.Manager", resolve_vtable, m);
        if (r < 0) {
                log_error("Failed to register object: %s", strerror(-r));
                return r;
        }

        r = sd_bus_request_name(m->bus, "org.freedesktop.resolve1", 0);
        if (r < 0) {
                log_error("Failed to register name: %s", strerror(-r));
                return r;
        }

        r = sd_bus_attach_event(m->bus, m->event, 0);
        if (r < 0) {
                log_error("Failed to attach bus to event loop: %s", strerror(-r));
                return r;
        }

        return 0;
}